/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <math.h>

#include <utility>

#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include <algorithm>
#include "windowing/GraphicContext.h"
#include "DVDClock.h"
#include "windowing/WinSystem.h"
#include "DVDStreamInfo.h"
#include "AMLCodec.h"
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/pixdesc.h>
}

#include "platform/linux/SysfsPath.h"
#include "utils/AMLUtils.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "utils/LogThrottle.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "threads/Thread.h"

#define __MODULE_NAME__ "DVDVideoCodecAmlogic"

namespace
{
constexpr const char* VC1_FORCE_FRAMEINT_PATH = "/sys/module/amvdec_vc1/parameters/force_frameint";
constexpr const char* VC1_DROP_FRAME_PATH = "/sys/module/amlvideodri/parameters/drop_frame_enable";
}

CAMLVideoBufferPool::~CAMLVideoBufferPool()
{
  CLog::Log(LOGDEBUG, "CAMLVideoBufferPool::~CAMLVideoBufferPool: Deleting {:d} buffers", static_cast<unsigned int>(m_videoBuffers.size()) );
  for (auto buffer : m_videoBuffers)
    delete buffer;
}

CVideoBuffer* CAMLVideoBufferPool::Get()
{
  std::unique_lock<CCriticalSection> lock(m_criticalSection);

  if (m_freeBuffers.empty())
  {
    m_freeBuffers.push_back(m_videoBuffers.size());
    m_videoBuffers.push_back(new CAMLVideoBuffer(static_cast<int>(m_videoBuffers.size())));
  }
  int bufferIdx(m_freeBuffers.back());
  m_freeBuffers.pop_back();

  m_videoBuffers[bufferIdx]->Acquire(shared_from_this());

  return m_videoBuffers[bufferIdx];
}

void CAMLVideoBufferPool::Return(int id)
{
  std::unique_lock<CCriticalSection> lock(m_criticalSection);
  if (m_videoBuffers[id]->m_amlCodec)
  {
    m_videoBuffers[id]->m_amlCodec->ReleaseFrame(m_videoBuffers[id]->m_bufferIndex, true);
    m_videoBuffers[id]->m_amlCodec = nullptr;
  }
  m_freeBuffers.push_back(id);
}

void CAMLVideoBufferPool::ReleaseAllBuffers()
{
  std::unique_lock<CCriticalSection> lock(m_criticalSection);
  for (auto& buf : m_videoBuffers)
  {
    if (buf && buf->m_amlCodec)
    {
      buf->m_amlCodec->ReleaseFrame(buf->m_bufferIndex, true);
      buf->m_amlCodec = nullptr;
    }
  }
}

/***************************************************************************/

CDVDVideoCodecAmlogic::CDVDVideoCodecAmlogic(CProcessInfo &processInfo)
  : CDVDVideoCodec(processInfo)
  , m_pFormatName("amcodec")
  , m_opened(false)
  , m_codecControlFlags(0)
  , m_framerate(0.0)
  , m_video_rate(0)
  , m_has_keyframe(false)
{
  if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
  {
    if (const auto settings = settingsComponent->GetSettings())
    {
      settings->RegisterCallback(this, {
          CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND,
          CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_AUTO_TRIGGER,
          CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_AUTO2_THRESHOLD,
          CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAX_LUM,
          CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE,
          CSettings::SETTING_COREELEC_AMLOGIC_DV_VIDEO_PROCESSOR,
          CSettings::SETTING_COREELEC_AMLOGIC_DV_STD_SOURCE_LEVEL_5,
          CSettings::SETTING_COREELEC_AMLOGIC_DV_STD_SOURCE_LEVEL_5_OSDST,
          CSettings::SETTING_SUBTITLES_DOLBYVISION_L5_SIGNAL_MODE,
      });
      m_settingsCallbackRegistered = true;
    }
  }

  UpdateAppendCMv40SettingCache();
}

CDVDVideoCodecAmlogic::~CDVDVideoCodecAmlogic()
{
  if (m_vc1ForceFrameIntEnabled)
    CSysfsPath(VC1_FORCE_FRAMEINT_PATH, 0);
  if (m_vc1DropFrameEnabled)
    CSysfsPath(VC1_DROP_FRAME_PATH, 0);

  if (m_settingsCallbackRegistered)
  {
    if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
    {
      if (const auto settings = settingsComponent->GetSettings())
        settings->UnregisterCallback(this);
    }
  }

  Close();
}

void CDVDVideoCodecAmlogic::UpdateAppendCMv40SettingCache()
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto settings = settingsComponent ? settingsComponent->GetSettings() : nullptr;
  if (!settings) return;

  m_appendCMv40ModeSetting.store(
      settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND));
  m_appendCMv40AutoThresholdSetting.store(
      settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_AUTO_TRIGGER));
  m_appendCMv40Auto2ThresholdSetting.store(
      settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_AUTO2_THRESHOLD));
  m_cmv40DisplayMaxLumSetting.store(
      settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAX_LUM));
  m_cmv40DvTypeSetting.store(
      settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE));
  m_cmv40VideoProcessorSetting.store(
      settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VIDEO_PROCESSOR));
  m_dvSettingsGeneration.fetch_add(1, std::memory_order_release);
}

void CDVDVideoCodecAmlogic::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (!setting) return;

  const std::string& id = setting->GetId();
  if (id == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND ||
      id == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_AUTO_TRIGGER ||
      id == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_AUTO2_THRESHOLD ||
      id == CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAX_LUM ||
      id == CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE ||
      id == CSettings::SETTING_COREELEC_AMLOGIC_DV_VIDEO_PROCESSOR)
    UpdateAppendCMv40SettingCache();
  else if (id == CSettings::SETTING_COREELEC_AMLOGIC_DV_STD_SOURCE_LEVEL_5 ||
           id == CSettings::SETTING_COREELEC_AMLOGIC_DV_STD_SOURCE_LEVEL_5_OSDST ||
           id == CSettings::SETTING_SUBTITLES_DOLBYVISION_L5_SIGNAL_MODE)
    aml_dv_push_l5_flags();
}

DOVICMv40Mode CDVDVideoCodecAmlogic::EffectiveCMv40Mode(DOVICMv40Mode mode)
{
  if (mode == DOVICMv40Mode::CMV40_NONE)
    return mode;

  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto settings = settingsComponent ? settingsComponent->GetSettings() : nullptr;
  if (settings &&
      settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VIDEO_PROCESSOR) != 0)
  {
    if (!m_cmv40VideoProcessorLogged)
    {
      logM(LOGINFO, "DV HEVC bitstream - CMv4.0 append suppressed: the video processor is "
                    "engaged, so the trims never reach the Dolby Vision engine");
      m_cmv40VideoProcessorLogged = true;
    }
    return DOVICMv40Mode::CMV40_NONE;
  }

  if (mode != DOVICMv40Mode::CMV40_AUTO2 ||
      !aml_dv_player_led_output_active(m_hints.hdrType, m_hints.bitdepth))
    return mode;

  if (!m_cmv40Auto2PinnedLogged)
  {
    logM(LOGINFO, "DV HEVC bitstream - CMv4.0 Auto2 pinned to Always on player-led output: "
                  "a per-frame CMv2.9/CMv4.0 change re-latches Dolby Vision at the sink");
    m_cmv40Auto2PinnedLogged = true;
  }
  return DOVICMv40Mode::CMV40_ALWAYS;
}

void CDVDVideoCodecAmlogic::ApplyDynamicDoViSettings()
{
  if (!m_bitstream) return;

  const bool vs10Converting = aml_dv_vs10_converting();
  if (vs10Converting != m_cmv40Vs10ConvertingApplied)
  {
    m_bitstream->SetCMv40Vs10Converting(vs10Converting);
    m_cmv40Vs10ConvertingApplied = vs10Converting;
    m_bitstream->SetAppendCMv40(
        EffectiveCMv40Mode(static_cast<DOVICMv40Mode>(m_appendCMv40ModeSetting.load())));
  }

  const unsigned int gen = m_dvSettingsGeneration.load(std::memory_order_acquire);
  if (gen == m_dvSettingsGenApplied) return;
  m_dvSettingsGenApplied = gen;

  const auto mode = static_cast<DOVICMv40Mode>(m_appendCMv40ModeSetting.load());
  const auto threshold = static_cast<DOVICMv40AutoThreshold>(m_appendCMv40AutoThresholdSetting.load());
  const int auto2Threshold = m_appendCMv40Auto2ThresholdSetting.load();
  const int displayMaxLum = m_cmv40DisplayMaxLumSetting.load();
  const int dvType = m_cmv40DvTypeSetting.load();
  const int videoProcessor = m_cmv40VideoProcessorSetting.load();
  if (mode == m_appendCMv40ModeApplied && threshold == m_appendCMv40AutoThresholdApplied &&
      auto2Threshold == m_appendCMv40Auto2ThresholdApplied &&
      displayMaxLum == m_cmv40DisplayMaxLumApplied && dvType == m_cmv40DvTypeApplied &&
      videoProcessor == m_cmv40VideoProcessorApplied)
    return;

  m_bitstream->SetCMv40DisplayParams(dvType, displayMaxLum);
  m_bitstream->SetAppendCMv40(EffectiveCMv40Mode(mode));
  m_bitstream->SetCMv40AutoThreshold(threshold);
  m_bitstream->SetCMv40Auto2ThresholdPct(auto2Threshold);
  m_appendCMv40ModeApplied = mode;
  m_appendCMv40AutoThresholdApplied = threshold;
  m_appendCMv40Auto2ThresholdApplied = auto2Threshold;
  m_cmv40DisplayMaxLumApplied = displayMaxLum;
  m_cmv40DvTypeApplied = dvType;
  m_cmv40VideoProcessorApplied = videoProcessor;

  logM(LOGINFO, "DV HEVC bitstream - CMv4.0 append mode [{:d}] auto trigger [{:d}] auto2 headroom [{:d}%] display [{:d}nits] dvType [{:d}]",
       static_cast<int>(mode), static_cast<int>(threshold), auto2Threshold, displayMaxLum, dvType);
}

std::unique_ptr<CDVDVideoCodec> CDVDVideoCodecAmlogic::Create(CProcessInfo& processInfo)
{
  return std::make_unique<CDVDVideoCodecAmlogic>(processInfo);
}

bool CDVDVideoCodecAmlogic::Register()
{
  CDVDFactoryCodec::RegisterHWVideoCodec("amlogic_dec", CDVDVideoCodecAmlogic::Create);
  return true;
}

bool CDVDVideoCodecAmlogic::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  const bool isVc1 = (hints.codec == AV_CODEC_ID_VC1 || hints.codec == AV_CODEC_ID_WMV3);
  if (!isVc1 &&
      !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEAMCODEC))
    return false;
  if ((hints.stills && hints.fpsrate == 0) || hints.width == 0)
    return false;

  // close open decoder if necessary
  if (m_opened)
    Close();

  m_hints = hints;
  m_hints.pClock = hints.pClock;

  m_nalLengthSize = 0;
  m_stripHdr10Plus = false;
  m_streamMeta = {};
  m_metadataSequencer.Reset();

  CLog::Log(LOGDEBUG, "{}::{} - codec {:d} profile:{:d} extra_size:{:d} fps:{:d}/{:d}",
    __MODULE_NAME__, __FUNCTION__, m_hints.codec, m_hints.profile, m_hints.extradata.GetSize(), m_hints.fpsrate, m_hints.fpsscale);

  switch(m_hints.codec)
  {
    case AV_CODEC_ID_MJPEG:
      m_pFormatName = "am-mjpeg";
      break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
      aml_set_mpeg2_keep_progressive(
          m_hints.width > CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECMPEG2) &&
          CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_AMLMPEG2KEEPPROG));

      if (m_hints.width <= CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECMPEG2))
        goto FAIL;

      switch(m_hints.profile)
      {
        case AV_PROFILE_MPEG2_422:
          CLog::Log(LOGDEBUG, "{}: MPEG2 unsupported hints.profile({:d})", __MODULE_NAME__, m_hints.profile);
          goto FAIL;
      }

      // if we have SD PAL content assume it is widescreen
      // correct aspect ratio will be detected later anyway
      if ((m_hints.width == 720 || m_hints.width == 544 || m_hints.width == 480) && m_hints.height == 576 && m_hints.aspect == 0.0)
          m_hints.aspect = 16.0 / 9.0;

      m_mpeg2_sequence_pts = 0;
      m_mpeg2_sequence = std::make_unique<mpeg2_sequence>();
      m_mpeg2_sequence->width  = m_hints.width;
      m_mpeg2_sequence->height = m_hints.height;
      m_mpeg2_sequence->ratio  = m_hints.aspect;
      m_mpeg2_sequence->fps_rate  = m_hints.fpsrate;
      m_mpeg2_sequence->fps_scale  = m_hints.fpsscale;
      m_pFormatName = "am-mpeg2";
      break;
    case AV_CODEC_ID_H264:
      if (m_hints.width <= CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECH264))
      {
        CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::h264 size check failed {:d}",CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECH264));
        goto FAIL;
      }
      switch(hints.profile)
      {
        case AV_PROFILE_H264_HIGH_10:
        case AV_PROFILE_H264_HIGH_10_INTRA:
        case AV_PROFILE_H264_HIGH_422:
        case AV_PROFILE_H264_HIGH_422_INTRA:
        case AV_PROFILE_H264_HIGH_444_PREDICTIVE:
        case AV_PROFILE_H264_HIGH_444_INTRA:
        case AV_PROFILE_H264_CAVLC_444:
          CLog::Log(LOGDEBUG, "{}: H264 unsupported hints.profile({:d})", __MODULE_NAME__, m_hints.profile);
          goto FAIL;
      }
      if ((aml_support_h264_4k2k() == AML_NO_H264_4K2K) && ((m_hints.width > 1920) || (m_hints.height > 1088)))
      {
        CLog::Log(LOGDEBUG, "{}::{} - 4K H264 is supported only on Amlogic S802 and S812 chips or newer", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }

      if (m_hints.aspect == 0.0)
      {
        m_h264_sequence_pts = 0;
        m_h264_sequence = std::make_unique<h264_sequence>();
        m_h264_sequence->width  = m_hints.width;
        m_h264_sequence->height = m_hints.height;
        m_h264_sequence->ratio  = m_hints.aspect;
      }

      if (m_hints.codec_tag == MKTAG('M', 'V', 'C', '1'))
        m_pFormatName = "am-h264mvc";
      else
        m_pFormatName = "am-h264";
      // convert h264-avcC to h264-annex-b as h264-avcC
      // under streamers can have issues when seeking.
      if (m_hints.extradata && m_hints.extradata.GetData()[0] == 1)
      {
        m_bitstream = std::make_unique<CBitstreamConverter>(m_hints);
        m_bitstream->Open(true);
        m_bitstream->ResetStartDecode();
        // make sure we do not leak the existing m_hints.extradata
        m_hints.extradata = {};
        m_hints.extradata = FFmpegExtraData(m_bitstream->GetExtraSize());
        memcpy(m_hints.extradata.GetData(), m_bitstream->GetExtraData(), m_hints.extradata.GetSize());
      }
      else
      {
        m_bitparser = std::make_unique<CBitstreamParser>();
        m_bitparser->Open();
      }

      // if we have SD PAL content assume it is widescreen
      // correct aspect ratio will be detected later anyway
      if (m_hints.width == 720 && m_hints.height == 576 && m_hints.aspect == 0.0)
          m_hints.aspect = 16.0 / 9.0;

      // assume widescreen for "HD Lite" channels
      // correct aspect ratio will be detected later anyway
      if ((m_hints.width == 1440 || m_hints.width ==1280) && m_hints.height == 1080 && m_hints.aspect == 0.0)
          m_hints.aspect = 16.0 / 9.0;;

      break;
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_MSMPEG4V2:
    case AV_CODEC_ID_MSMPEG4V3:
      if (m_hints.width <= CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECMPEG4))
        goto FAIL;
      m_pFormatName = "am-mpeg4";
      break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
      // amcodec can't handle h263
      CLog::Log(LOGDEBUG, "{}::{} - amcodec does not support H263", __MODULE_NAME__, __FUNCTION__);
      goto FAIL;
//    case AV_CODEC_ID_FLV1:
//      m_pFormatName = "am-flv1";
//      break;
    case AV_CODEC_ID_RV10:
    case AV_CODEC_ID_RV20:
    case AV_CODEC_ID_RV30:
    case AV_CODEC_ID_RV40:
      // m_pFormatName = "am-rv";
      // rmvb is not handled well by amcodec
      CLog::Log(LOGDEBUG, "{}::{} - amcodec does not support RMVB", __MODULE_NAME__, __FUNCTION__);
      goto FAIL;
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
      if (m_hints.width <= CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
        CSettings::SETTING_VIDEOPLAYER_USEAMCODECVC1))
      {
        if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
          CSettings::SETTING_VIDEOPLAYER_USEAMCODECVC1) != 9998)
        {
          CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::vc1 {:d} disabled by user",
            CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
            CSettings::SETTING_VIDEOPLAYER_USEAMCODECVC1));
          goto FAIL;
        }
        else if (m_hints.fpsrate <= 24000)
        {
          CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::vc1 {:d} disabled by user",
            CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
            CSettings::SETTING_VIDEOPLAYER_USEAMCODECVC1));
          goto FAIL;
        }
      }

      switch(m_hints.codec)
      {
        case AV_CODEC_ID_VC1:
          m_pFormatName = "am-vc1";
          break;
        case AV_CODEC_ID_WMV3:
          m_pFormatName = "am-wmv3";
          break;
        default:
          goto FAIL;
      }
      m_vc1Parser = std::make_unique<CVC1BitstreamParser>();
      {
        const auto settingsComponent = CServiceBroker::GetSettingsComponent();
        const auto advancedSettings =
            settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;
        m_vc1ForceFrameIntEnabled = advancedSettings && advancedSettings->m_vc1ForceFrameInt;
        m_vc1DropFrameEnabled = advancedSettings && advancedSettings->m_vc1DropFrame;
      }
      m_vc1ForceFrameIntLast = -1;
      m_vc1ForceFrameIntLocked = false;
      if (m_vc1ForceFrameIntEnabled)
        CSysfsPath(VC1_FORCE_FRAMEINT_PATH, 0);
      if (m_vc1DropFrameEnabled)
        CSysfsPath(VC1_DROP_FRAME_PATH, 1);
      break;
    case AV_CODEC_ID_AVS:
    case AV_CODEC_ID_CAVS:
      m_pFormatName = "am-avs";
      break;
    case AV_CODEC_ID_VP9:
      if (!aml_support_vp9())
      {
        CLog::Log(LOGDEBUG, "{}::{} - VP9 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-vp9";
      break;
    case AV_CODEC_ID_AV1:
      if (!aml_support_av1())
      {
        CLog::Log(LOGDEBUG, "{}::{} - AV1 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-av1";
      break;
    case AV_CODEC_ID_HEVC:
      if (aml_support_hevc()) {
        if (!aml_support_hevc_8k4k() && ((m_hints.width > 4096) || (m_hints.height > 2176)))
        {
          CLog::Log(LOGDEBUG, "{}::{} - 8K HEVC hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
          goto FAIL;
        } else if (!aml_support_hevc_4k2k() && ((m_hints.width > 1920) || (m_hints.height > 1088)))
        {
          CLog::Log(LOGDEBUG, "{}::{} - 4K HEVC hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
          goto FAIL;
        }
      } else {
        CLog::Log(LOGDEBUG, "{}::{} - HEVC hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      if ((hints.profile == AV_PROFILE_HEVC_MAIN_10) && !aml_support_hevc_10bit())
      {
        CLog::Log(LOGDEBUG, "{}::{} - HEVC 10-bit hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      if (m_hints.bitdepth > 10)
      {
        logM(LOGDEBUG,
             "CDVDVideoCodecAmlogic::Open - HEVC {}-bit exceeds Amlogic AVE-10 HW Main 10 limit "
             "(vh265.c returns DECODER_FATAL_ERROR_SIZE_OVERFLOW on bit_depth_luma/chroma > 10); "
             "falling back to software",
             m_hints.bitdepth);
        goto FAIL;
      }
      {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(
            static_cast<AVPixelFormat>(m_hints.pixelFormat));
        if (desc && desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0)
        {
          logM(LOGDEBUG,
               "CDVDVideoCodecAmlogic::Open - HEVC 4:4:4 chroma ({}) not supported by Amlogic HW "
               "HEVC decoder (Main Profile spec is 4:2:0 only); falling back to software",
               desc->name);
          goto FAIL;
        }
        if (desc && desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0)
        {
          logM(LOGDEBUG,
               "CDVDVideoCodecAmlogic::Open - HEVC 4:2:2 chroma ({}) not supported by Amlogic HW "
               "HEVC decoder (Main Profile spec is 4:2:0 only); falling back to software",
               desc->name);
          goto FAIL;
        }
      }
      m_pFormatName = "am-h265";
      if (m_hints.extradata.GetSize() > 21 && m_hints.extradata.GetData()[0] == 1)
        m_nalLengthSize = (m_hints.extradata.GetData()[21] & 0x3) + 1;

      m_bitstream = std::make_unique<CBitstreamConverter>(m_hints);
      m_bitstream->Open(true);

      // check for hevc-hvcC and convert to h265-annex-b - and DV is on.
      if (m_hints.extradata && !m_hints.cryptoSession && m_bitstream && (aml_dv_mode() != DV_MODE_OFF))
      {
        auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

        const int dualPriority = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_DUAL_PRIORITY);
        m_bitstream->SetDualPriority(dualPriority);

        if (m_hints.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
        {
          const int cmv40DvType = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE);
          const int cmv40MaxLum = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAX_LUM);
          m_bitstream->SetCMv40DisplayParams(cmv40DvType, cmv40MaxLum);
          const bool cmv40Vs10Converting = aml_dv_refresh_vs10_converting(m_hints.hdrType, m_hints.bitdepth);
          m_bitstream->SetCMv40Vs10Converting(cmv40Vs10Converting);
          m_cmv40Vs10ConvertingApplied = cmv40Vs10Converting;
          auto cmv40Mode = static_cast<DOVICMv40Mode>(m_appendCMv40ModeSetting.load());
          auto cmv40Threshold = static_cast<DOVICMv40AutoThreshold>(m_appendCMv40AutoThresholdSetting.load());
          const int cmv40Auto2Threshold = m_appendCMv40Auto2ThresholdSetting.load();
          if (cmv40Mode != DOVICMv40Mode::CMV40_NONE)
          {
            logM(LOGINFO, "DV HEVC bitstream - CMv4.0 append mode [{:d}] auto trigger [{:d}] auto2 headroom [{:d}%] display [{:d}nits]",
                 static_cast<int>(cmv40Mode), static_cast<int>(cmv40Threshold), cmv40Auto2Threshold, cmv40MaxLum);
            m_bitstream->SetAppendCMv40(EffectiveCMv40Mode(cmv40Mode));
            m_bitstream->SetCMv40AutoThreshold(cmv40Threshold);
            m_bitstream->SetCMv40Auto2ThresholdPct(cmv40Auto2Threshold);
          }
          m_appendCMv40ModeApplied = cmv40Mode;
          m_appendCMv40AutoThresholdApplied = cmv40Threshold;
          m_appendCMv40Auto2ThresholdApplied = cmv40Auto2Threshold;
          m_cmv40DisplayMaxLumApplied = cmv40MaxLum;
          m_cmv40DvTypeApplied = cmv40DvType;
          m_cmv40VideoProcessorApplied =
              settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VIDEO_PROCESSOR);

          if (m_hints.dovi.dv_profile == 7)
          {
            DOVIMode convertDovi = static_cast<DOVIMode>(settings->GetInt(CSettings::SETTING_VIDEOPLAYER_CONVERTDOVI));
            if (convertDovi)
            {
              CLog::Log(LOGINFO, "{}::{} - DV HEVC bitstream - user chooses to convert to mode [{:d}]",
                        __MODULE_NAME__, __FUNCTION__, convertDovi);
              m_bitstream->SetConvertDovi(convertDovi);
              m_streamMeta.flags.push_back("converted");
            }
          }
        }

        // F9-aligned Priority Dispatch: apply for all HEVC streams with dynamic HDR metadata
        if (dualPriority == 1)
        {
          CLog::Log(LOGINFO, "{}::{} - HEVC bitstream - if stream also contains HDR10+, native HDR10+ has priority.",
                    __MODULE_NAME__, __FUNCTION__);
                  }
        else if (dualPriority == 2)
        {
          CLog::Log(LOGINFO, "{}::{} - HEVC bitstream - if stream also contains HDR Vivid, native HDR Vivid has priority.",
                    __MODULE_NAME__, __FUNCTION__);
        }
        else
        {
          if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDR10PLUS_CONVERT)) 
          {
            bool preferConvertHdr10Plus = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDR10PLUS_PREFER_CONVERT);
            m_bitstream->SetPreferCovertHdr10Plus(preferConvertHdr10Plus);

            if (preferConvertHdr10Plus) 
              CLog::Log(LOGINFO, "{}::{} - HEVC bitstream - if stream also contains HDR10+, conversion will be preferred over original Dolby Vision.",
                        __MODULE_NAME__, __FUNCTION__);
          }
          if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDRVIVID_CONVERT))
          {
            bool preferConvertVivid = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDRVIVID_PREFER_CONVERT);
            m_bitstream->SetPreferConvertHdrVivid(preferConvertVivid);

            if (preferConvertVivid)
              CLog::Log(LOGINFO, "{}::{} - HEVC bitstream - if stream also contains HDR Vivid, conversion will be preferred over original Dolby Vision.",
                        __MODULE_NAME__, __FUNCTION__);
          }
        }

        // Potential HDR10+ (Cannot tell at this point)
        if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDR10PLUS_CONVERT))
        {
          PeakBrightnessSource peakBrightnessSource = static_cast<PeakBrightnessSource>(settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDR10PLUS_PEAK_BRIGHTNESS_SOURCE));
          CLog::Log(LOGDEBUG, "{}::{} - HDR10 HEVC bitstream - if HDR10+ then will be converted to Dolby Vision P8.1 with brightness source [{:d}]",
            __MODULE_NAME__, __FUNCTION__, peakBrightnessSource);
          m_bitstream->SetConvertHdr10Plus(true);
          m_bitstream->SetConvertHdr10PlusPeakBrightnessSource(peakBrightnessSource);
        }

        // Potential HDR Vivid (convert to Dolby Vision P8.1 if enabled)
        if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDRVIVID_CONVERT))
        {
          CLog::Log(LOGDEBUG, "{}::{} - HDR10 HEVC bitstream - if HDR Vivid then will be converted to Dolby Vision P8.1",
            __MODULE_NAME__, __FUNCTION__);
          m_bitstream->SetConvertHdrVivid(true);
        }

        // If HDR10 or Dual Priority HDR10+ and doing VS10 - remove the HDR10+ and DV if present to avoid conflict with VS10.
        if (((m_hints.hdrType == StreamHdrType::HDR_TYPE_HDR10 && dualPriority != 0) || dualPriority == 1))
        {
          unsigned int mode(aml_vs10_by_setting(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10PLUS));
          if (mode < DOLBY_VISION_OUTPUT_MODE_BYPASS)
          {
            // for VS10 conversion need to remove the HDR10plus metadata.
            CLog::Log(LOGINFO, "{}::{} - HDR10 HEVC bitstream - if HDR10+ then metadata will be removed to allow correct VS10 processing",
              __MODULE_NAME__, __FUNCTION__);
            m_bitstream->SetRemoveHdr10Plus(true);
            m_bitstream->SetRemoveDovi(true);
            m_stripHdr10Plus = true;
            if (m_hints.dovi.dv_profile > 0)
              m_streamMeta.flags.push_back("rpu-removed");
          }
        }
      }

      // make sure we do not leak the existing m_hints.extradata
      m_hints.extradata = {};
      m_hints.extradata = FFmpegExtraData(m_bitstream->GetExtraSize());
      memcpy(m_hints.extradata.GetData(), m_bitstream->GetExtraData(), m_hints.extradata.GetSize());
      break;
    default:
      CLog::Log(LOGDEBUG, "{}: Unknown hints.codec({:d})", __MODULE_NAME__, m_hints.codec);
      goto FAIL;
  }

  m_aspect_ratio = m_hints.aspect;

  m_Codec = std::make_shared<CAMLCodec>(m_processInfo, m_hints);
  if (!m_Codec)
  {
    CLog::Log(LOGERROR, "{}: Failed to create Amlogic Codec", __MODULE_NAME__);
    goto FAIL;
  }

  // allocate a dummy VideoPicture buffer.
  m_videobuffer.Reset();

  m_videobuffer.iWidth  = m_hints.width;
  m_videobuffer.iHeight = m_hints.height;

  m_videobuffer.iDisplayWidth  = m_videobuffer.iWidth;
  m_videobuffer.iDisplayHeight = m_videobuffer.iHeight;
  if (m_hints.aspect > 0.0 && !m_hints.forced_aspect)
  {
    m_videobuffer.iDisplayWidth  = ((int)lrint(m_videobuffer.iHeight * m_hints.aspect)) & ~3;
    if (m_videobuffer.iDisplayWidth > m_videobuffer.iWidth)
    {
      m_videobuffer.iDisplayWidth  = m_videobuffer.iWidth;
      m_videobuffer.iDisplayHeight = ((int)lrint(m_videobuffer.iWidth / m_hints.aspect)) & ~3;
    }
  }

  m_videobuffer.hdrType = m_hints.hdrType;
  m_videobuffer.color_space = m_hints.colorSpace;
  m_videobuffer.color_primaries = m_hints.colorPrimaries;
  m_videobuffer.color_transfer = m_hints.colorTransferCharacteristic;

  m_processInfo.SetVideoDecoderName(m_pFormatName, true);
  m_processInfo.SetVideoDimensions(m_hints.width, m_hints.height);
  m_processInfo.SetVideoDeintMethod("hardware");
  m_processInfo.SetVideoDAR(m_hints.aspect);

  m_has_keyframe = false;

  if (m_hints.contentLightMetadata)
    m_streamMeta.hdrCll = AMLSerializeContentLight(*m_hints.contentLightMetadata);
  if (m_hints.masteringMetadata &&
      (m_hints.masteringMetadata->has_primaries || m_hints.masteringMetadata->has_luminance))
    m_streamMeta.hdrMdcv = AMLSerializeMastering(*m_hints.masteringMetadata);
  if (hints.dovi.dv_profile > 0)
    m_streamMeta.doviConfig = AMLSerializeDoviConfig(hints.dovi);
  m_dualLayer = hints.dovi.el_present_flag;

  m_pendingMeta = m_streamMeta;
  m_lastMeta = m_streamMeta;
  m_metadataToken = CAMLFrameMetadataStore::GetInstance().Register();
  CAMLFrameMetadataStore::GetInstance().Publish(m_metadataToken, m_streamMeta);

  logM(LOGDEBUG, "Opened Amlogic Codec");
  return true;
FAIL:
  Close();
  return false;
}

void CDVDVideoCodecAmlogic::Close(void)
{
  if (m_metadataToken)
  {
    CAMLFrameMetadataStore::GetInstance().Unregister(m_metadataToken);
    m_metadataToken = 0;
  }

  CLog::Log(LOGDEBUG, "{}::{}", __MODULE_NAME__, __FUNCTION__);

  if (m_videoBufferPool)
  {
    auto pool = m_videoBufferPool->GetPtr();
    if (pool)
      static_cast<CAMLVideoBufferPool*>(pool.get())->ReleaseAllBuffers();
  }
  m_videoBufferPool = nullptr;

  if (m_Codec)
    m_Codec->CloseDecoder(false), m_Codec = nullptr;

  m_videobuffer.iFlags = 0;

  m_opened = false;

  while (!m_packages.empty())
  {
    RecycleDualLayerPacket(std::move(m_packages.front()));
    m_packages.pop_front();
  }
  m_last_added = true;
  m_last_pData = nullptr;
  m_last_iSize = 0;
  m_mpeg2_sequence_pts = 0;
  m_has_keyframe = false;

  if (m_bitstream && m_hints.codec == AV_CODEC_ID_H264)
    m_bitstream->ResetStartDecode();
}

DLDemuxPacket CDVDVideoCodecAmlogic::AcquireDualLayerPacket(std::size_t requiredCapacity)
{
  DLDemuxPacket packet;
  if (!m_freePackages.empty())
  {
    packet = std::move(m_freePackages.back());
    m_freePackages.pop_back();
  }

  if (packet.buffer.GetSize() < requiredCapacity)
    packet.buffer = FFmpegExtraData(requiredCapacity);

  return packet;
}

void CDVDVideoCodecAmlogic::RecycleDualLayerPacket(DLDemuxPacket&& packet)
{
  packet.size = 0;
  packet.isELPackage = false;
  packet.dts = 0.0;

  if (m_freePackages.size() < MAX_CACHED_DUAL_LAYER_PACKETS)
    m_freePackages.emplace_back(std::move(packet));
}

bool CDVDVideoCodecAmlogic::DualLayerConvert(uint8_t *pData, uint32_t iSize, const DemuxPacket &packet)
{
  bool dual_layer_converted = false;

  if (packet.isELPackage)
    ++m_dlStatEL;
  else
    ++m_dlStatBL;

  const bool isFel = (m_hints.dovi_el_type == DOVIELType::TYPE_FEL);

  if (!isFel)
  {
    // =========================================================================
    // MEL / Initial unconfirmed phase: FIFO in-order pairing (aligns with pannal-xbmc).
    // =========================================================================
    if (!m_packages.empty())
    {
      auto& frontPacket = m_packages.front();
      if (frontPacket.isELPackage != packet.isELPackage)
      {
        if (packet.isELPackage)
          dual_layer_converted = m_bitstream->Convert(frontPacket.buffer.GetData(), frontPacket.size, pData, iSize, packet.pts);
        else
          dual_layer_converted = m_bitstream->Convert(pData, iSize, frontPacket.buffer.GetData(), frontPacket.size, packet.pts);

        if (dual_layer_converted)
        {
          if (m_hints.hdrType != StreamHdrType::HDR_TYPE_NONE &&
              m_hints.codec == AV_CODEC_ID_HEVC)
          {
            m_pendingMeta = m_streamMeta;
            if (packet.isELPackage)
            {
              AMLLatchHevcDoviRpu(pData, iSize, m_nalLengthSize, m_pendingMeta);
              AMLLatchHevcSei(frontPacket.buffer.GetData(), frontPacket.size, m_nalLengthSize,
                              m_pendingMeta);
            }
            else
            {
              AMLLatchHevcDoviRpu(frontPacket.buffer.GetData(), frontPacket.size, m_nalLengthSize,
                                  m_pendingMeta);
              AMLLatchHevcSei(pData, iSize, m_nalLengthSize, m_pendingMeta);
            }
            if (!m_pendingMeta.hdrMdcv.empty())
              m_streamMeta.hdrMdcv = m_pendingMeta.hdrMdcv;
            if (!m_pendingMeta.hdrCll.empty())
              m_streamMeta.hdrCll = m_pendingMeta.hdrCll;
          }

          ++m_dlStatPaired;
          RecycleDualLayerPacket(std::move(frontPacket));
          m_packages.pop_front();
        }
      }
    }
  }
  else
  {
    // =========================================================================
    // Confirmed FEL phase: Strict PTS nearest-neighbor matching with dynamic tolerance.
    // =========================================================================
    const double fps = (m_hints.fpsrate > 0 && m_hints.fpsscale > 0)
      ? (static_cast<double>(m_hints.fpsrate) / static_cast<double>(m_hints.fpsscale))
      : 24.0;
    const double frame_period = static_cast<double>(DVD_TIME_BASE) / (fps > 0.0 ? fps : 24.0);
    const double match_tolerance = frame_period * 0.8;

    auto matchIt = m_packages.end();
    double best_delta = -1.0;
    bool positional_match = false;
    for (auto it = m_packages.begin(); it != m_packages.end(); ++it)
    {
      if (it->isELPackage == packet.isELPackage)
        continue;
      if (packet.pts == DVD_NOPTS_VALUE || it->pts == DVD_NOPTS_VALUE)
        continue;
      const double raw_delta = packet.pts - it->pts;
      const double delta = raw_delta < 0.0 ? -raw_delta : raw_delta;
      if (best_delta < 0.0 || delta < best_delta)
      {
        best_delta = delta;
        matchIt = it;
      }
    }

    if (matchIt == m_packages.end() && packet.pts == DVD_NOPTS_VALUE)
    {
      for (auto rit = m_packages.rbegin(); rit != m_packages.rend(); ++rit)
      {
        if (rit->isELPackage != packet.isELPackage)
        {
          matchIt = std::prev(rit.base());
          positional_match = true;
          break;
        }
      }
    }

    const bool have_match =
        (matchIt != m_packages.end()) && (positional_match || best_delta <= match_tolerance);

    if (positional_match)
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000,
                            "dlpair: positional pairing for pts-less packet (isEL={})",
                            packet.isELPackage);

    if (have_match)
    {
      DLDemuxPacket& dualLayerPacket = *matchIt;

      if (packet.isELPackage)
        dual_layer_converted = m_bitstream->Convert(dualLayerPacket.buffer.GetData(), dualLayerPacket.size, pData, iSize, packet.pts);
      else
        dual_layer_converted = m_bitstream->Convert(pData, iSize, dualLayerPacket.buffer.GetData(), dualLayerPacket.size, packet.pts);

      if (dual_layer_converted && m_hints.hdrType != StreamHdrType::HDR_TYPE_NONE &&
          m_hints.codec == AV_CODEC_ID_HEVC)
      {
        m_pendingMeta = m_streamMeta;
        if (packet.isELPackage)
        {
          AMLLatchHevcDoviRpu(pData, iSize, m_nalLengthSize, m_pendingMeta);
          AMLLatchHevcSei(dualLayerPacket.buffer.GetData(), dualLayerPacket.size, m_nalLengthSize,
                          m_pendingMeta);
        }
        else
        {
          AMLLatchHevcDoviRpu(dualLayerPacket.buffer.GetData(), dualLayerPacket.size, m_nalLengthSize,
                              m_pendingMeta);
          AMLLatchHevcSei(pData, iSize, m_nalLengthSize, m_pendingMeta);
        }
        if (!m_pendingMeta.hdrMdcv.empty())
          m_streamMeta.hdrMdcv = m_pendingMeta.hdrMdcv;
        if (!m_pendingMeta.hdrCll.empty())
          m_streamMeta.hdrCll = m_pendingMeta.hdrCll;
      }

      if (dual_layer_converted)
      {
        ++m_dlStatPaired;
        RecycleDualLayerPacket(std::move(*matchIt));
        m_packages.erase(matchIt);
      }
    }
    else if (best_delta >= 0.0)
      m_dlStatMissDelta = best_delta;
  }

  if (!dual_layer_converted)
  {
    DLDemuxPacket queuedPacket = AcquireDualLayerPacket(packet.iSize + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(queuedPacket.buffer.GetData(), packet.pData, packet.iSize);
    memset(queuedPacket.buffer.GetData() + packet.iSize, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    queuedPacket.size = iSize;
    queuedPacket.isELPackage = packet.isELPackage;
    queuedPacket.dts = packet.dts;
    queuedPacket.pts = packet.pts;
    m_packages.emplace_back(std::move(queuedPacket));
  }

  // 128 packets provide ~5.3s interleaving buffer at 24fps and ~2.1s at 60fps.
  // This safely absorbs M2TS chunk interleaving and network I/O jitter for both
  // 24fps UHD Blu-ray (FEL/MEL) and 60fps high-framerate MEL streams.
  constexpr size_t DUAL_LAYER_MAX_QUEUE_DEPTH = 128;
  const size_t max_queue_depth = DUAL_LAYER_MAX_QUEUE_DEPTH;

  while (m_packages.size() > max_queue_depth)
  {
    ++m_dlStatEvicted;
    RecycleDualLayerPacket(std::move(m_packages.front()));
    m_packages.pop_front();
  }

  const int64_t now = CurrentHostCounter();
  if (now - m_dlStatLastLog >= CurrentHostFrequency())
  {
    m_dlStatLastLog = now;
    const double frame_period = static_cast<double>(DVD_TIME_BASE) / (fps > 0.0 ? fps : 24.0);
    const double match_tolerance = frame_period * 0.8;
    logComponentM(LOGDEBUG, LOGVIDEO,
                  "dlpair: mode={} bl={} el={} paired={} evicted={} depth={} missDeltaMs={:.1f} tolMs={:.1f}",
                  isFel ? "FEL-PTS" : "MEL-FIFO",
                  m_dlStatBL, m_dlStatEL, m_dlStatPaired, m_dlStatEvicted, m_packages.size(),
                  m_dlStatMissDelta / 1000.0, match_tolerance / 1000.0);
    m_dlStatBL = 0;
    m_dlStatEL = 0;
    m_dlStatPaired = 0;
    m_dlStatEvicted = 0;
  }

  if (!dual_layer_converted)
    return false;

  if (!m_bitstream->CanStartDecode())
  {
    LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000, "waiting for keyframe (bitstream)");
    return false;
  }

  return true;
}

bool CDVDVideoCodecAmlogic::SingleLayerConvert(uint8_t *pData, uint32_t iSize, const DemuxPacket &packet) const {
  if (!m_bitstream->Convert(pData, iSize, packet.pts))
    return false;

  if (!m_bitstream->CanStartDecode())
  {
    LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000, "waiting for keyframe (bitstream)");
    return false;
  }

  return true;
}

namespace
{
bool PacketHasHdr10PlusSideData(const DemuxPacket& packet)
{
  if (!packet.pSideData || packet.iSideDataElems <= 0)
    return false;

  const AVPacketSideData* raw =
      av_packet_side_data_get(static_cast<AVPacketSideData*>(packet.pSideData),
                              packet.iSideDataElems, AV_PKT_DATA_DYNAMIC_HDR10_PLUS_RAW);
  if (raw && raw->size >= 6)
    return true;

  const AVPacketSideData* sd =
      av_packet_side_data_get(static_cast<AVPacketSideData*>(packet.pSideData),
                              packet.iSideDataElems, AV_PKT_DATA_DYNAMIC_HDR10_PLUS);
  return sd && sd->size >= sizeof(AVDynamicHDRPlus);
}

bool ReadLeb128(const uint8_t* p, uint32_t avail, uint64_t& value, uint32_t& len)
{
  value = 0;
  len = 0;
  for (uint32_t i = 0; i < 8 && i < avail; i++)
  {
    const uint8_t b = p[i];
    value |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
    if (!(b & 0x80))
    {
      len = i + 1;
      return true;
    }
  }
  return false;
}

bool Av1PacketHasHdr10PlusT35(const uint8_t* data, uint32_t size)
{
  static constexpr uint8_t OBU_METADATA = 5;
  static constexpr uint64_t METADATA_TYPE_ITUT_T35 = 4;
  static constexpr uint8_t HDR10P_T35_HEAD[6] = {0xB5, 0x00, 0x3C, 0x00, 0x01, 0x04};

  uint32_t pos = 0;
  while (pos < size)
  {
    const uint8_t header = data[pos];
    if (header & 0x80)
      return false;
    const uint8_t obuType = (header >> 3) & 0x0F;
    const bool hasExtension = (header >> 2) & 0x01;
    const bool hasSize = (header >> 1) & 0x01;
    pos += 1 + (hasExtension ? 1 : 0);
    if (pos > size)
      return false;

    uint64_t payloadSize = size - pos;
    if (hasSize)
    {
      uint32_t lebLen = 0;
      if (!ReadLeb128(data + pos, size - pos, payloadSize, lebLen))
        return false;
      pos += lebLen;
      if (payloadSize > size - pos)
        return false;
    }

    if (obuType == OBU_METADATA && payloadSize > 0)
    {
      uint64_t metadataType = 0;
      uint32_t lebLen = 0;
      if (ReadLeb128(data + pos, static_cast<uint32_t>(payloadSize), metadataType, lebLen) &&
          metadataType == METADATA_TYPE_ITUT_T35 &&
          payloadSize >= lebLen + sizeof(HDR10P_T35_HEAD) &&
          memcmp(data + pos + lebLen, HDR10P_T35_HEAD, sizeof(HDR10P_T35_HEAD)) == 0)
        return true;
    }

    if (!hasSize)
      return false;
    pos += static_cast<uint32_t>(payloadSize);
  }
  return false;
}
}

bool CDVDVideoCodecAmlogic::AddData(const DemuxPacket &packet)
{
  uint8_t *pData(packet.pData);
  uint32_t iSize(packet.iSize);
  bool set_osd_max = false;

  DrainMetadataToClock();

  if (pData)
  {
    if (m_dualLayer && m_streamMeta.structure != "dt-dl")
    {
      m_streamMeta.structure = packet.isDualStream ? "dt-dl" : "st-dl";
      m_pendingMeta.structure = m_streamMeta.structure;
    }

    if (!packet.isDualStream && m_hints.hdrType != StreamHdrType::HDR_TYPE_NONE)
    {
      switch (m_hints.codec)
      {
        case AV_CODEC_ID_HEVC:
          AMLLatchHevcDoviRpu(pData, iSize, m_nalLengthSize, m_pendingMeta);
          AMLLatchHevcSei(pData, iSize, m_nalLengthSize, m_pendingMeta);
          if (!m_pendingMeta.hdrMdcv.empty())
            m_streamMeta.hdrMdcv = m_pendingMeta.hdrMdcv;
          if (!m_pendingMeta.hdrCll.empty())
            m_streamMeta.hdrCll = m_pendingMeta.hdrCll;
          break;
        case AV_CODEC_ID_AV1:
          AMLLatchAv1Metadata(pData, iSize, m_pendingMeta);
          break;
        default:
          break;
      }
    }

    ApplyDynamicDoViSettings();

    if (m_bitstream)
    {
      if (!m_last_added) // Try again
      {
        pData = m_last_pData;
        iSize = m_last_iSize;
      }
      else
      {
        if (packet.isDualStream)
        {
          if (!DualLayerConvert(pData, iSize, packet))
          {
            m_pendingMeta = m_streamMeta;
            return true;
          }
        }
        else
        {
          if (!SingleLayerConvert(pData, iSize, packet))
          {
            m_pendingMeta = m_streamMeta;
            return true;
          }
        }
        m_last_pData = pData = m_bitstream->GetConvertBuffer();
        m_last_iSize = iSize = m_bitstream->GetConvertSize();
      }
    }
    else if (!m_has_keyframe && m_bitparser)
    {
      if (!m_bitparser->CanStartDecode(pData, iSize))
      {
        LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000, "CDVDVideoCodecAmlogic::{}: waiting for keyframe (bitparser)", __FUNCTION__);
        m_pendingMeta = m_streamMeta;
        return true;
      }
      else
        m_has_keyframe = true;
    }
    FrameRateTracking( pData, iSize, packet.dts, packet.pts);

    if (!m_hdr10PlusUpgraded &&
        (m_hints.codec == AV_CODEC_ID_VP9 || m_hints.codec == AV_CODEC_ID_AV1) &&
        (m_hints.hdrType == StreamHdrType::HDR_TYPE_HDR10 ||
         m_hints.hdrType == StreamHdrType::HDR_TYPE_NONE))
    {
      if (PacketHasHdr10PlusSideData(packet) ||
          (m_hints.codec == AV_CODEC_ID_AV1 && Av1PacketHasHdr10PlusT35(pData, iSize)))
      {
        m_hdr10PlusUpgraded = true;
        m_hints.hdrType = StreamHdrType::HDR_TYPE_HDR10PLUS;
        CServiceBroker::GetDataCacheCore().SetVideoSourceHdrType(StreamHdrType::HDR_TYPE_HDR10PLUS);
        logM(LOGINFO,
             "CDVDVideoCodecAmlogic::AddData - {} HDR10+ dynamic metadata detected {} decoder "
             "open; stream reclassified to HDR10+",
             m_hints.codec == AV_CODEC_ID_VP9 ? "VP9" : "AV1", m_opened ? "after" : "before");
      }
    }

    if (!m_opened)
    {
      if (packet.pts == DVD_NOPTS_VALUE)
        m_hints.ptsinvalid = true;

      logComponentM(LOGDEBUG, LOGVIDEO, "Open decoder: fps:{:d}/{:d}", m_hints.fpsrate, m_hints.fpsscale);
      if (m_Codec && !m_Codec->OpenDecoder(false))
        logM(LOGERROR, "Failed to open Amlogic Codec");

      m_videoBufferPool = std::make_shared<CAMLVideoBufferPool>();

      m_opened = true;
      set_osd_max = true;
    }
  }

  AMLFrameMetadata sideDataMeta;

  if (packet.pSideData && packet.iSideDataElems > 0)
  {
    const AVPacketSideData* rawSideData = av_packet_side_data_get(static_cast<AVPacketSideData*>(packet.pSideData),
                                                                  packet.iSideDataElems,
                                                                  AV_PKT_DATA_DYNAMIC_HDR10_PLUS_RAW);
    if (rawSideData && rawSideData->size >= 6)
    {
      AMLLatchHdr10PlusT35(rawSideData->data, rawSideData->size, sideDataMeta);
      if (m_Codec->AddHDR10PData(rawSideData->data, rawSideData->size) < 0)
        LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000, "CDVDVideoCodecAmlogic::{}: hdr10p raw side data ioctl failed, size {}", __FUNCTION__, rawSideData->size);
    }
    else
    {
      const AVPacketSideData* sideData = av_packet_side_data_get(static_cast<AVPacketSideData*>(packet.pSideData),
                                                                 packet.iSideDataElems,
                                                                 AV_PKT_DATA_DYNAMIC_HDR10_PLUS);
      if (sideData && sideData->size >= sizeof(AVDynamicHDRPlus))
      {
        uint8_t t35[6 + AV_HDR_PLUS_MAX_PAYLOAD_SIZE] = {0xb5, 0x00, 0x3c, 0x00, 0x01, 0x04};
        uint8_t* payload = t35 + 6;
        size_t payloadSize = sizeof(t35) - 6;
        if (av_dynamic_hdr_plus_to_t35(reinterpret_cast<const AVDynamicHDRPlus*>(sideData->data), &payload, &payloadSize) >= 0)
        {
          AMLLatchHdr10PlusT35(t35, payloadSize + 6, sideDataMeta);
          if (m_Codec->AddHDR10PData(t35, payloadSize + 6) < 0)
            LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000, "CDVDVideoCodecAmlogic::{}: hdr10p side data ioctl failed, size {}", __FUNCTION__, payloadSize + 6);
        }
      }
    }
  }

  m_last_added = m_Codec->AddData(pData, iSize, packet.dts, m_hints.ptsinvalid ? DVD_NOPTS_VALUE : packet.pts);

  if (m_stripHdr10Plus && !m_pendingMeta.hdr10pSei.empty() &&
      std::find(m_streamMeta.flags.begin(), m_streamMeta.flags.end(), "hdr10plus-removed") ==
          m_streamMeta.flags.end())
  {
    m_streamMeta.flags.push_back("hdr10plus-removed");
    m_pendingMeta.flags = m_streamMeta.flags;
  }

  if (m_last_added && packet.pData)
  {
    if (!sideDataMeta.hdr10pSei.empty())
      m_pendingMeta.hdr10pSei = sideDataMeta.hdr10pSei;
    m_pendingMeta.Inherit(m_lastMeta);
    m_lastMeta = m_pendingMeta;
    if (m_hints.ptsinvalid || packet.pts == DVD_NOPTS_VALUE)
      CAMLFrameMetadataStore::GetInstance().Publish(m_metadataToken, m_pendingMeta);
    else
    {
      m_metadataSequencer.Commit(packet.pts, m_pendingMeta);
      m_lastCommitPts = packet.pts;
    }
    m_pendingMeta = m_streamMeta;
  }

  // Make change in luminance as late a possible to try and avoid starting change in luminance in menu.
  if (set_osd_max)
  {
    if (aml_dv_mode() == DV_MODE_ON) aml_dv_set_osd_max(aml_dv_osd_max_nits());
  }

  return m_last_added;
}

double CDVDVideoCodecAmlogic::RenderDisplayLatency()
{
  const auto winSystem = CServiceBroker::GetWinSystem();
  if (!winSystem)
    return 0.0;

  CGraphicContext& gfx = winSystem->GetGfxContext();
  float refresh = gfx.GetFPS();
  if (gfx.GetVideoResolution() == RES_WINDOW)
    refresh = 0;

  const double latencyTweak = static_cast<double>(
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->GetVideoLatencyTweak(
          refresh, static_cast<unsigned int>(gfx.GetResInfo().iScreenHeight)));
  const double videoDelay =
      static_cast<double>(m_processInfo.GetVideoSettings().m_AudioDelay) * 1000.0;
  const float displayLatency = gfx.GetDisplayLatency();

  return DVD_MSEC_TO_TIME(latencyTweak + static_cast<double>(displayLatency) - videoDelay -
                          static_cast<double>(winSystem->GetFrameLatencyAdjustment()));
}

void CDVDVideoCodecAmlogic::DrainMetadataToClock()
{
  if (!m_hints.pClock || m_metadataSequencer.Empty())
    return;

  double target = m_hints.pClock->GetClock();
  if (!m_hints.pClock->IsPaused())
    target += RenderDisplayLatency();

  AMLFrameMetadata meta;
  if (m_metadataSequencer.Consume(target, meta))
  {
    CAMLFrameMetadataStore::GetInstance().Publish(m_metadataToken, meta);
    if (!m_metaLeadLogged)
    {
      m_metaLeadLogged = true;
      logM(LOGDEBUG, "frame metadata pts lead {:.3f}", (m_lastCommitPts - target) / DVD_TIME_BASE);
    }
  }
}

void CDVDVideoCodecAmlogic::Reset(void)
{
  m_Codec->Reset();

  while (!m_packages.empty())
  {
    RecycleDualLayerPacket(std::move(m_packages.front()));
    m_packages.pop_front();
  }
  m_last_added = true;
  m_last_pData = nullptr;
  m_last_iSize = 0;

  m_mpeg2_sequence_pts = 0;
  m_has_keyframe = false;
  m_metadataSequencer.Reset();
  m_pendingMeta = m_streamMeta;
  if (m_bitstream && m_hints.codec == AV_CODEC_ID_H264)
    m_bitstream->ResetStartDecode();
}

void CDVDVideoCodecAmlogic::Abort()
{
  if (m_Codec)
    m_Codec->Abort();
}

CDVDVideoCodec::VCReturn CDVDVideoCodecAmlogic::GetPicture(VideoPicture* pVideoPicture)
{
  DrainMetadataToClock();

  if (!m_Codec)
    return VC_ERROR;

  VCReturn retVal = m_Codec->GetPicture(m_videobuffer);

  if (retVal == VC_PICTURE)
  {
    m_videobuffer.hdrType = m_hints.hdrType;
    m_videobuffer.color_space = m_hints.colorSpace;
    m_videobuffer.color_primaries = m_hints.colorPrimaries;
    m_videobuffer.color_transfer = m_hints.colorTransferCharacteristic;

    pVideoPicture->SetParams(m_videobuffer);

    pVideoPicture->videoBuffer = m_videoBufferPool->Get();
    static_cast<CAMLVideoBuffer*>(pVideoPicture->videoBuffer)->Set(this, m_Codec,
     m_Codec->GetOMXPts(), m_Codec->GetAmlDuration(), m_Codec->GetBufferIndex());;
  }

  // check for mpeg2 aspect ratio changes
  if (m_mpeg2_sequence && pVideoPicture->pts >= m_mpeg2_sequence_pts)
    m_aspect_ratio = m_mpeg2_sequence->ratio;

  // check for h264 aspect ratio changes
  if (m_h264_sequence && pVideoPicture->pts >= m_h264_sequence_pts)
    m_aspect_ratio = m_h264_sequence->ratio;

  pVideoPicture->iDisplayWidth  = pVideoPicture->iWidth;
  pVideoPicture->iDisplayHeight = pVideoPicture->iHeight;
  if (m_aspect_ratio > 1.0f && !m_hints.forced_aspect)
  {
    pVideoPicture->iDisplayWidth  = ((int)lrint(pVideoPicture->iHeight * m_aspect_ratio)) & ~3;
    if (pVideoPicture->iDisplayWidth > pVideoPicture->iWidth)
    {
      pVideoPicture->iDisplayWidth  = pVideoPicture->iWidth;
      pVideoPicture->iDisplayHeight = ((int)lrint(pVideoPicture->iWidth / m_aspect_ratio)) & ~3;
    }
  }

  return retVal;
}

void CDVDVideoCodecAmlogic::SetCodecControl(int flags)
{
  if (m_codecControlFlags != flags)
  {
    CLog::Log(LOGDEBUG, LOGVIDEO, "{} {:x}->{:x}",  __func__, m_codecControlFlags, flags);
    m_codecControlFlags = flags;

    if (flags & DVD_CODEC_CTRL_DROP)
      m_videobuffer.iFlags |= DVP_FLAG_DROPPED;
    else
      m_videobuffer.iFlags &= ~DVP_FLAG_DROPPED;

    if (m_Codec)
      m_Codec->SetDrain((flags & DVD_CODEC_CTRL_DRAIN) != 0);
  }
}

void CDVDVideoCodecAmlogic::SetSpeed(int iSpeed)
{
  if (m_Codec)
    m_Codec->SetSpeed(iSpeed);
}

void CDVDVideoCodecAmlogic::FrameRateTracking(uint8_t *pData, int iSize, double dts, double pts)
{
  // mpeg2 handling
  if (m_mpeg2_sequence)
  {
    // probe demux for sequence_header_code NAL and
    // decode aspect ratio and frame rate.
    if (CBitstreamConverter::mpeg2_sequence_header(pData, iSize, m_mpeg2_sequence.get()) &&
       (m_mpeg2_sequence->fps_rate > 0) && (m_mpeg2_sequence->fps_scale > 0))
    {
      if (!m_mpeg2_sequence->fps_scale || !m_mpeg2_sequence->fps_scale)
        return;

      m_mpeg2_sequence_pts = pts;
      if (m_mpeg2_sequence_pts == DVD_NOPTS_VALUE)
        m_mpeg2_sequence_pts = dts;

      CLog::Log(LOGDEBUG, "{}::{} fps:{:d}/{:d} mpeg2_fps:{:d}/{:d} options:0x{:2x}", __MODULE_NAME__, __FUNCTION__,
              m_hints.fpsrate, m_hints.fpsscale, m_mpeg2_sequence->fps_rate, m_mpeg2_sequence->fps_scale, m_hints.codecOptions);
      if  (!(m_hints.codecOptions & CODEC_INTERLACED))
      {
        m_hints.fpsrate = m_mpeg2_sequence->fps_rate;
        m_hints.fpsscale = m_mpeg2_sequence->fps_scale;
      }
      if (m_hints.fpsrate && m_hints.fpsscale)
      {
        m_framerate = static_cast<float>(m_hints.fpsrate) / m_hints.fpsscale;
        if (m_hints.codecOptions & CODEC_UNKNOWN_I_P)
          if (std::abs(m_framerate - 25.0) < 0.02 || std::abs(m_framerate - 29.97) < 0.02)
          {
            m_framerate += m_framerate;
            m_hints.fpsrate += m_hints.fpsrate;
          }
        m_video_rate = (int)(0.5 + (96000.0 / m_framerate));
      }
      m_hints.width    = m_mpeg2_sequence->width;
      m_hints.height   = m_mpeg2_sequence->height;
      m_hints.aspect   = m_mpeg2_sequence->ratio;

      m_processInfo.SetVideoFps(m_framerate);
//      m_processInfo.SetVideoDAR(m_hints.aspect);
    }
    return;
  }

  // h264 aspect ratio handling
  if (m_h264_sequence)
  {
    // probe demux for SPS NAL and decode aspect ratio
    if (CBitstreamConverter::h264_sequence_header(pData, iSize, m_h264_sequence.get()))
    {
      m_h264_sequence_pts = pts;
      if (m_h264_sequence_pts == DVD_NOPTS_VALUE)
          m_h264_sequence_pts = dts;

      CLog::Log(LOGDEBUG, "{}: detected h264 aspect ratio({:f})",
        __MODULE_NAME__, m_h264_sequence->ratio);
      m_hints.width    = m_h264_sequence->width;
      m_hints.height   = m_h264_sequence->height;
      m_hints.aspect   = m_h264_sequence->ratio;
    }
  }

  if (m_vc1ForceFrameIntEnabled && !m_vc1ForceFrameIntLocked && m_vc1Parser)
  {
    const auto scan = m_vc1Parser->GetScanType(pData, iSize);
    if (scan != CVC1BitstreamParser::ScanType::Unknown)
    {
      const bool progressive = (scan == CVC1BitstreamParser::ScanType::Progressive);

      CSysfsPath(VC1_FORCE_FRAMEINT_PATH, progressive ? 1 : 0);
      m_vc1ForceFrameIntLast = progressive ? 1 : 0;
      m_vc1ForceFrameIntLocked = true;
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "{}::FrameRateTracking VC1 force_frameint -> {:d} (scan={}) latched",
                    __MODULE_NAME__, m_vc1ForceFrameIntLast,
                    progressive ? "progressive" : "interlaced");
    }
  }

  if (!m_vc1ScanDetected && m_vc1Parser)
  {
    const auto scan = m_vc1Parser->GetScanType(pData, iSize);
    if (scan != CVC1BitstreamParser::ScanType::Unknown)
    {
      m_vc1ScanDetected = true;
      const bool interlaced = (scan == CVC1BitstreamParser::ScanType::Interlaced);

      if (interlaced)
        m_hints.codecOptions = (m_hints.codecOptions & ~CODEC_PROGRESSIVE) | CODEC_INTERLACED;
      else
        m_hints.codecOptions = (m_hints.codecOptions & ~CODEC_INTERLACED) | CODEC_PROGRESSIVE;

      m_processInfo.SetVideoInterlaced(interlaced);

      const double oldFps = m_framerate;
      if (!interlaced && m_hints.fpsrate && m_hints.fpsscale)
      {
        const double fps = static_cast<double>(m_hints.fpsrate) / m_hints.fpsscale;
        static constexpr double kBaseRates[] = {23.976, 24.0, 25.0, 29.97, 30.0};
        for (double base : kBaseRates)
        {
          if (std::abs(fps / 2.0 - base) < 0.02)
          {
            m_hints.fpsrate /= 2;
            m_framerate = static_cast<float>(fps / 2.0);
            m_processInfo.SetVideoFps(static_cast<float>(m_framerate));
            break;
          }
        }
      }

      logComponentM(LOGDEBUG, LOGVIDEO,
        "{}::FrameRateTracking VC1 scan={} codecOptions=0x{:02x} fps={}/{} framerate {:.3f}->{:.3f}",
        __MODULE_NAME__, interlaced ? "interlaced" : "progressive",
        m_hints.codecOptions, m_hints.fpsrate, m_hints.fpsscale, oldFps, m_framerate);
    }
    else
    {
      LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000,
        "{}::FrameRateTracking VC1 scan=unknown (no FRAME in packet size={})",
        __MODULE_NAME__, iSize);
    }
  }
}
