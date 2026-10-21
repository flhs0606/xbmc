/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BitstreamConverter.h"

#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "utils/StringUtils.h"
#include "utils/HDR10PlusConvert.h"
#include "utils/log.h"

#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "ServiceBroker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <fmt/format.h>

extern "C"
{
#include <libdovi/rpu_parser.h>
}

enum
{
  HEVC_NAL_UNSPEC62 = 62, // Dolby Vision RPU
  HEVC_NAL_UNSPEC63 = 63 // Dolby Vision EL
};

namespace
{
bool IsValidPtsForInjection(double pts)
{
  return std::isfinite(pts) && pts >= 0.0;
}

constexpr char PTS_MARKER[] = "PTS_US64=";

bool AppendPtsToDoviRpuNalu(std::vector<uint8_t>& nalu, uint64_t ptsUs64)
{
  // Expect the final rbsp_trailing_bits byte.
  if (nalu.size() < 1 || nalu.back() != 0x80)
    return false;

  std::vector<uint8_t> trailer;
  trailer.reserve(sizeof(PTS_MARKER) - 1 + 16 + 1);
  trailer.insert(trailer.end(), reinterpret_cast<const uint8_t*>(PTS_MARKER),
                 reinterpret_cast<const uint8_t*>(PTS_MARKER) + (sizeof(PTS_MARKER) - 1));
  const std::string ptsHex = fmt::format("{:016X}", ptsUs64);
  trailer.insert(trailer.end(), ptsHex.begin(), ptsHex.end());
  trailer.push_back(static_cast<uint8_t>(';'));

  // Insert trailer right before the final 0x80 byte.
  nalu.insert(nalu.end() - 1, trailer.begin(), trailer.end());
  return true;
}

bool CachedRpuInputMatches(const std::vector<uint8_t>& cachedNalu,
                           const uint8_t* nalBuf,
                           int32_t nalSize)
{
  if (!nalBuf || nalSize <= 0) return false;

  const size_t size = static_cast<size_t>(nalSize);

  if (cachedNalu.size() != size) return false;

  // DoVi RPU RBSPs end with a CRC32 followed by rbsp_trailing_bits (0x80).
  // On the encoded NAL bytes, start-code emulation prevention can insert up to
  // two 0x03 bytes inside that 5-byte tail, so compare the last 7 bytes first
  // for a cheap early reject, then fall back to a full compare on a match.
  constexpr size_t crcAndTrailingSize = 7;
  if (size <= crcAndTrailingSize)
    return std::equal(cachedNalu.begin(), cachedNalu.end(), nalBuf);

  const auto cachedSuffixBegin = cachedNalu.end() - crcAndTrailingSize;
  if (!std::equal(cachedSuffixBegin, cachedNalu.end(), nalBuf + (size - crcAndTrailingSize)))
    return false;

  return std::equal(cachedNalu.begin(), cachedSuffixBegin, nalBuf);
}

bool IsCMv29NoL2(const DoviRpuDataHeader* header,
                 const DoviVdrDmData* vdrDmData)
{
  if (!header || !vdrDmData) return false;

  if (vdrDmData->dm_data.level254) return false;

  if (vdrDmData->dm_data.level2.len > 0) return false;

  return true;
}

inline void PopulateDoviRpuInfo(DoviRpuOpaque* opaque,
                                bool firstFrame,
                                DOVIELType& doviElType,
                                AVDOVIDecoderConfigurationRecord& dovi,
                                double pts,
                                CDataCacheCore& dataCacheCore,
                                DOVIFrameMetadata* outDoViFrameMetadata = nullptr)
{
  const DoviVdrDmData* vdrDmData = dovi_rpu_get_vdr_dm_data(opaque);

  if (vdrDmData)
  {
    DOVIFrameMetadata doviFrameMetadata;

    if (vdrDmData->dm_data.level1)
    {
      doviFrameMetadata.level1_min_pq = vdrDmData->dm_data.level1->min_pq;
      doviFrameMetadata.level1_max_pq = vdrDmData->dm_data.level1->max_pq;
      doviFrameMetadata.level1_avg_pq = vdrDmData->dm_data.level1->avg_pq;
      doviFrameMetadata.pts = pts;
    }

    if (vdrDmData->dm_data.level5)
    {
      doviFrameMetadata.has_level5_metadata = true;
      doviFrameMetadata.level5_active_area_left_offset =
          vdrDmData->dm_data.level5->active_area_left_offset;
      doviFrameMetadata.level5_active_area_right_offset =
          vdrDmData->dm_data.level5->active_area_right_offset;
      doviFrameMetadata.level5_active_area_top_offset =
          vdrDmData->dm_data.level5->active_area_top_offset;
      doviFrameMetadata.level5_active_area_bottom_offset =
          vdrDmData->dm_data.level5->active_area_bottom_offset;
    }

    dataCacheCore.SetVideoDoViFrameMetadata(doviFrameMetadata);
    if (outDoViFrameMetadata)
      *outDoViFrameMetadata = doviFrameMetadata;
  }

  if (firstFrame)
  {
    DOVIStreamMetadata doviStreamMetadata;

    if (vdrDmData)
    {
      doviStreamMetadata.source_min_pq = vdrDmData->source_min_pq;
      doviStreamMetadata.source_max_pq = vdrDmData->source_max_pq;
    }

    if (vdrDmData && vdrDmData->dm_data.level6)
    {
      doviStreamMetadata.has_level6_metadata = true;

      doviStreamMetadata.level6_max_lum =
          vdrDmData->dm_data.level6->max_display_mastering_luminance;
      doviStreamMetadata.level6_min_lum =
          vdrDmData->dm_data.level6->min_display_mastering_luminance;

      doviStreamMetadata.level6_max_cll = vdrDmData->dm_data.level6->max_content_light_level;
      doviStreamMetadata.level6_max_fall =
          vdrDmData->dm_data.level6->max_frame_average_light_level;
    }

    std::string metaVersion;
    bool hasLevel254 = false;
    unsigned int level2Count = 0;
    unsigned int level8Count = 0;
    if (vdrDmData && vdrDmData->dm_data.level254)
    {
      hasLevel254 = true;
      level8Count = vdrDmData->dm_data.level8.len;
      const unsigned int noL8 = vdrDmData->dm_data.level8.len;
      if (noL8 > 0)
        metaVersion = fmt::format("CMv4.0 {}-{} {}-L8", vdrDmData->dm_data.level254->dm_version_index,
                                 vdrDmData->dm_data.level254->dm_mode, noL8);
      else
        metaVersion = fmt::format("CMv4.0 {}-{}", vdrDmData->dm_data.level254->dm_version_index,
                                 vdrDmData->dm_data.level254->dm_mode);
    }
    else if (vdrDmData && vdrDmData->dm_data.level1)
    {
      level2Count = vdrDmData->dm_data.level2.len;
      const unsigned int noL2 = vdrDmData->dm_data.level2.len;
      if (noL2 > 0)
        metaVersion = fmt::format("CMv2.9 {}-L2", noL2);
      else
        metaVersion = "CMv2.9";
    }

    static bool loggedParsedMetadata = false;
    if (!loggedParsedMetadata)
    {
      loggedParsedMetadata = true;
      logM(LOGINFO, "CBitstreamConverterDoVi",
           "Parsed DoVi metadata (first frame): meta='{}' has_l254={} l2_count={} l8_count={}",
           metaVersion, hasLevel254, level2Count, level8Count);
    }

    doviStreamMetadata.meta_version = metaVersion;
    dataCacheCore.SetVideoDoViStreamMetadata(doviStreamMetadata);
    aml_dv_send_md_levels();

    DOVIStreamInfo doviStreamInfo;
    const DoviRpuDataHeader* header = dovi_rpu_get_header(opaque);
    doviElType = DOVIELType::TYPE_NONE;

    // header can be NULL even when opaque is valid, e.g. on resume
    // where a mid-stream seek lands on a partially corrupt RPU.
    if (header)
    {
      aml_dv_send_profile(header->guessed_profile);

      if ((header->guessed_profile == 4) || (header->guessed_profile == 7))
      {
        if (header->el_type)
        {
          if (StringUtils::EqualsNoCase(header->el_type, "FEL"))
            doviElType = DOVIELType::TYPE_FEL;
          else if (StringUtils::EqualsNoCase(header->el_type, "MEL"))
            doviElType = DOVIELType::TYPE_MEL;
        }
      }
    }

    doviStreamInfo.dovi_el_type = doviElType;
    doviStreamInfo.dovi = dovi;

    doviStreamInfo.has_config =
        (memcmp(&dovi, &CDVDStreamInfo::empty_dovi, sizeof(AVDOVIDecoderConfigurationRecord)) != 0);
    doviStreamInfo.has_header = (header != nullptr);

    dataCacheCore.SetVideoDoViStreamInfo(doviStreamInfo);
    aml_dv_send_el_type();
    dovi_rpu_free_header(header);
  }

  dovi_rpu_free_vdr_dm_data(vdrDmData);
}

void GetDoviRpuInfo(uint8_t* nalBuf,
                    uint32_t nalSize,
                    bool firstFrame,
                    DOVIELType& doviElType,
                    AVDOVIDecoderConfigurationRecord& dovi,
                    double pts,
                    CDataCacheCore& dataCacheCore)
{
  // https://professionalsupport.dolby.com/s/article/Dolby-Vision-Metadata-Levels?language=en_US

  DoviRpuOpaque* opaque = dovi_parse_unspec62_nalu(nalBuf, nalSize);
  PopulateDoviRpuInfo(opaque, firstFrame, doviElType, dovi, pts, dataCacheCore);
  dovi_rpu_free(opaque);
}

inline DOVIELType GetElTypeFromHeader(const DoviRpuDataHeader* header)
{
  if (header && ((header->guessed_profile == 4) || (header->guessed_profile == 7)) &&
      header->el_type)
  {
    if (StringUtils::EqualsNoCase(header->el_type, "FEL"))
      return DOVIELType::TYPE_FEL;
    if (StringUtils::EqualsNoCase(header->el_type, "MEL"))
      return DOVIELType::TYPE_MEL;
  }

  return DOVIELType::TYPE_NONE;
}

inline void ConvertDoVi(DOVIMode convertMode,
                        bool firstFrame,
                        DoviRpuOpaque* opaque,
                        const DoviRpuDataHeader*& header,
                        const DoviVdrDmData*& vdrDmData,
                        CDVDStreamInfo& hints,
                        CDataCacheCore& dataCacheCore,
                        uint8_t*& nalBuf,
                        int32_t& nalSize,
                        const DoviData*& rpuData)
{
  if (!header || (header->guessed_profile != 7)) return;

  if (firstFrame)
  {
    DOVIStreamInfo doviStreamInfo;
    doviStreamInfo.dovi_el_type = GetElTypeFromHeader(header);
    doviStreamInfo.dovi = hints.dovi;
    dataCacheCore.SetVideoSourceDoViStreamInfo(doviStreamInfo);
  }

  if (dovi_convert_rpu_with_mode(opaque, convertMode) >= 0)
    rpuData = dovi_write_unspec62_nalu(opaque);

  if (!rpuData) return;

  nalBuf = const_cast<uint8_t*>(rpuData->data);
  nalSize = rpuData->len;

  hints.dovi.el_present_flag = 0; // EL removed in both conversion cases - to MEL and to P8.1
  if (convertMode == DOVIMode::MODE_TO81)
  {
    hints.dovi.dv_profile = 8;
    hints.dovi.dv_bl_signal_compatibility_id = 1;
  }

  dovi_rpu_free_header(header);

  header = dovi_rpu_get_header(opaque);
  dovi_rpu_free_vdr_dm_data(vdrDmData);
  vdrDmData = dovi_rpu_get_vdr_dm_data(opaque);
}

inline bool AppendCMv40(DoviRpuOpaque* opaque,
                        const DoviVdrDmData* vdrDmData,
                        uint8_t*& nalBuf,
                        int32_t& nalSize,
                        const DoviData*& rpuData)
{
  if (!vdrDmData || !opaque) return false;

  if (vdrDmData->dm_data.level254) return false;

  // Caller has already gated on `shouldAppend`, which encodes the L2 trim
  // decision (no-L2 → append unconditionally; L2 + low nits → append;
  // L2 + high nits → bypass). This helper is a writer, not a gate.
  if (dovi_rpu_add_cmv40_safe_default_metadata(opaque) != 1)
    return false;

  rpuData = dovi_write_unspec62_nalu(opaque);
  if (!rpuData) return false;

  nalBuf = const_cast<uint8_t*>(rpuData->data);
  nalSize = static_cast<int32_t>(rpuData->len);
  return true;
}

inline void InjectPtsForFel(DOVIMode convertMode,
                            DOVIELType doviElType,
                            double pts,
                            uint8_t*& nalBuf,
                            int32_t& nalSize,
                            std::vector<uint8_t>& nalu)
{
  if ((convertMode != DOVIMode::MODE_NONE) ||
      (doviElType != DOVIELType::TYPE_FEL) ||
      !IsValidPtsForInjection(pts) || !nalBuf || (nalSize <= 0)) return;

  if (nalu.empty())
    nalu.assign(nalBuf, nalBuf + nalSize);

  if (AppendPtsToDoviRpuNalu(nalu, static_cast<uint64_t>(pts)))
  {
    nalBuf = nalu.data();
    nalSize = static_cast<int32_t>(nalu.size());
  }
}
} // namespace

void CBitstreamConverter::ProcessDoViRpuWrap(
  uint8_t* nalBuf,
  int32_t nalSize,
  uint8_t** poutbuf,
  uint32_t& poutbufSize,
  double pts)
{
  int intPoutbufSize = poutbufSize;
  ProcessDoViRpu(nalBuf, nalSize, poutbuf, &intPoutbufSize, pts);
  poutbufSize = static_cast<uint32_t>(intPoutbufSize);
}

void CBitstreamConverter::ProcessDoViRpu(
  uint8_t* nalBuf,
  int32_t nalSize,
  uint8_t** poutbuf,
  int* poutbufSize,
  double pts)
{
  const DoviData* rpuData = nullptr;
  bool appended = false;
  std::vector<uint8_t> nalu;

  // Optimization: If the input RPU NAL is exactly identical to the previous frame's RPU NAL,
  // AND we are not processing the first frame (which parses stream metadata),
  // we can completely skip `dovi_parse_unspec62_nalu` and avoid all allocations & format processing.
  // This cache deliberately tracks the original input RPU, before any FEL PTS trailer is injected,
  // so a per-frame PTS never invalidates reuse.
  if (!m_first_frame && CachedRpuInputMatches(m_cached_dovi_rpu_in_nal, nalBuf, nalSize))
  {
    m_cached_dovi_frame_metadata.pts = pts;
    m_dataCacheCore.SetVideoDoViFrameMetadata(m_cached_dovi_frame_metadata);

    // Skip all processing and restore the fully configured/converted output NAL
    nalBuf = m_cached_dovi_rpu_out_nal.data();
    nalSize = static_cast<int32_t>(m_cached_dovi_rpu_out_nal.size());
  }
  else
  {
    // Save the original input stream bits before processing modifications
    m_cached_dovi_rpu_in_nal.assign(nalBuf, nalBuf + nalSize);

    DoviRpuOpaque* opaque = dovi_parse_unspec62_nalu(nalBuf, nalSize);
    const DoviRpuDataHeader* header = dovi_rpu_get_header(opaque);
    const DoviVdrDmData* vdrDmData = dovi_rpu_get_vdr_dm_data(opaque);

    if (m_convert_dovi != DOVIMode::MODE_NONE)
      ConvertDoVi(m_convert_dovi,
                  m_first_frame,
                  opaque,
                  header,
                  vdrDmData,
                  m_hints,
                  m_dataCacheCore,
                  nalBuf,
                  nalSize,
                  rpuData);

    if (m_append_cmv40 != DOVICMv40Mode::CMV40_NONE &&
        vdrDmData && !vdrDmData->dm_data.level254)
    {
      const bool level2IsEmpty = (vdrDmData->dm_data.level2.len == 0);
      bool shouldAppend = level2IsEmpty;
      if (m_append_cmv40 == DOVICMv40Mode::CMV40_SMART)
      {
        const bool hasData = (m_smart_display_nits > 0 && vdrDmData->dm_data.level1);
        const int contentNits = hasData
            ? max_pq_to_nits(static_cast<int>(vdrDmData->dm_data.level1->max_pq))
            : 0;
        const int threshold = m_smart_display_nits * (100 + SMART_CMV40_THRESHOLD_PCT) / 100;
        const bool bypass = !level2IsEmpty && hasData && (contentNits > threshold);
        shouldAppend = level2IsEmpty || !bypass;
        const DOVICMv40Mode effectiveMode =
            bypass ? DOVICMv40Mode::CMV40_NONE : DOVICMv40Mode::CMV40_SMART;

        if (effectiveMode != m_smart_last_effective)
        {
          std::string detail;
          if (level2IsEmpty)
            detail = "no L2 trims, appending CMv4.0";
          else if (!hasData)
            detail = "display nits unavailable, defaulting to append";
          else
            detail = fmt::format(
                "content {}nits display {}nits threshold {}nits ({}%) -> {}",
                contentNits, m_smart_display_nits, threshold, SMART_CMV40_THRESHOLD_PCT,
                bypass ? "bypass (no append)" : "append CMv4.0");
          logM(LOGINFO, "CBitstreamConverterDoVi",
               "Smart CMv4.0: {} (decision changed; evaluated per-frame)", detail);
          m_smart_last_effective = effectiveMode;
        }
      }

      if (shouldAppend)
        appended = AppendCMv40(opaque, vdrDmData, nalBuf, nalSize, rpuData);
    }

    PopulateDoviRpuInfo(opaque,
                        m_first_frame,
                        m_hints.dovi_el_type,
                        m_hints.dovi,
                        pts,
                        m_dataCacheCore,
                        &m_cached_dovi_frame_metadata);

    if (appended && m_first_frame)
      logM(LOGINFO, "CBitstreamConverterDoVi", "CMv4.0 extension appended to RPU");

    dovi_rpu_free_header(header);
    dovi_rpu_free_vdr_dm_data(vdrDmData);
    dovi_rpu_free(opaque);

    // Update cache with the newly calculated modified NAL out for the next frame
    m_cached_dovi_rpu_out_nal.assign(nalBuf, nalBuf + nalSize);
  }

  InjectPtsForFel(m_convert_dovi,
                  m_hints.dovi_el_type,
                  pts,
                  nalBuf,
                  nalSize,
                  nalu);

  // HEVC NAL unit type 62 is Dolby Vision RPU (UNSPEC62)
  BitstreamAllocAndCopy(poutbuf, poutbufSize, nullptr, 0, nalBuf, nalSize, 62);

  if (rpuData) dovi_data_free(rpuData);
}

void CBitstreamConverter::AddDoViRpuNaluWrap(const Hdr10PlusMetadata& meta,
                                            uint8_t** poutbuf,
                                            uint32_t& poutbufSize,
                                            double pts)
{
  int intPoutbufSize = poutbufSize;
  AddDoViRpuNalu(meta, poutbuf, &intPoutbufSize, pts);
  poutbufSize = static_cast<uint32_t>(intPoutbufSize);
}

void CBitstreamConverter::AddDoViRpuNalu(const Hdr10PlusMetadata& meta,
                                        uint8_t** poutbuf,
                                        int* poutbufSize,
                                        double pts) const
{
  auto nalu = create_dovi_rpu_nalu_from_hdr10plus(meta, m_convert_Hdr10Plus_peak_brightness_source,
                                                  m_hdrStaticMetadataInfo);

  if (nalu.empty()) return;

  if (m_first_frame)
  {
    m_hints.hdrType = StreamHdrType::HDR_TYPE_DOLBYVISION;
    m_hints.dovi.dv_version_major = 1;
    m_hints.dovi.dv_version_minor = 0;
    m_hints.dovi.dv_profile = 8;
    m_hints.dovi.dv_level = 6;
    m_hints.dovi.rpu_present_flag = 1;
    m_hints.dovi.el_present_flag = 0;
    m_hints.dovi.bl_present_flag = 1;
    m_hints.dovi.dv_bl_signal_compatibility_id = 1;
  }

  GetDoviRpuInfo(nalu.data(), static_cast<uint32_t>(nalu.size()), m_first_frame, m_hints.dovi_el_type,
                m_hints.dovi, pts, m_dataCacheCore);

  BitstreamAllocAndCopy(poutbuf, poutbufSize, nullptr, 0, nalu.data(),
                        static_cast<uint32_t>(nalu.size()), HEVC_NAL_UNSPEC62);
}

void CBitstreamConverter::AddDoViRpuNaluFromVivid(const HdrVividMetadata& meta,
                                                  uint8_t** poutbuf,
                                                  int* poutbufSize) const
{
  auto nalu = create_dovi_rpu_nalu_from_vivid(meta, m_hdrStaticMetadataInfo);

  if (nalu.empty()) return;

  if (m_first_frame)
  {
    m_hints.hdrType = StreamHdrType::HDR_TYPE_DOLBYVISION;
    m_hints.dovi.dv_version_major = 1;
    m_hints.dovi.dv_version_minor = 0;
    m_hints.dovi.dv_profile = 8;
    m_hints.dovi.dv_level = 6;
    m_hints.dovi.rpu_present_flag = 1;
    m_hints.dovi.el_present_flag = 0;
    m_hints.dovi.bl_present_flag = 1;
    m_hints.dovi.dv_bl_signal_compatibility_id = 1;
  }

  GetDoviRpuInfo(nalu.data(), static_cast<uint32_t>(nalu.size()), m_first_frame, m_hints.dovi_el_type,
                m_hints.dovi, AV_NOPTS_VALUE, m_dataCacheCore);

  BitstreamAllocAndCopy(poutbuf, poutbufSize, nullptr, 0, nalu.data(),
                        static_cast<uint32_t>(nalu.size()), HEVC_NAL_UNSPEC62);
}
