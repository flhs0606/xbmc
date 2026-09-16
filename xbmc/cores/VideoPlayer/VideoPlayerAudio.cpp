/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerAudio.h"

#include "DVDCodecs/Audio/DVDAudioCodec.h"
#include "DVDCodecs/Audio/DVDAudioCodecPassthrough.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "ServiceBroker.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Utils/AEStreamInfo.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "settings/Settings.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/SettingsManager.h"
#include "utils/AudioDelayTrace.h"
#include "utils/AudioSyncReset.h"
#include "utils/MathUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include "utils/AMLUtils.h"
#include "ServiceBroker.h"

#include <algorithm>
#include <mutex>
#include <set>

#ifdef TARGET_RASPBERRY_PI
#include "platform/linux/RBP.h"
#endif

#include <sstream>
#include <iomanip>
#include <math.h>

#include <unistd.h>

using namespace std::chrono_literals;

//==============================================================================
// LAV PTS validation utilities (same as DVDAudioCodecPassthrough.cpp)
//==============================================================================
// Sentinel for invalid PTS (avoids confusion with garbage values like DVD_NOPTS_VALUE cast to double)
constexpr double LOCAL_NOPTS = -1.0;

// Maximum reasonable PTS value (24 hours in DVD_TIME_BASE units)
constexpr double MAX_REASONABLE_PTS = 86400.0 * DVD_TIME_BASE;

// Check if a PTS value is valid (not sentinel and not garbage)
inline bool IsValidPts(double pts) {
    return (pts >= 0.0) && (pts <= MAX_REASONABLE_PTS);
}


constexpr int EAC3_FULL_FRAME_SAMPLES = 1536;

namespace
{
std::string FormatObjectAudioDescription(const CAEStreamInfo& streamInfo)
{
  if (streamInfo.m_hasDtsX && streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_MA)
  {
    if (streamInfo.m_atmosChannels == 0)
      return "DTS:X";

    if (streamInfo.m_atmosObjects > 0)
      return StringUtils::Format("DTS:X [{} bed ch, {} obj]", streamInfo.m_atmosChannels,
                                 streamInfo.m_atmosObjects);

    return StringUtils::Format("DTS:X [{} bed ch]", streamInfo.m_atmosChannels);
  }

  if (!streamInfo.m_hasAtmos)
    return {};

  if (streamInfo.m_type != CAEStreamInfo::STREAM_TYPE_TRUEHD &&
      streamInfo.m_type != CAEStreamInfo::STREAM_TYPE_EAC3)
    return {};

  if (streamInfo.m_atmosChannels == 0)
    return "Atmos";

  if (streamInfo.m_atmosObjects > 0 && streamInfo.m_bedChannels > 0)
    return StringUtils::Format("Atmos [{} pres ch, {} bed ch, {} obj]", streamInfo.m_atmosChannels,
                               streamInfo.m_bedChannels, streamInfo.m_atmosObjects);

  if (streamInfo.m_atmosObjects > 0 && streamInfo.m_bedIsLfeOnly)
    return StringUtils::Format("Atmos [{} pres ch, LFE Bed, {} obj]", streamInfo.m_atmosChannels,
                               streamInfo.m_atmosObjects);

  if (streamInfo.m_atmosObjects > 0)
    return StringUtils::Format("Atmos [{} pres ch, {} obj]", streamInfo.m_atmosChannels,
                               streamInfo.m_atmosObjects);

  return StringUtils::Format("Atmos [{} pres ch]", streamInfo.m_atmosChannels);
}

std::string FormatDialNormDescription(const CAEStreamInfo& streamInfo)
{
  if (!streamInfo.m_hasDialNorm)
    return {};

  return StringUtils::Format("{} dB [COLOR FF404040]|[/COLOR] applied {} dB",
                             streamInfo.m_dialNorm, streamInfo.m_dialNormApplied);
}
}

class CDVDMsgAudioCodecChange : public CDVDMsg
{
public:
  CDVDMsgAudioCodecChange(const CDVDStreamInfo& hints, std::unique_ptr<CDVDAudioCodec> codec)
    : CDVDMsg(GENERAL_STREAMCHANGE), m_codec(std::move(codec)), m_hints(hints)
  {}
  ~CDVDMsgAudioCodecChange() override = default;

  std::unique_ptr<CDVDAudioCodec> m_codec;
  CDVDStreamInfo  m_hints;
};


CVideoPlayerAudio::CVideoPlayerAudio(
    CDVDClock* pClock,
    CDVDMessageQueue& parent,
    CRenderManager& renderManager,
    CProcessInfo &processInfo,
    double messageQueueTimeSize)
: CThread("VideoPlayerAudio"), IDVDStreamPlayerAudio(processInfo)
, m_messageQueue("audio")
, m_messageParent(parent)
, m_renderManager(renderManager)
, m_audioSink(pClock)
{
  m_pClock = pClock;
  m_audioClock = 0;
  m_speed = DVD_PLAYSPEED_NORMAL;
  m_stalled = true;
  m_paused = false;
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_synctype = SYNC_DISCON;
  m_prevsynctype = -1;
  m_prevskipped = false;
  m_maxspeedadjust = 0.0;

  // allows max bitrate of 18 Mbit/s (TrueHD max peak) during m_messageQueueTimeSize seconds
  m_messageQueue.SetMaxDataSize(18 * messageQueueTimeSize / 8 * 1024 * 1024);
  m_messageQueue.SetMaxTimeSize(messageQueueTimeSize);

  m_disconAdjustTimeMs = processInfo.GetMaxPassthroughOffSyncDuration();

  std::set<std::string> settingSet = {CSettings::SETTING_AUDIOOUTPUT_AC3PASSTHROUGH,
                                      CSettings::SETTING_AUDIOOUTPUT_AC3TRANSCODE,
                                      CSettings::SETTING_AUDIOOUTPUT_EAC3PASSTHROUGH,
                                      CSettings::SETTING_AUDIOOUTPUT_DTSPASSTHROUGH,
                                      CSettings::SETTING_AUDIOOUTPUT_DTSHDPASSTHROUGH,
                                      CSettings::SETTING_AUDIOOUTPUT_TRUEHDPASSTHROUGH,
                                      CSettings::SETTING_AUDIOOUTPUT_DTSHDCOREFALLBACK};
  CServiceBroker::GetSettingsComponent()->GetSettings()->GetSettingsManager()->RegisterCallback(
      this, settingSet);
}

CVideoPlayerAudio::~CVideoPlayerAudio()
{
  CServiceBroker::GetSettingsComponent()->GetSettings()->GetSettingsManager()->UnregisterCallback(
      this);

  StopThread();

  // close the stream, and don't wait for the audio to be finished
  // CloseStream(true);
}

void CVideoPlayerAudio::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (setting)
    m_audioSettingsChanged = true;
}

bool CVideoPlayerAudio::OpenStream(CDVDStreamInfo hints)
{
  CLog::Log(LOGDEBUG, "Finding audio codec for: {}", hints.codec);
  m_audioSettingsChanged = false;
  bool allowpassthrough = true;

  CAEStreamInfo::DataType streamType =
      m_audioSink.GetPassthroughStreamType(hints.codec, hints.samplerate, hints.profile);
  std::unique_ptr<CDVDAudioCodec> codec = CDVDFactoryCodec::CreateAudioCodec(
      hints, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode(), streamType);
  if(!codec)
  {
    CLog::Log(LOGERROR, "Unsupported audio codec");
    return false;
  }

  if(m_messageQueue.IsInited())
    m_messageQueue.Put(std::make_shared<CDVDMsgAudioCodecChange>(hints, std::move(codec)), 0);
  else
  {
    OpenStream(hints, std::move(codec));
    m_messageQueue.Init();
    CLog::Log(LOGDEBUG, "Creating audio thread");
    Create();
  }
  return true;
}

void CVideoPlayerAudio::OpenStream(CDVDStreamInfo& hints, std::unique_ptr<CDVDAudioCodec> codec)
{
  m_lastAudioObjects = -1;
  m_lastAudioObjectChannels = -1;
  m_lastAudioBedChannels = -1;
  m_lastAudioObjectDescription.clear();
  m_lastAudioDialNorm.clear();
  m_processInfo.SetAudioObjectCount(-1);
  m_processInfo.SetAudioObjectChannels(-1);
  m_processInfo.SetAudioBedChannels(-1);
  m_dataCacheCore.SetAudioObjectDescription({});
  m_dataCacheCore.SetAudioDialNorm({});
  m_pAudioCodec = std::move(codec);
  m_forcePcmEac3 = false;
  m_eac3SmallSpfLimit = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      CSettings::SETTING_COREELEC_AUDIO_EAC3_SMALL_SPF_PCM);

  //============================================================================
  // LAV A/V Sync Enablement
  //============================================================================
  // LAV Full (internal clock + jitter tracking + seamless branch)
  // LAV SB (seamless branch fix ONLY)
  const int algoValue = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
                                    CSettings::SETTING_COREELEC_AMLOGIC_DV_AUDIO_SEAMLESSBRANCH);
  m_seamlessBranchAlgo = algoValue;
  const bool realtimeStream = m_processInfo.IsRealtimeStream();
  if (realtimeStream && algoValue != 0)
    logM(LOGDEBUG, "CVideoPlayerAudio::OpenStream - realtime stream, LAV audio sync disabled");
  const bool enableLavFull = !realtimeStream && ((algoValue == 3) || (algoValue == 5));
  const bool enableLavSeamlessBranch =
      !realtimeStream && ((algoValue != 0) && (algoValue != 3) && (algoValue != 5));

  // Enable LAV sync for passthrough codec if applicable
  // For passthrough: enable in the codec, NOT in this class (PCM sync would interfere)
  // For PCM/decoded: enable in this class
  if (m_pAudioCodec->NeedPassthrough())
  {
    m_lavStylePcmSyncEnabled = false;  // Passthrough has its own LAV sync in codec
    m_lavFullSyncEnabled = false;
    CDVDAudioCodecPassthrough* passthroughCodec =
        dynamic_cast<CDVDAudioCodecPassthrough*>(m_pAudioCodec.get());
    if (passthroughCodec)
    {
      // Full LAV sync, Seamless branch only
      passthroughCodec->SetLavStyleSyncEnabled(enableLavFull);
      if (enableLavSeamlessBranch && !enableLavFull)
        passthroughCodec->SetLavSeamlessBranchEnabled(true);
      m_lavFullSyncEnabled = enableLavFull;
      
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio::OpenStream - LAV passthrough: {}",
                enableLavFull ? "FULL" : 
                (enableLavSeamlessBranch ? "SEAMLESS BRANCH ONLY" : "disabled"));

      // If we're already in sync (codec recreation during playback, e.g., display reset),
      // sync the new codec to the master clock immediately. This prevents the codec from
      // syncing to demuxer PTS which may be offset from where video actually is.
      // NOTE: Only for LAV Full which uses internal clock
      if (enableLavFull && m_syncState == IDVDStreamPlayer::SYNC_INSYNC && m_pClock)
      {
        double masterClock = m_pClock->GetClock();
        double audioDelay = m_audioSink.GetDelay();
        double syncPts = masterClock + audioDelay;
        passthroughCodec->SyncToResyncPts(syncPts);
        CLog::Log(LOGDEBUG, "CVideoPlayerAudio::OpenStream - Synced new codec to master clock {:.3f}s (delay {:.3f}s)",
                  masterClock / DVD_TIME_BASE, audioDelay / DVD_TIME_BASE);
      }
    }
  }
  else
  {
    m_lavStylePcmSyncEnabled = enableLavFull;  // PCM uses this class for LAV sync
    m_lavFullSyncEnabled = enableLavFull;
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio::OpenStream - LAV PCM sync {}",
              enableLavFull ? "ENABLED" : "disabled");
  }
  /* store our stream hints */
  m_streaminfo = hints;
  m_streaminfoOrig = hints;

  /* update codec information from what codec gave out, if any */
  int channelsFromCodec   = m_pAudioCodec->GetFormat().m_channelLayout.Count();
  int samplerateFromCodec = m_pAudioCodec->GetFormat().m_sampleRate;

  if (channelsFromCodec > 0)
    m_streaminfo.channels = channelsFromCodec;
  if (samplerateFromCodec > 0)
    m_streaminfo.samplerate = samplerateFromCodec;

  /* check if we only just got sample rate, in which case the previous call
   * to CreateAudioCodec() couldn't have started passthrough */
  if (hints.samplerate != m_streaminfo.samplerate)
    SwitchCodecIfNeeded();

  m_audioClock = 0;
  m_stalled = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;

  m_prevsynctype = -1;
  m_createFailures = 0;
  m_createRetryTimer.SetExpired();
  const bool useDisplayAsClock = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
      CSettings::SETTING_COREELEC_AMLOGIC_USE_DISPLAY_AS_CLOCK);
  m_synctype = (!m_pAudioCodec->NeedPassthrough() && !enableLavFull &&
                (m_processInfo.IsRealtimeStream() || useDisplayAsClock))
                   ? SYNC_RESAMPLE
                   : SYNC_DISCON;

  m_audioSink.SetHybridFirstCycleInterval(
      (!m_processInfo.IsRealtimeStream() && hints.codec == AV_CODEC_ID_TRUEHD)
          ? std::chrono::milliseconds(200)
          : std::chrono::milliseconds(0));

  if (m_synctype == SYNC_DISCON)
    CLog::LogF(LOGDEBUG, "Allowing max Out-Of-Sync Value of {} ms", m_disconAdjustTimeMs);

  m_prevskipped = false;

  m_maxspeedadjust = 5.0;

  m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;

  // LAV: Reset PCM jitter tracking on stream open
  if (m_lavStylePcmSyncEnabled)
  {
    m_pcmJitterTracker.Reset();
    m_pcmOutputClock = LOCAL_NOPTS;
    m_pcmResyncTimestamp = true;
  }
}

void CVideoPlayerAudio::CloseStream(bool bWaitForBuffers)
{
  bool bWait = bWaitForBuffers && m_speed > 0 && !CServiceBroker::GetActiveAE()->IsSuspended();

  // wait until buffers are empty
  if (bWait)
    m_messageQueue.WaitUntilEmpty();

  // send abort message to the audio queue
  m_messageQueue.Abort();

  CLog::Log(LOGDEBUG, "Waiting for audio thread to exit");

  // shut down the adio_decode thread and wait for it
  StopThread(); // will set this->m_bStop to true

  m_processInfo.SetAudioObjectCount(-1);
  m_processInfo.SetAudioObjectChannels(-1);
  m_processInfo.SetAudioBedChannels(-1);

  // destroy audio device
  CLog::Log(LOGDEBUG, "Closing audio device");
  if (bWait)
  {
    m_bStop = false;
    m_audioSink.Drain();
    m_bStop = true;
  }
  else
  {
    m_audioSink.Flush();
  }

  m_audioSink.Destroy(true);

  // uninit queue
  m_messageQueue.End();

  CLog::Log(LOGDEBUG, "Deleting audio codec");
  if (m_pAudioCodec)
  {
    m_pAudioCodec->Dispose();
    m_pAudioCodec.reset();
  }

  m_lastAudioObjectDescription.clear();
  m_lastAudioDialNorm.clear();
  m_lastAudioObjects = -1;
  m_lastAudioObjectChannels = -1;
  m_lastAudioBedChannels = -1;
  m_dataCacheCore.SetAudioObjectDescription({});
  m_dataCacheCore.SetAudioDialNorm({});

  std::ostringstream s;
  SInfo info;
  info.info        = s.str();
  info.pts         = DVD_NOPTS_VALUE;
  info.passthrough = false;

  { std::unique_lock<CCriticalSection> lock(m_info_section);
    m_info = info;
  }
}

void CVideoPlayerAudio::OnStartup()
{
}

void CVideoPlayerAudio::UpdatePlayerInfo()
{
  if (!m_playerInfoTimer.IsTimePast())
    return;
  m_playerInfoTimer.Set(167ms);

  int level, dataLevel;
  m_messageQueue.GetLevels(level, dataLevel);
  std::ostringstream s;
  s << "aq:"     << std::setw(2) << std::min(99, level) << "% (" << std::setw(2)
    << std::min(99, dataLevel) << "%)";
  s << ", Kb/s:" << std::fixed << std::setprecision(2) << m_audioStats.GetBitrate() / 1024.0;
  s << ", ac:"   << m_processInfo.GetAudioDecoderName().c_str();
  if (!m_info.passthrough)
    s << ", chan:" << m_processInfo.GetAudioChannels().c_str();
  s << ", " << m_streaminfo.samplerate/1000 << " kHz";

  // print a/v discontinuity adjustments counter when audio is not resampled (passthrough mode)
  if (m_synctype == SYNC_DISCON)
    s << ", a/v corrections (" << m_disconAdjustTimeMs << "ms): " << m_disconAdjustCounter;

  //print the inverse of the resample ratio, since that makes more sense
  //if the resample ratio is 0.5, then we're playing twice as fast
  else if (m_synctype == SYNC_RESAMPLE)
    s << ", rr:" << std::fixed << std::setprecision(5) << 1.0 / m_audioSink.GetResampleRatio();

  SInfo info;
  info.info        = s.str();
  info.pts         = m_audioSink.GetPlayingPts();
  info.absClock    = m_pClock->GetAbsoluteClock();
  info.cache       = m_audioSink.GetCacheTime() * DVD_TIME_BASE;
  info.delay       = m_audioSink.GetDelay();
  info.passthrough = m_pAudioCodec && m_pAudioCodec->NeedPassthrough();

  {
    std::unique_lock<CCriticalSection> lock(m_info_section);
    m_info = info;
  }

  m_dataCacheCore.SetAudioLiveBitRate(m_audioStats.GetBitrate());
  m_dataCacheCore.SetAudioQueueLevel(std::min(99, level));
  m_dataCacheCore.SetAudioQueueDataLevel(std::min(99, dataLevel));

  if (info.passthrough)
  {
    const CAEStreamInfo& streamInfo = m_pAudioCodec->GetFormat().m_streamInfo;
    std::string objectDescription = FormatObjectAudioDescription(streamInfo);
    std::string dialNorm = FormatDialNormDescription(streamInfo);

    if (objectDescription != m_lastAudioObjectDescription)
    {
      m_lastAudioObjectDescription = objectDescription;
      m_dataCacheCore.SetAudioObjectDescription(std::move(objectDescription));
    }
    if (dialNorm != m_lastAudioDialNorm)
    {
      m_lastAudioDialNorm = dialNorm;
      m_dataCacheCore.SetAudioDialNorm(std::move(dialNorm));
    }
  }
  else if (!m_lastAudioObjectDescription.empty() || !m_lastAudioDialNorm.empty())
  {
    m_lastAudioObjectDescription.clear();
    m_lastAudioDialNorm.clear();
    m_dataCacheCore.SetAudioObjectDescription({});
    m_dataCacheCore.SetAudioDialNorm({});
  }
}

void CVideoPlayerAudio::Process()
{
  logComponentM(LOGDEBUG, LOGAUDIO, "running thread: CVideoPlayerAudio::Process()");

  DVDAudioFrame audioframe;
  audioframe.nb_frames = 0;
  audioframe.framesOut = 0;
  m_audioStats.Start();
  m_disconAdjustCounter = 0;

  bool onlyPrioMsgs = false;

  while (!m_bStop)
  {
    std::shared_ptr<CDVDMsg> pMsg;
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double, std::ratio<1>>(m_audioSink.GetCacheTime()));

    // read next packet and return -1 on error
    int priority = 1;
    //Do we want a new audio frame?
    if (m_syncState == IDVDStreamPlayer::SYNC_STARTING ||              /* when not started */
        m_processInfo.IsTempoAllowed(static_cast<float>(m_speed)/DVD_PLAYSPEED_NORMAL) ||
        m_speed <  DVD_PLAYSPEED_PAUSE  || /* when rewinding */
        (m_speed >  DVD_PLAYSPEED_NORMAL && m_audioClock < m_pClock->GetClock())) /* when behind clock in ff */
      priority = 0;

    if (m_syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
      priority = 1;

    if (m_paused)
      priority = 1;

    if (onlyPrioMsgs)
    {
      priority = 1;
      timeout = 0ms;
    }

    MsgQueueReturnCode ret = m_messageQueue.Get(pMsg, timeout, priority);

    onlyPrioMsgs = false;

    if (MSGQ_IS_ERROR(ret))
    {
      if (!m_messageQueue.ReceivedAbortRequest())
        logM(LOGERROR, "MSGQ_IS_ERROR returned true ({})", ret);

      break;
    }
    else if (ret == MSGQ_TIMEOUT)
    {
      if (ProcessDecoderOutput(audioframe))
      {
        onlyPrioMsgs = true;
        continue;
      }

      // if we only wanted priority messages, this isn't a stall
      if (priority)
        continue;

      if (m_processInfo.IsTempoAllowed(static_cast<float>(m_speed)/DVD_PLAYSPEED_NORMAL) &&
          !m_stalled && m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
      {
        // while AE sync is active, we still have time to fill buffers
        if (m_syncTimer.IsTimePast())
        {
          logComponentM(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio::Process - stream stalled");
          m_stalled = true;
        }
      }
      if (timeout == 0ms)
        CThread::Sleep(10ms);

      continue;
    }

    // handle messages
    if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if (std::static_pointer_cast<CDVDMsgGeneralSynchronize>(pMsg)->Wait(100ms, SYNCSOURCE_AUDIO))
        logComponentM(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio - CDVDMsg::GENERAL_SYNCHRONIZE");
      else
        m_messageQueue.Put(pMsg, 1); // push back as prio message, to process other prio messages
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    { //player asked us to set internal clock
      double pts = std::static_pointer_cast<CDVDMsgDouble>(pMsg)->m_value;
      logComponentM(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio - CDVDMsg::GENERAL_RESYNC({:.3f} level: {:d} cache:{:.3f}",
                pts / DVD_TIME_BASE, m_messageQueue.GetLevel(), m_audioSink.GetDelay() / DVD_TIME_BASE);

      double delay = m_audioSink.GetDelay();
      const bool willFlush = (pts > m_audioClock - delay + 0.5 * DVD_TIME_BASE);
      AUDIODELAY_LOG("VPA.GENERAL_RESYNC",
                     "pts={:.3f} prevSyncState={} audioClock={:.3f} delay={:.3f} willFlush={} speed={}",
                     pts / DVD_TIME_BASE,
                     static_cast<int>(m_syncState),
                     m_audioClock / DVD_TIME_BASE,
                     delay / DVD_TIME_BASE,
                     willFlush ? 1 : 0,
                     m_speed);
      if (pts > m_audioClock - delay + 0.5 * DVD_TIME_BASE)
      {
        m_audioSink.Flush();
      }
      m_audioClock = pts + delay;
      if (m_speed != DVD_PLAYSPEED_PAUSE)
        m_audioSink.Resume();
      m_syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_syncTimer.Set(3000ms);

      // LAV passthrough: Reset and sync codec's internal clock to RESYNC pts
      // This is THE critical sync point - RESYNC contains the coordinated A/V clock
      // IMPORTANT: Must call ResetLavSyncState() BEFORE SyncToResyncPts() to clear
      // any stale jitter values that could contaminate the new sync baseline
      if (m_pAudioCodec && m_pAudioCodec->NeedPassthrough())
      {
        CDVDAudioCodecPassthrough* passthroughCodec =
            dynamic_cast<CDVDAudioCodecPassthrough*>(m_pAudioCodec.get());
        if (passthroughCodec && passthroughCodec->IsLavStyleSyncEnabled())
        {
          passthroughCodec->ResetLavSyncState();  // Clear jitter tracker FIRST
          passthroughCodec->SyncToResyncPts(pts + delay);
          logComponentM(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio::RESYNC - Reset and synced passthrough codec to {:.3f}s",
                    (pts + delay) / DVD_TIME_BASE);
        }
      }
      // LAV PCM: reset output clock to resync with new PTS baseline
      else if (m_lavStylePcmSyncEnabled)
      {
        m_pcmOutputClock = LOCAL_NOPTS;
        m_pcmResyncTimestamp = true;
        m_pcmJitterTracker.Reset();
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
      m_audioSink.Flush();
      m_stalled = true;
      m_audioClock = 0;
      audioframe.nb_frames = 0;
      m_syncState = IDVDStreamPlayer::SYNC_STARTING;

      // LAV: Reset PCM jitter tracking on GENERAL_RESET
      if (m_lavStylePcmSyncEnabled)
      {
        m_pcmJitterTracker.Reset();
        m_pcmOutputClock = LOCAL_NOPTS;
        m_pcmResyncTimestamp = true;
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
    {
      bool sync = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      m_audioSink.Flush();
      m_stalled = true;
      m_audioClock = 0;
      audioframe.nb_frames = 0;

      if (sync)
      {
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        m_audioSink.Pause();
      }

      if (m_pAudioCodec)
        m_pAudioCodec->Reset();

      // LAV: Reset PCM jitter tracking on GENERAL_FLUSH
      if (m_lavStylePcmSyncEnabled)
      {
        m_pcmJitterTracker.Reset();
        m_pcmOutputClock = LOCAL_NOPTS;
        m_pcmResyncTimestamp = true;
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_EOF))
    {
      logComponentM(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio - CDVDMsg::GENERAL_EOF");
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      double speed = std::static_pointer_cast<CDVDMsgInt>(pMsg)->m_value;
      logComponentM(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio - CDVDMsg::PLAYER_SETSPEED: {:f} last: {:d}", speed, m_speed);

      if (m_processInfo.IsTempoAllowed(static_cast<float>(speed)/DVD_PLAYSPEED_NORMAL))
      {
        if (speed != m_speed)
        {
          if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
          {
            m_audioSink.Resume();
            m_stalled = false;
          }
        }
      }
      else
      {
        m_audioSink.Pause();
      }
      m_speed = (int)speed;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      auto msg = std::static_pointer_cast<CDVDMsgAudioCodecChange>(pMsg);
      OpenStream(msg->m_hints, std::move(msg->m_codec));
      msg->m_codec = nullptr;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_PAUSE))
    {
      m_paused = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      logComponentM(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio - CDVDMsg::GENERAL_PAUSE: {}", m_paused);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_REQUEST_STATE))
    {
      SStateMsg msg;
      msg.player = VideoPlayer_AUDIO;
      msg.syncState = m_syncState;
      m_messageParent.Put(
          std::make_shared<CDVDMsgType<SStateMsg>>(CDVDMsg::PLAYER_REPORT_STATE, msg));
    }
    else if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacket();
      bool bPacketDrop = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacketDrop();

      if (bPacketDrop)
      {
        if (m_syncState != IDVDStreamPlayer::SYNC_STARTING)
        {
          m_audioSink.Drain();
          m_audioSink.Flush();
          audioframe.nb_frames = 0;
        }
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        continue;
      }

      if (!m_processInfo.IsTempoAllowed(static_cast<float>(m_speed) / DVD_PLAYSPEED_NORMAL) &&
          m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
      {
        continue;
      }

      if (m_createFailures < 20U && !m_createRetryTimer.IsTimePast())
      {
        m_messageQueue.PutBack(pMsg);
        CThread::Sleep(10ms);
        continue;
      }

      if (!m_pAudioCodec->AddData(*pPacket))
      {
        m_messageQueue.PutBack(pMsg);
        onlyPrioMsgs = true;
        continue;
      }

      m_audioStats.AddSampleBytes(pPacket->iSize);
      UpdatePlayerInfo();

      if (ProcessDecoderOutput(audioframe))
      {
        onlyPrioMsgs = true;
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_DISPLAY_RESET))
    {
      m_displayReset = true;
    }
  }
}

bool CVideoPlayerAudio::ProcessDecoderOutput(DVDAudioFrame &audioframe)
{
  if (audioframe.nb_frames <= audioframe.framesOut)
  {
    if (m_createFailures < 20U && !m_createRetryTimer.IsTimePast())
      return false;

    audioframe.hasDownmix = false;

    m_pAudioCodec->GetData(audioframe);

    if (audioframe.nb_frames == 0)
    {
      return false;
    }

    // LAV: Initialize discontinuity fields for non-passthrough audio
    // (passthrough codec sets these, FFmpeg decoders don't)
    if (m_lavStylePcmSyncEnabled && !audioframe.passthrough)
    {
      audioframe.hasDiscontinuity = false;
      audioframe.discontinuityCorrection = 0.0;
    }

    audioframe.hasTimestamp = true;
    if (audioframe.pts == DVD_NOPTS_VALUE)
    {
      audioframe.pts = m_audioClock;
      audioframe.hasTimestamp = false;
    }
    else
    {
      const int algoValue = m_seamlessBranchAlgo;
      auto* advancedSettings = &CAudioSyncReset::GetInstance();
      const int algoForReset = advancedSettings->GetAlgoForReset();
      const int algoForResetSub = advancedSettings->GetAlgoForResetSub();
      double lastResetTimeSub = advancedSettings->GetLastResetTimeSub();
      const double currentTimeSub = m_pClock->GetAbsoluteClock() / 1000.0;
      if (lastResetTimeSub == 0.0)
      {
        lastResetTimeSub = currentTimeSub;
        advancedSettings->SetLastResetTimeSub(lastResetTimeSub);
      }
      if (((algoValue > 0) && (algoValue < 4)) || (algoForResetSub == 99))
      {
        if (advancedSettings->TakeResetSync())
        {
          m_audioSink.AbortAddPackets();
          m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_RESYNC));
          m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        }

        const bool resetSeek = advancedSettings->GetResetSeek();
        const bool resetSeekSub = advancedSettings->GetResetSeekSub();
        if ((resetSeek && (algoForReset != 0)) || (resetSeekSub && (algoForResetSub != 0)))
        {
          double iTimeValue = 0.0;
          double offsetValue = 0.0;
          bool performOffset = false;
          switch (algoValue)
          {
            case 1:
              iTimeValue = 2250.0;
              offsetValue = 1500.0;
              performOffset = false;
              break;
            case 2:
              iTimeValue = 5000.0;
              offsetValue = 2000.0;
              performOffset = true;
              break;
            case 3:
              iTimeValue = 2250.0;
              offsetValue = 1500.0;
              performOffset = false;
              break;
            default:
              break;
          }
          bool timeToReset = false;
          bool timeToResetSub = false;
          double offset = 0;
          double lastResetTime = advancedSettings->GetLastResetTime();
          const double currentTime = m_pClock->GetAbsoluteClock() / 1000.0;
          if (lastResetTime == 0.0)
          {
            lastResetTime = currentTime;
            advancedSettings->SetLastResetTime(lastResetTime);
          }
          double iTime = m_pClock->GetClock() / 1000.0;
          switch (algoForReset)
          {
            case 1:
              timeToReset = ((currentTime - lastResetTime) > 2250.0);
              offset = 1500.0;
              performOffset = true;
              break;
            case 2:
              timeToReset = (iTime > iTimeValue);
              offset = offsetValue;
              break;
            case 3:
              timeToReset = (iTime > 45000.0);
              offset = 10000.0;
              performOffset = true;
              break;
            default:
              break;
          }
          if (algoForResetSub == 99)
          {
            if ((currentTime - lastResetTimeSub) < 7000.0)
            {
              timeToResetSub = false;
              offset = 0.0;
              performOffset = false;
              advancedSettings->SetResetSeekSub(false);
              advancedSettings->SetLastResetTimeSub(0.0);
              advancedSettings->SetAlgoForResetSub(0);
            }
            else
            {
              timeToResetSub = true;
              offset = 1500.0;
              performOffset = false;
            }
          }
          if (timeToReset || timeToResetSub)
          {
            CDVDMsgPlayerSeek::CMode mode;
            mode.time = iTime - offset;
            mode.backward = true;
            mode.accurate = true;
            mode.trickplay = true;
            mode.sync = true;
            mode.restore = false;
            m_messageParent.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));

            if (algoForReset == 3) usleep(250000);

            if (performOffset)
            {
              mode.time = (int)offset;
              mode.relative = true;
              mode.backward = false;
              mode.accurate = false;
              mode.trickplay = true;
              mode.sync = true;
              m_messageParent.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
            }
            if (timeToReset)
            {
              advancedSettings->SetResetSeek(false);
              advancedSettings->SetLastResetTime(0.0);
              advancedSettings->SetAlgoForReset(0);
            }
            if (timeToResetSub)
            {
              advancedSettings->SetResetSeekSub(false);
              advancedSettings->SetLastResetTimeSub(0.0);
              advancedSettings->SetAlgoForResetSub(0);
            }
          }
        }
      }

      m_audioClock = audioframe.pts;
    }

    //==========================================================================
    // LAV PCM jitter tracking (for non-passthrough audio only)
    // This runs AFTER baseline PTS handling, potentially adjusting audioframe.pts
    //==========================================================================
    if (m_lavStylePcmSyncEnabled && !audioframe.passthrough && audioframe.hasTimestamp)
    {
      double inputPts = audioframe.pts;
      bool inputPtsValid = IsValidPts(inputPts);

      // Handle resync on first valid PTS after discontinuity
      if (m_pcmResyncTimestamp && inputPtsValid)
      {
        m_pcmOutputClock = inputPts;
        m_pcmResyncTimestamp = false;
        m_pcmJitterTracker.Reset();
      }
      else if (IsValidPts(m_pcmOutputClock) && inputPtsValid)
      {
        // Calculate jitter: our running output clock vs demuxer input PTS
        double jitter = m_pcmOutputClock - inputPts;

        // Track jitter in floating average
        m_pcmJitterTracker.Sample(jitter);

        // Get minimum absolute jitter (more stable than average)
        double absMinJitter = m_pcmJitterTracker.AbsMinimum();

        // Convert threshold from microseconds to DVD_TIME_BASE
        double thresholdDvdTime = PCM_JITTER_THRESHOLD * DVD_TIME_BASE / 1000000.0;

        // Check for large jumps (> 1 second) - trigger resync instead of correction
        if (std::abs(jitter) > DVD_TIME_BASE)
        {
          m_pcmOutputClock = inputPts;
          m_pcmJitterTracker.Reset();
          logComponentM(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio::ProcessDecoderOutput: LAV PCM resync due to large jump ({:.2f}s)",
                    jitter / DVD_TIME_BASE);
        }
        // Correct when jitter exceeds threshold
        else if (std::abs(absMinJitter) > thresholdDvdTime)
        {
          // Adjust output clock by the jitter amount
          m_pcmOutputClock -= absMinJitter;

          // Offset all tracked values so we continue from new baseline
          m_pcmJitterTracker.OffsetValues(-absMinJitter);

          // Signal discontinuity for potential clock adjustment
          audioframe.hasDiscontinuity = true;
          audioframe.discontinuityCorrection = absMinJitter;

          logComponentM(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio::ProcessDecoderOutput: LAV PCM jitter correction, "
                    "adjusting by {:.2f}ms (absMinJitter={:.2f}ms, threshold={:.0f}ms)",
                    absMinJitter / DVD_TIME_BASE * 1000.0,
                    absMinJitter / DVD_TIME_BASE * 1000.0,
                    PCM_JITTER_THRESHOLD / 1000.0);
        }

        // Use output clock as the frame PTS for downstream
        audioframe.pts = m_pcmOutputClock;
      }
    }

    if (audioframe.format.m_sampleRate && m_streaminfo.samplerate != (int) audioframe.format.m_sampleRate)
    {
      // The sample rate has changed or we just got it for the first time
      // for this stream. See if we should enable/disable passthrough due
      // to it.
      m_streaminfo.samplerate = audioframe.format.m_sampleRate;
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    // Display reset event has occurred
    // See if we should enable passthrough
    if (m_displayReset)
    {
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    const CAEStreamInfo& objectStreamInfo = audioframe.format.m_streamInfo;
    const bool haveAtmosInfo =
        audioframe.passthrough &&
        ((objectStreamInfo.m_hasAtmos &&
          (objectStreamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD ||
           objectStreamInfo.m_type == CAEStreamInfo::STREAM_TYPE_EAC3)) ||
         (objectStreamInfo.m_hasDtsX &&
          objectStreamInfo.m_type == CAEStreamInfo::STREAM_TYPE_DTSHD_MA));
    const int objectCount = haveAtmosInfo ? objectStreamInfo.m_atmosObjects : -1;
    const int objectChannels =
        haveAtmosInfo && objectStreamInfo.m_atmosChannels > 0
            ? static_cast<int>(objectStreamInfo.m_atmosChannels)
            : -1;
    const int bedChannels = haveAtmosInfo ? objectStreamInfo.m_bedChannels : -1;
    if (objectCount != m_lastAudioObjects)
    {
      m_lastAudioObjects = objectCount;
      m_processInfo.SetAudioObjectCount(objectCount);
    }
    if (objectChannels != m_lastAudioObjectChannels)
    {
      m_lastAudioObjectChannels = objectChannels;
      m_processInfo.SetAudioObjectChannels(objectChannels);
    }
    if (bedChannels != m_lastAudioBedChannels)
    {
      m_lastAudioBedChannels = bedChannels;
      m_processInfo.SetAudioBedChannels(bedChannels);
    }

    if (!m_forcePcmEac3 && m_eac3SmallSpfLimit > 0 &&
        audioframe.format.m_dataFormat == AE_FMT_RAW &&
        audioframe.format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_EAC3 &&
        audioframe.format.m_streamInfo.m_repeat > 1)
    {
      const int spf =
          EAC3_FULL_FRAME_SAMPLES / static_cast<int>(audioframe.format.m_streamInfo.m_repeat);
      if (spf <= m_eac3SmallSpfLimit)
      {
        m_forcePcmEac3 = true;
        logComponentM(LOGDEBUG, LOGAUDIO,
                      "CVideoPlayerAudio::ProcessDecoderOutput: EAC3 {} SPF at or below limit {}, "
                      "switching to PCM decode",
                      spf, m_eac3SmallSpfLimit);
        if (SwitchCodecIfNeeded())
        {
          audioframe.nb_frames = 0;
          return false;
        }
      }
    }

    if (m_audioSettingsChanged.exchange(false))
    {
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    // demuxer reads metatags that influence channel layout
    if (m_streaminfo.codec == AV_CODEC_ID_FLAC && m_streaminfo.channellayout)
      audioframe.format.m_channelLayout = CAEUtil::GetAEChannelLayout(m_streaminfo.channellayout);

    // If we have a stream bits per sample set on the stream info bit depth.
    if (m_streaminfo.bitspersample)
      audioframe.format.m_streamInfo.m_bitDepth = m_streaminfo.bitspersample;

    // we have successfully decoded an audio frame, setup renderer to match
    if (!m_audioSink.IsValidFormat(audioframe))
    {
      if (m_speed)
        m_audioSink.Drain();

      m_audioSink.Destroy(false);

      if (!m_createRetryTimer.IsTimePast())
      {
        audioframe.nb_frames = 0;
        return false;
      }

      if (!m_audioSink.Create(audioframe, m_streaminfo.codec, m_synctype == SYNC_RESAMPLE))
      {
        m_createFailures++;
        m_createRetryTimer.Set(m_createFailures < 20U ? 100ms : 2000ms);
        logM(LOGERROR, "failed to create audio renderer, m_createFailures={}", m_createFailures);
        audioframe.nb_frames = 0;
        return false;
      }

      m_createFailures = 0;
      m_drcValid = false;

      m_prevsynctype = -1;

      if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
        m_audioSink.Resume();
    }

    if (m_videoSettingsTimer.IsTimePast())
    {
      m_videoSettingsTimer.Set(250ms);
      const CVideoSettings videoSettings = m_processInfo.GetVideoSettings();
      m_volumeAmplification = videoSettings.m_VolumeAmplification;
      m_centerMixLevel = videoSettings.m_CenterMixLevel;
      m_seamlessBranchAlgo = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
          CSettings::SETTING_COREELEC_AMLOGIC_DV_AUDIO_SEAMLESSBRANCH);
    }
    const long drc = static_cast<long>(m_volumeAmplification * 100);
    if (!m_drcValid || drc != m_lastDrc)
    {
      m_drcValid = true;
      m_lastDrc = drc;
      m_audioSink.SetDynamicRangeCompression(drc);
    }

    SetSyncType(audioframe.passthrough);

    // downmix
    double clev = audioframe.hasDownmix ? audioframe.centerMixLevel : M_SQRT1_2;
    double curDB = 20 * log10(clev);
    audioframe.centerMixLevel = pow(10, (curDB + m_centerMixLevel) / 20);
    if (!audioframe.hasDownmix)
      audioframe.surroundMixLevel = M_SQRT1_2;
    audioframe.hasDownmix = true;
  }

  //============================================================================
  // A/V Sync Correction (SYNC_DISCON mode)
  // For passthrough with LAV: Apply one-time aggressive
  // correction on first large error to compensate for DV mode switch delays.
  // For all other cases: Use normal gradual correction via ErrorAdjust.
  //============================================================================

  if (m_synctype == SYNC_DISCON)
  {
    double syncerror = m_audioSink.GetSyncError();
    unsigned int threshold = m_audioSink.IsSyncFromFirstCycle()
                                 ? m_disconAdjustTimeMsFirstCycle
                                 : m_disconAdjustTimeMs;

    if (std::abs(syncerror) > DVD_MSEC_TO_TIME(threshold))
    {
      // Normal gradual correction via ErrorAdjust
      double correction = m_pClock->ErrorAdjust(syncerror, "CVideoPlayerAudio::OutputPacket");
      if (correction != 0)
      {
        m_audioSink.SetSyncErrorCorrection(-correction);
        m_disconAdjustCounter++;
        logComponentM(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio:: sync error correction:{:.3f}",
                  correction / DVD_TIME_BASE);
      }
    }
  }
  int framesOutput = m_audioSink.AddPackets(audioframe);

  // guess next pts
  m_audioClock += audioframe.duration * ((double)framesOutput / audioframe.nb_frames);

  // LAV: Accumulate output clock by actual duration output
  if (m_lavStylePcmSyncEnabled && !audioframe.passthrough && IsValidPts(m_pcmOutputClock))
  {
    double durationOutput = audioframe.duration * (static_cast<double>(framesOutput) / audioframe.nb_frames);
    m_pcmOutputClock += durationOutput;
  }

  audioframe.framesOut += framesOutput;

  // signal to our parent that we have initialized
  if (m_syncState == IDVDStreamPlayer::SYNC_STARTING)
  {
    double cachetotal = m_audioSink.GetCacheTotal();
    double cachetime = m_audioSink.GetCacheTime();
    AUDIODELAY_LOG("VPA.Gate75",
                   "cachetime={:.4f} cachetotal={:.4f} ratio={:.4f} target={:.4f} "
                   "passthrough={} dataFormat={} pts={:.3f}",
                   cachetime,
                   cachetotal,
                   (cachetotal > 0.0) ? (cachetime / cachetotal) : 0.0,
                   cachetotal * 0.75,
                   audioframe.passthrough ? 1 : 0,
                   static_cast<int>(audioframe.format.m_dataFormat),
                   audioframe.pts / DVD_TIME_BASE);
    if (cachetime >= cachetotal * 0.75)
    {
      AUDIODELAY_LOG("VPA.SyncTransition",
                     "old=SYNC_STARTING new=SYNC_WAITSYNC pts={:.3f} cachetime={:.4f} cachetotal={:.4f}",
                     audioframe.pts / DVD_TIME_BASE,
                     cachetime,
                     cachetotal);
      m_syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
      m_stalled = false;
      SStartMsg msg;
      msg.player = VideoPlayer_AUDIO;
      msg.cachetotal = m_audioSink.GetMaxDelay() * DVD_TIME_BASE;
      msg.cachetime = m_audioSink.GetDelay();
      msg.timestamp = audioframe.hasTimestamp ? audioframe.pts : DVD_NOPTS_VALUE;
      m_messageParent.Put(std::make_shared<CDVDMsgType<SStartMsg>>(CDVDMsg::PLAYER_STARTED, msg));

      m_streaminfo.channels = audioframe.format.m_channelLayout.Count();
      logComponentM(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio::ProcessDecoderOutput: GetAudioChannelsSink: {}",
        m_processInfo.GetAudioChannelsSink());
      if (m_pAudioCodec->NeedPassthrough() && m_streaminfo.channellayout != 0)
        m_processInfo.SetAudioChannels(CAEUtil::GetAEChannelLayout(m_streaminfo.channellayout));
      else
        m_processInfo.SetAudioChannels(audioframe.format.m_channelLayout);
      if (audioframe.format.m_streamInfo.m_sampleRate > 0)
        m_processInfo.SetAudioSampleRate(audioframe.format.m_streamInfo.m_sampleRate);
      else
        m_processInfo.SetAudioSampleRate(audioframe.format.m_sampleRate);

      int bitsPerSample = audioframe.bits_per_sample;
      if (audioframe.passthrough && audioframe.format.m_streamInfo.m_bitDepth > 0)
        bitsPerSample = audioframe.format.m_streamInfo.m_bitDepth;
      m_processInfo.SetAudioBitsPerSample(bitsPerSample);

      m_processInfo.SetAudioDecoderName(m_pAudioCodec->GetName());
      m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));

      m_renderManager.SetAudioLatencyTweak(CServiceBroker::GetSettingsComponent()
                                            ->GetAdvancedSettings()
                                            ->GetAudioLatencyTweak(audioframe.format.m_streamInfo.m_type));
    }
  }

  return true;
}

void CVideoPlayerAudio::SetSyncType(bool passthrough)
{
  if (passthrough && m_synctype == SYNC_RESAMPLE)
    m_synctype = SYNC_DISCON;

  //if SetMaxSpeedAdjust returns false, it means no video is played and we need to use clock feedback
  double maxspeedadjust = 0.0;
  if (m_synctype == SYNC_RESAMPLE)
    maxspeedadjust = m_maxspeedadjust;

  m_pClock->SetMaxSpeedAdjust(maxspeedadjust);

  if (m_synctype != m_prevsynctype)
  {
    const char *synctypes[] = {"clock feedback", "resample", "invalid"};
    int synctype = (m_synctype >= 0 && m_synctype <= 1) ? m_synctype : 2;
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio:: synctype set to {}: {}", m_synctype,
              synctypes[synctype]);
    m_prevsynctype = m_synctype;
    if (m_synctype == SYNC_RESAMPLE)
      m_audioSink.SetResampleMode(1);
    else
      m_audioSink.SetResampleMode(0);
  }
}

void CVideoPlayerAudio::OnExit()
{
#ifdef TARGET_WINDOWS
  CoUninitialize();
#endif

  CLog::Log(LOGDEBUG, "thread end: CVideoPlayerAudio::OnExit()");
}

void CVideoPlayerAudio::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    m_messageQueue.Put(std::make_shared<CDVDMsgInt>(CDVDMsg::PLAYER_SETSPEED, speed), 1);
  else
    m_speed = speed;

  m_audioSink.AbortAddPackets();
}

void CVideoPlayerAudio::Flush(bool sync)
{
  m_messageQueue.Flush();
  SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_FLUSH, sync), 1);

  m_audioSink.AbortAddPackets();
}

bool CVideoPlayerAudio::AcceptsData() const
{
  bool full = m_messageQueue.IsFull();
  return !full;
}

bool CVideoPlayerAudio::SwitchCodecIfNeeded()
{
  if (m_displayReset)
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio: display reset occurred, checking for passthrough");
  else
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio: stream props changed, checking for passthrough");

  m_displayReset = false;

  bool allowpassthrough = true;
  if (m_synctype == SYNC_RESAMPLE)
    allowpassthrough = false;
  if (m_forcePcmEac3)
    allowpassthrough = false;

  CDVDStreamInfo probeHints = m_streaminfoOrig;
  probeHints.samplerate = m_streaminfo.samplerate;

  CAEStreamInfo::DataType streamType = m_audioSink.GetPassthroughStreamType(
      probeHints.codec, probeHints.samplerate, probeHints.profile);

  const int algoValue = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
                                    CSettings::SETTING_COREELEC_AMLOGIC_DV_AUDIO_SEAMLESSBRANCH);
  const bool realtimeStream = m_processInfo.IsRealtimeStream();
  if (realtimeStream && algoValue != 0)
    logM(LOGDEBUG, "CVideoPlayerAudio::SwitchCodecIfNeeded - realtime stream, LAV audio sync disabled");
  const bool lavFullEnabled = !realtimeStream && ((algoValue == 3) || (algoValue == 5));
  const bool lavSeamlessBranchEnabled =
      !realtimeStream && ((algoValue != 0) && (algoValue != 3) && (algoValue != 5));

  const bool currentPassthrough = m_pAudioCodec->NeedPassthrough();
  const bool wouldPassthrough = allowpassthrough && streamType != CAEStreamInfo::STREAM_TYPE_NULL;

  if (wouldPassthrough == currentPassthrough)
  {
    logM(LOGDEBUG,
         "wouldPassthrough={:d} currentPassthrough={:d} allowpassthrough={:d} streamType={} "
         "lavFullEnabled={:d} lavSeamlessBranchEnabled={:d} - keeping m_pAudioCodec, "
         "CDVDFactoryCodec::CreateAudioCodec not called",
         wouldPassthrough, currentPassthrough, allowpassthrough,
         CAEUtil::StreamTypeToStr(streamType), lavFullEnabled, lavSeamlessBranchEnabled);
    return false;
  }

  std::unique_ptr<CDVDAudioCodec> codec = CDVDFactoryCodec::CreateAudioCodec(
      probeHints, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode(), streamType);

  if (!codec)
    return false;

  const bool isPassthrough = codec->NeedPassthrough();

  if (isPassthrough == currentPassthrough)
  {
    logM(LOGDEBUG,
         "wouldPassthrough={:d} isPassthrough={:d} currentPassthrough={:d} streamType={} - "
         "CDVDFactoryCodec::CreateAudioCodec returned unexpected codec, keeping m_pAudioCodec",
         wouldPassthrough, isPassthrough, currentPassthrough,
         CAEUtil::StreamTypeToStr(streamType));
    return false;
  }

  // Use the new codec (passthrough state changed)
  m_pAudioCodec = std::move(codec);

  // LAV: Set up sync on the new codec if it's passthrough
  if (isPassthrough)
  {
    m_lavStylePcmSyncEnabled = false;  // Passthrough has its own LAV sync
    m_lavFullSyncEnabled = false;
    CDVDAudioCodecPassthrough* passthroughCodec =
        dynamic_cast<CDVDAudioCodecPassthrough*>(m_pAudioCodec.get());
    if (passthroughCodec)
    {
      // Full LAV sync, Seamless branch only
      passthroughCodec->SetLavStyleSyncEnabled(lavFullEnabled);
      if (lavSeamlessBranchEnabled && !lavFullEnabled)
        passthroughCodec->SetLavSeamlessBranchEnabled(true);
      m_lavFullSyncEnabled = lavFullEnabled;
      
      // Reset LAV sync state for fresh codec (clears jitter tracker, PTS cache, etc.)
      if (lavFullEnabled)
        passthroughCodec->ResetLavSyncState();
      
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio::SwitchCodecIfNeeded - LAV passthrough: {} (state changed)",
                lavFullEnabled ? "FULL" : 
                (lavSeamlessBranchEnabled ? "SEAMLESS BRANCH ONLY" : "disabled"));
    }
  }
  else
  {
    // Switched to PCM/decoded
    m_lavStylePcmSyncEnabled = lavFullEnabled;
    m_lavFullSyncEnabled = lavFullEnabled;
    m_pcmJitterTracker.Reset();
    m_pcmOutputClock = LOCAL_NOPTS;
    m_pcmResyncTimestamp = true;
  }

  return true;
}

std::string CVideoPlayerAudio::GetPlayerInfo()
{
  std::unique_lock<CCriticalSection> lock(m_info_section);
  return m_info.info;
}

int CVideoPlayerAudio::GetAudioChannels()
{
  return m_streaminfo.channels;
}

bool CVideoPlayerAudio::IsPassthrough() const
{
  std::unique_lock<CCriticalSection> lock(m_info_section);
  return m_info.passthrough;
}

void CVideoPlayerAudio::SetAllowPassthrough(bool allow)
{
  if (!m_messageQueue.IsInited())
    return;

  CAEStreamInfo::DataType streamType = allow ?
      m_audioSink.GetPassthroughStreamType(m_streaminfo.codec, m_streaminfo.samplerate, m_streaminfo.profile) :
      CAEStreamInfo::STREAM_TYPE_NULL;

  std::unique_ptr<CDVDAudioCodec> codec = CDVDFactoryCodec::CreateAudioCodec(
      m_streaminfo, m_processInfo, allow, m_processInfo.AllowDTSHDDecode(), streamType);

  if (codec)
  {
    CLog::Log(LOGINFO, "CVideoPlayerAudio::SetAllowPassthrough - dynamic codec switch: allow passthrough = {}", allow);
    m_messageQueue.Put(std::make_shared<CDVDMsgAudioCodecChange>(m_streaminfo, std::move(codec)), 0);
  }
}

