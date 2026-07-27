/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <math.h>

#include "DVDCodecs/DVDFactoryCodec.h"
#include "utils/MemUtils.h"
#include "DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "DVDStreamInfo.h"
#include "AMLCodec.h"
#include "ServiceBroker.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "threads/Thread.h"

#define __MODULE_NAME__ "DVDVideoCodecAmlogic"

namespace
{
constexpr int FEL_SEEK_FIX_DETECTION_PAIRS = 4;

bool IsNoPts(double pts)
{
  return pts == DVD_NOPTS_VALUE;
}

double PacketTime(double pts, double dts)
{
  return !IsNoPts(pts) ? pts : dts;
}


} // namespace

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
      settings->RegisterCallback(this, {CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND,
                                       CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE,
                                       CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAX_LUM});
      m_settingsCallbackRegistered = true;
    }
  }

  UpdateAppendCMv40SettingCache();
}

CDVDVideoCodecAmlogic::~CDVDVideoCodecAmlogic()
{
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

  int appendCMv40 = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND);
  if (aml_dv_type() != DV_TYPE_DISPLAY_LED)
    appendCMv40 = 0;

  if (appendCMv40 != 0)
  {
    m_smartDisplayNits.store(settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAX_LUM));
  }

  m_appendCMv40ModeSetting.store(static_cast<int>(appendCMv40 != 0 ?
      DOVICMv40Mode::CMV40_SMART : DOVICMv40Mode::CMV40_NONE));
}

void CDVDVideoCodecAmlogic::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (!setting) return;

  if (setting->GetId() == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND ||
      setting->GetId() == CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE ||
      setting->GetId() == CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAX_LUM)
    UpdateAppendCMv40SettingCache();
}

void CDVDVideoCodecAmlogic::ApplyDynamicDoViSettings()
{
  if (!m_bitstream) return;

  const auto mode = static_cast<DOVICMv40Mode>(m_appendCMv40ModeSetting.load());
  if (mode == m_appendCMv40ModeApplied) return;

  PushCMv40Settings(mode);

  logM(LOGINFO, "CDVDVideoCodecAmlogic", "DV HEVC bitstream - CMv4.0 append mode changed to [{:d}]",
       static_cast<int>(mode));
}

void CDVDVideoCodecAmlogic::PushCMv40Settings(DOVICMv40Mode mode)
{
  if (mode == DOVICMv40Mode::CMV40_SMART)
  {
    m_bitstream->SetSmartBypassDisplayNits(m_smartDisplayNits.load());
  }

  m_bitstream->SetAppendCMv40(mode);
  m_appendCMv40ModeApplied = mode;
}

bool CDVDVideoCodecAmlogic::IsDvP7FelStream() const
{
  return m_bitstream && m_hints.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION &&
         m_hints.dovi.dv_profile == 7 && m_hints.dovi_el_type == DOVIELType::TYPE_FEL;
}

CDVDVideoCodecAmlogic::FelSeekFixSettings CDVDVideoCodecAmlogic::GetFelSeekFixSettings() const
{
  FelSeekFixSettings settings;

  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto advancedSettings = settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;
  if (!advancedSettings)
    return settings;

  settings.enabled = advancedSettings->m_videoFelSeekFix;
  if (advancedSettings->m_videoFelSeekFixThresholdFrames > 0)
    settings.thresholdFrames = advancedSettings->m_videoFelSeekFixThresholdFrames;

  return settings;
}

bool CDVDVideoCodecAmlogic::CanRunFelSeekFixDetection() const
{
  return m_felSeekFixSettings.enabled && IsDvP7FelStream() &&
         m_userConvertDoviMode == DOVIMode::MODE_NONE && !m_felSeekFixActive;
}

void CDVDVideoCodecAmlogic::ArmFelSeekFixDetection()
{
  m_felSeekFixDetectionPairsAfterReset = 0;
  m_felSeekFixSettings = GetFelSeekFixSettings();

  if (CanRunFelSeekFixDetection())
    m_felSeekFixDetectionPairsAfterReset = FEL_SEEK_FIX_DETECTION_PAIRS;

  CLog::Log(LOGDEBUG,
            "{} FEL_SEEK_FIX detection armed: pairs={} enabled={} thresholdFrames={} userConvertDovi={} hdr={} profile={} el={} bitstream={}",
            __FUNCTION__, m_felSeekFixDetectionPairsAfterReset, m_felSeekFixSettings.enabled,
            m_felSeekFixSettings.thresholdFrames, static_cast<int>(m_userConvertDoviMode),
            static_cast<int>(m_hints.hdrType), m_hints.dovi.dv_profile,
            static_cast<int>(m_hints.dovi_el_type), static_cast<bool>(m_bitstream));
}

void CDVDVideoCodecAmlogic::EnableFelSeekFix(double ptsBl,
                                             double dtsBl,
                                             double ptsEl,
                                             double dtsEl,
                                             double timeDeltaMs,
                                             double thresholdMs,
                                             int pairsLeft)
{
  if (!CanRunFelSeekFixDetection())
    return;

  m_bitstream->SetConvertDovi(DOVIMode::MODE_TOMEL);
  m_felSeekFixActive = true;
  m_felSeekFixDetectionPairsAfterReset = 0;

  CLog::Log(LOGINFO,
            "CDVDVideoCodecAmlogic FEL_SEEK_FIX fallback enabled after {}: BL pts {:.3f} dts {:.3f}, EL pts {:.3f} dts {:.3f}, time-delta {:.3f} ms > threshold {:.3f} ms ({} frames), detectPairsLeft {}",
            "seek", ptsBl / DVD_TIME_BASE, dtsBl / DVD_TIME_BASE, ptsEl / DVD_TIME_BASE,
            dtsEl / DVD_TIME_BASE, timeDeltaMs, thresholdMs,
            m_felSeekFixSettings.thresholdFrames, pairsLeft);
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
  if (!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEAMCODEC))
    return false;
  if ((hints.stills && hints.fpsrate == 0) || hints.width == 0)
    return false;

  // close open decoder if necessary
  if (m_opened)
    Close();

  m_hints = hints;
  m_hints.pClock = hints.pClock;

  CLog::Log(LOGDEBUG, "{}::{} - codec {:d} profile:{:d} extra_size:{:d} fps:{:d}/{:d}",
    __MODULE_NAME__, __FUNCTION__, m_hints.codec, m_hints.profile, m_hints.extradata.GetSize(), m_hints.fpsrate, m_hints.fpsscale);

  switch(m_hints.codec)
  {
    case AV_CODEC_ID_MJPEG:
      m_pFormatName = "am-mjpeg";
      break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
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
      m_pFormatName = "am-h265";
      m_bitstream = std::make_unique<CBitstreamConverter>(m_hints);
      m_bitstream->Open(true);

      // check for hevc-hvcC and convert to h265-annex-b - and DV is on.
      if (m_hints.extradata && !m_hints.cryptoSession && m_bitstream && (aml_dv_mode() != DV_MODE_OFF))
      {
        auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

        int dualPriorityValue = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_DUAL_PRIORITY);
        bool dualPriorityHdr10Plus = (dualPriorityValue == 1);
        bool dualPriorityHdrVivid  = (dualPriorityValue == 2);
        // Strip Vivid metadata when the user disabled Vivid and Vivid does
        // not have priority.  Used by both the DV and HDR10 paths below.
        const bool vividDisabled =
            (dualPriorityValue != 2) &&
            settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDRVIVID_DISABLE);

        if (m_hints.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
        {
          const auto cmv40Mode =
              static_cast<DOVICMv40Mode>(m_appendCMv40ModeSetting.load());
          if (cmv40Mode != DOVICMv40Mode::CMV40_NONE)
            PushCMv40Settings(cmv40Mode);

          // Global Vivid disable: strip Vivid metadata regardless of priority.
          // Only applies when Vivid does NOT have priority (dual_priority != 2).
          if (vividDisabled)
          {
            CLog::Log(LOGINFO, "{}::{} - DV HEVC bitstream - HDR Vivid is disabled; removing Vivid metadata if present.",
                      __MODULE_NAME__, __FUNCTION__);
            m_bitstream->SetRemoveHdrVivid(true);
          }

          if (dualPriorityHdr10Plus)
          {
            CLog::Log(LOGINFO, "{}::{} - DV HEVC bitstream - if stream also contains HDR10+, native HDR10+ has priority.",
                      __MODULE_NAME__, __FUNCTION__);
            m_bitstream->SetDualPriorityHdr10Plus(true);
          }
          else if (dualPriorityHdrVivid)
          {
            CLog::Log(LOGINFO, "{}::{} - DV HEVC bitstream - if stream also contains HDR Vivid, native HDR Vivid has priority.",
                      __MODULE_NAME__, __FUNCTION__);
            m_bitstream->SetDualPriorityHdrVivid(true);
          }
          else if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDR10PLUS_CONVERT)) 
          {
            bool preferConvertHdr10Plus = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDR10PLUS_PREFER_CONVERT);
            m_bitstream->SetPreferCovertHdr10Plus(preferConvertHdr10Plus);

            if (preferConvertHdr10Plus) 
              CLog::Log(LOGINFO, "{}::{} - DV HEVC bitstream - if stream also contains HDR10+, conversion will be prefered over original Dolby Vision.",
                        __MODULE_NAME__, __FUNCTION__);
          }
          else if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDRVIVID_CONVERT))
          {
            bool preferConvertHdrVivid = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDRVIVID_PREFER_CONVERT);
            m_bitstream->SetPreferCovertHdrVivid(preferConvertHdrVivid);

            if (preferConvertHdrVivid)
              CLog::Log(LOGINFO, "{}::{} - DV HEVC bitstream - if stream also contains HDR Vivid, conversion will be prefered over original Dolby Vision.",
                        __MODULE_NAME__, __FUNCTION__);
          }

          if (m_hints.dovi.dv_profile == 7)
          {
            m_userConvertDoviMode = static_cast<DOVIMode>(settings->GetInt(CSettings::SETTING_VIDEOPLAYER_CONVERTDOVI));
            if (m_userConvertDoviMode != DOVIMode::MODE_NONE)
            {
              CLog::Log(LOGINFO, "{}::{} - DV HEVC bitstream - user chooses to convert to mode [{:d}]",
                        __MODULE_NAME__, __FUNCTION__, static_cast<int>(m_userConvertDoviMode));
              m_bitstream->SetConvertDovi(m_userConvertDoviMode);
            }
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

        // Potential HDR Vivid (Cannot tell at this point)
        // Check disable flag first — when disabled, strip all Vivid metadata
        // and degrade to plain HDR10.  This is only applicable when priority
        // is DV (0) or HDR10+ (1), i.e. Vivid does NOT have priority.
        if (vividDisabled)
        {
          CLog::Log(LOGINFO, "{}::{} - HDR10 HEVC bitstream - if HDR Vivid then metadata will be removed and content downgraded to HDR10.",
                    __MODULE_NAME__, __FUNCTION__);
          m_bitstream->SetRemoveHdrVivid(true);
        }
        else if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_HDRVIVID_CONVERT))
        {
          CLog::Log(LOGDEBUG, "{}::{} - HDR10 HEVC bitstream - if HDR Vivid then will be converted to Dolby Vision P8.1",
            __MODULE_NAME__, __FUNCTION__);
          m_bitstream->SetConvertHdrVivid(true);
        }

        // If HDR10 or Dual Priority HDR10+ and doing VS10 - remove the HDR10+ and DV if present to avoid conflict with VS10.
        if ((m_hints.hdrType == StreamHdrType::HDR_TYPE_HDR10) || dualPriorityHdr10Plus)
        {
          unsigned int mode(aml_vs10_by_setting(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10PLUS));
          if (mode < DOLBY_VISION_OUTPUT_MODE_BYPASS)
          {
            // for VS10 conversion need to remove the HDR10plus metadata.
            CLog::Log(LOGINFO, "{}::{} - HDR10 HEVC bitstream - if HDR10+ then metadata will be removed to allow correct VS10 processing",
              __MODULE_NAME__, __FUNCTION__);
            m_bitstream->SetRemoveHdr10Plus(true);
            m_bitstream->SetRemoveDovi(true);
          }
        }

        // If HDR10 or Dual Priority HDR Vivid and doing VS10 — remove the
        // Vivid and DV if present to avoid conflict with VS10.
        //
        // NOTE: Do NOT include vividDisabled here.  vividDisabled means
        // "strip Vivid, degrade to HDR10" which is handled by
        // SetRemoveHdrVivid() above.  Adding it here would also trigger
        // SetRemoveDovi() for pure DV streams (P5/P8) that carry no
        // Vivid metadata — accidentally killing legitimate Dolby Vision.
        if ((m_hints.hdrType == StreamHdrType::HDR_TYPE_HDR10) || dualPriorityHdrVivid)
        {
          unsigned int mode(aml_vs10_by_setting(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10PLUS));
          if (mode < DOLBY_VISION_OUTPUT_MODE_BYPASS)
          {
            CLog::Log(LOGINFO, "{}::{} - HDR10 HEVC bitstream - if HDR Vivid then metadata will be removed to allow correct VS10 processing",
              __MODULE_NAME__, __FUNCTION__);
            m_bitstream->SetRemoveHdrVivid(true);
            m_bitstream->SetRemoveDovi(true);
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

  CLog::Log(LOGINFO, "{}: Opened Amlogic Codec", __MODULE_NAME__);
  return true;
FAIL:
  Close();
  return false;
}

void CDVDVideoCodecAmlogic::Close(void)
{
  CLog::Log(LOGDEBUG, "{}::{}", __MODULE_NAME__, __FUNCTION__);

  aml_kodi_reset_cd_cs();

  m_videoBufferPool = nullptr;

  if (m_Codec)
    m_Codec->CloseDecoder(false), m_Codec = nullptr;

  m_videobuffer.iFlags = 0;

  m_opened = false;

  m_felSeekFixActive = false;
  m_felSeekFixDetectionPairsAfterReset = 0;
  m_felSeekFixSettings = FelSeekFixSettings{};
  m_userConvertDoviMode = DOVIMode::MODE_NONE;

  while (!m_packages.empty())
    PopFrontPackage();
  m_mpeg2_sequence_pts = 0;
  m_has_keyframe = false;

  if (m_bitstream)
    m_bitstream->ResetStartDecode();
}

void CDVDVideoCodecAmlogic::PopFrontPackage()
{
  KODI::MEMORY::AlignedFree(m_packages.front().data);
  m_packages.pop_front();
}

bool CDVDVideoCodecAmlogic::AddData(const DemuxPacket &packet)
{
  // Handle Input, add demuxer packet to input queue, we must accept it or
  // it will be discarded as VideoPlayerVideo has no concept of "try again".

  uint8_t *pData(packet.pData);
  uint32_t iSize(packet.iSize);
  bool data_added = false;
  bool dual_layer_converted = false;
  bool set_osd_max = false;
  double outputPts = packet.pts;
  double outputDts = packet.dts;

  if (pData)
  {
    ApplyDynamicDoViSettings();

    if (m_bitstream)
    {
      if (packet.isDualStream && aml_dolby_vision_enabled())
      {
        logComponentM(LOGDEBUG, LOGVIDEO, "CDVDVideoCodecAmlogic",
          "{} package with dts: {:.3f}, pts: {:.3f} and size {} arrived, list {} empty",
          packet.isELPackage ? "EL" : "BL", packet.dts/DVD_TIME_BASE, packet.pts/DVD_TIME_BASE, iSize,
          m_packages.empty() ? "is" : "is not");

        if (!m_packages.empty())
        {
          // Pair the queued packet with the current one to form a BL+EL pair.
          // Convert() takes the BL as the first argument; if the queued packet
          // is BL and the new one is EL (or vice versa), swap so the call is
          // uniform below.
          const DLDemuxPacket dualLayerPacket = m_packages.front();

          if (dualLayerPacket.isELPackage != packet.isELPackage)
          {
            uint8_t* pDataBl = packet.isELPackage ? dualLayerPacket.data : pData;
            uint32_t iSizeBl = packet.isELPackage ? dualLayerPacket.size : iSize;
            uint8_t* pDataEl = packet.isELPackage ? pData : dualLayerPacket.data;
            uint32_t iSizeEl = packet.isELPackage ? iSize : dualLayerPacket.size;
            const double ptsBl = packet.isELPackage ? dualLayerPacket.pts : packet.pts;
            const double dtsBl = packet.isELPackage ? dualLayerPacket.dts : packet.dts;
            const double ptsEl = packet.isELPackage ? packet.pts : dualLayerPacket.pts;
            const double dtsEl = packet.isELPackage ? packet.dts : dualLayerPacket.dts;
            const double timeBl = PacketTime(ptsBl, dtsBl);
            const double timeEl = PacketTime(ptsEl, dtsEl);
            if (m_felSeekFixDetectionPairsAfterReset > 0 && CanRunFelSeekFixDetection())
            {
              const bool hasValidTimes = !IsNoPts(timeBl) && !IsNoPts(timeEl);
              const double timeDelta = hasValidTimes ? fabs(timeBl - timeEl) : 0.0;
              const double fps = (m_hints.fpsrate > 0 && m_hints.fpsscale > 0)
                                     ? static_cast<double>(m_hints.fpsrate) / m_hints.fpsscale
                                     : 24000.0 / 1001.0;
              const int thresholdFrames = m_felSeekFixSettings.thresholdFrames;
              const double staleElThreshold = DVD_TIME_BASE * static_cast<double>(thresholdFrames) / fps;

              m_felSeekFixDetectionPairsAfterReset--;
              if (hasValidTimes && timeDelta > staleElThreshold)
                EnableFelSeekFix(ptsBl, dtsBl, ptsEl, dtsEl, DVD_TIME_TO_MSEC(timeDelta),
                                 DVD_TIME_TO_MSEC(staleElThreshold),
                                 m_felSeekFixDetectionPairsAfterReset);
              else if (m_felSeekFixDetectionPairsAfterReset == 0)
                CLog::Log(LOGDEBUG,
                          "CDVDVideoCodecAmlogic FEL_SEEK_FIX detection window closed without fallback");
            }

            outputPts = !IsNoPts(ptsBl) ? ptsBl : packet.pts;
            outputDts = !IsNoPts(dtsBl) ? dtsBl : packet.dts;
            logComponentM(LOGDEBUG, LOGVIDEO, "CDVDVideoCodecAmlogic",
              "found DT-DL pair in list: bl size {}, el size {}, pts: {:.3f}",
              iSizeBl, iSizeEl, outputPts/DVD_TIME_BASE);
            dual_layer_converted = m_bitstream->Convert(pDataBl, iSizeBl, pDataEl, iSizeEl, outputPts);
          }
        }

        if (!dual_layer_converted)
        {
          // backup package and don't send to decoder yet
          uint8_t *pDataBackup = static_cast<uint8_t*>(KODI::MEMORY::AlignedMalloc(packet.iSize + AV_INPUT_BUFFER_PADDING_SIZE, 16));
          memcpy(pDataBackup, packet.pData, packet.iSize);
          m_packages.push_back({pDataBackup, iSize, packet.isELPackage, packet.dts, packet.pts});
          logComponentM(LOGDEBUG, LOGVIDEO, "CDVDVideoCodecAmlogic",
            "did add {} package with dts: {:.3f}, pts: {:.3f} and size {} in list",
            packet.isELPackage ? "EL" : "BL", packet.dts/DVD_TIME_BASE, packet.pts/DVD_TIME_BASE, packet.iSize);

          return true;
        }
      }
      else
      {
        if (!m_bitstream->Convert(pData, iSize, packet.pts))
          return true;
      }

      // For single-layer content, guard on CanStartDecode (IDR detection).
      // Dual-layer (FEL P7) conversion outputs combined BL+EL bitstream that
      // may start mid-GOP (e.g. ISO playlists with non-zero start PTS);
      // the hardware Amlogic decoder handles its own IDR wait — skip the
      // software-level guard to avoid discarding pre-IDR access units.
      if (!m_bitstream->CanStartDecode() && !dual_layer_converted)
      {
        logM(LOGDEBUG, "CDVDVideoCodecAmlogic", "waiting for keyframe (bitstream)");
        return true;
      }
      pData = m_bitstream->GetConvertBuffer();
      iSize = m_bitstream->GetConvertSize();
    }
    else if (!m_has_keyframe && m_bitparser)
    {
      if (!m_bitparser->CanStartDecode(pData, iSize))
      {
        logM(LOGDEBUG, "CDVDVideoCodecAmlogic", "waiting for keyframe (bitparser)");
        return true;
      }
      else
        m_has_keyframe = true;
    }
    FrameRateTracking( pData, iSize, outputDts, outputPts);

    if (!m_opened)
    {
      if (outputPts == DVD_NOPTS_VALUE)
        m_hints.ptsinvalid = true;

      CLog::Log(LOGINFO, "CDVDVideoCodecAmlogic::{}: Open decoder: fps:{:d}/{:d}", __FUNCTION__, m_hints.fpsrate, m_hints.fpsscale);
      if (m_Codec && !m_Codec->OpenDecoder(false))
        CLog::Log(LOGERROR, "CDVDVideoCodecAmlogic::{}: Failed to open Amlogic Codec", __FUNCTION__);

      m_videoBufferPool = std::make_shared<CAMLVideoBufferPool>();

      m_opened = true;
      set_osd_max = true;
    }
  }

  data_added = m_Codec->AddData(pData, iSize, outputDts, m_hints.ptsinvalid ? DVD_NOPTS_VALUE : outputPts);

  // pop package only from list if hardware decoder did accept the data
  if (data_added && dual_layer_converted)
    PopFrontPackage();

  // Make change in luminance as late a possible to try and avoid starting change in luminance in menu.
  if (set_osd_max)
  {
    // if DV_MODE_ON (i.e. on in Kodi Menu), then set graphics max to 0 (OSD luminance will be handled by amlogic).
    if (aml_dv_mode() == DV_MODE_ON) aml_dv_set_osd_max(0);
  }

  return data_added;
}

void CDVDVideoCodecAmlogic::Reset(void)
{
  m_Codec->Reset();

  while (!m_packages.empty())
    PopFrontPackage();

  ArmFelSeekFixDetection();

  m_mpeg2_sequence_pts = 0;
  m_has_keyframe = false;
  if (m_bitstream)
    m_bitstream->ResetStartDecode();
}

CDVDVideoCodec::VCReturn CDVDVideoCodecAmlogic::GetPicture(VideoPicture* pVideoPicture)
{
  if (!m_Codec)
    return VC_ERROR;

  VCReturn retVal = m_Codec->GetPicture(m_videobuffer);

  if (retVal == VC_PICTURE)
  {
    pVideoPicture->videoBuffer = nullptr;
    pVideoPicture->SetParams(m_videobuffer);

    pVideoPicture->videoBuffer = m_videoBufferPool->Get();
    static_cast<CAMLVideoBuffer*>(pVideoPicture->videoBuffer)->Set(this, m_Codec,
     m_Codec->GetOMXPts(), m_Codec->GetAmlDuration(), m_Codec->GetBufferIndex());;

    m_dataCacheCore.SetVideoPts(m_Codec->GetPts());

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
}
