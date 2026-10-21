/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BitstreamConverter.h"
#include "BitstreamIoReader.h"
#include "BitstreamIoWriter.h"
#include "Crc32.h"

#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/StringUtils.h"
#include "utils/LogThrottle.h"
#include "utils/log.h"

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
  return pts != DVD_NOPTS_VALUE && std::isfinite(pts) && pts >= 0.0;
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
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  char ptsHex[16];
  for (int i = 15; i >= 0; --i)
  {
    ptsHex[i] = HEX_DIGITS[ptsUs64 & 0xF];
    ptsUs64 >>= 4;
  }
  trailer.insert(trailer.end(), ptsHex, ptsHex + 16);
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

std::string BuildMetaVersionString(const DoviVdrDmData* vdrDmData)
{
  if (vdrDmData && vdrDmData->dm_data.level254)
  {
    const unsigned int level8Count = vdrDmData->dm_data.level8.len;
    if (level8Count > 0)
      return fmt::format("CMv4.0 {}-{} {}-L8", vdrDmData->dm_data.level254->dm_version_index,
                         vdrDmData->dm_data.level254->dm_mode, level8Count);

    return fmt::format("CMv4.0 {}-{}", vdrDmData->dm_data.level254->dm_version_index,
                       vdrDmData->dm_data.level254->dm_mode);
  }

  if (vdrDmData && vdrDmData->dm_data.level1)
  {
    const unsigned int level2Count = vdrDmData->dm_data.level2.len;
    if (level2Count > 0)
      return fmt::format("CMv2.9 {}-L2", level2Count);

    return "CMv2.9";
  }

  return "";
}

const std::string& BuildMetaVersionStringCached(const DoviVdrDmData* vdrDmData,
                                                DoviMetaVersionMemo& memo)
{
  uint8_t kind = 0;
  uint64_t a = 0, b = 0, c = 0;
  if (vdrDmData && vdrDmData->dm_data.level254)
  {
    a = vdrDmData->dm_data.level254->dm_version_index;
    b = vdrDmData->dm_data.level254->dm_mode;
    c = vdrDmData->dm_data.level8.len;
    kind = (c > 0) ? 4 : 3;
  }
  else if (vdrDmData && vdrDmData->dm_data.level1)
  {
    c = vdrDmData->dm_data.level2.len;
    kind = (c > 0) ? 2 : 1;
  }

  if (memo.valid && memo.kind == kind && memo.a == a && memo.b == b && memo.c == c)
    return memo.str;

  memo.kind = kind;
  memo.a = a;
  memo.b = b;
  memo.c = c;
  memo.valid = true;
  memo.str = BuildMetaVersionString(vdrDmData);
  return memo.str;
}

inline void PopulateDoviFrameMetadata(const DoviVdrDmData* vdrDmData,
                                      const DoviVdrDmData* sourceVdrDmData,
                                      double pts,
                                      CDataCacheCore& dataCacheCore,
                                      DoviMetaVersionMemo& metaMemo,
                                      DoviMetaVersionMemo& srcMemo,
                                      DOVIFrameMetadata* outDoViFrameMetadata)
{
  DOVIFrameMetadata doviFrameMetadata;
  doviFrameMetadata.pts = pts;
  doviFrameMetadata.meta_version = BuildMetaVersionStringCached(vdrDmData, metaMemo);
  doviFrameMetadata.source_meta_version =
      (sourceVdrDmData == vdrDmData)
          ? doviFrameMetadata.meta_version
          : BuildMetaVersionStringCached(sourceVdrDmData, srcMemo);

  if (vdrDmData == nullptr)
  {
    dataCacheCore.SetVideoDoViFrameMetadata(doviFrameMetadata);
    if (outDoViFrameMetadata)
      *outDoViFrameMetadata = doviFrameMetadata;
    return;
  }

  if (vdrDmData->dm_data.level1)
  {
    doviFrameMetadata.level1_min_pq = vdrDmData->dm_data.level1->min_pq;
    doviFrameMetadata.level1_max_pq = vdrDmData->dm_data.level1->max_pq;
    doviFrameMetadata.level1_avg_pq = vdrDmData->dm_data.level1->avg_pq;
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

inline void PopulateDoviStreamMetadata(const DoviVdrDmData* vdrDmData,
                                       const DoviVdrDmData* sourceVdrDmData,
                                       CDataCacheCore& dataCacheCore)
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
    doviStreamMetadata.level6_max_lum = vdrDmData->dm_data.level6->max_display_mastering_luminance;
    doviStreamMetadata.level6_min_lum = vdrDmData->dm_data.level6->min_display_mastering_luminance;
    doviStreamMetadata.level6_max_cll = vdrDmData->dm_data.level6->max_content_light_level;
    doviStreamMetadata.level6_max_fall = vdrDmData->dm_data.level6->max_frame_average_light_level;
  }

  const std::string metaVersion = BuildMetaVersionString(vdrDmData);
  const std::string sourceMetaVersion = BuildMetaVersionString(sourceVdrDmData);
  bool hasLevel254 = false;
  unsigned int level2Count = 0;
  unsigned int level8Count = 0;
  if (vdrDmData && vdrDmData->dm_data.level254)
  {
    hasLevel254 = true;
    level8Count = vdrDmData->dm_data.level8.len;
  }
  else if (vdrDmData && vdrDmData->dm_data.level1)
  {
    level2Count = vdrDmData->dm_data.level2.len;
  }

  logM(LOGDEBUG, "Parsed DoVi metadata (first frame): meta [{}] source_meta [{}] has_l254 [{}] l2_count [{}] l8_count [{}]",
                 metaVersion, sourceMetaVersion, hasLevel254, level2Count, level8Count);

  doviStreamMetadata.meta_version = metaVersion;
  doviStreamMetadata.source_meta_version = sourceMetaVersion;
  dataCacheCore.SetVideoDoViStreamMetadata(doviStreamMetadata);
  aml_dv_send_md_levels();
}

inline void PublishCMv40OutputMeta(int scenario,
                                   const DoviVdrDmData* sourceVdrDmData,
                                   DoviRpuOpaque* appendedOpaque)
{
  std::string outputMetaVersion;
  if ((scenario == 4) && appendedOpaque)
  {
    const DoviVdrDmData* appendedVdr = dovi_rpu_get_vdr_dm_data(appendedOpaque);
    outputMetaVersion = BuildMetaVersionString(appendedVdr);
    dovi_rpu_free_vdr_dm_data(appendedVdr);
  }
  else
  {
    outputMetaVersion = BuildMetaVersionString(sourceVdrDmData);
  }

  auto& dataCacheCore = CServiceBroker::GetDataCacheCore();
  DOVIStreamMetadata current = dataCacheCore.GetVideoDoViStreamMetadata();
  if (!(outputMetaVersion.empty() && !sourceVdrDmData))
    current.meta_version = outputMetaVersion;
  dataCacheCore.SetVideoDoViStreamMetadata(current);
  aml_dv_send_md_levels();
}

inline DOVIELType GetDoviElType(const DoviRpuDataHeader* header)
{
  if (header && ((header->guessed_profile == 4) || (header->guessed_profile == 7)) && header->el_type)
  {
    if (StringUtils::EqualsNoCase(header->el_type, "FEL"))
      return DOVIELType::TYPE_FEL;
    if (StringUtils::EqualsNoCase(header->el_type, "MEL"))
      return DOVIELType::TYPE_MEL;
  }

  return DOVIELType::TYPE_NONE;
}

inline void PopulateDoviStreamInfo(const DoviRpuDataHeader* header,
                                   DOVIELType& doviElType,
                                   const AVDOVIDecoderConfigurationRecord& dovi,
                                   CDataCacheCore& dataCacheCore,
                                   bool isDualTrack = false)
{
  DOVIStreamInfo doviStreamInfo;

  doviElType = GetDoviElType(header);
  doviStreamInfo.dovi_el_type = doviElType;
  doviStreamInfo.dovi = dovi;
  doviStreamInfo.has_config =
      (memcmp(&dovi, &CDVDStreamInfo::empty_dovi, sizeof(AVDOVIDecoderConfigurationRecord)) != 0);
  doviStreamInfo.has_header = (header != nullptr);
  doviStreamInfo.is_dual_track = isDualTrack;

  dataCacheCore.SetVideoDoViStreamInfo(doviStreamInfo);
  aml_dv_send_el_type();
  if (header)
    aml_dv_send_profile(header->guessed_profile);
}

inline void PopulateDoviFirstFrameStreamInfo(DoviRpuOpaque* metadataOpaque,
                                             DoviRpuOpaque* sourceOpaque,
                                             DOVIELType& doviElType,
                                             AVDOVIDecoderConfigurationRecord& dovi,
                                             CDataCacheCore& dataCacheCore,
                                             bool isDualTrack = false)
{
  const DoviVdrDmData* vdrDmData = dovi_rpu_get_vdr_dm_data(metadataOpaque);

  const bool needsSeparateSourceFetch = sourceOpaque && sourceOpaque != metadataOpaque;
  // When sourceOpaque == metadataOpaque we are reading the original unmodified metadata, so reuse
  // the already-fetched vdrDmData allocation instead of requesting and freeing a duplicate copy.
  const DoviVdrDmData* sourceVdrDmData =
      needsSeparateSourceFetch ? dovi_rpu_get_vdr_dm_data(sourceOpaque) : vdrDmData;

  PopulateDoviStreamMetadata(vdrDmData, sourceVdrDmData, dataCacheCore);

  const DoviRpuDataHeader* header = dovi_rpu_get_header(metadataOpaque);
  PopulateDoviStreamInfo(header, doviElType, dovi, dataCacheCore, isDualTrack);
  dovi_rpu_free_header(header);

  // Only free sourceVdrDmData when it came from a separate sourceOpaque request above.
  if (needsSeparateSourceFetch)
    dovi_rpu_free_vdr_dm_data(sourceVdrDmData);

  dovi_rpu_free_vdr_dm_data(vdrDmData);
}

inline void PopulateDoviRpuInfo(DoviRpuOpaque* metadataOpaque,
                                const DoviVdrDmData* sourceVdrDmData,
                                bool metadataIsSource,
                                double pts,
                                CDataCacheCore& dataCacheCore,
                                DoviMetaVersionMemo& metaMemo,
                                DoviMetaVersionMemo& srcMemo,
                                DOVIFrameMetadata* outDoViFrameMetadata = nullptr)
{
  const DoviVdrDmData* vdrDmData =
      metadataIsSource ? sourceVdrDmData : dovi_rpu_get_vdr_dm_data(metadataOpaque);

  PopulateDoviFrameMetadata(vdrDmData, sourceVdrDmData, pts, dataCacheCore, metaMemo, srcMemo,
                            outDoViFrameMetadata);

  if (!metadataIsSource && vdrDmData)
    dovi_rpu_free_vdr_dm_data(vdrDmData);
}

void GetDoviRpuInfo(uint8_t* nalBuf,
                    uint32_t nalSize,
                    bool firstFrame,
                    DOVIELType& doviElType,
                    AVDOVIDecoderConfigurationRecord& dovi,
                    double pts,
                    CDataCacheCore& dataCacheCore,
                    bool isDualTrack,
                    DoviMetaVersionMemo& metaMemo,
                    DoviMetaVersionMemo& srcMemo)
{
  // https://professionalsupport.dolby.com/s/article/Dolby-Vision-Metadata-Levels?language=en_US

  DoviRpuOpaque* opaque = dovi_parse_unspec62_nalu(nalBuf, nalSize);
  if (opaque)
  {
    const DoviVdrDmData* vdrDmData = dovi_rpu_get_vdr_dm_data(opaque);
    PopulateDoviRpuInfo(opaque, vdrDmData, true, pts, dataCacheCore, metaMemo, srcMemo);
    if (firstFrame)
      PopulateDoviFirstFrameStreamInfo(opaque, opaque, doviElType, dovi, dataCacheCore, isDualTrack);
    if (vdrDmData)
      dovi_rpu_free_vdr_dm_data(vdrDmData);
    dovi_rpu_free(opaque);
  }
}

DoviRpuOpaque* AppendCMv40ToRpuNalu(uint8_t* nalBuf,
                                    int32_t nalSize,
                                    std::vector<uint8_t>& out,
                                    int& addResult)
{
  addResult = -1;
  if (!nalBuf || (nalSize <= 2)) return nullptr;

  DoviRpuOpaque* opaque = dovi_parse_unspec62_nalu(nalBuf, nalSize);
  if (!opaque) return nullptr;

  addResult = dovi_rpu_add_cmv40_safe_default_metadata(opaque);
  if (addResult != 1)
  {
    dovi_rpu_free(opaque);
    return nullptr;
  }

  const DoviData* rpuData = dovi_write_unspec62_nalu(opaque);
  if (!rpuData)
  {
    dovi_rpu_free(opaque);
    return nullptr;
  }

  out.assign(rpuData->data, rpuData->data + rpuData->len);
  dovi_data_free(rpuData);
  return opaque;
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

  if (dovi_convert_rpu_with_mode(opaque, convertMode) < 0)
    return;

  dovi_rpu_free_header(header);
  header = dovi_rpu_get_header(opaque);
  dovi_rpu_free_vdr_dm_data(vdrDmData);
  vdrDmData = dovi_rpu_get_vdr_dm_data(opaque);

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
}

inline void AppendCMv40(DOVICMv40Mode cmv40Mode,
                        const DoviRpuDataHeader* header,
                        const DoviVdrDmData* vdrDmData,
                        uint8_t*& nalBuf,
                        int32_t& nalSize,
                        std::vector<uint8_t>& nalu,
                        DoviRpuOpaque*& opaque,
                        int dvType,
                        bool vs10Converting,
                        int maxLumNits,
                        DOVICMv40AutoThreshold autoThreshold,
                        int auto2ThresholdPct,
                        int auto2ThresholdPq,
                        int& srcPqMemo,
                        int& srcNitsMemo,
                        DoViCMv40LogState& logState)
{
  bool hasL254 = false;
  int l2Count = 0;
  bool level2IsEmpty = false;
  int srcMaxPq = 0;
  int srcMaxNits = 0;
  bool isDisplayBrighter = false;
  bool autoTrigger = false;
  bool shouldAppend = false;
  int l1MaxPq = 0;
  bool auto2Trigger = false;
  int addResult = -1;

  int scenario = 0;

  if (!header || !vdrDmData || vs10Converting)
  {
    scenario = 1;
  }
  else
  {
    hasL254 = (vdrDmData->dm_data.level254 != nullptr);
    if (hasL254)
    {
      scenario = 2;
    }
    else
    {
      l2Count = static_cast<int>(vdrDmData->dm_data.level2.len);
      level2IsEmpty = (l2Count == 0);
      if (cmv40Mode == DOVICMv40Mode::CMV40_AUTO2)
      {
        const DoviExtMetadataBlockLevel1* level1 = vdrDmData->dm_data.level1;
        if (maxLumNits <= 0)
          auto2Trigger = false;
        else if (level1 == nullptr)
          auto2Trigger = true;
        else
        {
          l1MaxPq = static_cast<int>(level1->max_pq);
          auto2Trigger = (l1MaxPq <= auto2ThresholdPq);
        }
        shouldAppend = (level2IsEmpty || auto2Trigger);
      }
      else
      {
        srcMaxPq = static_cast<int>(vdrDmData->source_max_pq);
        if (srcMaxPq != srcPqMemo)
        {
          srcPqMemo = srcMaxPq;
          srcNitsMemo = max_pq_to_nits(srcMaxPq);
        }
        srcMaxNits = srcNitsMemo;
        isDisplayBrighter = (maxLumNits >= srcMaxNits);
        if (autoThreshold == DOVICMv40AutoThreshold::CMV40_AUTO_SOURCE)
        {
          autoTrigger = isDisplayBrighter;
        }
        else
        {
          int thresholdNits = 0;
          switch (autoThreshold)
          {
            case DOVICMv40AutoThreshold::CMV40_AUTO_1000_NITS:  thresholdNits = 1000;  break;
            case DOVICMv40AutoThreshold::CMV40_AUTO_2000_NITS:  thresholdNits = 2000;  break;
            case DOVICMv40AutoThreshold::CMV40_AUTO_4000_NITS:  thresholdNits = 4000;  break;
            case DOVICMv40AutoThreshold::CMV40_AUTO_10000_NITS: thresholdNits = 10000; break;
            default: break;
          }
          autoTrigger = (srcMaxNits <= thresholdNits);
        }
        shouldAppend = ((cmv40Mode == DOVICMv40Mode::CMV40_ALWAYS) ||
                        ((cmv40Mode == DOVICMv40Mode::CMV40_NO_L2) && level2IsEmpty) ||
                        ((cmv40Mode == DOVICMv40Mode::CMV40_AUTO) &&
                         (level2IsEmpty || autoTrigger)));
      }

      if (!shouldAppend)
      {
        scenario = 3;
      }
      else
      {
        opaque = AppendCMv40ToRpuNalu(nalBuf, nalSize, nalu, addResult);
        if (opaque)
        {
          nalBuf = nalu.data();
          nalSize = static_cast<int32_t>(nalu.size());
          scenario = 4;
        }
        else if (addResult == 0)
        {
          scenario = 6;
        }
        else
        {
          scenario = 5;
        }
      }
    }
  }

  if (scenario != logState.last_published_scenario)
  {
    PublishCMv40OutputMeta(scenario, vdrDmData, opaque);
    logState.last_published_scenario = scenario;
  }

  if (!CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) ||
      !CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO))
    return;

  DoViCMv40LogStateSnapshot snap;
  snap.scenario = scenario;
  snap.headerPresent = (header != nullptr);
  snap.vdrDmDataPresent = (vdrDmData != nullptr);
  snap.dvType = dvType;
  snap.vs10Converting = vs10Converting;
  snap.cmv40Mode = static_cast<int>(cmv40Mode);
  snap.maxLumNits = maxLumNits;

  if (scenario >= 2)
    snap.hasL254 = hasL254;
  if (scenario >= 3)
  {
    snap.l2Count = l2Count;
    snap.level2IsEmpty = level2IsEmpty;
    snap.shouldAppend = shouldAppend;
    if (cmv40Mode == DOVICMv40Mode::CMV40_AUTO2)
    {
      snap.l1MaxNits = max_pq_to_nits(l1MaxPq);
      snap.auto2ThresholdPct = auto2ThresholdPct;
      snap.auto2ThresholdNits = maxLumNits * (100 + auto2ThresholdPct) / 100;
      snap.autoTrigger = auto2Trigger;
    }
    else
    {
      snap.srcMaxPq = srcMaxPq;
      snap.srcMaxNits = srcMaxNits;
      snap.isDisplayBrighter = isDisplayBrighter;
      snap.autoThreshold = static_cast<int>(autoThreshold);
      snap.autoTrigger = autoTrigger;
    }
  }

  if (scenario >= 4)
    snap.appendResult = addResult;

  if (logState.last.has_value() && (*logState.last == snap)) return;

  static constexpr const char* kScenarioNames[] = {
      "?", "missingData", "skipAlready", "skipMode", "append", "failed", "skipLibAlready"};
  static constexpr const char* kCmv40ModeNames[] = {
      "NONE", "NO_L2", "ALWAYS", "AUTO", "AUTO2"};

  auto fmt_opt = [](const auto& opt) -> std::string {
    return opt.has_value() ? fmt::format("{}", *opt) : std::string("undefined");
  };

  LOG_THROTTLE_ONCHANGE(LOGDEBUG, LOGVIDEO, snap.scenario, 1000,
                "DoVi CMv4.0 state: scenario [{}] header [{}] vdrDmData [{}] dvType [{}] "
                "vs10Converting [{:d}] "
                "cmv40Mode [{}] maxLumNits [{}] hasL254 [{}] l2Count [{}] "
                "level2IsEmpty [{}] srcMaxPq [{}] srcMaxNits [{}] "
                "isDisplayBrighter [{}] autoThreshold [{}] autoTrigger [{}] shouldAppend [{}] "
                "l1MaxNits [{}] auto2ThresholdPct [{}] auto2ThresholdNits [{}] appendResult [{}]",
                kScenarioNames[snap.scenario], snap.headerPresent, snap.vdrDmDataPresent,
                snap.dvType, snap.vs10Converting, kCmv40ModeNames[snap.cmv40Mode], snap.maxLumNits,
                fmt_opt(snap.hasL254), fmt_opt(snap.l2Count), fmt_opt(snap.level2IsEmpty),
                fmt_opt(snap.srcMaxPq), fmt_opt(snap.srcMaxNits),
                fmt_opt(snap.isDisplayBrighter), fmt_opt(snap.autoThreshold),
                fmt_opt(snap.autoTrigger), fmt_opt(snap.shouldAppend),
                fmt_opt(snap.l1MaxNits), fmt_opt(snap.auto2ThresholdPct),
                fmt_opt(snap.auto2ThresholdNits), fmt_opt(snap.appendResult));

  logState.last = snap;
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
  DoviRpuOpaque* appendOpaque = nullptr;
  std::vector<uint8_t>& nalu = m_doviEmitNalu;
  nalu.clear();

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
    DoviRpuOpaque* opaque = dovi_parse_unspec62_nalu(nalBuf, nalSize);
    if (opaque)
      m_cached_dovi_rpu_in_nal.assign(nalBuf, nalBuf + nalSize);
    else
      m_cached_dovi_rpu_in_nal.clear();

    const bool headerNeeded = (m_convert_dovi != DOVIMode::MODE_NONE) ||
                              (m_append_cmv40 != DOVICMv40Mode::CMV40_NONE);
    const DoviRpuDataHeader* header =
        (opaque && headerNeeded) ? dovi_rpu_get_header(opaque) : nullptr;
    const DoviVdrDmData* vdrDmData = opaque ? dovi_rpu_get_vdr_dm_data(opaque) : nullptr;

    if (opaque && (m_convert_dovi != DOVIMode::MODE_NONE))
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

    if (m_append_cmv40 != DOVICMv40Mode::CMV40_NONE)
      AppendCMv40(m_append_cmv40,
                  header,
                  vdrDmData,
                  nalBuf,
                  nalSize,
                  nalu,
                  appendOpaque,
                  m_cmv40_dv_type,
                  m_cmv40_vs10_converting,
                  m_cmv40_max_lum_nits,
                  m_cmv40_auto_threshold,
                  m_cmv40_auto2_threshold_pct,
                  m_cmv40_auto2_threshold_pq,
                  m_cmv40_src_pq_memo,
                  m_cmv40_src_nits_memo,
                  m_cmv40LogState);
    else if (m_cmv40LogState.last_published_scenario != 0)
    {
      PublishCMv40OutputMeta(0, vdrDmData, nullptr);
      m_cmv40LogState.last_published_scenario = 0;
    }


    // Use the appendOpaque from the append CMv4.0 if available
    DoviRpuOpaque* metadataOpaque = appendOpaque ? appendOpaque : opaque;
    if (metadataOpaque)
    {
      PopulateDoviRpuInfo(metadataOpaque, vdrDmData, metadataOpaque == opaque, pts,
                          m_dataCacheCore, m_doviMetaVerMemo, m_doviSrcMetaVerMemo,
                          &m_cached_dovi_frame_metadata);
      if (m_first_frame)
        PopulateDoviFirstFrameStreamInfo(
            metadataOpaque, opaque, m_hints.dovi_el_type, m_hints.dovi, m_dataCacheCore,
            m_hints.is_dual_track);
    }

    if (header) dovi_rpu_free_header(header);
    if (vdrDmData) dovi_rpu_free_vdr_dm_data(vdrDmData);
    if (opaque) dovi_rpu_free(opaque);
    if (appendOpaque)
      dovi_rpu_free(appendOpaque);

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
                                        double pts)
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
                m_hints.dovi, pts, m_dataCacheCore, m_hints.is_dual_track, m_doviMetaVerMemo,
                m_doviSrcMetaVerMemo);

  BitstreamAllocAndCopy(poutbuf, poutbufSize, nullptr, 0, nalu.data(),
                        static_cast<uint32_t>(nalu.size()), HEVC_NAL_UNSPEC62);
}

void CBitstreamConverter::AddDoViRpuNaluFromVividWrap(const HdrVividMetadata& meta,
                                                          uint8_t** poutbuf,
                                                          uint32_t& poutbufSize,
                                                          double pts)
{
  int intPoutbufSize = static_cast<int>(poutbufSize);
  AddDoViRpuNaluFromVivid(meta, poutbuf, &intPoutbufSize, pts);
  poutbufSize = static_cast<uint32_t>(intPoutbufSize);
}

void CBitstreamConverter::AddDoViRpuNaluFromVivid(const HdrVividMetadata& meta,
                                                  uint8_t** poutbuf,
                                                  int* poutbufSize,
                                                  double pts)
{
  auto nalu = create_dovi_rpu_nalu_from_vivid(meta, m_hdrStaticMetadataInfo);

  if (nalu.empty())
    return;

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
                m_hints.dovi, pts, m_dataCacheCore, m_hints.is_dual_track, m_doviMetaVerMemo,
                m_doviSrcMetaVerMemo);

  BitstreamAllocAndCopy(poutbuf, poutbufSize, nullptr, 0, nalu.data(),
                        static_cast<uint32_t>(nalu.size()), HEVC_NAL_UNSPEC62);
}
