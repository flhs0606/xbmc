/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDClock.h"
#include "DebugRenderer.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"
#include "cores/VideoPlayer/VideoRenderers/OverlayRenderer.h"
#include "cores/VideoSettings.h"
#include "application/ApplicationPlayer.h"
#include "threads/CriticalSection.h"
#include "threads/Event.h"
#include "threads/SystemClock.h"
#include "utils/Geometry.h"
#include "utils/StreamDetails.h"
#include "windowing/Resolution.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <list>
#include <map>
#include <optional>

#include "PlatformDefs.h"

class CRenderCapture;
struct VideoPicture;

class CWinRenderer;
class CLinuxRenderer;
class CLinuxRendererGL;
class CLinuxRendererGLES;
class CRenderManager;

class IRenderMsg
{
  friend CRenderManager;
public:
  virtual ~IRenderMsg() = default;
protected:
  virtual void VideoParamsChange() = 0;
  virtual void GetDebugInfo(std::string &audio, std::string &video, std::string &general) = 0;
  virtual void UpdateClockSync(bool enabled) = 0;
  virtual void UpdateRenderInfo(CRenderInfo &info) = 0;
  virtual void UpdateRenderBuffers(int queued, int discard, int free) = 0;
  virtual void UpdateGuiRender(bool gui) = 0;
  virtual void UpdateVideoRender(bool video) = 0;
  virtual CVideoSettings GetVideoSettings() const = 0;
};

class CRenderManager
{
public:
  CRenderManager(CDVDClock &clock, IRenderMsg *player);
  ~CRenderManager();

  // Functions called from render thread
  void SetVsyncAdjust(double adjustment);
  void GetVideoRect(CRect& source, CRect& dest, CRect& view) const;
  float GetAspectRatio() const;
  void FrameMove();
  void SetAsyncVideoWorkerActive(bool active);
  void RequestAsyncVideoWorkerStop();
  bool AsyncVideoWorkerIteration();
  bool CanRunAsyncVideoWorker();
  void FrameWait(std::chrono::milliseconds duration);
  void Render(bool clear, DWORD flags = 0, DWORD alpha = 255, bool gui = true);
  bool IsVideoLayer() const;
  RESOLUTION GetResolution() const;
  void UpdateResolution(bool force = false);
  void TriggerUpdateResolution(float fps, int width, int height, std::string &stereomode);
  void TriggerUpdateResolutionHdr(StreamHdrType m_hdrType);
  void SetViewMode(int iViewMode) const;
  void PreInit();
  void UnInit();
  bool Flush(bool wait, bool saveBuffers);
  bool IsConfigured() const;
  void ToggleDebug();
  void ToggleDebugVideo();

  /*!
   * \brief Set the subtitle vertical position,
   * it depends on current screen resolution
   * \param value The subtitle position in pixels
   * \param save If true, the value will be saved to resolution info
   */
  void SetSubtitleVerticalPosition(const int value, bool save);

  unsigned int AllocRenderCapture();
  void ReleaseRenderCapture(unsigned int captureId);
  void StartRenderCapture(unsigned int captureId, unsigned int width, unsigned int height, int flags);
  bool RenderCaptureGetPixels(unsigned int captureId, unsigned int millis, uint8_t *buffer, unsigned int size);

  // Functions called from GUI
  bool Supports(ERENDERFEATURE feature) const;
  bool Supports(ESCALINGMETHOD method) const;

  int GetSkippedFrames() const { return m_QueueSkip; }

  bool HasFutureFrame(double clockPts) const;

  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation, StreamHdrType hdrType, int buffers = 0);
  bool AddVideoPicture(const VideoPicture& picture, volatile std::atomic_bool& bStop, EINTERLACEMETHOD deintMethod, bool wait);
  void AddOverlay(std::shared_ptr<CDVDOverlay> o, double pts);
  void ShowVideo(bool enable);
  void PauseAsyncVideoLayerPoll();
  void WaitAsyncMainPace();
  uint64_t GetVisibleOverlaySetSignature(bool& animated) const;

  void DisplayReset() { m_displayReset = true; }

  /**
   * If player uses buffering it has to wait for a buffer before it calls
   * AddVideoPicture and AddOverlay. It waits for max 50 ms before it returns -1
   * in case no buffer is available. Player may call this in a loop and decides
   * by itself when it wants to drop a frame.
   */
  int WaitForBuffer(volatile std::atomic_bool& bStop,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(100));

  /**
   * Can be called by player for lateness detection. This is done best by
   * looking at the end of the queue.
   */
  bool GetStats(int &lateframes, double &pts, int &queued, int &discard);

  double GetRenderPts();

  /**
   * Video player call this on flush in oder to discard any queued frames
   */
  void DiscardBuffer();

  void SetDelay(int delay) { m_videoDelay = delay; }
  int GetDelay() { return m_videoDelay; }

  void SetDeinterlaceDelay(int delay) { m_deinterlaceDelay = delay; }
  int GetDeinterlaceDelay() { return m_deinterlaceDelay; }

  int GetVideoLatencyTweak() { return m_videoLatencyTweak; }

  void SetAudioLatencyTweak(int tweak) { m_audioLatencyTweak = tweak; }
  int GetAudioLatencyTweak() { return m_audioLatencyTweak; }

  void SetVideoSettings(const CVideoSettings& settings) const;

protected:

  inline void NotifyPresentWaiters()
  {
    if (m_presentWaiters.load(std::memory_order_relaxed) != 0)
      m_presentevent.notifyAll();
  }

  inline void WaitPresent(std::unique_lock<CCriticalSection>& lock, std::chrono::milliseconds duration)
  {
    m_presentWaiters.fetch_add(1, std::memory_order_relaxed);
    m_presentevent.wait(lock, duration);
    m_presentWaiters.fetch_sub(1, std::memory_order_relaxed);
  }

  inline void WaitPresent(std::unique_lock<CCriticalSection>& lock, unsigned int durationMs)
  {
    WaitPresent(lock, std::chrono::milliseconds(durationMs));
  }

  void PresentSingle(bool clear, DWORD flags, DWORD alpha);
  void PresentFields(bool clear, DWORD flags, DWORD alpha);
  void PresentBlend(bool clear, DWORD flags, DWORD alpha);

  void DiscardBufferLocked();
  void PrepareNextRender();
  bool IsPresenting();
  bool IsGuiLayer();

  bool Configure();
  void CreateRenderer();
  void DeleteRenderer();
  void ManageCaptures();

  void VideoBody(bool firstFrame);
  void GetGuiVideoRect(CRect& source, CRect& dest, CRect& view);
  void PublishAsyncPresentMirror(int source, double pts);
  void PublishAsyncMirrorRects(const CRect& source, const CRect& dest, const CRect& view);
  bool VideoWorkerParked();
  bool ParkVideoWorker(std::chrono::milliseconds timeout);
  void ResumeVideoWorker();

  void UpdateVideoLatencyTweak();
  void CheckEnableClockSync();
  void UpdateActiveAreaInfo();
  void UpdateDvOsdLift();
  void FillDoViActiveAreaMeta(DOVIFrameMetadata& meta);

  CBaseRenderer *m_pRenderer = nullptr;
  OVERLAY::CRenderer m_overlays;
  int m_lastPushedActiveTopPx{-1};
  int m_lastPushedActiveBottomPx{-1};
  int m_lastActiveAreaTopPct{-1};
  int m_lastActiveAreaBottomPct{-1};
  int m_lastActiveAreaTopPx{-1};
  int m_lastActiveAreaBottomPx{-1};
  int m_contentAreaTopPx{0};
  int m_contentAreaBottomPx{0};
  int m_kernelFrameWidth{0};
  int m_kernelFrameHeight{0};
  uint64_t m_lastKernelDimsKey{0};
  std::chrono::steady_clock::time_point m_kernelFrameDimsRead{};
  CDebugRenderer m_debugRenderer;
  mutable CCriticalSection m_statelock;
  CCriticalSection m_resolutionlock;
  mutable CCriticalSection m_presentlock;
  CCriticalSection m_datalock;
  std::atomic_bool m_bTriggerUpdateResolution{false};
  std::atomic_bool m_bTriggerUpdateResolutionNoParams{false};
  std::atomic_bool m_triggerStereoNonEmpty{false};
  bool m_bRenderGUI = true;
  std::atomic_bool m_asyncPinArmed{false};
  std::atomic_bool m_asyncVideoWorkerActive{false};
  std::atomic_bool m_asyncWorkerParked{false};
  std::atomic_bool m_asyncParkRequest{false};
  std::atomic_bool m_asyncWorkerStop{false};
  std::atomic_bool m_asyncRefilling{false};
  CEvent m_asyncParkedEvent;
  CEvent m_asyncResumeEvent;
  CEvent m_asyncFloorPaceEvent;
  CEvent m_asyncMainPaceEvent;
  std::atomic<uint32_t> m_asyncMainPaceSets{0};
  std::atomic<bool> m_renderedOverlay{false};
  std::atomic_bool m_renderDebug{false};
  std::atomic_bool m_renderDebugVideo{false};
  XbmcThreads::EndTime<> m_debugTimer;
  std::atomic_bool m_showVideo = {false};

  enum EPRESENTSTEP
  {
    PRESENT_IDLE     = 0
  , PRESENT_FLIP
  , PRESENT_FRAME
  , PRESENT_FRAME2
  , PRESENT_READY
  };

  enum EPRESENTMETHOD
  {
    PRESENT_METHOD_SINGLE = 0,
    PRESENT_METHOD_BLEND,
    PRESENT_METHOD_BOB,
  };

  enum ERENDERSTATE
  {
    STATE_UNCONFIGURED = 0,
    STATE_CONFIGURING,
    STATE_CONFIGURED,
  };
  
  std::atomic<ERENDERSTATE> m_renderState{STATE_UNCONFIGURED};

  CEvent m_stateEvent;

  // Display latency tweak from AdvancedSettings for the current refresh rate and resolution in milliseconds
  std::atomic_int m_videoLatencyTweak = 0;

  // Display latency tweak from AdvancedSettings for audio in milliseconds
  std::atomic_int m_audioLatencyTweak = 0;

  // User set latency
  std::atomic_int m_videoDelay = {};
  std::atomic_int m_deinterlaceDelay = {};

  std::atomic<double> m_displayLatency{0.0};

  std::atomic_int m_QueueSize{2};
  std::atomic_int m_QueueSkip{0};

  struct SPresent
  {
    double         pts;
    EFIELDSYNC     presentfield;
    EPRESENTMETHOD presentmethod;
  } m_Queue[NUM_BUFFERS]{};

  std::deque<int> m_free;
  std::deque<int> m_queued;
  std::deque<int> m_discard;

  std::unique_ptr<VideoPicture> m_pConfigPicture;

  VideoPicture m_picture{};

  float m_fps = 0.0;
  unsigned int m_orientation = 0;
  StreamHdrType m_hdrType = StreamHdrType::HDR_TYPE_NONE;
  std::atomic<StreamHdrType> m_hdrType_override{StreamHdrType::HDR_TYPE_NONE};
  int m_NumberBuffers = 0;
  std::atomic<int> m_lateframes{-1};
  std::atomic<double> m_presentpts{0.0};
  EPRESENTSTEP m_presentstep = PRESENT_IDLE;
  bool m_cadenceArmed = false;
  int m_cadenceCalls = 0;
  int m_cadenceAdvance = 0;
  int m_cadenceHalf = 0;
  int m_cadenceEmpty = 0;
  int m_cadenceQueueIn = 0;
  int m_cadenceQueueMin = 0;
  int m_cadenceQueueMax = 0;
  int m_cadenceSkipBase = 0;
  double m_cadencePtsBase = 0.0;
  std::chrono::steady_clock::time_point m_cadenceStamp{};
  XbmcThreads::EndTime<> m_presentTimer;
  bool m_forceNext = false;
  bool m_presentstarted = false;
  bool m_asyncDiscardPause = false;
  int m_presentsource = 0;
  int m_presentsourcePast = -1;
  std::atomic_uint m_presentWaiters{0};
  XbmcThreads::ConditionVariable m_presentevent;
  CEvent m_flushEvent;
  CEvent m_initEvent;
  CDVDClock &m_dvdClock;
  IRenderMsg *m_playerPort;

  struct CClockSync
  {
    void Reset();
    double m_error;
    double m_ref;
    bool m_refValid;
    int m_errCount;
    std::atomic<double> m_syncOffset;
    bool m_enabled;
  };
  CClockSync m_clockSync;

  struct AsyncPresentMirror
  {
    int presentsource = 0;
    double pts = 0.0;
    CRect srcRect;
    CRect dstRect;
    CRect viewRect;
  };
  AsyncPresentMirror m_asyncMirror;
  std::optional<bool> m_lastPublishedClockSync;
  std::vector<CRect> m_l5Bars;
  int m_lastOsdKey = -1;

  void RenderCapture(CRenderCapture* capture) const;
  void RemoveCaptures();
  CCriticalSection m_captCritSect;
  std::map<unsigned int, CRenderCapture*> m_captures;
  static unsigned int m_nextCaptureId;
  unsigned int m_captureWaitCounter = 0;
  //set to true when adding something to m_captures, set to false when m_captures is made empty
  //std::list::empty() isn't thread safe, using an extra bool will save a lock per render when no captures are requested
  bool m_hasCaptures = false;

  std::atomic_bool m_displayReset{false};

private:
  void RenderUpdate(bool clear, unsigned int flags, unsigned int alpha);

  CDataCacheCore &m_dataCacheCore;
};
