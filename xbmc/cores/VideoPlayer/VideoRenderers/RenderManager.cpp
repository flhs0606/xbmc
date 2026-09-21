/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RenderManager.h"

#include "FileItem.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlay.h"

/* to use the same as player */
#include "../VideoPlayer/DVDClock.h"
#include "RenderCapture.h"
#include "RenderFactory.h"
#include "RenderFlags.h"
#include "ServiceBroker.h"
#include "application/Application.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/StereoscopicsManager.h"
#include "guilib/WindowIDs.h"
#include "messaging/ApplicationMessenger.h"
#include "platform/linux/SysfsPath.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/ISettingCallback.h"
#include "threads/SingleLock.h"
#include "utils/AMLUtils.h"
#include "utils/StreamDetails.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"
#include "utils/LogThrottle.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

using namespace std::chrono_literals;

namespace
{
class CRenderSettingsCache : public ISettingCallback
{
public:
  static CRenderSettingsCache& Get()
  {
    static CRenderSettingsCache* cache = new CRenderSettingsCache();
    return *cache;
  }

  void Register()
  {
    if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
    {
      if (const auto settings = settingsComponent->GetSettings())
        settings->RegisterCallback(this,
                                   {CSettings::SETTING_SUBTITLES_RESTRICT_TO_ACTIVE_AREA,
                                    CSettings::SETTING_SUBTITLES_ALIGN,
                                    CSettings::SETTING_SUBTITLES_MARGINVERTICAL,
                                    CSettings::SETTING_SUBTITLES_DOLBYVISION_L5_SIGNAL_MODE,
                                    CSettings::SETTING_COREELEC_AMLOGIC_DV_STD_SOURCE_LEVEL_5,
                                    CSettings::SETTING_COREELEC_AMLOGIC_DV_STD_SOURCE_LEVEL_5_OSDST});
    }
    Refresh();
  }

  void OnSettingChanged(const std::shared_ptr<const CSetting>&) override { Refresh(); }

  std::atomic<bool> m_restrictToActiveArea{false};
  std::atomic<int> m_subtitleAlign{0};
  std::atomic<float> m_verticalMarginPerc{0.0f};
  std::atomic<int> m_dvL5SubsSignalMode{1};
  std::atomic<bool> m_dvStdL5{false};
  std::atomic<bool> m_dvStdL5Osdst{false};

private:
  CRenderSettingsCache() = default;

  void Refresh()
  {
    const auto settingsComponent = CServiceBroker::GetSettingsComponent();
    if (!settingsComponent)
      return;
    if (const auto subSettings = settingsComponent->GetSubtitlesSettings())
    {
      m_restrictToActiveArea.store(subSettings->GetRestrictToActiveArea(),
                                   std::memory_order_release);
      m_subtitleAlign.store(static_cast<int>(subSettings->GetAlignment()),
                            std::memory_order_release);
      m_verticalMarginPerc.store(subSettings->GetVerticalMarginPerc(), std::memory_order_release);
    }
    if (const auto settings = settingsComponent->GetSettings())
    {
      m_dvL5SubsSignalMode.store(
          settings->GetInt(CSettings::SETTING_SUBTITLES_DOLBYVISION_L5_SIGNAL_MODE),
          std::memory_order_release);
      m_dvStdL5.store(settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_STD_SOURCE_LEVEL_5),
                      std::memory_order_release);
      m_dvStdL5Osdst.store(
          settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_STD_SOURCE_LEVEL_5_OSDST),
          std::memory_order_release);
    }
  }
};
}

void CRenderManager::CClockSync::Reset()
{
  m_error = 0;
  m_ref = 0;
  m_refValid = false;
  m_errCount = 0;
  m_syncOffset.store(0.0, std::memory_order_relaxed);
  m_enabled = false;
}

unsigned int CRenderManager::m_nextCaptureId = 0;

CRenderManager::CRenderManager(CDVDClock &clock, IRenderMsg *player) :
  m_dvdClock(clock),
  m_playerPort(player),
  m_dataCacheCore(CServiceBroker::GetDataCacheCore())
{
  CRenderSettingsCache::Get().Register();
}

CRenderManager::~CRenderManager()
{
  delete m_pRenderer;
}

void CRenderManager::FillDoViActiveAreaMeta(DOVIFrameMetadata& meta)
{
  uint16_t t = 0, b = 0, l = 0, r = 0;
  meta.has_level5_metadata = m_dataCacheCore.GetVideoDoViActiveAreaRect(t, b, l, r);
  meta.level5_active_area_top_offset = t;
  meta.level5_active_area_bottom_offset = b;
  meta.level5_active_area_left_offset = l;
  meta.level5_active_area_right_offset = r;
}

void CRenderManager::SetVsyncAdjust(double adjustment)
{
  m_dvdClock.SetVsyncAdjust(adjustment);
}

void CRenderManager::GetVideoRect(CRect& source, CRect& dest, CRect& view) const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    m_pRenderer->GetVideoRect(source, dest, view);
}

float CRenderManager::GetAspectRatio() const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->GetAspectRatio();
  else
    return 1.0f;
}

void CRenderManager::SetVideoSettings(const CVideoSettings& settings) const {
  std::unique_lock lock(m_statelock);

  if (m_pRenderer)
  {
    m_pRenderer->SetVideoSettings(settings);
  }
}

bool CRenderManager::Configure(const VideoPicture& picture, float fps, unsigned int orientation,
  StreamHdrType hdrType, int buffers)
{

  // check if something has changed
  {
    std::unique_lock lock(m_statelock);

    if (!m_bRenderGUI)
      return true;

    if (m_picture.IsSameParams(picture) && m_fps == fps && m_orientation == orientation &&
        m_NumberBuffers == buffers && m_pRenderer != nullptr &&
        !m_pRenderer->ConfigChanged(picture))
    {
      return true;
    }

    // If only fps changed, update it without full reconfigure to avoid
    // destroying/recreating the renderer mid-playback. The display mode
    // switch (if needed) is handled by UpdateResolution() in FrameMove().
    if (m_pRenderer != nullptr && m_fps != fps &&
        m_picture.IsSameParams(picture) && m_orientation == orientation &&
        m_NumberBuffers == buffers && !m_pRenderer->ConfigChanged(picture))
    {
      bool sameScreen = true;
      if (fps > 0.0f &&
          CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
              CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) != ADJUST_REFRESHRATE_OFF)
      {
        CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
        const RESOLUTION_INFO curInfo = gfx.GetResInfo(gfx.GetVideoResolution());
        const RESOLUTION_INFO newInfo = gfx.GetResInfo(CResolutionUtils::ChooseBestResolution(
            fps, picture.iWidth, picture.iHeight, !picture.stereoMode.empty()));
        sameScreen = curInfo.iScreenWidth == newInfo.iScreenWidth &&
                     curInfo.iScreenHeight == newInfo.iScreenHeight &&
                     curInfo.fPixelRatio == newInfo.fPixelRatio;
      }
      if (sameScreen)
      {
        logComponentM(LOGDEBUG, LOGVIDEO,
                      "CRenderManager::Configure - fps change only ({:4.2f} -> {:4.2f}), "
                      "updating without full reconfigure",
                      m_fps, fps);
        m_fps = fps;
        m_triggerStereoNonEmpty = !picture.stereoMode.empty();
        m_bTriggerUpdateResolution = true;
        m_clockSync.Reset();
        SetVsyncAdjust(0);
        return true;
      }
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "CRenderManager::Configure - fps change ({:4.2f} -> {:4.2f}) moves display to a "
                    "different screen geometry, doing full reconfigure",
                    m_fps, fps);
    }
  }

  const std::string hdrStr = CStreamDetails::HdrTypeToString(picture.hdrType);
  CLog::Log(LOGDEBUG,
            "CRenderManager::Configure - change configuration. {}x{}. display: {}x{}. framerate: "
            "{:4.2f}. hdrType: {}.",
            picture.iWidth, picture.iHeight, picture.iDisplayWidth, picture.iDisplayHeight, fps,
            hdrStr.empty() ? "none" : hdrStr);

  // make sure any queued frame was fully presented
  {
    std::unique_lock lock(m_presentlock);
    XbmcThreads::EndTime<> endtime(5000ms);
    m_forceNext = true;
    while (m_presentstep != PRESENT_IDLE)
    {
      if (endtime.IsTimePast())
      {
        CLog::Log(LOGWARNING, "CRenderManager::Configure - timeout waiting for state");
        m_forceNext = false;
        return false;
      }
      WaitPresent(lock, endtime.GetTimeLeft());
    }
    m_forceNext = false;
  }

  {
    std::unique_lock lock(m_statelock);
    m_picture.SetParams(picture);
    m_fps = fps;
    m_orientation = orientation;
    m_NumberBuffers  = buffers;
    m_renderState = STATE_CONFIGURING;
    logComponentM(LOGDEBUG, LOGVIDEO, "RenderManager.state -> CONFIGURING fps={} buffers={}", fps, buffers);
    m_stateEvent.Reset();
    m_clockSync.Reset();
    SetVsyncAdjust(0);
    m_pConfigPicture = std::make_unique<VideoPicture>();
    m_pConfigPicture->CopyRef(picture);

    std::unique_lock lock2(m_presentlock);
    m_presentstep = PRESENT_READY;
    NotifyPresentWaiters();
  }

  // Waiting on m_stateEvent returns immediately once the render thread finishes configuring.
  // Keep this per-attempt timeout short; higher-level code can retry for a bounded time window
  // during slow display mode switches (refresh rate / HDR / DV / AVR handshakes).
  auto configureWaitTimeout = 1000ms;

  if (!m_stateEvent.Wait(configureWaitTimeout))
  {
    CLog::Log(LOGWARNING, "CRenderManager::Configure - timeout waiting for configure");
    std::unique_lock<CCriticalSection> lock(m_statelock);

    return false;
  }

  std::unique_lock lock(m_statelock);
  if (m_renderState != STATE_CONFIGURED)
  {
    CLog::Log(LOGWARNING, "CRenderManager::Configure - failed to configure");
    return false;
  }

  return true;
}

bool CRenderManager::Configure()
{
  // lock all interfaces
  std::unique_lock lock(m_statelock);
  std::unique_lock lock2(m_presentlock);
  std::unique_lock lock3(m_datalock);

  if (m_pRenderer)
  {
    DeleteRenderer();
  }

  if (!m_pRenderer)
  {
    CreateRenderer();
    if (!m_pRenderer)
      return false;
  }

  m_pRenderer->SetVideoSettings(m_playerPort->GetVideoSettings());
  bool result = m_pRenderer->Configure(*m_pConfigPicture, m_fps, m_orientation);
  if (result)
  {
    CRenderInfo info = m_pRenderer->GetRenderInfo();
    int renderbuffers = info.max_buffer_size;
    m_QueueSize = renderbuffers;
    if (m_NumberBuffers > 0)
      m_QueueSize = std::min(m_NumberBuffers, renderbuffers);

    if (m_QueueSize < 2)
    {
      m_QueueSize = 2;
      CLog::Log(LOGWARNING, "CRenderManager::Configure - queue size too small ({}, {}, {})",
                m_QueueSize, renderbuffers, m_NumberBuffers);
    }

    m_pRenderer->SetBufferSize(m_QueueSize);
    m_pRenderer->Update();

    m_playerPort->UpdateRenderInfo(info);
    m_playerPort->UpdateGuiRender(true);
    m_playerPort->UpdateVideoRender(!m_pRenderer->IsGuiLayer());

    m_queued.clear();
    m_discard.clear();
    m_free.clear();
    m_presentstarted = false;
    m_presentsource = 0;
    m_presentsourcePast = -1;
    for (int i = 0; i < m_QueueSize; i++)
      m_free.push_back(i);

    m_bRenderGUI = true;
    m_triggerStereoNonEmpty = !m_picture.stereoMode.empty();
    m_bTriggerUpdateResolution = true;
    m_presentstep = PRESENT_IDLE;
    m_presentpts = DVD_NOPTS_VALUE;
    m_lateframes = -1;
    NotifyPresentWaiters();
    m_renderedOverlay.store(false, std::memory_order_relaxed);
    m_renderDebug = false;
    m_clockSync.Reset();
    SetVsyncAdjust(0);
    m_overlays.Reset();
    m_overlays.SetStereoMode(m_picture.stereoMode);

    m_lastPushedActiveTopPx = -1;
    m_lastPushedActiveBottomPx = -1;
    m_lastActiveAreaTopPct = -1;
    m_lastActiveAreaBottomPct = -1;
    m_lastActiveAreaTopPx = -1;
    m_lastActiveAreaBottomPx = -1;
    m_contentAreaTopPx = 0;
    m_contentAreaBottomPx = 0;
    m_dataCacheCore.SetVideoActiveArea(0, 0, 0, 0);

    m_renderState = STATE_CONFIGURED;
    logComponentM(LOGDEBUG, LOGVIDEO, "RenderManager.state -> CONFIGURED");

    UpdateResolution(true);

    CLog::Log(LOGDEBUG, "CRenderManager::Configure - {}", m_QueueSize);
  }
  else
  {
    m_renderState = STATE_UNCONFIGURED;
    logComponentM(LOGDEBUG, LOGVIDEO, "RenderManager.state -> UNCONFIGURED (configure failed)");
  }

  m_pConfigPicture.reset();

  m_stateEvent.Set();
  m_playerPort->VideoParamsChange();
  return result;
}

bool CRenderManager::IsConfigured() const
{
  std::unique_lock lock(m_statelock);
  return m_renderState == STATE_CONFIGURED;
}

bool CRenderManager::HasFutureFrame(double clockPts) const
{
  std::unique_lock<CCriticalSection> lock(m_presentlock);
  return !m_queued.empty() && m_Queue[m_queued.back()].pts > clockPts;
}

void CRenderManager::ShowVideo(bool enable)
{
  m_showVideo = enable;
  if (!enable)
    DiscardBuffer();
  else if (m_asyncVideoWorkerActive.load(std::memory_order_relaxed))
    m_asyncFloorPaceEvent.Set();
}

void CRenderManager::PauseAsyncVideoLayerPoll()
{
  std::unique_lock lock(m_presentlock);
  m_asyncDiscardPause = true;
}

void CRenderManager::FrameWait(std::chrono::milliseconds duration)
{
  XbmcThreads::EndTime<> timeout{duration};
  std::unique_lock lock(m_presentlock);
  while(m_presentstep == PRESENT_IDLE && !timeout.IsTimePast())
    WaitPresent(lock, timeout.GetTimeLeft());
}

bool CRenderManager::IsPresenting()
{
  if (!IsConfigured())
    return false;

  std::unique_lock lock(m_presentlock);
  return !m_presentTimer.IsTimePast();
}

void CRenderManager::FrameMove()
{
  bool firstFrame = false;
  UpdateResolution();

  {
    std::unique_lock lock(m_statelock);

    if (m_renderState == STATE_UNCONFIGURED)
      return;
    else if (m_renderState == STATE_CONFIGURING)
    {
      if (!VideoWorkerParked())
        return;
      lock.unlock();
      if (!Configure())
        return;
      UpdateVideoLatencyTweak();
      firstFrame = true;
      FrameWait(50ms);
    }
  }

  if (!m_asyncVideoWorkerActive.load(std::memory_order_relaxed))
    VideoBody(firstFrame);
  else if (firstFrame)
    m_playerPort->UpdateGuiRender(true);

  ManageCaptures();

  bool presenting = false;
  {
    std::unique_lock lock(m_statelock);
    presenting = m_pRenderer && m_presentstarted && (m_renderState == STATE_CONFIGURED);
  }

  if (presenting)
  {
    UpdateActiveAreaInfo();
    UpdateDvOsdLift();
  }
}

void CRenderManager::VideoBody(bool firstFrame)
{
  {
    std::unique_lock lock(m_statelock);
    CheckEnableClockSync();
  }
  std::vector<int> overlaysToRelease;
  {
    std::unique_lock lock2(m_presentlock);

    if (m_queued.empty())
    {
      m_presentstep = PRESENT_IDLE;
    }
    else
    {
      m_presentTimer.Set(1000ms);
    }

    m_cadenceArmed = CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
                     CServiceBroker::GetLogging().CanLogComponent(LOGAVTIMING);
    if (m_cadenceArmed)
    {
      const int depth = static_cast<int>(m_queued.size());
      if (m_cadenceCalls == 0)
      {
        m_cadenceQueueMin = depth;
        m_cadenceQueueMax = depth;
      }
      else
      {
        m_cadenceQueueMin = std::min(m_cadenceQueueMin, depth);
        m_cadenceQueueMax = std::max(m_cadenceQueueMax, depth);
      }
      m_cadenceCalls++;
      if (depth == 0)
        m_cadenceEmpty++;
    }

    if (m_presentstep == PRESENT_READY)
      PrepareNextRender();

    if (m_cadenceArmed)
    {
      const auto cadenceNow = std::chrono::steady_clock::now();
      const double cadencePts = m_presentpts.load(std::memory_order_relaxed);
      if (m_cadenceStamp.time_since_epoch().count() == 0)
      {
        m_cadenceStamp = cadenceNow;
        m_cadenceSkipBase = m_QueueSkip;
        m_cadencePtsBase = cadencePts;
      }
      else if (cadenceNow - m_cadenceStamp >= std::chrono::seconds(1))
      {
        const double elapsed = std::chrono::duration<double>(cadenceNow - m_cadenceStamp).count();
        const int shown = m_cadenceAdvance + m_cadenceHalf;
        const int skipped = std::max(0, m_QueueSkip - m_cadenceSkipBase);
        const bool ptsUsable =
            cadencePts != DVD_NOPTS_VALUE && m_cadencePtsBase != DVD_NOPTS_VALUE;
        const double moved = (cadencePts - m_cadencePtsBase) / DVD_TIME_BASE;
        const double advanced =
            (ptsUsable && std::fabs(moved) <= 10.0 * elapsed) ? moved : -1.0 * elapsed;
        logComponentM(LOGDEBUG, LOGAVTIMING,
                      "mpegcadence: dec={} srcfps={:.3f} dispfps={:.3f} calls={} adv={} half={} "
                      "hold={} empty={} skip={} qmin={} qmax={} qinps={:.2f} shownps={:.2f} "
                      "mediarate={:.3f}",
                      m_dataCacheCore.GetVideoDecoderName(), m_fps,
                      CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS(), m_cadenceCalls,
                      m_cadenceAdvance, m_cadenceHalf, m_cadenceCalls - shown - m_cadenceEmpty,
                      m_cadenceEmpty, skipped, m_cadenceQueueMin, m_cadenceQueueMax,
                      m_cadenceQueueIn / elapsed, shown / elapsed, advanced / elapsed);
        m_cadenceStamp = cadenceNow;
        m_cadenceSkipBase = m_QueueSkip;
        m_cadencePtsBase = cadencePts;
        m_cadenceCalls = 0;
        m_cadenceAdvance = 0;
        m_cadenceHalf = 0;
        m_cadenceEmpty = 0;
        m_cadenceQueueIn = 0;
      }
    }

    if (m_presentstep == PRESENT_FLIP)
    {
      m_presentstep = PRESENT_FRAME;
      NotifyPresentWaiters();
    }

    // release all previous
    const int asyncIdx = m_asyncPinArmed.load(std::memory_order_relaxed)
                             ? m_pRenderer->GetAsyncRenderIndex()
                             : -1;
    for (std::deque<int>::iterator it = m_discard.begin(); it != m_discard.end(); )
    {
      // renderer may want to keep the frame for postprocessing
      if ((!m_pRenderer->NeedBuffer(*it) || !m_bRenderGUI) &&
          (!m_asyncPinArmed.load(std::memory_order_relaxed) || *it != asyncIdx))
      {
        m_pRenderer->ReleaseBuffer(*it);
        overlaysToRelease.push_back(*it);
        it = m_discard.erase(it);
      }
      else
        ++it;
    }

    m_playerPort->UpdateRenderBuffers(m_queued.size(), m_discard.size(),
                                      m_free.size() + overlaysToRelease.size());
    m_bRenderGUI = true;
  }

  for (int idx : overlaysToRelease)
    m_overlays.Release(idx);

  if (!overlaysToRelease.empty())
  {
    std::unique_lock lock2(m_presentlock);
    for (int idx : overlaysToRelease)
      m_free.push_back(idx);
  }

  m_playerPort->UpdateGuiRender(IsGuiLayer() || firstFrame);
}

void CRenderManager::GetGuiVideoRect(CRect& source, CRect& dest, CRect& view)
{
  if (m_asyncVideoWorkerActive.load(std::memory_order_relaxed))
  {
    std::unique_lock lock(m_presentlock);
    source = m_asyncMirror.srcRect;
    dest = m_asyncMirror.dstRect;
    view = m_asyncMirror.viewRect;
    return;
  }
  m_pRenderer->GetVideoRect(source, dest, view);
}

void CRenderManager::PublishAsyncPresentMirror(int source, double pts)
{
  std::unique_lock lock(m_presentlock);
  m_asyncMirror.presentsource = source;
  m_asyncMirror.pts = pts;
}

void CRenderManager::PublishAsyncMirrorRects(const CRect& source, const CRect& dest, const CRect& view)
{
  std::unique_lock lock(m_presentlock);
  m_asyncMirror.srcRect = source;
  m_asyncMirror.dstRect = dest;
  m_asyncMirror.viewRect = view;
}

bool CRenderManager::VideoWorkerParked()
{
  return !m_asyncVideoWorkerActive.load(std::memory_order_relaxed) ||
         m_asyncWorkerParked.load(std::memory_order_relaxed);
}

bool CRenderManager::ParkVideoWorker(std::chrono::milliseconds timeout)
{
  {
    std::unique_lock lock(m_statelock);
    if (!m_asyncVideoWorkerActive.load(std::memory_order_relaxed))
      return true;
  }
  m_asyncParkRequest.store(true, std::memory_order_relaxed);
  m_asyncFloorPaceEvent.Set();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  CSingleExit exit(CServiceBroker::GetWinSystem()->GetGfxContext());
  while (true)
  {
    {
      std::unique_lock lock(m_statelock);
      if (!m_asyncVideoWorkerActive.load(std::memory_order_relaxed) ||
          m_asyncWorkerParked.load(std::memory_order_relaxed))
        return true;
    }
    if (std::chrono::steady_clock::now() >= deadline)
      break;
    m_asyncParkedEvent.Wait(50ms);
  }
  logM(LOGERROR, "async video worker failed to park within {} ms", timeout.count());
  return false;
}

void CRenderManager::SetAsyncVideoWorkerActive(bool active)
{
  std::unique_lock lock(m_statelock);
  if (active)
  {
    m_asyncWorkerStop.store(false, std::memory_order_relaxed);
    m_asyncParkRequest.store(false, std::memory_order_relaxed);
    m_asyncWorkerParked.store(false, std::memory_order_relaxed);
    m_asyncParkedEvent.Reset();
    m_asyncResumeEvent.Reset();
    m_asyncFloorPaceEvent.Reset();
    m_asyncRefilling.store(true, std::memory_order_relaxed);
    {
      std::unique_lock lock2(m_presentlock);
      m_asyncDiscardPause = false;
      m_asyncMirror.presentsource = m_presentsource;
      m_asyncMirror.pts = m_Queue[m_presentsource].pts;
    }
    if (m_pRenderer)
    {
      CRect src, dst, view;
      m_pRenderer->GetVideoRect(src, dst, view);
      PublishAsyncMirrorRects(src, dst, view);
    }
  }
  m_asyncPinArmed.store(active, std::memory_order_relaxed);
  m_asyncVideoWorkerActive.store(active, std::memory_order_relaxed);
}

void CRenderManager::RequestAsyncVideoWorkerStop()
{
  m_asyncWorkerStop.store(true, std::memory_order_relaxed);
  m_asyncResumeEvent.Set();
  m_asyncFloorPaceEvent.Set();
  m_asyncMainPaceEvent.Set();
}

uint64_t CRenderManager::GetVisibleOverlaySetSignature(bool& animated) const
{
  int guiSource;
  if (m_asyncVideoWorkerActive.load(std::memory_order_relaxed))
  {
    std::unique_lock<CCriticalSection> lockMirror(m_presentlock);
    guiSource = m_asyncMirror.presentsource;
  }
  else
  {
    guiSource = m_presentsource;
  }

  const uint64_t signature = m_overlays.GetOverlaySetSignature(guiSource, animated);
  if (m_renderDebug.load(std::memory_order_relaxed))
    animated = true;
  return signature;
}

void CRenderManager::WaitAsyncMainPace()
{
  static uint32_t s_loops = 0, s_imm = 0, s_waits = 0, s_caps = 0;
  static auto s_statWindow = std::chrono::steady_clock::now();

  if (!m_asyncVideoWorkerActive.load(std::memory_order_relaxed) ||
      m_asyncWorkerParked.load(std::memory_order_relaxed))
    return;

  ++s_loops;
  if (m_asyncMainPaceEvent.Wait(0ms))
    ++s_imm;
  else if (m_asyncMainPaceEvent.Wait(20ms))
    ++s_waits;
  else
    ++s_caps;

  const auto now = std::chrono::steady_clock::now();
  if (now - s_statWindow >= std::chrono::seconds(1))
  {
    logComponentM(LOGDEBUG, LOGVIDEO, "mainpace: loops={} imm={} waits={} caps={} sets={}",
                  s_loops, s_imm, s_waits, s_caps,
                  m_asyncMainPaceSets.exchange(0, std::memory_order_relaxed));
    s_statWindow = now;
    s_loops = s_imm = s_waits = s_caps = 0;
  }
}

bool CRenderManager::CanRunAsyncVideoWorker()
{
  std::unique_lock lock(m_statelock);
  return m_renderState == STATE_CONFIGURED && m_pRenderer &&
         m_pRenderer->SupportsAsyncVideoLayerRender();
}

bool CRenderManager::AsyncVideoWorkerIteration()
{
  static auto s_statWindow = std::chrono::steady_clock::time_point{};
  static int s_loops = 0;
  static int s_fsel = 0;
  static int s_commits = 0;
  static int s_parks = 0;
  static int s_floorWaits = 0;
  static int s_starveWaits = 0;
  static int s_hb = 0;
  static int64_t s_maxWorkUs = 0;
  static int s_overBudget = 0;

  if (m_asyncWorkerStop.load(std::memory_order_relaxed))
    return false;

  ++s_loops;
  const auto iterStart = std::chrono::steady_clock::now();

  bool needPark = false;
  {
    std::unique_lock lock(m_statelock);
    needPark = (m_renderState != STATE_CONFIGURED) ||
               m_asyncParkRequest.load(std::memory_order_relaxed) || !m_pRenderer ||
               !m_pRenderer->SupportsAsyncVideoLayerRender();
  }
  if (needPark)
  {
    ++s_parks;
    m_asyncWorkerParked.store(true, std::memory_order_relaxed);
    m_asyncParkedEvent.Set();
    m_asyncMainPaceEvent.Set();
    while (!m_asyncWorkerStop.load(std::memory_order_relaxed))
    {
      m_asyncResumeEvent.Wait(250ms);
      if (m_asyncWorkerStop.load(std::memory_order_relaxed))
        break;
      std::unique_lock lock(m_statelock);
      if (m_renderState == STATE_CONFIGURED &&
          !m_asyncParkRequest.load(std::memory_order_relaxed) && m_pRenderer &&
          m_pRenderer->SupportsAsyncVideoLayerRender())
        break;
    }
    m_asyncWorkerParked.store(false, std::memory_order_relaxed);
    return !m_asyncWorkerStop.load(std::memory_order_relaxed);
  }

  VideoBody(false);

  bool haveFrame = false;
  bool presentStarted = false;
  bool discardPause = false;
  int source = -1;
  double queuePts = 0.0;
  {
    std::unique_lock lock(m_presentlock);
    presentStarted = m_presentstarted;
    if (m_presentstep == PRESENT_FRAME || m_presentstep == PRESENT_FRAME2)
    {
      haveFrame = true;
      source = m_presentsource;
      queuePts = m_Queue[source].pts;
      m_pRenderer->BeginAsyncVideoLayerRender(source);
      const SPresent& present = m_Queue[m_presentsource];
      if (m_presentstep == PRESENT_FRAME)
      {
        if (present.presentmethod == PRESENT_METHOD_BOB)
          m_presentstep = PRESENT_FRAME2;
        else
          m_presentstep = PRESENT_IDLE;
      }
      else if (m_presentstep == PRESENT_FRAME2)
        m_presentstep = PRESENT_IDLE;
      if (m_presentstep == PRESENT_IDLE)
      {
        if (!m_queued.empty())
          m_presentstep = PRESENT_READY;
      }
      NotifyPresentWaiters();
      PublishAsyncPresentMirror(source, queuePts);
    }
    discardPause = m_asyncDiscardPause;
    m_asyncRefilling.store(!haveFrame, std::memory_order_relaxed);
  }
  m_pRenderer->PrepareVideoLayer();
  CRect src, dst, view;
  m_pRenderer->GetVideoRect(src, dst, view);
  PublishAsyncMirrorRects(src, dst, view);

  if (haveFrame)
  {
    ++s_fsel;
    m_pRenderer->RenderVideoLayerCommit(source, src, dst);
    ++s_commits;
    m_pRenderer->EndAsyncVideoLayerRender(source);
    m_dataCacheCore.SetRenderPts(m_presentpts -
                                 m_displayLatency.load(std::memory_order_relaxed));
    m_dataCacheCore.SetVideoDoViLookupPts(queuePts);
  }

  const bool parkPending = m_asyncParkRequest.load(std::memory_order_relaxed) ||
                           m_asyncWorkerStop.load(std::memory_order_relaxed) ||
                           m_renderState.load(std::memory_order_relaxed) != STATE_CONFIGURED;
  if (!parkPending && presentStarted && !discardPause)
  {
    const int polled = m_pRenderer->PollVideoLayer();
    s_hb += polled;
    const float fps = (m_fps > 0.0f && m_fps <= 240.0f) ? m_fps : 25.0f;
    const auto floorCap = std::chrono::milliseconds(
        std::clamp(static_cast<int>(500.0f / fps), 2, 8));
    const auto workDur = std::chrono::steady_clock::now() - iterStart;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(workDur);
    const int64_t workUs = std::chrono::duration_cast<std::chrono::microseconds>(workDur).count();
    if (workUs > s_maxWorkUs)
      s_maxWorkUs = workUs;
    if (static_cast<float>(workUs) > 1500.0f / fps * 1000.0f)
      ++s_overBudget;
    if (elapsed < floorCap)
    {
      ++s_floorWaits;
      m_asyncFloorPaceEvent.Wait(floorCap - elapsed);
    }
  }
  else if (!parkPending)
  {
    ++s_starveWaits;
    m_asyncFloorPaceEvent.Wait(50ms);
  }

  m_asyncMainPaceEvent.Set();
  m_asyncMainPaceSets.fetch_add(1, std::memory_order_relaxed);

  const auto now = std::chrono::steady_clock::now();
  if (now - s_statWindow >= std::chrono::seconds(1))
  {
    logComponentM(LOGDEBUG, LOGVIDEO,
                  "asyncvid: loops={} fsel={} commits={} parks={} floorWaits={} starve={} "
                  "maxWorkMs={:.1f} overBudget={}",
                  s_loops, s_fsel, s_commits, s_parks, s_floorWaits, s_starveWaits,
                  s_maxWorkUs / 1000.0f, s_overBudget);
    logComponentM(LOGDEBUG, LOGAVTIMING,
                  "asyncvidclk: hb={} vadjUs={:.0f} qskip={} late={} starve={}", s_hb,
                  m_dvdClock.GetVsyncAdjust(), m_QueueSkip.load(), m_lateframes.load(),
                  s_starveWaits);
    s_statWindow = now;
    s_loops = s_fsel = s_commits = s_parks = s_floorWaits = s_starveWaits = s_hb = 0;
    s_maxWorkUs = 0;
    s_overBudget = 0;
  }

  return true;
}

void CRenderManager::ResumeVideoWorker()
{
  {
    std::unique_lock lock(m_statelock);
    if (!m_asyncVideoWorkerActive.load(std::memory_order_relaxed))
      return;
  }
  m_asyncParkRequest = false;
  m_asyncResumeEvent.Set();
}

void CRenderManager::PreInit()
{
  {
    std::unique_lock lock(m_statelock);
    if (m_renderState != STATE_UNCONFIGURED)
      return;
  }

  if (!CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    m_initEvent.Reset();
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_RENDERER_PREINIT);
    if (!m_initEvent.Wait(2000ms))
    {
      CLog::Log(LOGERROR, "{} - timed out waiting for renderer to preinit", __FUNCTION__);
    }
  }

  std::unique_lock lock(m_statelock);

  if (!m_pRenderer)
  {
    CreateRenderer();
  }

  m_debugRenderer.Initialize();

  UpdateVideoLatencyTweak();

  m_QueueSize   = 2;
  m_QueueSkip   = 0;
  m_presentstep = PRESENT_IDLE;
  m_bRenderGUI = true;

  m_initEvent.Set();
}

void CRenderManager::UnInit()
{
  if (!CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    m_initEvent.Reset();
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_RENDERER_UNINIT);
    if (!m_initEvent.Wait(2000ms))
    {
      CLog::Log(LOGERROR, "{} - timed out waiting for renderer to uninit", __FUNCTION__);
    }
  }

  if (m_asyncVideoWorkerActive.load(std::memory_order_relaxed))
    ParkVideoWorker(2000ms);

  std::unique_lock lock(m_statelock);

  if (m_asyncVideoWorkerActive.load(std::memory_order_relaxed))
    logM(LOGERROR, "UnInit entered with the async video worker still active");

  m_overlays.UnInit();
  m_debugRenderer.Dispose();

  DeleteRenderer();

  m_renderState = STATE_UNCONFIGURED;
  logComponentM(LOGDEBUG, LOGVIDEO, "RenderManager.state -> UNCONFIGURED (UnInit)");
  m_picture.Reset();
  m_bRenderGUI = false;
  CServiceBroker::GetWinSystem()->GetGfxContext().SetHDRType(m_picture.hdrType);
  RemoveCaptures();

  m_initEvent.Set();
}

bool CRenderManager::Flush(bool wait, bool saveBuffers)
{
  if (!m_pRenderer)
    return true;

  if (CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    CLog::Log(LOGDEBUG, "{} - flushing renderer", __FUNCTION__);

// fix deadlock on Windows only when is enabled 'Sync playback to display'
#ifndef TARGET_WINDOWS
    CSingleExit exitlock(CServiceBroker::GetWinSystem()->GetGfxContext());
#endif

    ParkVideoWorker(800ms);

    std::unique_lock lock(m_statelock);
    std::unique_lock lock2(m_presentlock);
    std::unique_lock lock3(m_datalock);

    if (m_pRenderer)
    {
      m_overlays.Flush();
      m_debugRenderer.Flush();

      if (!m_pRenderer->Flush(saveBuffers))
      {
        m_queued.clear();
        m_discard.clear();
        m_free.clear();
        m_presentstarted = false;
        m_presentsource = 0;
        m_presentsourcePast = -1;
        m_presentstep = PRESENT_IDLE;
        for (int i = 0; i < m_QueueSize; i++)
          m_free.push_back(i);
      }

      m_flushEvent.Set();
    }

    ResumeVideoWorker();
  }
  else
  {
    m_flushEvent.Reset();
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_RENDERER_FLUSH);
    if (wait)
    {
      if (!m_flushEvent.Wait(1000ms))
      {
        CLog::Log(LOGERROR, "{} - timed out waiting for renderer to flush", __FUNCTION__);
        return false;
      }
      else
        return true;
    }
  }
  return true;
}

void CRenderManager::CreateRenderer()
{
  if (!m_pRenderer)
  {
    CVideoBuffer *buffer = nullptr;
    if (m_pConfigPicture)
      buffer = m_pConfigPicture->videoBuffer;

    auto renderers = VIDEOPLAYER::CRendererFactory::GetRenderers();
    for (auto &id : renderers)
    {
      if (id == "default")
        continue;

      m_pRenderer = VIDEOPLAYER::CRendererFactory::CreateRenderer(id, buffer);
      if (m_pRenderer)
      {
        return;
      }
    }
    m_pRenderer = VIDEOPLAYER::CRendererFactory::CreateRenderer("default", buffer);
  }
}

void CRenderManager::DeleteRenderer()
{
  if (m_pRenderer)
  {
    CLog::Log(LOGDEBUG, "{} - deleting renderer", __FUNCTION__);

    delete m_pRenderer;
    m_pRenderer = nullptr;
  }
}

unsigned int CRenderManager::AllocRenderCapture()
{
  std::unique_lock lock(m_captCritSect);

  if (m_pRenderer)
  {
    CRenderCapture* capture = m_pRenderer->GetRenderCapture();
    if (capture)
    {
      m_captures[m_nextCaptureId] = capture;
      return m_nextCaptureId++;
    }
  }

  return m_nextCaptureId;
}

void CRenderManager::ReleaseRenderCapture(unsigned int captureId)
{
  std::unique_lock lock(m_captCritSect);

  std::map<unsigned int, CRenderCapture*>::iterator it;
  it = m_captures.find(captureId);

  if (it != m_captures.end())
    it->second->SetState(CAPTURESTATE_NEEDSDELETE);
}

void CRenderManager::StartRenderCapture(unsigned int captureId, unsigned int width, unsigned int height, int flags)
{
  std::unique_lock lock(m_captCritSect);

  std::map<unsigned int, CRenderCapture*>::iterator it;
  it = m_captures.find(captureId);
  if (it == m_captures.end())
  {
    CLog::Log(LOGERROR, "CRenderManager::Capture - unknown capture id: {}", captureId);
    return;
  }

  CRenderCapture *capture = it->second;

  capture->SetState(CAPTURESTATE_NEEDSRENDER);
  capture->SetUserState(CAPTURESTATE_WORKING);
  capture->SetWidth(width);
  capture->SetHeight(height);
  capture->SetFlags(flags);
  capture->GetEvent().Reset();

  if (CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    if (flags & CAPTUREFLAG_IMMEDIATELY)
    {
      // render capture and read out immediately
      RenderCapture(capture);
      capture->SetUserState(capture->GetState());
      capture->GetEvent().Set();
    }
  }

  if (!m_captures.empty())
    m_hasCaptures = true;
}

bool CRenderManager::RenderCaptureGetPixels(unsigned int captureId, unsigned int millis, uint8_t *buffer, unsigned int size)
{
  std::unique_lock lock(m_captCritSect);

  std::map<unsigned int, CRenderCapture*>::iterator it;
  it = m_captures.find(captureId);
  if (it == m_captures.end())
    return false;

  m_captureWaitCounter++;

  {
    if (!millis)
      millis = 1000;

    CSingleExit exitlock(m_captCritSect);
    if (!it->second->GetEvent().Wait(std::chrono::milliseconds(millis)))
    {
      m_captureWaitCounter--;
      return false;
    }
  }

  m_captureWaitCounter--;

  if (it->second->GetUserState() != CAPTURESTATE_DONE)
    return false;

  unsigned int srcSize = it->second->GetWidth() * it->second->GetHeight() * 4;
  unsigned int bytes = std::min(srcSize, size);

  memcpy(buffer, it->second->GetPixels(), bytes);
  return true;
}

void CRenderManager::ManageCaptures()
{
  // no captures, return here so we don't do an unnecessary lock
  if (!m_hasCaptures)
    return;

  std::unique_lock lock(m_captCritSect);

  std::map<unsigned int, CRenderCapture*>::iterator it = m_captures.begin();
  while (it != m_captures.end())
  {
    CRenderCapture* capture = it->second;

    if (capture->GetState() == CAPTURESTATE_NEEDSDELETE)
    {
      delete capture;
      it = m_captures.erase(it);
      continue;
    }

    if (capture->GetState() == CAPTURESTATE_NEEDSRENDER)
      RenderCapture(capture);
    else if (capture->GetState() == CAPTURESTATE_NEEDSREADOUT)
      capture->ReadOut();

    if (capture->GetState() == CAPTURESTATE_DONE || capture->GetState() == CAPTURESTATE_FAILED)
    {
      // tell the thread that the capture is done or has failed
      capture->SetUserState(capture->GetState());
      capture->GetEvent().Set();

      if (capture->GetFlags() & CAPTUREFLAG_CONTINUOUS)
      {
        capture->SetState(CAPTURESTATE_NEEDSRENDER);

        // if rendering this capture continuously, and readout is async, render a new capture immediately
        if (capture->IsAsync() && !(capture->GetFlags() & CAPTUREFLAG_IMMEDIATELY))
          RenderCapture(capture);
      }
      ++it;
    }
    else
    {
      ++it;
    }
  }

  if (m_captures.empty())
    m_hasCaptures = false;
}

void CRenderManager::RenderCapture(CRenderCapture* capture) const {
  if (!m_pRenderer || !m_pRenderer->RenderCapture(m_presentsource, capture))
    capture->SetState(CAPTURESTATE_FAILED);
}

void CRenderManager::RemoveCaptures()
{
  std::unique_lock lock(m_captCritSect);

  while (m_captureWaitCounter > 0)
  {
    for (auto entry : m_captures)
    {
      entry.second->GetEvent().Set();
    }
    CSingleExit lockexit(m_captCritSect);
    KODI::TIME::Sleep(10ms);
  }

  for (auto entry : m_captures)
  {
    delete entry.second;
  }
  m_captures.clear();
}

void CRenderManager::SetViewMode(int iViewMode) const {
  std::unique_lock<CCriticalSection> lock(m_statelock);

  if (m_pRenderer)
    m_pRenderer->SetViewMode(iViewMode);
  m_playerPort->VideoParamsChange();
}

RESOLUTION CRenderManager::GetResolution() const {
  RESOLUTION res = CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution();

  std::unique_lock<CCriticalSection> lock(m_statelock);

  if (m_renderState == STATE_UNCONFIGURED)
    return res;

  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) != ADJUST_REFRESHRATE_OFF)
    res = CResolutionUtils::ChooseBestResolution(m_fps, m_picture.iWidth, m_picture.iHeight,
                                                 !m_picture.stereoMode.empty());

  return res;
}

void CRenderManager::UpdateActiveAreaInfo()
{
  if (!m_pRenderer)
    return;

  CRect src, dst, view;
  GetGuiVideoRect(src, dst, view);
  int frameW = static_cast<int>(src.Width());
  int frameH = static_cast<int>(src.Height());
  if (frameH <= 0)
    return;

  const auto nowDims = std::chrono::steady_clock::now();
  if (nowDims - m_kernelFrameDimsRead >= std::chrono::seconds(1))
  {
    m_kernelFrameDimsRead = nowDims;
    m_kernelFrameWidth = CSysfsPath("/sys/class/video/frame_width").GetOrDefault<int>();
    m_kernelFrameHeight = CSysfsPath("/sys/class/video/frame_height").GetOrDefault<int>();
  }

  const bool kernelDimsUsable =
      m_kernelFrameWidth > 0 && m_kernelFrameHeight > 0 &&
      m_kernelFrameHeight * 100 >= frameH * 85 && m_kernelFrameHeight * 100 <= frameH * 115 &&
      m_kernelFrameWidth * 100 >= frameW * 85 && m_kernelFrameWidth * 100 <= frameW * 115;
  if (kernelDimsUsable)
  {
    frameW = m_kernelFrameWidth;
    frameH = m_kernelFrameHeight;
  }

  {
    const uint64_t dimsKey =
        (static_cast<uint64_t>(kernelDimsUsable ? 1 : 0) << 63) |
        (static_cast<uint64_t>(static_cast<uint16_t>(m_kernelFrameHeight)) << 32) |
        static_cast<uint64_t>(static_cast<uint32_t>(frameH));
    if (dimsKey != m_lastKernelDimsKey)
    {
      m_lastKernelDimsKey = dimsKey;
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "active-area frame dims: src {}x{} kernel {}x{} using {}x{}",
                    static_cast<int>(src.Width()), static_cast<int>(src.Height()),
                    m_kernelFrameWidth, m_kernelFrameHeight, frameW, frameH);
    }
  }

  const bool narrowContainer = frameW * 10 > frameH * 19;
  if (narrowContainer)
  {
    m_contentAreaTopPx = 0;
    m_contentAreaBottomPx = 0;
  }
  else
  {
    uint16_t l5Top = 0;
    uint16_t l5Bottom = 0;
    if (m_picture.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION &&
        m_dataCacheCore.GetVideoDoViActiveArea(l5Top, l5Bottom) && (l5Top > 0 || l5Bottom > 0))
    {
      m_contentAreaTopPx = static_cast<int>(l5Top);
      m_contentAreaBottomPx = static_cast<int>(l5Bottom);
    }
    else
    {
      m_contentAreaTopPx = 0;
      m_contentAreaBottomPx = 0;
    }
  }
  int topPx = m_contentAreaTopPx;
  int botPx = m_contentAreaBottomPx;

  int geoTopPx = 0;
  int geoBotPx = 0;
  if (dst.Height() > 0.0f)
  {
    const float toSrc = frameH / dst.Height();
    geoTopPx = static_cast<int>(std::max(0.0f, dst.y1 - view.y1) * toSrc + 0.5f);
    geoBotPx = static_cast<int>(std::max(0.0f, view.y2 - dst.y2) * toSrc + 0.5f);
  }
  topPx += geoTopPx;
  botPx += geoBotPx;
  const int totalH = frameH + geoTopPx + geoBotPx;

  const int topPct = static_cast<int>((static_cast<int64_t>(topPx) * 100 + totalH / 2) / totalH);
  const int botPct = static_cast<int>((static_cast<int64_t>(botPx) * 100 + totalH / 2) / totalH);
  if (topPct != m_lastActiveAreaTopPct || botPct != m_lastActiveAreaBottomPct ||
      topPx != m_lastActiveAreaTopPx || botPx != m_lastActiveAreaBottomPx)
  {
    m_lastActiveAreaTopPct = topPct;
    m_lastActiveAreaBottomPct = botPct;
    m_lastActiveAreaTopPx = topPx;
    m_lastActiveAreaBottomPx = botPx;
    m_dataCacheCore.SetVideoActiveArea(topPct, botPct, topPx, botPx);
  }
}

void CRenderManager::UpdateDvOsdLift()
{
  bool osdInBars = false;
  bool videoMenu = false;
  bool avChangeExt = false;
  bool guiInFront = false;
  if (m_picture.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION &&
      CRenderSettingsCache::Get().m_dvStdL5.load(std::memory_order_acquire) &&
      CRenderSettingsCache::Get().m_dvStdL5Osdst.load(std::memory_order_acquire))
  {
    auto& wmgr = CServiceBroker::GetGUI()->GetWindowManager();
    videoMenu = wmgr.IsWindowVisible(WINDOW_VIDEO_MENU);
    guiInFront = wmgr.GetActiveWindow() != WINDOW_FULLSCREEN_VIDEO;
    avChangeExt = CServiceBroker::GetDataCacheCore().GetAVChangeExtended();
    DOVIFrameMetadata doviFrameMeta{};
    FillDoViActiveAreaMeta(doviFrameMeta);
    if (doviFrameMeta.has_level5_metadata)
    {
      CRect osdSrc, osdDst, osdView;
      GetGuiVideoRect(osdSrc, osdDst, osdView);
      if (osdSrc.Height() > 0 && osdSrc.Width() > 0)
      {
        const float osdScaleV = osdDst.Height() / osdSrc.Height();
        const float osdScaleH = osdDst.Width() / osdSrc.Width();
        const float topEdge = osdDst.y1 + doviFrameMeta.level5_active_area_top_offset * osdScaleV;
        const float botEdge =
            osdDst.y2 - doviFrameMeta.level5_active_area_bottom_offset * osdScaleV;
        const float leftEdge = osdDst.x1 + doviFrameMeta.level5_active_area_left_offset * osdScaleH;
        const float rightEdge =
            osdDst.x2 - doviFrameMeta.level5_active_area_right_offset * osdScaleH;
        m_l5Bars.clear();
        if (topEdge > osdView.y1)
          m_l5Bars.emplace_back(osdView.x1, osdView.y1, osdView.x2, topEdge);
        if (botEdge < osdView.y2)
          m_l5Bars.emplace_back(osdView.x1, botEdge, osdView.x2, osdView.y2);
        if (leftEdge > osdView.x1)
          m_l5Bars.emplace_back(osdView.x1, osdView.y1, leftEdge, osdView.y2);
        if (rightEdge < osdView.x2)
          m_l5Bars.emplace_back(rightEdge, osdView.y1, osdView.x2, osdView.y2);
        if (!m_l5Bars.empty())
          osdInBars = wmgr.HasVisibleDialogContentInRegions(m_l5Bars);
      }
    }
    else
    {
      CRect osdSrc, osdDst, osdView;
      GetGuiVideoRect(osdSrc, osdDst, osdView);
      if (osdView.Height() > 0)
        osdInBars = wmgr.HasVisibleDialogContentInRegions({osdView});
    }

    const int osdKey =
        (osdInBars ? 1 : 0) | (videoMenu ? 2 : 0) | (avChangeExt ? 4 : 0) | (guiInFront ? 8 : 0);
    if (osdKey != m_lastOsdKey)
    {
      m_lastOsdKey = osdKey;
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "L5 OSD decision osdInBars={} videoMenu={} avChangeExt={} guiInFront={} "
                    "hasVisDialog={} topDialog={}",
                    osdInBars, videoMenu, avChangeExt, guiInFront, wmgr.HasVisibleDialog(),
                    wmgr.GetTopmostDialog());
    }
  }
  aml_dv_set_xbmc_osd(osdInBars || videoMenu || avChangeExt || guiInFront);
}

void CRenderManager::Render(bool clear, DWORD flags, DWORD alpha, bool gui)
{
  CSingleExit exitLock(CServiceBroker::GetWinSystem()->GetGfxContext());

  {
    std::unique_lock<CCriticalSection> lock(m_statelock);

    if (!m_presentstarted || (m_renderState != STATE_CONFIGURED))
    {
      static int s_lastSkipState = -1;
      static bool s_lastSkipPresent = false;
      const int curState = static_cast<int>(m_renderState);
      if (curState != s_lastSkipState || m_presentstarted != s_lastSkipPresent)
      {
        logComponentM(LOGDEBUG, LOGVIDEO,
                      "RenderManager.Render skipped: presentStarted={} state={} gui={}",
                      m_presentstarted, curState, gui);
        s_lastSkipState = curState;
        s_lastSkipPresent = m_presentstarted;
      }
      return;
    }
  }

  if (!gui && m_pRenderer->IsGuiLayer())
    return;

  const SPresent& present = m_Queue[m_presentsource];

  if (!gui || m_pRenderer->IsGuiLayer())
  {
    if (present.presentmethod == PRESENT_METHOD_BOB)
      PresentFields(clear, flags, alpha);
    else if (present.presentmethod == PRESENT_METHOD_BLEND)
      PresentBlend(clear, flags, alpha);
    else
      PresentSingle(clear, flags, alpha);
  }

  if (gui)
  {
    if (!m_pRenderer->IsGuiLayer())
      m_pRenderer->Update();

    int guiSource;
    double guiPts;
    if (m_asyncVideoWorkerActive.load(std::memory_order_relaxed))
    {
      std::unique_lock lockMirror(m_presentlock);
      guiSource = m_asyncMirror.presentsource;
      guiPts = m_asyncMirror.pts;
    }
    else
    {
      guiSource = m_presentsource;
      guiPts = present.pts;
    }

    const bool hasOverlay = m_overlays.HasOverlay(guiSource);
    m_renderedOverlay.store(hasOverlay, std::memory_order_relaxed);
    {
      auto curOverlayCount = m_overlays.GetOverlayCount(guiSource);
      static bool s_lastHasOverlay = false;
      static decltype(curOverlayCount) s_lastOverlayCount = static_cast<decltype(curOverlayCount)>(-1);
      if (hasOverlay != s_lastHasOverlay || curOverlayCount != s_lastOverlayCount)
      {
        logComponentM(LOGDEBUG, LOGVIDEO,
                      "RenderManager.Render gui=true presentSource={} hasOverlay={} overlayCount={}",
                      guiSource, hasOverlay, curOverlayCount);
        s_lastHasOverlay = hasOverlay;
        s_lastOverlayCount = curOverlayCount;
      }
    }

    const bool restrictSubsOn = CRenderSettingsCache::Get().m_restrictToActiveArea.load(std::memory_order_acquire);
    const auto subAlign =
        static_cast<KODI::SUBTITLES::Align>(CRenderSettingsCache::Get().m_subtitleAlign.load(std::memory_order_acquire));
    const int pgsMode = m_overlays.GetPgsVerticalMode();
    const bool restrictAppliesToText =
        restrictSubsOn && pgsMode == 0 && subAlign == KODI::SUBTITLES::Align::ORIGINAL;
    const bool restrictAppliesToImage =
        restrictSubsOn && pgsMode == 0 && subAlign == KODI::SUBTITLES::Align::ORIGINAL;

    DOVIFrameMetadata doviFrameMeta{};
    bool doviFrameMetaValid = false;

    if (hasOverlay || m_renderDebug)
    {
      CRect src, dst, view;
      GetGuiVideoRect(src, dst, view);

      if (hasOverlay)
      {
        m_overlays.SetVideoRect(src, dst, view);

        const bool needsActiveArea = (pgsMode == 1 || pgsMode == 2 || restrictSubsOn);

        if (needsActiveArea)
        {
          int topPx = 0, botPx = 0;
          bool haveOffsets = false;
          const int frameW = static_cast<int>(src.Width());
          const int frameH = static_cast<int>(src.Height());

          if (!doviFrameMetaValid)
          {
            FillDoViActiveAreaMeta(doviFrameMeta);
            doviFrameMetaValid = true;
          }
          bool l5UsableForMode = false;
          if (m_picture.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION &&
              doviFrameMeta.has_level5_metadata)
          {
            if (pgsMode == 1)
              l5UsableForMode = (doviFrameMeta.level5_active_area_bottom_offset > 0);
            else
              l5UsableForMode = (doviFrameMeta.level5_active_area_top_offset > 0);
          }

          if (l5UsableForMode)
          {
            topPx = doviFrameMeta.level5_active_area_top_offset;
            botPx = doviFrameMeta.level5_active_area_bottom_offset;
            haveOffsets = true;
          }

          {
            static int64_t s_lastAaKey = -1;
            const int64_t aaKey = static_cast<int64_t>(pgsMode) |
                                  (static_cast<int64_t>(haveOffsets) << 3) |
                                  (static_cast<int64_t>(l5UsableForMode) << 4) |
                                  (static_cast<int64_t>(topPx & 0xFFFF) << 8) |
                                  (static_cast<int64_t>(botPx & 0xFFFF) << 24);
            if (aaKey != s_lastAaKey)
            {
              s_lastAaKey = aaKey;
              logComponentM(LOGDEBUG, LOGVIDEO,
                            "ActiveArea pgsMode={} doviL5={} l5Usable={} "
                            "topPx={} botPx={} haveOffsets={} frameH={}",
                            pgsMode, doviFrameMeta.has_level5_metadata, l5UsableForMode,
                            topPx, botPx, haveOffsets, frameH);
            }
          }

          if (haveOffsets && frameH > 0)
          {
            const float scale = dst.Height() / static_cast<float>(frameH);
            const int topRdPx = static_cast<int>(topPx * scale + 0.5f);
            const int botRdPx = static_cast<int>(botPx * scale + 0.5f);
            if (topRdPx != m_lastPushedActiveTopPx || botRdPx != m_lastPushedActiveBottomPx)
            {
              m_overlays.SetActiveAreaPx(topRdPx, botRdPx);
              m_lastPushedActiveTopPx = topRdPx;
              m_lastPushedActiveBottomPx = botRdPx;
            }
          }
        }
        else
        {
          if (m_lastPushedActiveTopPx != 0 || m_lastPushedActiveBottomPx != 0)
          {
            m_overlays.SetActiveAreaPx(0, 0);
            m_lastPushedActiveTopPx = 0;
            m_lastPushedActiveBottomPx = 0;
          }
        }

        m_overlays.Render(guiSource);
        if (CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
            CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO))
        {
          auto curOverlayCount = m_overlays.GetOverlayCount(guiSource);
          const auto curPlaneState = aml_get_plane_state_diag();
          static decltype(curOverlayCount) s_lastOverlayCount = static_cast<decltype(curOverlayCount)>(-1);
          static std::string s_lastPlaneState;
          if (curOverlayCount != s_lastOverlayCount || curPlaneState != s_lastPlaneState)
          {
            logComponentM(LOGDEBUG, LOGVIDEO,
                          "RenderManager.Render overlay composite source={} count={} | AMLPlaneState: {}",
                          guiSource, curOverlayCount, curPlaneState);
            s_lastOverlayCount = curOverlayCount;
            s_lastPlaneState = curPlaneState;
          }
        }
      }

      if (m_renderDebug)
      {
        if (m_renderDebugVideo)
        {
          DEBUG_INFO_VIDEO video = m_pRenderer->GetDebugInfo(guiSource);
          DEBUG_INFO_RENDER render = CServiceBroker::GetWinSystem()->GetDebugInfo();

          m_debugRenderer.SetInfo(video, render);
        }
        else
        {
          DEBUG_INFO_PLAYER info;

          m_playerPort->GetDebugInfo(info.audio, info.video, info.player);

          double refreshrate, clockspeed;
          int missedvblanks;

          info.vsync = StringUtils::Format("VSync Off:{:.1f}", (m_clockSync.m_syncOffset.load(std::memory_order_relaxed) / 1000));

          if (m_dvdClock.GetClockInfo(missedvblanks, clockspeed, refreshrate))
            info.vsync += StringUtils::Format(" VSync: refresh:{:.3f} missed:{} speed:{:.3f}%",
                                              refreshrate, missedvblanks, (clockspeed * 100));

          double videoLatency = (m_videoLatencyTweak / 1000.0);
          double audioLatency = (m_audioLatencyTweak / 1000.0);
          double videoDelay = (-m_videoDelay / 1000.0);
          double totalLatency = videoLatency + audioLatency + videoDelay;

          info.latency = StringUtils::Format(
              "Latency: video:{:.3f} audio:{:.3f} user:{:.3f} total:{:.3f}", videoLatency, audioLatency,
              videoDelay, totalLatency);

          m_debugRenderer.SetInfo(info);
        }

        m_debugRenderer.Render(src, dst, view);

        m_debugTimer.Set(1000ms);
        m_renderedOverlay.store(true, std::memory_order_relaxed);
      }
    }

    bool signalSubtitles = false;
    if (hasOverlay)
    {
      const bool dvSourceLevel5 =
          m_picture.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION &&
          CRenderSettingsCache::Get().m_dvStdL5.load(std::memory_order_acquire);
      const int subsSignalMode =
          dvSourceLevel5
              ? CRenderSettingsCache::Get().m_dvL5SubsSignalMode.load(std::memory_order_acquire)
              : 0;
      if (subsSignalMode == 1)
      {
        signalSubtitles = true;
      }
      else if (subsSignalMode == 2)
      {
        int subTopPx = 0, subBotPx = 0;
        bool subImageVisible = false;
        const bool subVisible = m_overlays.GetVisibleSubtitleSpan(subTopPx, subBotPx, subImageVisible);

        uint16_t sigTop = 0, sigBottom = 0;
        if (m_picture.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
        {
          if (!doviFrameMetaValid)
          {
            FillDoViActiveAreaMeta(doviFrameMeta);
            doviFrameMetaValid = true;
          }
          if (doviFrameMeta.has_level5_metadata)
          {
            sigTop = doviFrameMeta.level5_active_area_top_offset;
            sigBottom = doviFrameMeta.level5_active_area_bottom_offset;
          }
        }

        if (!subVisible)
        {
          signalSubtitles = false;
        }
        else if (sigTop == 0 && sigBottom == 0)
        {
          signalSubtitles = true;
        }
        else if ((pgsMode == 3 || pgsMode == 4 || pgsMode == 5) && subImageVisible)
        {
          signalSubtitles = true;
        }
        else
        {
          signalSubtitles = true;
          CRect sigSrc, sigDst, sigView;
          GetGuiVideoRect(sigSrc, sigDst, sigView);
          if (sigSrc.Height() > 0)
          {
            const float scaleY = sigDst.Height() / sigSrc.Height();
            const float topBarEdge = sigDst.y1 + sigTop * scaleY;
            const float botBarEdge = sigDst.y2 - sigBottom * scaleY;
            const bool inTop = sigTop > 0 && static_cast<float>(subTopPx) < topBarEdge;
            const bool inBottom = sigBottom > 0 && static_cast<float>(subBotPx) > botBarEdge;
            signalSubtitles = inTop || inBottom;
          }
        }

        if (signalSubtitles && restrictAppliesToText)
          signalSubtitles = false;

        static int s_lastSig = -1;
        static int s_lastBars = -1;
        const int barsKey = (static_cast<int>(sigTop) << 16) | static_cast<int>(sigBottom);
        if (static_cast<int>(signalSubtitles) != s_lastSig || barsKey != s_lastBars)
        {
          s_lastSig = static_cast<int>(signalSubtitles);
          s_lastBars = barsKey;
          logComponentM(LOGDEBUG, LOGVIDEO,
                        "L5 subs decision signal={} subVisible={} imgVis={} subTopPx={} subBotPx={} pgsMode={} "
                        "sigTop={} sigBottom={} restrictText={} restrictImage={} renderPts={:.0f}",
                        signalSubtitles, subVisible, subImageVisible, subTopPx, subBotPx, pgsMode, sigTop,
                        sigBottom, restrictAppliesToText, restrictAppliesToImage, guiPts);
        }
      }
    }
    aml_dv_set_subtitles(signalSubtitles);
  }

  if (!m_asyncVideoWorkerActive.load(std::memory_order_relaxed))
  {
    std::unique_lock<CCriticalSection> lock(m_presentlock);

    if (m_presentstep == PRESENT_FRAME)
    {
      if (present.presentmethod == PRESENT_METHOD_BOB)
        m_presentstep = PRESENT_FRAME2;
      else
        m_presentstep = PRESENT_IDLE;
    }
    else if (m_presentstep == PRESENT_FRAME2)
      m_presentstep = PRESENT_IDLE;

    if (m_presentstep == PRESENT_IDLE)
    {
      if (!m_queued.empty())
        m_presentstep = PRESENT_READY;
    }

    NotifyPresentWaiters();
  }
}

bool CRenderManager::IsGuiLayer()
{
  std::unique_lock<CCriticalSection> lock(m_statelock);

  if (!m_pRenderer)
    return false;

  // Inline the IsPresenting() check to avoid:
  //  - recursive re-lock of m_statelock (via IsConfigured())
  //  - an extra m_presentlock acquisition when we'd return true via overlay/debug paths anyway
  if (m_pRenderer->IsGuiLayer() && m_renderState == STATE_CONFIGURED)
  {
    std::unique_lock<CCriticalSection> plock(m_presentlock);
    if (!m_presentTimer.IsTimePast())
      return true;
  }

  if (m_renderedOverlay.load(std::memory_order_relaxed) || m_overlays.HasOverlay(m_presentsource))
    return true;

  if (m_renderDebug && m_debugTimer.IsTimePast())
    return true;

  return false;
}

bool CRenderManager::IsVideoLayer() const {
  {
    std::unique_lock<CCriticalSection> lock(m_statelock);

    if (!m_pRenderer)
      return false;

    if (!m_pRenderer->IsGuiLayer())
      return true;
  }
  return false;
}

void inline CRenderManager::RenderUpdate(bool clear, unsigned int flags, unsigned int alpha)
{
  m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, clear, flags, alpha);
  m_dataCacheCore.SetRenderPts(m_presentpts - m_displayLatency.load(std::memory_order_relaxed));
  m_dataCacheCore.SetVideoDoViLookupPts(m_Queue[m_presentsource].pts);
}

/* simple present method */
void CRenderManager::PresentSingle(bool clear, DWORD flags, DWORD alpha)
{
  const SPresent& present = m_Queue[m_presentsource];

  if (present.presentfield == FS_BOT)
    RenderUpdate(clear, flags | RENDER_FLAG_BOT, alpha);
  else if (present.presentfield == FS_TOP)
    RenderUpdate(clear, flags | RENDER_FLAG_TOP, alpha);
  else
    RenderUpdate(clear, flags, alpha);
}

/* new simpler method of handling interlaced material, *
 * we just render the two fields right after each other */
void CRenderManager::PresentFields(bool clear, DWORD flags, DWORD alpha)
{
  const SPresent& present = m_Queue[m_presentsource];

  if (m_presentstep == PRESENT_FRAME)
  {
    if (present.presentfield == FS_BOT)
      RenderUpdate(clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_FIELD0, alpha);
    else
      RenderUpdate(clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_FIELD0, alpha);
  }
  else
  {
    if (present.presentfield == FS_TOP)
      RenderUpdate(clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_FIELD1, alpha);
    else
      RenderUpdate(clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_FIELD1, alpha);
  }
}

void CRenderManager::PresentBlend(bool clear, DWORD flags, DWORD alpha)
{
  const SPresent& present = m_Queue[m_presentsource];

  if (present.presentfield == FS_BOT)
  {
    RenderUpdate(clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_NOOSD, alpha);
    RenderUpdate(false, flags | RENDER_FLAG_TOP, alpha / 2);
  }
  else
  {
    RenderUpdate(clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_NOOSD, alpha);
    RenderUpdate(false, flags | RENDER_FLAG_BOT, alpha / 2);
  }
}

void CRenderManager::UpdateVideoLatencyTweak()
{
  float fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
  const RESOLUTION_INFO res = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();

  float refresh = fps;
  if (CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution() == RES_WINDOW)
    refresh = 0; // No idea about refresh rate when windowed, just get the default latency

  m_videoLatencyTweak = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->GetVideoLatencyTweak(refresh, res.iScreenHeight);
}

void CRenderManager::UpdateResolution(bool force)
{
  std::unique_lock<CCriticalSection> lock(m_resolutionlock);

  if (m_bTriggerUpdateResolution)
  {
    if (force ||
        (CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenVideo() &&
         CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenRoot()))
    {
      RENDER_STEREO_MODE user_stereo_mode =
        CServiceBroker::GetGUI()->GetStereoscopicsManager().GetStereoModeByUser();
      STEREOSCOPIC_PLAYBACK_MODE playbackMode =
        static_cast<STEREOSCOPIC_PLAYBACK_MODE>(CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_STEREOSCOPICPLAYBACKMODE));
      if (m_triggerStereoNonEmpty &&
          playbackMode == STEREOSCOPIC_PLAYBACK_MODE_ASK &&
          user_stereo_mode == RENDER_STEREO_MODE_UNDEFINED)
        m_bTriggerUpdateResolution = false;
      if (m_bTriggerUpdateResolution &&
          CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) != ADJUST_REFRESHRATE_OFF && m_fps > 0.0f)
      {
        auto& gfxContext = CServiceBroker::GetWinSystem()->GetGfxContext();

        // Some platforms send a follow-up "reassert" trigger with no params (fps/width/height = 0)
        // after a mode switch/reset. In that case, prefer keeping the currently applied HDR type
        // (if any) to avoid an unnecessary second mode switch (e.g. VS10 HDR10->DV mapping).
        const StreamHdrType hdrOverride = m_hdrType_override;
        StreamHdrType desiredHdrType = (hdrOverride != StreamHdrType::HDR_TYPE_NONE)
                                           ? hdrOverride
                                           : m_picture.hdrType;
        if (hdrOverride == StreamHdrType::HDR_TYPE_NONE && m_bTriggerUpdateResolutionNoParams)
        {
          const auto currentHdrType = gfxContext.GetHDRType();
          if (currentHdrType != StreamHdrType::HDR_TYPE_NONE)
            desiredHdrType = currentHdrType;
        }

        const RESOLUTION desiredRes = CResolutionUtils::ChooseBestResolution(
            m_fps, m_picture.iWidth, m_picture.iHeight, m_triggerStereoNonEmpty);

        const bool needsApply = force || gfxContext.GetHDRType() != desiredHdrType ||
                                gfxContext.GetVideoResolution() != desiredRes;

        if (needsApply)
        {
          ParkVideoWorker(2000ms);
          gfxContext.SetHDRType(desiredHdrType);
          gfxContext.SetVideoResolution(desiredRes, false);
          UpdateVideoLatencyTweak();

          if (m_pRenderer)
            m_pRenderer->Update();
          ResumeVideoWorker();
        }
      }
      m_bTriggerUpdateResolution = false;
      m_bTriggerUpdateResolutionNoParams = false;
      m_hdrType_override = StreamHdrType::HDR_TYPE_NONE;
      m_playerPort->VideoParamsChange();
    }
  }
}

void CRenderManager::TriggerUpdateResolutionHdr(StreamHdrType hdrType)
{
  m_hdrType_override = hdrType;
  m_bTriggerUpdateResolution = true;
}

void CRenderManager::TriggerUpdateResolution(float fps, int width, int height, std::string &stereomode)
{
  m_bTriggerUpdateResolutionNoParams = (width == 0);
  if (width)
  {
    m_fps = fps;
    m_picture.iWidth = width;
    m_picture.iHeight = height;
    m_triggerStereoNonEmpty = !stereomode.empty();
  }
  m_bTriggerUpdateResolution = true;
}

void CRenderManager::ToggleDebug()
{
  m_renderDebug = !m_renderDebug;
  m_debugTimer.SetExpired();
  m_renderDebugVideo = false;
}

void CRenderManager::ToggleDebugVideo()
{
  m_renderDebug = !m_renderDebug;
  m_debugTimer.SetExpired();
  m_renderDebugVideo = true;
}

void CRenderManager::SetSubtitleVerticalPosition(int value, bool save)
{
  m_overlays.SetSubtitleVerticalPosition(value, save);
}

bool CRenderManager::AddVideoPicture(const VideoPicture& picture, volatile std::atomic_bool& bStop, EINTERLACEMETHOD deintMethod, bool wait)
{
  int index;
  {
    std::unique_lock lock(m_presentlock);
    if (m_free.empty())
      return false;
    index = m_free.front();
    m_free.pop_front();
  }

  bool wantsDoublePass = false;
  bool haveRenderer = false;
  {
    std::lock_guard lock2(m_datalock);
    haveRenderer = (m_pRenderer != nullptr);
    if (haveRenderer)
    {
      m_pRenderer->AddVideoPicture(picture, index);
      wantsDoublePass = m_pRenderer->WantsDoublePass();
    }
  }
  if (!haveRenderer)
  {
    std::unique_lock lock(m_presentlock);
    if (std::find(m_free.begin(), m_free.end(), index) == m_free.end())
      m_free.push_front(index);
    return false;
  }

  // set fieldsync if picture is interlaced
  EFIELDSYNC displayField = FS_NONE;
  if (picture.iFlags & DVP_FLAG_INTERLACED)
  {
    if (deintMethod != EINTERLACEMETHOD::VS_INTERLACEMETHOD_NONE)
    {
      if (picture.iFlags & DVP_FLAG_TOP_FIELD_FIRST)
        displayField = FS_TOP;
      else
        displayField = FS_BOT;
    }
  }

  EPRESENTMETHOD presentmethod = PRESENT_METHOD_SINGLE;
  if (deintMethod == VS_INTERLACEMETHOD_NONE)
  {
    presentmethod = PRESENT_METHOD_SINGLE;
    displayField = FS_NONE;
  }
  else
  {
    if (displayField == FS_NONE)
      presentmethod = PRESENT_METHOD_SINGLE;
    else
    {
      if (deintMethod == VS_INTERLACEMETHOD_RENDER_BLEND)
        presentmethod = PRESENT_METHOD_BLEND;
      else if (deintMethod == VS_INTERLACEMETHOD_RENDER_BOB)
        presentmethod = PRESENT_METHOD_BOB;
      else
      {
        if (!wantsDoublePass)
          presentmethod = PRESENT_METHOD_SINGLE;
        else
          presentmethod = PRESENT_METHOD_BOB;
      }
    }
  }

  std::unique_lock lock(m_presentlock);

  SPresent& present = m_Queue[index];
  present.presentfield = displayField;
  present.presentmethod = presentmethod;
  present.pts = picture.pts;

  // Keep the queue sorted by pts (and avoid duplicate indices) so the render tick doesn't
  // have to scan/sort.
  if (std::find(m_queued.begin(), m_queued.end(), index) == m_queued.end())
  {
    if (m_cadenceArmed)
      m_cadenceQueueIn++;
    const double pts = present.pts;
    if (m_queued.empty() || m_Queue[m_queued.back()].pts <= pts)
    {
      m_queued.push_back(index);
    }
    else
    {
      auto insertPos = std::upper_bound(
          m_queued.begin(), m_queued.end(), pts,
          [this](double ptsValue, int queuedIndex) { return ptsValue < m_Queue[queuedIndex].pts; });
      m_queued.insert(insertPos, index);
    }
  }

  m_asyncDiscardPause = false;
  m_playerPort->UpdateRenderBuffers(m_queued.size(), m_discard.size(), m_free.size());

  // signal to any waiters to check state
  if (m_presentstep == PRESENT_IDLE)
  {
    m_presentstep = PRESENT_READY;
    NotifyPresentWaiters();
    if (m_asyncVideoWorkerActive.load(std::memory_order_relaxed) &&
        m_asyncRefilling.load(std::memory_order_relaxed))
      m_asyncFloorPaceEvent.Set();
  }

  if (wait)
  {
    m_forceNext = true;
    XbmcThreads::EndTime<> endtime(200ms);
    while (m_presentstep == PRESENT_READY)
    {
      WaitPresent(lock, 20ms);
      if (endtime.IsTimePast() || bStop)
      {
        if (!bStop)
        {
          CLog::Log(LOGWARNING, "CRenderManager::AddVideoPicture - timeout waiting for render");
        }
        break;
      }
    }
    m_forceNext = false;
  }

  return true;
}

void CRenderManager::AddOverlay(std::shared_ptr<CDVDOverlay> o, double pts)
{
  int idx;
  {
    std::unique_lock<CCriticalSection> lock(m_presentlock);

    if (m_free.empty())
      return;
    idx = m_free.front();
  }
  std::unique_lock<CCriticalSection> lock(m_datalock);

  const bool isBitmap = o && (o->IsOverlayType(DVDOVERLAY_TYPE_IMAGE) ||
                              o->IsOverlayType(DVDOVERLAY_TYPE_SPU));
  const bool isText = o && (o->IsOverlayType(DVDOVERLAY_TYPE_SSA) ||
                            o->IsOverlayType(DVDOVERLAY_TYPE_TEXT));
  m_overlays.AddOverlay(std::move(o), pts, idx);
}

bool CRenderManager::Supports(ERENDERFEATURE feature) const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->Supports(feature);
  else
    return false;
}

bool CRenderManager::Supports(ESCALINGMETHOD method) const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->Supports(method);
  else
    return false;
}

int CRenderManager::WaitForBuffer(volatile std::atomic_bool& bStop,
                                  std::chrono::milliseconds timeout)
{
  std::unique_lock lock(m_presentlock);

  // check if gui is active and discard buffer if not
  // this keeps videoplayer going
  if (!m_bRenderGUI || !g_application.GetRenderGUI())
  {
    m_bRenderGUI = false;
    double presenttime = 0;
    const double clock = m_dvdClock.GetClock();
    if (!m_queued.empty())
    {
      int idx = m_queued.front();
      presenttime = m_Queue[idx].pts;
    }
    else
      presenttime = clock + 0.02;

    auto sleeptime = std::chrono::milliseconds(static_cast<int>((presenttime - clock) * 1000));
    if (sleeptime < 0ms)
      sleeptime = 0ms;
    sleeptime = std::min(sleeptime, 20ms);
    WaitPresent(lock, sleeptime);
    DiscardBufferLocked();
    return 0;
  }

  XbmcThreads::EndTime<> endtime{timeout};
  while(m_free.empty())
  {
    WaitPresent(lock, std::min(50ms, timeout));
    if (endtime.IsTimePast() || bStop)
    {
      return -1;
    }
  }

  // make sure overlay buffer is released, this won't happen on AddOverlay
  m_overlays.Release(m_free.front());

  // return buffer level
  return m_queued.size() + m_discard.size();
}

void CRenderManager::PrepareNextRender()
{
  if (m_queued.empty())
  {
    logComponentM(LOGERROR, LOGAVTIMING, "CRenderManager::PrepareNextRender - asked to prepare with nothing available");
    m_presentstep = PRESENT_IDLE;
    NotifyPresentWaiters();
    return;
  }

  if (!m_showVideo && !m_forceNext)
    return;

  const double frameOnScreen = m_dvdClock.GetClock();
  const double frametime = 1.0 /
                     static_cast<double>(CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS()) *
                     DVD_TIME_BASE;

  m_displayLatency.store(DVD_MSEC_TO_TIME(static_cast<double>(
                             m_videoLatencyTweak + m_audioLatencyTweak - m_videoDelay -
                             m_deinterlaceDelay)),
                         std::memory_order_relaxed);

  double renderPts = frameOnScreen + m_displayLatency.load(std::memory_order_relaxed);

  double nextFramePts = m_Queue[m_queued.front()].pts;

  if (m_dvdClock.GetClockSpeed() < 0)
    nextFramePts = renderPts;

  if (m_clockSync.m_enabled)
  {
    double err = fmod(renderPts - nextFramePts, frametime);
    if (m_clockSync.m_errCount == 0 && !m_clockSync.m_refValid)
    {
      m_clockSync.m_ref = err;
      m_clockSync.m_refValid = true;
    }
    else
      err -= frametime * std::round((err - m_clockSync.m_ref) / frametime);
    m_clockSync.m_error += err;
    m_clockSync.m_errCount++;
    if (m_clockSync.m_errCount > 30)
    {
      double average = m_clockSync.m_error / m_clockSync.m_errCount;
      m_clockSync.m_syncOffset.store(average, std::memory_order_relaxed);
      m_clockSync.m_error = 0;
      m_clockSync.m_errCount = 0;

      double ref = average;
      if (!std::isfinite(ref))
        ref = 0;
      else if (ref <= -frametime)
        ref += frametime;
      else if (ref > frametime)
        ref -= frametime;
      m_clockSync.m_ref = ref;

      SetVsyncAdjust(-average);
    }
    renderPts += frametime / 2 - m_clockSync.m_syncOffset.load(std::memory_order_relaxed);
  }
  else
  {
    SetVsyncAdjust(0);
  }

  LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGAVTIMING, 1000,
              "renderPts: {:.3f} renderPts: {:.3f} nextFramePts: {:.3f} -> diff: {:.3f}  render: {:d} "
              "forceNext: {:d}",
              renderPts / DVD_TIME_BASE, renderPts / DVD_TIME_BASE, nextFramePts / DVD_TIME_BASE,
              (renderPts - nextFramePts) / DVD_TIME_BASE, renderPts >= nextFramePts, m_forceNext);

  LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGAVTIMING, 1000,
              "VP.AVVideo clockMs={:.1f} renderPtsMs={:.1f} nextFramePtsMs={:.1f} displayLatencyMs={:.1f} "
              "videoLatencyTweakMs={} audioLatencyTweakMs={} videoDelayMs={} videoFrameMinusClockMs={:.1f}",
              frameOnScreen / DVD_TIME_BASE * 1000.0,
              renderPts / DVD_TIME_BASE * 1000.0,
              nextFramePts / DVD_TIME_BASE * 1000.0,
              m_displayLatency.load(std::memory_order_relaxed) / DVD_TIME_BASE * 1000.0,
              m_videoLatencyTweak.load(std::memory_order_relaxed),
              m_audioLatencyTweak.load(std::memory_order_relaxed),
              m_videoDelay.load(std::memory_order_relaxed),
              (nextFramePts - frameOnScreen) / DVD_TIME_BASE * 1000.0);

  bool combined = false;
  if (m_presentsourcePast >= 0)
  {
    m_discard.push_back(m_presentsourcePast);
    m_presentsourcePast = -1;
    combined = true;
  }

  if ((renderPts >= nextFramePts) || m_forceNext)
  {
    // push back present source index before other lates to keep order
    if (m_presentstarted) m_discard.push_back(m_presentsource);
    
    double diff = (renderPts - nextFramePts);
    const bool skip = ((m_dataCacheCore.GetSpeed() == 1.0f) && (diff > 79000.0));

    // skip late frames
    while ((diff > 62000.0) && (m_queued.size() > 2))
    {
      int late = m_queued.front();
      m_queued.pop_front();
      m_discard.push_back(late);
      if (skip) m_QueueSkip++;
      diff = (renderPts - m_Queue[m_queued.front()].pts);
    }

    if (m_displayReset)
    {
      m_QueueSkip = 0;
      m_lateframes = 0;
      m_displayReset = false;
    }

    int idx = m_queued.front();
    m_lateframes = static_cast<int>(std::max(0.0, diff / frametime));

    if (m_cadenceArmed)
      m_cadenceAdvance++;

    m_presentstep = PRESENT_FLIP;
    m_presentsource = idx;
    m_presentstarted = true;
    m_queued.pop_front();
    m_presentpts = m_Queue[m_presentsource].pts;
    NotifyPresentWaiters();

    m_playerPort->UpdateRenderBuffers(m_queued.size(), m_discard.size(), m_free.size());
  }
  else if (!combined && renderPts > (nextFramePts - frametime))
  {
    m_lateframes = 0;
    m_presentstep = PRESENT_FLIP;
    m_presentsourcePast = m_presentsource;
    m_presentsource = m_queued.front();
    m_presentstarted = true;
    m_queued.pop_front();
    m_presentpts = m_Queue[m_presentsource].pts - frametime / 2;
    NotifyPresentWaiters();
    if (m_cadenceArmed)
      m_cadenceHalf++;
  }
}

void CRenderManager::DiscardBuffer()
{
  std::unique_lock lock2(m_presentlock);

  DiscardBufferLocked();
}

void CRenderManager::DiscardBufferLocked()
{
  while (!m_queued.empty())
  {
    m_discard.push_back(m_queued.front());
    m_queued.pop_front();
  }

  if (m_presentstep == PRESENT_READY)
    m_presentstep = PRESENT_IDLE;
  m_asyncDiscardPause = true;
  NotifyPresentWaiters();
}

bool CRenderManager::GetStats(int &lateframes, double &pts, int &queued, int &discard)
{
  std::unique_lock lock(m_presentlock);
  lateframes = m_lateframes / 10;
  pts = m_presentpts - m_displayLatency.load(std::memory_order_relaxed);
  queued = m_queued.size();
  discard  = m_discard.size();
  return true;
}

double CRenderManager::GetRenderPts()
{
  return (m_presentpts.load(std::memory_order_relaxed) -
          m_displayLatency.load(std::memory_order_relaxed));
}

void CRenderManager::CheckEnableClockSync()
{
  // refresh rate can be a multiple of video fps
  double diff = 1.0;
  bool refClockRunning = false;

  if (m_fps != 0)
  {
    double fps = static_cast<double>(m_fps);
    double refreshrate, clockspeed;
    int missedvblanks;
    refClockRunning = m_dvdClock.GetClockInfo(missedvblanks, clockspeed, refreshrate);
    if (refClockRunning)
    {
      fps *= clockspeed;
    }

    diff = static_cast<double>(CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS()) / fps;
    if (diff < 1.0)
      diff = 1.0 / diff;

    // Calculate distance from nearest integer proportion
    diff = std::abs(std::round(diff) - diff);
  }

  if (refClockRunning && diff < 0.0005)
  {
    m_clockSync.m_enabled = true;
  }
  else
  {
    m_clockSync.m_enabled = false;
    SetVsyncAdjust(0);
  }

  if (m_lastPublishedClockSync != m_clockSync.m_enabled)
  {
    m_lastPublishedClockSync = m_clockSync.m_enabled;
    m_playerPort->UpdateClockSync(m_clockSync.m_enabled);
  }
}
