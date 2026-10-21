/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerVideo.h"
#include "cores/VideoPlayer/BDStageTrace.h"

#include "DVDCodecs/DVDCodecUtils.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDCodecs/Overlay/DVDOverlay.h"
#include "DVDCodecs/Video/DVDVideoCodecFFmpeg.h"
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayLibass.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/MathUtils.h"
#include "utils/log.h"
#include "utils/LogThrottle.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include "platform/linux/SysfsPath.h"

#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <chrono>
#include <string_view>

using namespace std::chrono_literals;

namespace
{
struct ScopedAtomicTrue
{
  std::atomic_bool& flag;
  explicit ScopedAtomicTrue(std::atomic_bool& f) : flag(f) { flag.store(true); }
  ~ScopedAtomicTrue() { flag.store(false); }
  ScopedAtomicTrue(const ScopedAtomicTrue&) = delete;
  ScopedAtomicTrue& operator=(const ScopedAtomicTrue&) = delete;
};

bool aml_keep_prog_film_enabled()
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  if (!settingsComponent)
    return false;
  const auto settings = settingsComponent->GetSettings();
  if (!settings)
    return false;
  return settings->GetBool(CSettings::SETTING_VIDEOPLAYER_AMLMPEG2KEEPPROG);
}
}

class CDVDMsgVideoCodecChange : public CDVDMsg
{
public:
  CDVDMsgVideoCodecChange(const CDVDStreamInfo& hints, std::unique_ptr<CDVDVideoCodec> codec)
    : CDVDMsg(GENERAL_STREAMCHANGE), m_codec(std::move(codec)), m_hints(hints)
  {}
  ~CDVDMsgVideoCodecChange() override = default;

  std::unique_ptr<CDVDVideoCodec> m_codec;
  CDVDStreamInfo  m_hints;
};

CVideoPlayerVideo::CVideoPlayerVideo(
  CDVDClock* pClock,
  CDVDOverlayContainer* pOverlayContainer,
  CDVDMessageQueue& parent,
  CRenderManager& renderManager,
  CProcessInfo &processInfo,
  double messageQueueTimeSize
)
: CThread("VideoPlayerVideo")
, IDVDStreamPlayerVideo(processInfo)
, m_messageQueue("video")
, m_messageParent(parent)
, m_renderManager(renderManager)
{
  m_pClock = pClock;
  m_pOverlayContainer = pOverlayContainer;
  m_speed = DVD_PLAYSPEED_NORMAL;

  m_bRenderSubs = false;
  m_paused = false;
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_iSubtitleDelay.store(0.0, std::memory_order_relaxed);
  m_iLateFrames = 0;
  m_iDroppedRequest = 0;
  m_fForcedAspectRatio = 0;

  // allows max bitrate of 128 Mbit/s (e.g. UHD Blu-Ray) during messageQueueTimeSize seconds
  m_messageQueue.SetMaxDataSize(128 * (messageQueueTimeSize / 8) * 1024 * 1024);
  m_messageQueue.SetMaxTimeSize(messageQueueTimeSize);

  m_iDroppedFrames = 0;
  m_fFrameRate = 25;
  m_fStableFrameRate = 0.0;
  m_iFrameRateCount = 0;
  m_bAllowDrop = false;
  m_iFrameRateErr = 0;
  m_iFrameRateLength = 0;
  m_bFpsInvalid = false;
  m_telecineProbe = 0;
  m_telecine = false;
  m_halvedFieldRate = 0.0;
  m_telecineTwoFieldPackets = 0;
  m_telecineProbeStart = std::chrono::steady_clock::now();
}

CVideoPlayerVideo::~CVideoPlayerVideo()
{
  m_bAbortOutput = true;
  StopThread();
}

double CVideoPlayerVideo::GetOutputDelay()
{
  double time = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET);
  if( m_fFrameRate )
    time = (time * DVD_TIME_BASE) / m_fFrameRate;
  else
    time = 0.0;

  if( m_speed != 0 )
    time = time * DVD_PLAYSPEED_NORMAL / abs(m_speed);

  return time;
}

bool CVideoPlayerVideo::OpenStream(CDVDStreamInfo hint)
{
  if (hint.flags & AV_DISPOSITION_ATTACHED_PIC)
    return false;
  if (!hint.extradata)
  {
    // codecs which require extradata
    // clang-format off
    if (hint.codec == AV_CODEC_ID_NONE ||
        hint.codec == AV_CODEC_ID_MPEG1VIDEO ||
        hint.codec == AV_CODEC_ID_MPEG2VIDEO ||
        (hint.codec == AV_CODEC_ID_H264 && (hint.codec_tag == 0 || hint.codec_tag == MKTAG('a','v','c','1') || hint.codec_tag == MKTAG('a','v','c','2'))) ||
        hint.codec == AV_CODEC_ID_HEVC ||
        hint.codec == AV_CODEC_ID_MPEG4 ||
        hint.codec == AV_CODEC_ID_WMV3 ||
        hint.codec == AV_CODEC_ID_VC1)
    {
      logComponentM(LOGDEBUG, LOGVIDEO, "Codec id {} require extradata.", hint.codec);
      return false;
    }
    // clang-format on
  }

  CLog::Log(LOGDEBUG, "Creating video codec with codec id: {:d}", hint.codec);
  hint.pClock = m_pClock;

  if (m_messageQueue.IsInited())
  {
    if (m_pVideoCodec && !m_processInfo.IsVideoHwDecoder())
      hint.codecOptions |= CODEC_ALLOW_FALLBACK;

    std::unique_ptr<CDVDVideoCodec> codec = CDVDFactoryCodec::CreateVideoCodec(hint, m_processInfo);

    if (!codec)
    {
      CLog::Log(LOGDEBUG, "CVideoPlayerVideo::OpenStream - could not open video codec");
      if (!m_pVideoCodec)
        m_processInfo.ResetVideoCodecInfo();
    }

    SendMessage(std::make_shared<CDVDMsgVideoCodecChange>(hint, std::move(codec)), 0);
  }
  else
  {
    m_processInfo.ResetVideoCodecInfo();
    hint.codecOptions |= CODEC_ALLOW_FALLBACK;

    std::unique_ptr<CDVDVideoCodec> codec = CDVDFactoryCodec::CreateVideoCodec(hint, m_processInfo);
    if (!codec)
    {
      CLog::Log(LOGERROR, "CVideoPlayerVideo::OpenStream - could not open video codec");
      return false;
    }

    if (!hint.stereo_mode.empty() && hint.stereo_mode != "mono")
      m_processInfo.SetVideoStereoMode(hint.stereo_mode);
    OpenStream(hint, std::move(codec));

    CLog::Log(LOGDEBUG, "Creating video thread");

    m_messageQueue.Init();
    Create();
  }
  return true;
}

void CVideoPlayerVideo::OpenStream(CDVDStreamInfo& hint, std::unique_ptr<CDVDVideoCodec> codec)
{
  CLog::Log(LOGDEBUG, "CVideoPlayerVideo::OpenStream - open stream with codec id: {:d} fps:{:d}/{:d} options:{:02x}",
    hint.codec, hint.fpsrate, hint.fpsscale, hint.codecOptions);

  m_processInfo.GetVideoBufferManager().ReleasePools();

  //reported fps is usually not completely correct
  if (hint.fpsrate && hint.fpsscale)
  {
    m_fFrameRate = DVD_TIME_BASE / CDVDCodecUtils::NormalizeFrameduration(
                                       (double)DVD_TIME_BASE * hint.fpsscale / hint.fpsrate);

    m_bFpsInvalid = false;

    const bool isVC1 = (hint.codec == AV_CODEC_ID_VC1 || hint.codec == AV_CODEC_ID_WMV3);
    if (hint.codecOptions & CODEC_UNKNOWN_I_P)
    {
      if (!isVC1 && MathUtils::FloatEquals(static_cast<float>(m_fFrameRate), 25.0f, 0.01f))
      {
        m_fFrameRate = 50.0;
        m_processInfo.SetVideoInterlaced(true);
      }
      else if (!isVC1 && MathUtils::FloatEquals(static_cast<float>(m_fFrameRate), 29.97f, 0.01f))
      {
        m_fFrameRate = 60000.0 / 1001.0;
        m_processInfo.SetVideoInterlaced(true);
      }
      else
        m_processInfo.SetVideoInterlaced(false);
    }
    else
      m_processInfo.SetVideoInterlaced(((hint.codecOptions & CODEC_INTERLACED) == CODEC_INTERLACED) &&
                                       !(hint.width > 1920 || hint.height > 1080));

    if (isVC1)
      logComponentM(LOGDEBUG, LOGVIDEO,
        "CVideoPlayerVideo::OpenStream VC1 codecOptions=0x{:02x} m_fFrameRate={:.3f} "
        "(doubling skipped for VC1; codec FrameRateTracking will confirm scan type)",
        hint.codecOptions, m_fFrameRate);

    m_retryProgressive = 0;
    m_processInfo.SetVideoFps(static_cast<float>(m_fFrameRate));
  }
  else
  {
    m_fFrameRate = 50;
    m_processInfo.SetVideoInterlaced(true);
    m_bFpsInvalid = true;
    m_processInfo.SetVideoFps(0);
  }

  m_ptsTracker.ResetVFRDetection();
  ResetFrameRateCalc();

  m_iDroppedRequest = 0;
  m_iLateFrames = 0;

  if( m_fFrameRate > 120 || m_fFrameRate < 5 )
  {
    CLog::Log(LOGERROR,
              "CVideoPlayerVideo::OpenStream - Invalid framerate {}, using forced 25fps and just "
              "trust timestamps",
              (int)m_fFrameRate);
    m_fFrameRate = 50;
    m_processInfo.SetVideoInterlaced(true);
  }

  // use aspect in stream if available
  if (hint.forced_aspect)
    m_fForcedAspectRatio = static_cast<float>(hint.aspect);
  else
    m_fForcedAspectRatio = 0.0f;

  if (m_pVideoCodec && m_pVideoCodec->Reconfigure(hint))
  {
    // reuse old decoder
    codec = std::move(m_pVideoCodec);
  }

  m_pVideoCodec.reset();

  if (!codec)
  {
    CLog::Log(LOGDEBUG, "CVideoPlayerVideo::OpenStream - Creating video codec with codec id: {:d} fps:{:d}/{:d} options:{:02x}",
      hint.codec, hint.fpsrate, hint.fpsscale, hint.codecOptions);
    hint.pClock = m_pClock;
    hint.codecOptions |= CODEC_ALLOW_FALLBACK;
    codec = CDVDFactoryCodec::CreateVideoCodec(hint, m_processInfo);
    if (!codec)
    {
      CLog::Log(LOGERROR, "CVideoPlayerVideo::OpenStream - could not open video codec");
      m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_ABORT));
      StopThread();
    }
  }

  m_pVideoCodec = std::move(codec);
  m_hints = hint;
  m_telecineProbe = 0;
  m_telecine = false;
  m_halvedFieldRate = 0.0;
  m_telecineTwoFieldPackets = 0;
  const bool mpegFieldRate =
      (hint.codec == AV_CODEC_ID_MPEG1VIDEO || hint.codec == AV_CODEC_ID_MPEG2VIDEO) &&
      (hint.codecOptions & CODEC_INTERLACED) &&
      m_fFrameRate > 55.0 && m_fFrameRate < 61.0;
  if (mpegFieldRate &&
      (!m_processInfo.IsVideoHwDecoder() || aml_keep_prog_film_enabled()))
  {
    m_telecineProbe = 6;
    m_telecineProbeStart = std::chrono::steady_clock::now();
  }
  m_stalled = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;
  m_playbackStalled = false;
  m_isEOS = false;
  m_packets.clear();
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_renderManager.ShowVideo(false);
}

void CVideoPlayerVideo::CloseStream(bool bWaitForBuffers)
{
  m_renderManager.SetDeinterlaceDelay(0);

  // wait until buffers are empty
  if (bWaitForBuffers && m_speed > 0)
  {
    SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::VIDEO_DRAIN), 0);
    m_messageQueue.WaitUntilEmpty();
  }

  m_messageQueue.Abort();

  // wait for decode_video thread to end
  CLog::Log(LOGDEBUG, "waiting for video thread to exit");

  m_bAbortOutput = true;
  StopThread();

  m_messageQueue.End();

  CLog::Log(LOGDEBUG, "deleting video codec");
  m_pVideoCodec.reset();

  if (m_picture.videoBuffer)
  {
    m_picture.videoBuffer->Release();
    m_picture.videoBuffer = nullptr;
  }
}

bool CVideoPlayerVideo::AcceptsData() const
{
  bool full = m_messageQueue.IsFull();
  return !full;
}

bool CVideoPlayerVideo::HasData() const
{
  return m_messageQueue.GetDataSize() > 0;
}

bool CVideoPlayerVideo::IsInited() const
{
  return m_messageQueue.IsInited();
}

bool CVideoPlayerVideo::IsEOS()
{
  return m_isEOS;
}

inline void CVideoPlayerVideo::SendMessage(std::shared_ptr<CDVDMsg> pMsg, int priority)
{
  if (!m_messageQueue.IsInited())
    return;
  m_messageQueue.Put(pMsg, priority);
}

inline void CVideoPlayerVideo::SendMessageBack(const std::shared_ptr<CDVDMsg>& pMsg, int priority)
{
  m_messageQueue.PutBack(pMsg, priority);
}

inline void CVideoPlayerVideo::FlushMessages()
{
  m_messageQueue.Flush();
}

inline MsgQueueReturnCode CVideoPlayerVideo::GetMessage(std::shared_ptr<CDVDMsg>& pMsg,
                                                        std::chrono::milliseconds timeout,
                                                        int& priority)
{
  MsgQueueReturnCode ret = m_messageQueue.Get(pMsg, timeout, priority);
  return ret;
}

void CVideoPlayerVideo::Process()
{
  logComponentM(LOGDEBUG, LOGVIDEO, "running thread: video_thread");

  double pts = 0;
  double frametime = (double)DVD_TIME_BASE / m_fFrameRate;

  bool bRequestDrop = false;
  int iDropDirective;
  bool onlyPrioMsgs = false;

  m_vfmt.clear();
  int vfmtCheckCount = 0;

  m_picture.Reset();
  m_swBlockResetPending = true;
  m_videoStats.Start();
  m_droppingStats.Reset();
  m_iDroppedFrames = 0;
  m_playbackStalled = false;
  m_outputSate = OUTPUT_NORMAL;

  while (!m_bStop)
  {
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double, std::micro>(m_stalled ? frametime : frametime * 10));
    int iPriority = 0;

    if (m_syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
      iPriority = 1;

    if (m_paused)
      iPriority = 1;

    if (onlyPrioMsgs)
    {
      iPriority = 1;
      timeout = 1ms;
    }

    std::shared_ptr<CDVDMsg> pMsg;
    if (m_swBlockResetPending)
    {
      m_swBlockResetPending = false;
      m_swBlockStamp = {};
      m_swQWaitUs = 0;
      m_swWaitUs = 0;
      m_swAddUs = 0;
      m_swBlockAgain = 0;
      m_swBlockDropped = 0;
      m_swVqMin = -1;
      m_swVqMax = -1;
    }
    const auto qEnter = m_swBlockArmed ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
    MsgQueueReturnCode ret = GetMessage(pMsg, timeout, iPriority);
    if (m_swBlockArmed)
      m_swQWaitUs += std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - qEnter)
                         .count();

    onlyPrioMsgs = false;

    if (MSGQ_IS_ERROR(ret))
    {
      if (!m_messageQueue.ReceivedAbortRequest())
        logM(LOGERROR, "MSGQ_IS_ERROR returned true ({})", ret);

      break;
    }
    else if (ret == MSGQ_TIMEOUT)
    {
      if (m_outputSate == OUTPUT_AGAIN &&
          m_picture.videoBuffer)
      {
        m_outputSate = OutputPicture(m_picture);
        if (m_processInfo.IsVideoHwDecoder())
        {
          vfmtCheckCount = 16;
          LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000, "CVideoPlayerVideo - OUTPUT_AGAIN - vfmt, interlace should be checked.");
        }
        if (m_outputSate == OUTPUT_AGAIN)
        {
          m_playbackStalled = true;
          onlyPrioMsgs = true;
          continue;
        }
      }
      // don't ask for a new frame if we can't deliver it to renderer
      else if ((m_speed != DVD_PLAYSPEED_PAUSE ||
                m_processInfo.IsFrameAdvance() ||
                m_syncState != IDVDStreamPlayer::SYNC_INSYNC) && !m_paused)
      {
        if (ProcessDecoderOutput(frametime, pts))
        {
          onlyPrioMsgs = true;
          continue;
        }
      }

      // if we only wanted priority messages, this isn't a stall
      if (iPriority)
        continue;

      if (m_isEOS && !m_processInfo.GetInMenu())
        continue;

      //Okey, start rendering at stream fps now instead, we are likely in a stillframe
      if (!m_stalled && m_telecineProbe == 0)
      {
        // squeeze pictures out
        while (!m_bStop && m_pVideoCodec)
        {
          m_pVideoCodec->SetCodecControl(DVD_CODEC_CTRL_DRAIN);
          if (!ProcessDecoderOutput(frametime, pts))
            break;
        }

        logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayerVideo - Stillframe detected, switching to forced {:f} fps", m_fFrameRate);
        m_stalled = true;
        pts += frametime * 4;
      }

      // Waiting timed out, output last picture
      if (m_picture.videoBuffer)
      {
        m_picture.pts = pts;
        m_outputSate = OutputPicture(m_picture);
        pts += frametime;
      }

      continue;
    }

    if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if (std::static_pointer_cast<CDVDMsgGeneralSynchronize>(pMsg)->Wait(100ms, SYNCSOURCE_VIDEO))
        logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayerVideo - CDVDMsg::GENERAL_SYNCHRONIZE");
      else
        SendMessage(pMsg, 1); /* push back as prio message, to process other prio messages */

      m_droppingStats.Reset();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    {
      pts = std::static_pointer_cast<CDVDMsgDouble>(pMsg)->m_value;

      m_syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_droppingStats.Reset();
      m_playbackStalled = false;
      m_renderManager.ShowVideo(true);

      logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayerVideo - CDVDMsg::GENERAL_RESYNC({:f})", pts);
      if (m_processInfo.IsVideoHwDecoder())
      {
        vfmtCheckCount = 16;
        logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayerVideo - OUTPUT_AGAIN - vfmt, interlace should be checked.");
      }
    }
    else if (pMsg->IsType(CDVDMsg::VIDEO_SET_ASPECT))
    {
      logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayerVideo - CDVDMsg::VIDEO_SET_ASPECT");
      m_fForcedAspectRatio = static_cast<float>(*std::static_pointer_cast<CDVDMsgDouble>(pMsg));
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      m_swBlockResetPending = true;
      if(m_pVideoCodec)
        m_pVideoCodec->Reset();

      if (m_telecineProbe > 0)
        m_telecineProbeStart = std::chrono::steady_clock::now();

      if (m_telecine && m_hints.fpsrate && m_hints.fpsscale &&
          !m_processInfo.IsVideoHwDecoder())
      {
        m_telecine = false;
        m_fFrameRate = DVD_TIME_BASE / CDVDCodecUtils::NormalizeFrameduration(
                                           (double)DVD_TIME_BASE * m_hints.fpsscale / m_hints.fpsrate);
        m_processInfo.SetVideoFps(static_cast<float>(m_fFrameRate));
        m_telecineProbe = 6;
        m_telecineTwoFieldPackets = 0;
        m_telecineProbeStart = std::chrono::steady_clock::now();
      }

      if (m_picture.videoBuffer)
      {
        m_picture.videoBuffer->Release();
        m_picture.videoBuffer = nullptr;
      }
      m_packets.clear();
      m_droppingStats.Reset();
      m_isEOS = false;
      m_syncState = IDVDStreamPlayer::SYNC_STARTING;
      m_renderManager.ShowVideo(false);
      m_playbackStalled = false;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH)) // private message sent by (CVideoPlayerVideo::Flush())
    {
      m_swBlockResetPending = true;
      bool sync = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      m_renderManager.PauseAsyncVideoLayerPoll();
      if(m_pVideoCodec)
        m_pVideoCodec->Reset();

      if (m_picture.videoBuffer)
      {
        m_picture.videoBuffer->Release();
        m_picture.videoBuffer = nullptr;
      }
      m_packets.clear();
      pts = 0;
      m_playbackStalled = false;

      m_ptsTracker.Flush();
      //we need to recalculate the framerate
      //! @todo this needs to be set on a streamchange instead
      ResetFrameRateCalc();
      m_droppingStats.Reset();

      m_stalled = true;
      m_isEOS = false;
      if (sync)
      {
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        m_renderManager.ShowVideo(false);
      }

      m_renderManager.DiscardBuffer();
      FlushMessages();
      m_messageQueue.Flush(CDVDMsg::VIDEO_DRAIN);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      m_speed = std::static_pointer_cast<CDVDMsgInt>(pMsg)->m_value;
      if (m_pVideoCodec)
        m_pVideoCodec->SetSpeed(m_speed);

      m_droppingStats.Reset();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      auto msg = std::static_pointer_cast<CDVDMsgVideoCodecChange>(pMsg);

      m_isEOS = false;
      if (m_telecineProbe > 0 && m_processInfo.IsVideoHwDecoder())
        ResolveTelecineProbe(frametime, true);
      while (!m_bStop && m_pVideoCodec)
      {
        m_pVideoCodec->SetCodecControl(DVD_CODEC_CTRL_DRAIN);
        bool cont = ProcessDecoderOutput(frametime, pts);

        if (!cont)
          break;
      }

      OpenStream(msg->m_hints, std::move(msg->m_codec));
      msg->m_codec = nullptr;
      if (m_picture.videoBuffer)
      {
        m_picture.videoBuffer->Release();
        m_picture.videoBuffer = nullptr;
      }
    }
    else if (pMsg->IsType(CDVDMsg::VIDEO_DRAIN))
    {
      m_isEOS = false;
      if (m_telecineProbe > 0 && m_processInfo.IsVideoHwDecoder())
        ResolveTelecineProbe(frametime, true);
      while (!m_bStop && m_pVideoCodec)
      {
        m_pVideoCodec->SetCodecControl(DVD_CODEC_CTRL_DRAIN);
        if (!ProcessDecoderOutput(frametime, pts))
          break;
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_PAUSE))
    {
      m_paused = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayerVideo - CDVDMsg::GENERAL_PAUSE: {}", m_paused);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_REQUEST_STATE))
    {
      SStateMsg msg;
      msg.player = VideoPlayer_VIDEO;
      msg.syncState = m_syncState;
      m_messageParent.Put(
          std::make_shared<CDVDMsgType<SStateMsg>>(CDVDMsg::PLAYER_REPORT_STATE, msg));
    }
    else if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacket();
      bool bPacketDrop = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacketDrop();

      if (pPacket->iSize == 0 && !pPacket->pData)
      {
        LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000,
                              "CVideoPlayerVideo - discarding empty demux packet");
        continue;
      }

      if (m_telecineProbe > 0)
      {
        const double fieldTime = (double)DVD_TIME_BASE / m_fFrameRate;
        if (pPacket->duration > fieldTime * 2.5 && pPacket->duration < fieldTime * 3.5)
        {
          m_telecineProbe = 0;
          m_telecine = true;
          m_fFrameRate *= 0.4;
          frametime = (double)DVD_TIME_BASE / m_fFrameRate;
          m_processInfo.SetVideoFps(static_cast<float>(m_fFrameRate));
          logM(LOGINFO,
               "soft telecine: three-field packet ({:.1f} ms) in a field-rate MPEG-2 stream, "
               "opening at {:.3f} fps",
               pPacket->duration / 1000.0, m_fFrameRate);
        }
        else
        {
          if (pPacket->duration > fieldTime * 1.5 && pPacket->duration < fieldTime * 2.5)
            m_telecineTwoFieldPackets++;

          if (--m_telecineProbe == 0)
            ResolveTelecineProbe(frametime, false);
        }
      }

      if (m_stalled)
      {
        logComponentM(LOGDEBUG, LOGVIDEO, "Stillframe left, switching to normal playback");
        m_stalled = false;
      }

      bRequestDrop = false;
      iDropDirective = CalcDropRequirement(pts);

      // Framerate calibration exists to avoid dropping frames during normal playback before we're
      // confident what the real framerate is. That doesn't apply during trick-play and
      // DROP_VERYLATE must not be silently disabled.
      if ((iDropDirective & DROP_VERYLATE) && (m_bAllowDrop || m_speed > DVD_PLAYSPEED_NORMAL) &&
          !bPacketDrop)
      {
        bRequestDrop = true;
      }
      if (iDropDirective & DROP_DROPPED)
      {
        m_iDroppedFrames++;
        m_ptsTracker.Flush();
      }
      if (m_messageQueue.GetDataSize() == 0 ||  m_speed < 0)
      {
        bRequestDrop = false;
        m_iDroppedRequest = 0;
        m_iLateFrames = 0;
      }

      int codecControl = 0;
      if (iDropDirective & DROP_BUFFER_LEVEL)
        codecControl |= DVD_CODEC_CTRL_HURRY;
      if (m_speed > DVD_PLAYSPEED_NORMAL)
        codecControl |= DVD_CODEC_CTRL_NO_POSTPROC;
      if (bPacketDrop)
        codecControl |= DVD_CODEC_CTRL_DROP;
      if (bRequestDrop)
        codecControl |= DVD_CODEC_CTRL_DROP_ANY;
      if (!m_renderManager.Supports(RENDERFEATURE_ROTATION))
        codecControl |= DVD_CODEC_CTRL_ROTATE;
      m_pVideoCodec->SetCodecControl(codecControl);

      if (m_pVideoCodec->AddData(*pPacket))
      {
        m_isEOS = false;
        // buffer packets so we can recover should decoder flush for some reason
        if (m_pVideoCodec->GetConvergeCount() > 0)
        {
          m_packets.emplace_back(pMsg, 0);
          if (m_packets.size() > m_pVideoCodec->GetConvergeCount() ||
              m_packets.size() * frametime > DVD_SEC_TO_TIME(10))
            m_packets.pop_front();
        }

        m_videoStats.AddSampleBytes(pPacket->iSize);
        UpdatePlayerInfo();

        if (ProcessDecoderOutput(frametime, pts))
        {
          onlyPrioMsgs = true;
        }

        if (vfmtCheckCount > 0 && --vfmtCheckCount % 5 == 0)
        {
          CSysfsPath frame_format{"/sys/class/deinterlace/di0/frame_format"};
          if (frame_format.Exists())
            m_vfmt = frame_format.Get<std::string>().value();
          if (m_vfmt.size() > 4)
          {
            bool vfmtIsInterlaced = m_vfmt.compare("progressive") != 0;
            if (vfmtIsInterlaced || !(m_hints.codecOptions & CODEC_INTERLACED))
              m_processInfo.SetVideoInterlaced(vfmtIsInterlaced);
          }
          logComponentM(LOGDEBUG, LOGVIDEO, "CDVDMsg::DEMUXER_PACKET - checking interlace vfmt: {}", m_vfmt);
        }
      }
      else
      {
        SendMessageBack(pMsg);
        onlyPrioMsgs = true;
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_DISPLAY_RESET))
    {
      logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayerVideo: display reset occurred, clear skipped frames");
      m_displayReset = true;
      m_lastDisplayReset = std::chrono::steady_clock::now();
      m_renderManager.DisplayReset();
    }
  }
}

void CVideoPlayerVideo::UpdatePlayerInfo()
{
  if (!m_playerInfoTimer.IsTimePast()) return;
  m_playerInfoTimer.Set(167ms);

  int level, dataLevel;
  m_messageQueue.GetLevels(level, dataLevel);
  m_dataCacheCore.SetVideoLiveBitRate(GetVideoBitrate());
  m_dataCacheCore.SetVideoQueueLevel(std::min(99, level));
  m_dataCacheCore.SetVideoQueueDataLevel(std::min(99, dataLevel));
}

bool CVideoPlayerVideo::ProcessDecoderOutput(double &frametime, double &pts)
{
  if (m_telecineProbe > 0)
  {
    if (std::chrono::steady_clock::now() - m_telecineProbeStart < 500ms)
      return false;

    ResolveTelecineProbe(frametime, true);
  }

  CDVDVideoCodec::VCReturn decoderState = m_pVideoCodec->GetPicture(&m_picture);

  if (decoderState == CDVDVideoCodec::VC_BUFFER)
  {
    return false;
  }

  // if decoder was flushed, we need to seek back again to resume rendering
  if (decoderState == CDVDVideoCodec::VC_FLUSHED)
  {
    logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayerVideo - video decoder was flushed");

    while (!m_packets.empty())
    {
      auto msg = std::static_pointer_cast<CDVDMsgDemuxerPacket>(m_packets.front().message);
      m_packets.pop_front();
      SendMessage(msg, 10);
    }

    m_renderManager.PauseAsyncVideoLayerPoll();
    m_pVideoCodec->Reset();
    m_packets.clear();
    // picture.iFlags &= ~DVP_FLAG_ALLOCATED;
    m_renderManager.DiscardBuffer();
    return false;
  }

  if (decoderState == CDVDVideoCodec::VC_REOPEN)
  {
    while (!m_packets.empty())
    {
      auto msg = std::static_pointer_cast<CDVDMsgDemuxerPacket>(m_packets.front().message);
      m_packets.pop_front();
      SendMessage(msg, 10);
    }

    m_renderManager.PauseAsyncVideoLayerPoll();
    m_pVideoCodec->Reopen();
    m_packets.clear();
    m_renderManager.DiscardBuffer();
    return false;
  }

  // if decoder had an error, tell it to reset to avoid more problems
  if (decoderState == CDVDVideoCodec::VC_ERROR)
  {
    logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayerVideo - video decoder returned error");
    m_isEOS = true;
    return false;
  }

  if (decoderState == CDVDVideoCodec::VC_EOF)
  {
    m_isEOS = true;
    if (m_syncState == IDVDStreamPlayer::SYNC_STARTING)
    {
      SStartMsg msg;
      msg.player = VideoPlayer_VIDEO;
      msg.cachetime = DVD_MSEC_TO_TIME(50);
      msg.cachetotal = DVD_MSEC_TO_TIME(100);
      msg.timestamp = DVD_NOPTS_VALUE;
      m_messageParent.Put(std::make_shared<CDVDMsgType<SStartMsg>>(CDVDMsg::PLAYER_STARTED, msg));
    }
    return false;
  }

  // check for a new picture
  if (decoderState == CDVDVideoCodec::VC_PICTURE)
  {
    m_isEOS = false;

    if (m_processInfo.GetVideoInterlaced() &&
        !(m_hints.codecOptions & CODEC_INTERLACED) &&
        m_vfmt == "progressive" &&
        MathUtils::FloatEquals(static_cast<float>(m_picture.iDuration), static_cast<float>(2 * DVD_TIME_BASE) / m_processInfo.GetVideoFps(), 700.0f))
    {
      if (++m_retryProgressive > 3)
      {
        float halvedFps = m_processInfo.GetVideoFps() / 2.0f;
        m_processInfo.SetVideoFps(halvedFps);
        m_processInfo.SetVideoInterlaced(false);
        m_renderManager.TriggerUpdateResolution(halvedFps, m_hints.width, m_hints.height, m_hints.stereo_mode);
      }
    }
    else
      m_retryProgressive = 0;

    // Validate timing and set pts
    bool hasTimestamp = true;
    if (m_picture.pts == DVD_NOPTS_VALUE)
    {
        if (m_picture.dts != DVD_NOPTS_VALUE)
            m_picture.pts = m_picture.dts;
        else
        {
            m_picture.pts = pts;
            hasTimestamp = false;
        }
    }

    pts = (m_picture.pts != DVD_NOPTS_VALUE) ? m_picture.pts : pts;

    m_picture.iDuration = frametime;

    if (m_picture.iRepeatPicture)
    {
        double extraDelay = m_picture.iRepeatPicture * m_picture.iDuration;
        if (m_telecine)
          extraDelay *= 0.4;
        else
          m_picture.iDuration += extraDelay;
        m_picture.pts += extraDelay;
    }

    // Update next frame pts
    if (m_speed != 0)
        pts += m_picture.iDuration * m_speed / abs(m_speed);

    // use forced aspect if any
    if (m_fForcedAspectRatio != 0.0f)
    {
      m_picture.iDisplayWidth = (int) (m_picture.iDisplayHeight * m_fForcedAspectRatio);
      if (m_picture.iDisplayWidth > m_picture.iWidth)
      {
        m_picture.iDisplayWidth =  m_picture.iWidth;
        m_picture.iDisplayHeight = (int) (m_picture.iDisplayWidth / m_fForcedAspectRatio);
      }
    }

    // set stereo mode if not set by decoder
    if (m_picture.stereoMode.empty())
    {
      const auto videoSettings = m_processInfo.GetVideoSettings();
      std::string_view stereoMode;
      switch (static_cast<RENDER_STEREO_MODE>(videoSettings.m_StereoMode))
      {
        case RENDER_STEREO_MODE_SPLIT_VERTICAL:
          stereoMode = "left_right";
          if (videoSettings.m_StereoInvert)
            stereoMode = "right_left";
          break;
        case RENDER_STEREO_MODE_SPLIT_HORIZONTAL:
          stereoMode = "top_bottom";
          if (videoSettings.m_StereoInvert)
            stereoMode = "bottom_top";
          break;
        case RENDER_STEREO_MODE_HARDWAREBASED:
          stereoMode = "block_lr";
          if (videoSettings.m_StereoInvert)
            stereoMode = "block_rl";
          break;
        default:
          stereoMode = m_hints.stereo_mode;
          break;
      }
      if (!stereoMode.empty() && stereoMode != "mono")
      {
        m_picture.stereoMode = stereoMode;
      }
    }

    m_outputSate = OutputPicture(m_picture);

    if (m_outputSate == OUTPUT_AGAIN)
    {
      return true;
    }
    else if (m_outputSate == OUTPUT_ABORT)
    {
      m_isEOS = true;
      return false;
    }
    else if (m_outputSate == OUTPUT_DROPPED)
    {
      if (!(m_picture.iFlags & DVP_FLAG_DROPPED))
      {
        m_iDroppedFrames++;
        m_ptsTracker.Flush();
      }

      if (m_picture.videoBuffer)
      {
        m_picture.videoBuffer->Release();
        m_picture.videoBuffer = nullptr;
      }
    }

    if (m_syncState == IDVDStreamPlayer::SYNC_STARTING &&
        m_outputSate != OUTPUT_DROPPED &&
        !(m_picture.iFlags & DVP_FLAG_DROPPED))
    {
      m_syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
      SStartMsg msg;
      msg.player = VideoPlayer_VIDEO;
      msg.cachetime = DVD_MSEC_TO_TIME(50); //! @todo implement
      msg.cachetotal = DVD_MSEC_TO_TIME(100); //! @todo implement
      const bool vc1VfmtOnlyInterlace =
          (m_hints.codec == AV_CODEC_ID_VC1 || m_hints.codec == AV_CODEC_ID_WMV3) &&
          !(m_hints.codecOptions & CODEC_INTERLACED);
      const auto settingsComponent = CServiceBroker::GetSettingsComponent();
      const auto advancedSettings =
          settingsComponent ? settingsComponent->GetAdvancedSettings() : nullptr;
      const bool diCompensation =
          !advancedSettings || advancedSettings->m_videoDeinterlaceDelayCompensation;
      const bool diInterlaced = m_processInfo.GetVideoInterlaced();
      const bool diHwDecoder = diInterlaced && m_processInfo.IsVideoHwDecoder();
      const bool diSysfs =
          diInterlaced && CSysfsPath{"/sys/class/deinterlace/di0/frame_format"}.Exists();
      const bool diApply =
          diCompensation && diInterlaced && diHwDecoder && !vc1VfmtOnlyInterlace && diSysfs;
      constexpr int DI_PIPELINE_FIELDS = 12;
      const int diDelayMs =
          diApply ? static_cast<int>(DI_PIPELINE_FIELDS * 1000.0 / m_fFrameRate) : 0;
      m_renderManager.SetDeinterlaceDelay(diDelayMs);
      if (diInterlaced)
        logComponentM(LOGDEBUG, LOGVIDEO,
            "DI compensation: applied={} delayMs={:d} setting={} hwdec={} vc1VfmtOnly={} "
            "sysfs={} fps={:.3f}",
            diApply, diDelayMs, diCompensation, diHwDecoder, vc1VfmtOnlyInterlace, diSysfs,
            m_fFrameRate);

      msg.timestamp = hasTimestamp ? (pts + (m_renderManager.GetDelay() + m_renderManager.GetDeinterlaceDelay()) * 1000) : DVD_NOPTS_VALUE;
      m_messageParent.Put(std::make_shared<CDVDMsgType<SStartMsg>>(CDVDMsg::PLAYER_STARTED, msg));
    }

    frametime = (double)DVD_TIME_BASE / m_fFrameRate;
  }

  return true;
}

void CVideoPlayerVideo::OnExit()
{
  CLog::Log(LOGDEBUG, "thread end: video_thread");
}

void CVideoPlayerVideo::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    SendMessage(std::make_shared<CDVDMsgInt>(CDVDMsg::PLAYER_SETSPEED, speed), 1);
  else
    m_speed = speed;
}

void CVideoPlayerVideo::Flush(bool sync)
{
  /* flush using message as this get's called from VideoPlayer thread */
  /* and any demux packet that has been taken out of queue need to */
  /* be disposed of before we flush */
  if (m_pVideoCodec)
    m_pVideoCodec->Abort();
  SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_FLUSH, sync), 1);
  m_bAbortOutput = true;
}

void CVideoPlayerVideo::ProcessOverlays(const VideoPicture& picture, double pts) const {
  double subsPts = pts - m_iSubtitleDelay.load(std::memory_order_relaxed);

  // remove any overlays that are out of time
  if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
    m_pOverlayContainer->CleanUp(subsPts);

  VecOverlays overlays;
  size_t containerSize = 0;
  size_t skippedNonForced = 0;
  size_t filteredByTime = 0;
  size_t groupsExpanded = 0;

  {
    std::unique_lock lock(*m_pOverlayContainer);

    VecOverlays* pVecOverlays = m_pOverlayContainer->GetOverlays();
    containerSize = pVecOverlays->size();
    auto it = pVecOverlays->begin();

    //Check all overlays and render those that should be rendered, based on time and forced
    //Both forced and subs should check timing
    while (it != pVecOverlays->end())
    {
      std::shared_ptr<CDVDOverlay>& pOverlay = *it++;
      if(!pOverlay->bForced && !m_bRenderSubs)
      {
        skippedNonForced++;
        continue;
      }

      double pts2 = pOverlay->bForced ? pts : subsPts;

      // Only attempt RTTI for overlay types that can be backed by libass.
      if (pOverlay->IsOverlayType(DVDOVERLAY_TYPE_TEXT) || pOverlay->IsOverlayType(DVDOVERLAY_TYPE_SSA))
      {
        auto libassOverlay = std::static_pointer_cast<CDVDOverlayLibass>(pOverlay);
        if (libassOverlay && !libassOverlay->GetLibassHandler()->EventActive(pts2))
          continue;
      }

      if((pOverlay->iPTSStartTime <= pts2 && (pOverlay->iPTSStopTime > pts2 || pOverlay->iPTSStopTime == 0LL)))
      {
        if(pOverlay->IsOverlayType(DVDOVERLAY_TYPE_GROUP))
        {
          const auto& g = static_cast<CDVDOverlayGroup&>(*pOverlay).m_overlays;
          overlays.insert(overlays.end(), g.begin(), g.end());
          groupsExpanded++;
        }
        else
          overlays.push_back(pOverlay);
      }
      else
        filteredByTime++;
    }
  }

  {
    static int s_lastSyncState = -1;
    static size_t s_lastContainerSize = ~static_cast<size_t>(0);
    static size_t s_lastOverlayCount = ~static_cast<size_t>(0);
    if (static_cast<int>(m_syncState) != s_lastSyncState ||
        containerSize != s_lastContainerSize ||
        overlays.size() != s_lastOverlayCount)
    {
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "ProcessOverlays pts={:.3f} syncState={} containerSize={} skipNonForced={} "
                    "filteredByTime={} groupsExpanded={} addToRenderer={}",
                    pts / DVD_TIME_BASE, static_cast<int>(m_syncState), containerSize,
                    skippedNonForced, filteredByTime, groupsExpanded, overlays.size());
      s_lastSyncState = static_cast<int>(m_syncState);
      s_lastContainerSize = containerSize;
      s_lastOverlayCount = overlays.size();
    }
  }

  // Add overlays outside the overlay container lock to keep the critical section small.
  for (auto& overlay : overlays)
  {
    double pts2 = overlay->bForced ? pts : subsPts;
    m_renderManager.AddOverlay(overlay, pts2);
  }
}

CVideoPlayerVideo::EOutputState CVideoPlayerVideo::OutputPicture(const VideoPicture& picture)
{
  ScopedAtomicTrue inFlightGuard(m_outputPictureInFlight);
  m_bAbortOutput = false;
  const auto videoSettings = m_processInfo.GetVideoSettings();

  if (m_processInfo.GetVideoStereoMode() != picture.stereoMode)
  {
    m_processInfo.SetVideoStereoMode(picture.stereoMode);
    // signal about changes in video parameters
    m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));
  }

  double config_framerate = m_bFpsInvalid
    ? static_cast<double>(CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS())
    : m_fFrameRate;

  if (m_halvedFieldRate > 0.0 && !m_bFpsInvalid)
    config_framerate = m_halvedFieldRate;
  else if (m_processInfo.GetVideoInterlaced())
  {
    if (MathUtils::FloatEquals(static_cast<float>(config_framerate), 25.0f, 0.02f))
      config_framerate = 50.0;
    else if (MathUtils::FloatEquals(static_cast<float>(config_framerate), 29.97f, 0.02f))
      config_framerate = 59.94;
  }

  int sorient = videoSettings.m_Orientation;
  int orientation = sorient != 0 ? (sorient + m_hints.orientation) % 360
                                 : m_hints.orientation;

  if (!m_renderManager.Configure(picture,
                                static_cast<float>(config_framerate),
                                orientation,
                                m_hints.hdrType,
                                m_pVideoCodec->GetAllowedReferences()))
  {
    const auto now = std::chrono::steady_clock::now();

    if (m_rendererConfigureRetryStart == std::chrono::steady_clock::time_point::min())
      m_rendererConfigureRetryStart = now;

    const auto retryElapsed = now - m_rendererConfigureRetryStart;
    if (retryElapsed < 5s)
    {
      logM(LOGWARNING, "renderer configure not ready, retrying ({} ms)",
                                            std::chrono::duration_cast<std::chrono::milliseconds>(retryElapsed).count());
      return OUTPUT_AGAIN;
    }
    CLog::Log(LOGERROR, "{} - failed to configure renderer", __FUNCTION__);
    
    return OUTPUT_ABORT;
  }

  // Successful configure clears any previous display-reset retry window.
  m_displayReset = false;
  m_rendererConfigureRetryStart = std::chrono::steady_clock::time_point::min();

  // try to calculate the framerate
  m_ptsTracker.Add(picture.pts);
  if (!m_stalled)
    CalcFrameRate();

  // signal to clock what our framerate is, it may want to adjust it's
  // speed to better match with our video renderer's output speed
  m_pClock->UpdateFramerate(m_fFrameRate);

  // calculate the time we need to delay this picture before displaying
  double iPlayingClock, iCurrentClock;

  iPlayingClock = m_pClock->GetClock(iCurrentClock, false); // snapshot current clock

  if (picture.iFlags & DVP_FLAG_DROPPED)
  {
    m_droppingStats.AddOutputDropGain(picture.pts, 1);
    CLog::Log(LOGDEBUG, "{} - dropped in output", __FUNCTION__);

    return OUTPUT_DROPPED;
  }

  auto timeToDisplay = std::chrono::milliseconds(DVD_TIME_TO_MSEC(picture.pts - iPlayingClock));

  // make sure waiting time is not negative
  // For HW decoders, use a lower minimum wait (10ms) since rendering is just
  // a buffer release — no GPU work needed. This reduces latency for late frames.
  const auto minWait = m_processInfo.IsVideoHwDecoder() ? 10ms : 50ms;
  std::chrono::milliseconds maxWaitTime = std::min(std::max(timeToDisplay + 500ms, minWait), 500ms);
  // don't wait when going ff
  if (m_speed > DVD_PLAYSPEED_NORMAL)
    maxWaitTime = std::max(timeToDisplay, 0ms);

  m_swBlockArmed = CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
                   CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO);
  if (m_swBlockArmed)
  {
    const int vq = m_messageQueue.GetLevel();
    m_swVqMin = (m_swVqMin < 0) ? vq : std::min(m_swVqMin, vq);
    m_swVqMax = (m_swVqMax < 0) ? vq : std::max(m_swVqMax, vq);
  }
  else
  {
    m_swBlockStamp = {};
  }
  const auto waitEnter = m_swBlockArmed ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};

  int buffer = m_renderManager.WaitForBuffer(m_bAbortOutput, maxWaitTime);

  if (m_swBlockArmed)
    m_swWaitUs += std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - waitEnter)
                      .count();
  LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000, "ttd:{:d}ms pts:{:.3f} Clock:{:.3f} Level:{:d}",
    timeToDisplay.count(), picture.pts / DVD_TIME_BASE, static_cast<double>(iPlayingClock) / DVD_TIME_BASE, buffer);
  if (timeToDisplay.count() <= 0 && buffer >= 0)
    BDSTAGE::PictureShown();

  if (buffer < 0)
  {
    // The render buffer pool can fill up in trick-play FF, with more new pictures decoded than
    // are taken out at the display rate.
    // Drop the picture to let the renderer naturally drain the pool and make room for next iteration.
    // It will receive the most current new picture instead of whatever was decoded first (likely stale)
    if (m_speed > DVD_PLAYSPEED_NORMAL)
    {
      m_droppingStats.AddOutputDropGain(picture.pts, 1);
      return OUTPUT_DROPPED;
    }

    m_playbackStalled = true;

    if (m_speed != DVD_PLAYSPEED_PAUSE)
      CLog::Log(LOGWARNING, "{} - timeout waiting for buffer", __FUNCTION__);

    if (m_swBlockArmed)
      m_swBlockAgain++;

    return OUTPUT_AGAIN;
  }

  ProcessOverlays(picture, picture.pts);

  EINTERLACEMETHOD deintMethod = videoSettings.m_InterlaceMethod;
  if (!m_processInfo.Supports(deintMethod))
    deintMethod = m_processInfo.GetDeinterlacingMethodDefault();

  const auto addEnter = m_swBlockArmed ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
  const bool added = m_renderManager.AddVideoPicture(picture, m_bAbortOutput, deintMethod,
                                                     (m_syncState == ESyncState::SYNC_STARTING));
  if (m_swBlockArmed)
  {
    const auto addNow = std::chrono::steady_clock::now();
    m_swAddUs += std::chrono::duration_cast<std::chrono::microseconds>(addNow - addEnter).count();
    if (m_swBlockStamp.time_since_epoch().count() == 0)
    {
      m_swBlockStamp = addNow;
      m_swQWaitUs = 0;
      m_swWaitUs = 0;
      m_swAddUs = 0;
      m_swVqMin = -1;
      m_swVqMax = -1;
      m_swBlockAgain = 0;
    }
    else if (addNow - m_swBlockStamp >= std::chrono::seconds(1))
    {
      const double secs = std::chrono::duration<double>(addNow - m_swBlockStamp).count();
      const int64_t windowUs = static_cast<int64_t>(secs * 1000000.0);
      const int64_t otherUs =
          std::max<int64_t>(0, windowUs - m_swQWaitUs - m_swWaitUs - m_swAddUs);
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "swblock: win={:.2f} vq={}-{} qwaitUs={} waitUs={} addUs={} otherUs={} "
                    "blocked={:.1f}% idle={:.1f}% again={} dropped={}",
                    secs, m_swVqMin, m_swVqMax, m_swQWaitUs, m_swWaitUs, m_swAddUs, otherUs,
                    100.0 * (m_swWaitUs + m_swAddUs) / (secs * 1000000.0),
                    100.0 * m_swQWaitUs / (secs * 1000000.0), m_swBlockAgain, m_swBlockDropped);
      m_swBlockStamp = addNow;
      m_swQWaitUs = 0;
      m_swWaitUs = 0;
      m_swAddUs = 0;
      m_swBlockAgain = 0;
      m_swBlockDropped = 0;
      m_swVqMin = -1;
      m_swVqMax = -1;
    }
  }

  if (!added)
  {
    m_playbackStalled = true;
    m_droppingStats.AddOutputDropGain(picture.pts, 1);
    if (m_swBlockArmed)
      m_swBlockDropped++;
    return OUTPUT_DROPPED;
  }
  m_playbackStalled = false;

  return OUTPUT_NORMAL;
}

std::string CVideoPlayerVideo::GetPlayerInfo()
{
  int width, height;
  m_processInfo.GetVideoDimensions(width, height);
  int level, dataLevel;
  m_messageQueue.GetLevels(level, dataLevel);
  std::ostringstream s;
  s << "vq:"   << std::setw(2) << std::min(99, level) << "% (" << std::setw(2) << std::min(99, dataLevel) << "%)";  s << ", Mb/s:" << std::fixed << std::setprecision(2) << (double)GetVideoBitrate() / (1024.0*1024.0);
  s << ", dc:"   << m_processInfo.GetVideoDecoderName().c_str();
  s << ", " << width << "x" << height << (m_processInfo.GetVideoInterlaced() ? "i" : "p") << " [" << std::setprecision(2) << m_processInfo.GetVideoDAR() << "]@" << std::fixed << std::setprecision(3) << m_processInfo.GetVideoFps() << ", deint:" << m_processInfo.GetVideoDeintMethod();
  s << ", drop:" << m_iDroppedFrames;
  s << ", skip:" << m_renderManager.GetSkippedFrames();

  int pc = m_ptsTracker.GetPatternLength();
  if (pc > 0)
    s << ", pc:" << pc;
  else
    s << ", pc:none";

  return s.str();
}

int CVideoPlayerVideo::GetVideoBitrate()
{
  return (int)m_videoStats.GetBitrate();
}

void CVideoPlayerVideo::ResetFrameRateCalc()
{
  const int videoFpsDetect =
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoFpsDetect;

  m_fStableFrameRate = 0.0;
  m_iFrameRateCount = 0;
  m_iFrameRateLength = 1;
  m_iFrameRateErr = 0;
  m_bAllowDrop = videoFpsDetect == 0;
}

double CVideoPlayerVideo::GetCurrentPts()
{
  double renderPts = m_renderManager.GetRenderPts();

  if (renderPts == DVD_NOPTS_VALUE)
    return DVD_NOPTS_VALUE;
  else if (m_stalled)
    return DVD_NOPTS_VALUE;
  else if (m_speed == DVD_PLAYSPEED_NORMAL)
  {
    if (renderPts < 0)
      renderPts = 0;
  }
  return renderPts;
}

#define MAXFRAMERATEDIFF   0.01
#define MAXFRAMESERR    1000

void CVideoPlayerVideo::ResolveTelecineProbe(double& frametime, bool timedOut)
{
  m_telecineProbe = 0;

  if (m_telecineTwoFieldPackets > 0 && m_hints.fpsrate_doubled &&
      !m_processInfo.IsVideoHwDecoder())
  {
    m_halvedFieldRate = m_fFrameRate;
    m_fFrameRate /= 2.0;
    frametime = (double)DVD_TIME_BASE / m_fFrameRate;
    m_processInfo.SetVideoInterlaced(true);
    m_processInfo.SetVideoFps(static_cast<float>(m_fFrameRate));
    logM(LOGINFO,
         "no three-field packet: this stream is truly interlaced and the software decoder emits "
         "frames, so playing at {:.3f} fps and displaying at {:.3f}",
         m_fFrameRate, m_halvedFieldRate);
    return;
  }

  logComponentM(LOGDEBUG, LOGVIDEO, "soft telecine probe: {}, keeping {:.3f} fps",
                timedOut ? "no verdict within 500 ms" : "no three-field packet seen", m_fFrameRate);
}

void CVideoPlayerVideo::CalcFrameRate()
{
  const int videoFpsDetect =
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoFpsDetect;

  if (m_iFrameRateLength >= 128 || videoFpsDetect == 0)
    return; //don't calculate the fps

  if (!m_ptsTracker.HasFullBuffer())
    return; //we can only calculate the frameduration if m_pullupCorrection has a full buffer

  //see if m_pullupCorrection was able to detect a pattern in the timestamps
  //and is able to calculate the correct frame duration from it
  double frameduration = m_ptsTracker.GetFrameDuration();

  if (m_ptsTracker.VFRDetection())
    frameduration = m_ptsTracker.GetMinFrameDuration();

  if ((frameduration==DVD_NOPTS_VALUE) ||
      ((videoFpsDetect == 1 || m_hints.bluray) &&
       ((m_ptsTracker.GetPatternLength() > 1) && !m_ptsTracker.VFRDetection())))
  {
    //reset the stored framerates if no good framerate was detected
    m_fStableFrameRate = 0.0;
    m_iFrameRateCount = 0;
    m_iFrameRateErr++;

    if (m_iFrameRateErr == MAXFRAMESERR && m_iFrameRateLength == 1)
    {
      CLog::Log(LOGDEBUG,
                "{} counted {} frames without being able to calculate the framerate, giving up",
                __FUNCTION__, m_iFrameRateErr);
      m_bAllowDrop = true;
      m_iFrameRateLength = 128;
    }
    return;
  }

  double framerate = DVD_TIME_BASE / frameduration;

  //store the current calculated framerate if we don't have any yet
  if (m_iFrameRateCount == 0)
  {
    m_fStableFrameRate = framerate;
    m_iFrameRateCount++;
  }
  //check if the current detected framerate matches with the stored ones
  else if (fabs(m_fStableFrameRate / m_iFrameRateCount - framerate) <= MAXFRAMERATEDIFF)
  {
    m_fStableFrameRate += framerate; //store the calculated framerate
    m_iFrameRateCount++;

    //if we've measured m_iFrameRateLength seconds of framerates,
    if (m_iFrameRateCount >= MathUtils::round_int(framerate) * m_iFrameRateLength)
    {
      //store the calculated framerate if it differs too much from m_fFrameRate
      if (fabs(m_fFrameRate - (m_fStableFrameRate / m_iFrameRateCount)) > MAXFRAMERATEDIFF || m_bFpsInvalid)
      {
        double calculated = m_fStableFrameRate / m_iFrameRateCount;
        bool skipHalving = (m_hints.codecOptions & CODEC_INTERLACED) &&
                           m_processInfo.IsVideoHwDecoder() &&
                           calculated > 24.5 &&
                           !(m_hints.width > 1920 || m_hints.height > 1080) &&
                           fabs(m_fFrameRate - 2.0 * calculated) < MAXFRAMERATEDIFF;
        if (skipHalving)
        {
          LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000,
                        "skipping halve: interlaced stream, keeping fps {:f} (measured {:f})",
                        m_fFrameRate, calculated);
        }
        else
        {
          logM(LOGDEBUG, "framerate was:{:f} calculated:{:f}", m_fFrameRate, calculated);
          m_fFrameRate = calculated;
          m_telecine = false;
          m_bFpsInvalid = false;
          if (m_hints.width > 1920 || m_hints.height > 1080 || calculated <= 24.5)
            m_processInfo.SetVideoInterlaced(false);
          m_processInfo.SetVideoFps(static_cast<float>(m_fFrameRate));
        }
      }

      //reset the stored framerates
      m_fStableFrameRate = 0.0;
      m_iFrameRateCount = 0;
      m_iFrameRateLength *= 2; //double the length we should measure framerates

      //we're allowed to drop frames because we calculated a good framerate
      m_bAllowDrop = true;
    }
  }
  else //the calculated framerate didn't match, reset the stored ones
  {
    m_fStableFrameRate = 0.0;
    m_iFrameRateCount = 0;
  }
}

int CVideoPlayerVideo::CalcDropRequirement(double pts)
{
  int result = 0;
  int lateframes;
  double iDecoderPts, iRenderPts;
  int iSkippedPicture = -1;
  int iDroppedFrames = -1;
  int iBufferLevel;
  int queued, discard;

  m_droppingStats.m_lastPts = pts;

  // get decoder stats
  if (!m_pVideoCodec->GetCodecStats(iDecoderPts, iDroppedFrames, iSkippedPicture))
    iDecoderPts = pts;
  if (iDecoderPts == DVD_NOPTS_VALUE)
    iDecoderPts = pts;

  // get render stats
  m_renderManager.GetStats(lateframes, iRenderPts, queued, discard);
  iBufferLevel = queued + discard;

  if (iBufferLevel < 0)
    result |= DROP_BUFFER_LEVEL;
  else if (iBufferLevel < 2 && !m_processInfo.IsVideoHwDecoder())
  {
    // For HW decoders (AML), a render queue of 1 is normal — the HW compositor
    // pulls frames at vsync. Don't trigger HURRY/DROP for a normally-operating
    // HW pipeline. Only flag low buffer for SW decoders that can actually hurry.
    result |= DROP_BUFFER_LEVEL;
    CLog::Log(LOGDEBUG, LOGVIDEO, "CVideoPlayerVideo::CalcDropRequirement - hurry: {}",
              iBufferLevel);
  }

  if (m_bAllowDrop)
  {
    if (iSkippedPicture > 0)
    {
      CDroppingStats::CGain gain;
      gain.frames = iSkippedPicture;
      gain.pts = iDecoderPts;
      m_droppingStats.m_gain.push_back(gain);
      m_droppingStats.m_totalGain += gain.frames;
      result |= DROP_DROPPED;
      CLog::Log(LOGDEBUG, LOGVIDEO,
                "CVideoPlayerVideo::CalcDropRequirement - dropped pictures, lateframes: {}, "
                "Bufferlevel: {}, dropped: {}",
                lateframes, iBufferLevel, iSkippedPicture);
    }
    if (iDroppedFrames > 0)
    {
      CDroppingStats::CGain gain;
      gain.frames = iDroppedFrames;
      gain.pts = iDecoderPts;
      m_droppingStats.m_gain.push_back(gain);
      m_droppingStats.m_totalGain += iDroppedFrames;
      result |= DROP_DROPPED;
      CLog::Log(LOGDEBUG, LOGVIDEO,
                "CVideoPlayerVideo::CalcDropRequirement - dropped in decoder, lateframes: {}, "
                "Bufferlevel: {}, dropped: {}",
                lateframes, iBufferLevel, iDroppedFrames);
    }
  }

  // subtract gains
  while (!m_droppingStats.m_gain.empty() &&
         iRenderPts >= m_droppingStats.m_gain.front().pts)
  {
    m_droppingStats.m_totalGain -= m_droppingStats.m_gain.front().frames;
    m_droppingStats.m_gain.pop_front();
  }

  // calculate lateness
  int lateness = lateframes - m_droppingStats.m_totalGain;

  if (lateness > 0 && m_speed)
  {
    result |= DROP_VERYLATE;
  }
  return result;
}

void CDroppingStats::Reset()
{
  m_gain.clear();
  m_totalGain = 0;
}

void CDroppingStats::AddOutputDropGain(double pts, int frames)
{
  CDroppingStats::CGain gain;
  gain.frames = frames;
  gain.pts = pts;
  m_gain.push_back(gain);
  m_totalGain += frames;
}
