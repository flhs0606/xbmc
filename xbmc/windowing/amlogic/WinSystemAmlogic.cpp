/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemAmlogic.h"

#include <algorithm>
#include <cmath>
#include <string.h>
#include <float.h>

#include "ServiceBroker.h"
#include "cores/RetroPlayer/process/amlogic/RPProcessInfoAmlogic.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererOpenGLES.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/Process/amlogic/ProcessInfoAmlogic.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.h"
#include "windowing/GraphicContext.h"
#include "windowing/Resolution.h"
#include "platform/linux/powermanagement/LinuxPowerSyscall.h"
#include "rendering/gles/ScreenshotSurfaceGLES.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "guilib/DispResource.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "threads/SingleLock.h"
#include "DolbyVisionAML.h"

#include "platform/linux/SysfsPath.h"

#include <linux/fb.h>
#include <linux/version.h>

#include "system_egl.h"

using namespace KODI;

CWinSystemAmlogic::CWinSystemAmlogic()
:  m_nativeWindow(nullptr)
,  m_libinput(new CLibInputHandler)
,  m_force_mode_switch(false)
,  m_modeSwitchBlanked(false)
,  m_modeSwitchFb0Blank(0)
,  m_modeSwitchFb1Blank(0)
{
  const char *env_framebuffer = getenv("FRAMEBUFFER");

  // default to framebuffer 0
  m_framebuffer_name = "fb0";
  if (env_framebuffer)
  {
    std::string framebuffer(env_framebuffer);
    std::string::size_type start = framebuffer.find("fb");
    m_framebuffer_name = framebuffer.substr(start);
  }

  m_nativeDisplay = EGL_NO_DISPLAY;

  m_stereo_mode = RENDER_STEREO_MODE_OFF;
  m_delayDispReset = false;

  m_libinput->Start();
}

bool CWinSystemAmlogic::InitWindowSystem()
{
  // Setup DV UI Elements etc.
  m_dolbyVisionAML = std::make_unique<CDolbyVisionAML>();
  if (!m_dolbyVisionAML->Setup()) m_dolbyVisionAML.reset();

  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  logM(LOGDEBUG, "disabling noise reduction");
  CSysfsPath("/sys/module/di/parameters/nr2_en", 0);

  if (((LINUX_VERSION_CODE >> 16) & 0xFF) < 5)
  {
    auto setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);
    if (setting)
    {
      setting->SetVisible(false);
      settings->SetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING, false);
    }
  }

  m_nativeDisplay = EGL_DEFAULT_DISPLAY;

  CDVDVideoCodecAmlogic::Register();
  CLinuxRendererGLES::Register();
  VIDEOPLAYER::CProcessInfoAmlogic::Register();
  RETRO::CRPProcessInfoAmlogic::Register();
  RETRO::CRPProcessInfoAmlogic::RegisterRendererFactory(new RETRO::CRendererFactoryOpenGLES);
  CRendererAML::Register();
  CScreenshotSurfaceGLES::Register();

  if (aml_get_cpufamily_id() <= AML_GXL)
    aml_set_framebuffer_resolution(1920, 1080, m_framebuffer_name);

  auto setting = settings->GetSetting(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (setting)
  {
    setting->SetVisible(false);
    settings->SetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK, false);
  }

  // kill a running animation
  CLog::Log(LOGDEBUG,"CWinSystemAmlogic: Sending SIGUSR1 to 'splash-image'");
  std::system("killall -s SIGUSR1 splash-image &> /dev/null");

  // Close the OpenVFD splash and switch the display into time mode.
  CSysfsPath("/tmp/openvfd_service", 0);

  return CWinSystemBase::InitWindowSystem();
}

bool CWinSystemAmlogic::DestroyWindowSystem()
{
  return true;
}

bool CWinSystemAmlogic::CreateNewWindow(const std::string& name,
                                    bool fullScreen,
                                    RESOLUTION_INFO& res)
{
  m_nWidth        = res.iWidth;
  m_nHeight       = res.iHeight;
  m_fRefreshRate  = res.fRefreshRate;

  if (m_nativeWindow == nullptr)
    m_nativeWindow = new fbdev_window;

  m_nativeWindow->width = res.iWidth;
  m_nativeWindow->height = res.iHeight;

  int delay = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt("videoscreen.delayrefreshchange");
  if (delay > 0)
  {
    m_delayDispReset = true;
    m_dispResetTimer.Set(std::chrono::milliseconds(static_cast<unsigned int>(delay * 100)));
  }

  BeginModeSwitchBlank();

  {
    std::lock_guard lock(m_resourceSection);

    for (auto i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnLostDisplay();
    }
  }

  aml_set_native_resolution(res, m_framebuffer_name, m_stereo_mode, m_force_mode_switch);
  // reset force mode switch
  m_force_mode_switch = false;

  if (!m_delayDispReset)
  {
    std::lock_guard lock(m_resourceSection);

    // tell any shared resources
    for (auto i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnResetDisplay();
    }
  }

  // Make sure DV Display activates if enabled - TODO: Why needed?
  aml_dv_display_trigger();

  m_bWindowCreated = true;
  return true;
}

void CWinSystemAmlogic::BeginModeSwitchBlank()
{
  if (m_modeSwitchBlanked)
    return;

  m_osdReassertFrames = 0;
  m_modeSwitchFb0Blank = aml_osd_blank(0, 1);
  m_modeSwitchFb1Blank = aml_osd_blank(1, 1);
  m_modeSwitchBlanked = true;
}

void CWinSystemAmlogic::EndModeSwitchBlank()
{
  if (!m_modeSwitchBlanked)
    return;

  aml_osd_blank(0, m_modeSwitchFb0Blank);
  aml_osd_blank(1, m_modeSwitchFb1Blank);
  m_modeSwitchBlanked = false;
  m_modeSwitchFb0Blank = 0;
  m_modeSwitchFb1Blank = 0;
  m_osdReassertFrames = 6;
}

void CWinSystemAmlogic::OsdReassertTick()
{
  if (m_osdReassertFrames <= 0)
    return;

  if (--m_osdReassertFrames == 0)
  {
    aml_osd_blank(0, 0);
    aml_osd_blank(1, 0);
    logM(LOGDEBUG, "CWinSystemAmlogic::OsdReassertTick - re-asserted OSD unblank after mode switch");
  }
}

bool CWinSystemAmlogic::DestroyWindow()
{
  if (m_nativeWindow != nullptr)
  {
    delete(m_nativeWindow);
    m_nativeWindow = nullptr;
  }

  m_bWindowCreated = false;
  return true;
}

void CWinSystemAmlogic::UpdateResolutions()
{
  CWinSystemBase::UpdateResolutions();

  RESOLUTION_INFO resDesktop, curDisplay;
  std::vector<RESOLUTION_INFO> resolutions;

  if (!aml_probe_resolutions(resolutions) || resolutions.empty())
  {
    CLog::Log(LOGWARNING, "{}: ProbeResolutions failed.",__FUNCTION__);
  }

  /* ProbeResolutions includes already all resolutions.
   * Only get desktop resolution so we can replace xbmc's desktop res
   */
  bool resDesktop_set = false;

  const auto settingsComp = CServiceBroker::GetSettingsComponent();
  if (settingsComp && settingsComp->GetSettings())
  {
    const std::string screenmode =
        settingsComp->GetSettings()->GetString(CSettings::SETTING_VIDEOSCREEN_SCREENMODE);
    if (screenmode.size() >= 20 && screenmode != "DESKTOP" && screenmode != "WINDOW")
    {
      const int width = std::strtol(screenmode.substr(0, 5).c_str(), nullptr, 10);
      const int height = std::strtol(screenmode.substr(5, 5).c_str(), nullptr, 10);
      const float refresh = static_cast<float>(
          std::strtod(screenmode.substr(10, 9).c_str(), nullptr));
      uint32_t modeFlags = 0;
      if (screenmode.substr(19, 1) == "i")
        modeFlags |= D3DPRESENTFLAG_INTERLACED;
      if (screenmode.find("sbs") != std::string::npos)
        modeFlags |= D3DPRESENTFLAG_MODE3DSBS;
      if (screenmode.find("tab") != std::string::npos)
        modeFlags |= D3DPRESENTFLAG_MODE3DTB;
      if (screenmode.find("frp") != std::string::npos)
        modeFlags |= D3DPRESENTFLAG_MODE3DFP;

      if (width > 0 && height > 0 && refresh > 0.0f)
      {
        for (const auto& r : resolutions)
        {
          if (r.iScreenWidth == width && r.iScreenHeight == height &&
              (r.dwFlags & D3DPRESENTFLAG_MODEMASK) == (modeFlags & D3DPRESENTFLAG_MODEMASK) &&
              ResolutionRefreshRateEquals(r.fRefreshRate, refresh))
          {
            resDesktop = r;
            resDesktop_set = true;
            logM(LOGINFO,
                 "RES_DESKTOP from videoscreen.screenmode={}: iScreenWidth={} iScreenHeight={} "
                 "iWidth={} fRefreshRate={:f} dwFlags={} strId={}",
                 screenmode, r.iScreenWidth, r.iScreenHeight, r.iWidth, r.fRefreshRate,
                 r.dwFlags, r.strId);
            break;
          }
        }
        if (!resDesktop_set)
          logM(LOGWARNING,
               "videoscreen.screenmode={} parsed width={} height={} refresh={:f} modeFlags={} "
               "matches no probed mode; falling back to snapshot then kernel mode",
               screenmode, width, height, refresh, modeFlags);
      }
    }
  }

  if (!resDesktop_set)
  {
    RESOLUTION_INFO snapshot;
    if (CGraphicContext::LoadPersistedDesktopResolution(snapshot))
    {
      for (const auto& r : resolutions)
      {
        if (r.iScreenWidth == snapshot.iScreenWidth &&
            r.iScreenHeight == snapshot.iScreenHeight &&
            std::fabs(r.fRefreshRate - snapshot.fRefreshRate) < FLT_EPSILON &&
            (r.dwFlags & D3DPRESENTFLAG_MODEMASK) ==
                (snapshot.dwFlags & D3DPRESENTFLAG_MODEMASK))
        {
          resDesktop = snapshot;
          resDesktop_set = true;
          CLog::Log(LOGINFO,
                    "RES_DESKTOP overridden by persisted snapshot: {}x{}@{:f}Hz",
                    snapshot.iScreenWidth, snapshot.iScreenHeight,
                    snapshot.fRefreshRate);
          break;
        }
      }
      if (!resDesktop_set)
        CLog::Log(LOGWARNING,
                  "persisted desktop snapshot {}x{}@{:f}Hz not in current EDID; "
                  "falling back to kernel mode",
                  snapshot.iScreenWidth, snapshot.iScreenHeight,
                  snapshot.fRefreshRate);
    }
  }

  if (!resDesktop_set && aml_get_native_resolution(curDisplay))
  {
    resDesktop = curDisplay;

    if (resDesktop.iScreenHeight >= 2160 && resDesktop.fRefreshRate > 40.0f)
    {
      const RESOLUTION_INFO* cand = nullptr;
      for (const auto& r : resolutions)
      {
        if (r.iScreenWidth != 1920 || r.iScreenHeight != 1080)
          continue;
        if ((r.dwFlags & D3DPRESENTFLAG_MODEMASK) != (resDesktop.dwFlags & D3DPRESENTFLAG_MODEMASK))
          continue;
        if (!cand || std::fabs(r.fRefreshRate - resDesktop.fRefreshRate) < std::fabs(cand->fRefreshRate - resDesktop.fRefreshRate))
          cand = &r;
      }
      if (cand && std::fabs(cand->fRefreshRate - resDesktop.fRefreshRate) < 1.0f)
      {
        logM(LOGINFO, "defaulting desktop to {}x{}@{:f} instead of native 2160p@{:f} to stay within the HDMI link budget (2160p50/60 stays selectable)",
             cand->iScreenWidth, cand->iScreenHeight, cand->fRefreshRate, resDesktop.fRefreshRate);
        resDesktop = *cand;
      }
    }
  }

  RESOLUTION ResDesktop = RES_INVALID;
  RESOLUTION res_index  = RES_DESKTOP;

  for (size_t i = 0; i < resolutions.size(); i++)
  {
    // if this is a new setting,
    // create a new empty setting to fill in.
    if ((int)CDisplaySettings::GetInstance().ResolutionInfoSize() <= res_index)
    {
      RESOLUTION_INFO res;
      CDisplaySettings::GetInstance().AddResolutionInfo(res);
    }

    CServiceBroker::GetWinSystem()->GetGfxContext().ResetOverscan(resolutions[i]);
    CDisplaySettings::GetInstance().GetResolutionInfo(res_index) = resolutions[i];

    CLog::Log(LOGINFO, "Found resolution {:d} x {:d} with {:d} x {:d}{} @ {:f} Hz",
      resolutions[i].iWidth,
      resolutions[i].iHeight,
      resolutions[i].iScreenWidth,
      resolutions[i].iScreenHeight,
      resolutions[i].dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "",
      resolutions[i].fRefreshRate);

    if(resDesktop.iScreenWidth == resolutions[i].iScreenWidth &&
       resDesktop.iScreenHeight == resolutions[i].iScreenHeight &&
       (resDesktop.dwFlags & D3DPRESENTFLAG_MODEMASK) == (resolutions[i].dwFlags & D3DPRESENTFLAG_MODEMASK) &&
       ResolutionRefreshRateEquals(resDesktop.fRefreshRate, resolutions[i].fRefreshRate))
    {
      ResDesktop = res_index;
    }

    res_index = (RESOLUTION)((int)res_index + 1);
  }

  // set RES_DESKTOP
  if (ResDesktop != RES_INVALID)
  {
    CLog::Log(LOGINFO, "Found ({:d}x{:d}{}@{:f}) at {:d}, setting to RES_DESKTOP at {:d}",
      resDesktop.iWidth, resDesktop.iHeight,
      resDesktop.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "",
      resDesktop.fRefreshRate,
      (int)ResDesktop, (int)RES_DESKTOP);

    CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP) = CDisplaySettings::GetInstance().GetResolutionInfo(ResDesktop);
  }
  else if (!resolutions.empty())
  {
    logM(LOGWARNING,
         "RES_DESKTOP unresolved: resDesktop={}x{}@{:f}Hz dwFlags={} resDesktop_set={:d} matched "
         "none of {} probed modes; RES_DESKTOP keeps probed mode 0 {}x{}@{:f}Hz strId={}",
         resDesktop.iScreenWidth, resDesktop.iScreenHeight, resDesktop.fRefreshRate,
         resDesktop.dwFlags, resDesktop_set, resolutions.size(), resolutions[0].iScreenWidth,
         resolutions[0].iScreenHeight, resolutions[0].fRefreshRate, resolutions[0].strId);
  }
}

bool CWinSystemAmlogic::IsHDRDisplay()
{
  CSysfsPath hdr_cap{"/sys/class/amhdmitx/amhdmitx0/hdr_cap"};
  CSysfsPath dv_cap{"/sys/class/amhdmitx/amhdmitx0/dv_cap"};
  std::string valstr;

  if (hdr_cap.Exists())
  {
    valstr = hdr_cap.Get<std::string>().value();
    if (valstr.find("Traditional HDR: 1") != std::string::npos)
      m_hdr_caps.SetHDR10();

    if (valstr.find("HDR10Plus Supported: 1") != std::string::npos)
      m_hdr_caps.SetHDR10Plus();

    if (valstr.find("Hybrid Log-Gamma: 1") != std::string::npos)
      m_hdr_caps.SetHLG();

    if (valstr.find("CUVA supported: 1") != std::string::npos)
      m_hdr_caps.SetHDRVivid();
  }

  if (dv_cap.Exists())
  {
    valstr = dv_cap.Get<std::string>().value();
    if (valstr.find("DolbyVision RX support list") != std::string::npos)
      m_hdr_caps.SetDolbyVision();
  }

  return (m_hdr_caps.SupportsHDR10() | m_hdr_caps.SupportsHDR10Plus() | m_hdr_caps.SupportsHLG());
}

CHDRCapabilities CWinSystemAmlogic::GetDisplayHDRCapabilities() const
{
  return m_hdr_caps;
}

float CWinSystemAmlogic::GetDisplayLatency()
{
  return 0.0f;
}

float CWinSystemAmlogic::GetGuiSdrPeakLuminance() const
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  const StreamHdrType hdrType =
      aml_get_output_hdr_type(CServiceBroker::GetWinSystem()->GetGfxContext().GetHDRType());

  if (hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION &&
      (!aml_dv_playback_active() || aml_dv_bdj_overlay_visible()))
  {
    const float osdMaxNits = static_cast<float>(std::clamp(aml_dv_osd_max_nits(), 50, 2000));
    return osdMaxNits / 100.0f;
  }

  std::string settingId;
  if (hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
    settingId = CSettings::SETTING_VIDEOSCREEN_GUIPEAKLUMINANCE_DOLBYVISION;
  else if (hdrType == StreamHdrType::HDR_TYPE_HLG)
    settingId = CSettings::SETTING_VIDEOSCREEN_GUIPEAKLUMINANCE_HLG;
  else
    settingId = CSettings::SETTING_VIDEOSCREEN_GUIPEAKLUMINANCE_HDR10;
  const int guiSdrPeak = std::clamp(settings->GetInt(settingId), 0, 100);

  if (hdrType == StreamHdrType::HDR_TYPE_HLG)
  {
    const float t = static_cast<float>(guiSdrPeak) / 100.0f;
    return std::clamp(0.02f + t * t * t * 1.18f, 0.02f, 1.20f);
  }

  // Map the 0-100 setting to a usable SDR white level in HDR mode.
  // The shader expects this value as "nits / 100".
  // Use an exponential curve but anchor the endpoints to a sensible range:
  //   0   -> 30 nits
  //   100 -> 500 nits
  constexpr float kMinNits = 30.0f;
  constexpr float kMaxNits = 1000.0f;
  const float exponent = std::log(kMaxNits / kMinNits) * (static_cast<float>(guiSdrPeak) / 100.0f);
  float sdrWhiteNits = std::clamp(kMinNits * std::exp(exponent), kMinNits, kMaxNits);

  // Shader expects "nits / 100".
  // Keep within the PQ domain (10,000 nits max) after any boost.
  sdrWhiteNits = std::clamp(sdrWhiteNits, 0.0f, 10000.0f);
  return sdrWhiteNits / 100.0f;
}

float CWinSystemAmlogic::GetGuiSdrSaturation() const
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  const StreamHdrType hdrType =
      aml_get_output_hdr_type(CServiceBroker::GetWinSystem()->GetGfxContext().GetHDRType());
  std::string settingId;
  if (hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
    settingId = CSettings::SETTING_VIDEOSCREEN_GUISATURATION_DOLBYVISION;
  else if (hdrType == StreamHdrType::HDR_TYPE_HLG)
    settingId = CSettings::SETTING_VIDEOSCREEN_GUISATURATION_HLG;
  else
    settingId = CSettings::SETTING_VIDEOSCREEN_GUISATURATION_HDR10;

  // UI is 0..100, where 50 is neutral. Map to shader saturation factor 0..2.
  const int satClamped = std::clamp(settings->GetInt(settingId), 0, 100);

  float saturation = static_cast<float>(satClamped) / 50.0f;

  return std::clamp(saturation, 0.0f, 2.0f);
}

bool CWinSystemAmlogic::GuiPqIsFinalStage() const
{
  return aml_gui_pq_is_final_stage();
}

float CWinSystemAmlogic::GetGuiSrgbDecode() const
{
  if (!GuiPqIsFinalStage())
    return 0.0f;

  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  return settings->GetBool(CSettings::SETTING_VIDEOSCREEN_GUISRGBTRANSFER) ? 1.0f : 0.0f;
}

float CWinSystemAmlogic::GetGuiDither8Bit() const
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  return settings->GetBool(CSettings::SETTING_VIDEOSCREEN_GUIDITHER8BIT) ? 1.0f : 0.0f;
}

bool CWinSystemAmlogic::Hide()
{
  return false;
}

bool CWinSystemAmlogic::Show(bool show)
{
  CSysfsPath("/sys/class/graphics/" + m_framebuffer_name + "/blank", (show ? 0 : 1));
  return true;
}

void CWinSystemAmlogic::Register(IDispResource *resource)
{
  std::lock_guard lock(m_resourceSection);

  m_resources.push_back(resource);
}

void CWinSystemAmlogic::Unregister(IDispResource *resource)
{
  std::lock_guard lock(m_resourceSection);

  auto i = find(m_resources.begin(), m_resources.end(), resource);
  if (i != m_resources.end())
    m_resources.erase(i);
}
