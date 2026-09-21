/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayer.h"
#include "cores/VideoPlayer/BDStageTrace.h"

#include "DVDCodecs/DVDCodecUtils.h"
#include "DVDDemuxers/DVDDemux.h"
#include "DVDDemuxers/DVDDemuxCC.h"
#include "DVDDemuxers/DVDDemuxFFmpeg.h"
#include "DVDDemuxers/DVDDemuxUtils.h"
#include "DVDDemuxers/DVDDemuxVobsub.h"
#include "DVDDemuxers/DVDFactoryDemuxer.h"
#include "DVDInputStreams/DVDFactoryInputStream.h"
#include "DVDInputStreams/DVDInputStream.h"
#if defined(HAVE_LIBBLURAY)
#include "DVDInputStreams/DVDInputStreamBluray.h"
#include "VideoPlayerSubtitle.h"
#endif
#include "DVDInputStreams/DVDInputStreamNavigator.h"
#include "DVDInputStreams/InputStreamPVRBase.h"
#include "DVDMessage.h"
#include "FileItem.h"
#include "GUIUserMessages.h"
#include "LangInfo.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "Util.h"
#include "VideoPlayerAudio.h"
#include "VideoPlayerRadioRDS.h"
#include "VideoPlayerVideo.h"
#include "application/Application.h"
#include "cores/DataCacheCore.h"
#include "cores/EdlEdit.h"
#include "cores/FFmpeg.h"
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/LocalizeStrings.h"
#include "guilib/StereoscopicsManager.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#include "messaging/ApplicationMessenger.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingUtils.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/SingleLock.h"
#include "threads/SystemClock.h"
#include "utils/AMLUtils.h"
#include "utils/AudioDelayTrace.h"
#include "utils/FontUtils.h"
#include "utils/JobManager.h"
#include "utils/LangCodeExpander.h"
#include "utils/MathUtils.h"
#include "utils/StereoAspect.h"
#include "utils/StreamDetails.h"
#include "utils/StreamUtils.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"
#include "utils/LogThrottle.h"
#include "video/Bookmark.h"
#include "video/VideoInfoTag.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

namespace
{
int64_t NormalizeTarget(int64_t seekTarget, int64_t currentTime, int64_t maxTime)
{
  if (seekTarget < 0) return 0;
  if (maxTime <= 0) return seekTarget;

  const int64_t minBeforeEof =
      CSettingUtils::GetAdvancedSettingValue(&CAdvancedSettings::m_videoSeekMinimumDistanceBeforeEof, 10);
  const int64_t minBeforeEofMs = std::max<int64_t>(0, minBeforeEof) * 1000;

  const int64_t maxSeekTarget = std::max<int64_t>(0, maxTime - minBeforeEofMs);

  if (seekTarget <= maxSeekTarget) return seekTarget;

  if ((currentTime > maxSeekTarget) && (seekTarget <= currentTime)) return seekTarget;

  logM(LOGDEBUG, "clamp seek [{}] ms to [{}] ms to stay at least [{}] ms before reported EOS",
                 seekTarget, maxSeekTarget, minBeforeEofMs);

  return maxSeekTarget;
}
}

constexpr int SUBTITLE_DEMUX_BACKFILL_SECONDS = 30;

//------------------------------------------------------------------------------
// selection streams
//------------------------------------------------------------------------------

#define PREDICATE_RETURN(lh, rh) \
  do { \
    if((lh) != (rh)) \
      return (lh) > (rh); \
  } while(0)

class PredicateSubtitleFilter
{
private:
  std::string m_playedAudioLang;
  std::string m_subLang;
  bool m_isPrefOriginal;
  bool m_isPrefForced;
  bool m_isPrefHearingImp;
  bool m_isSubNone;
  int m_subStream;

public:
  explicit PredicateSubtitleFilter(const std::string& lang, int subStream)
    : m_playedAudioLang(lang), m_subStream(subStream)
  {
    auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    const std::string subLangSetting =
        settings->GetString(CSettings::SETTING_LOCALE_SUBTITLELANGUAGE);

    m_isSubNone = StringUtils::EqualsNoCase(subLangSetting, "none");
    m_isPrefOriginal = StringUtils::EqualsNoCase(subLangSetting, "original");
    m_isPrefForced = StringUtils::EqualsNoCase(subLangSetting, "forced_only");
    m_isPrefHearingImp = settings->GetBool(CSettings::SETTING_ACCESSIBILITY_SUBHEARING);

    m_subLang = g_langInfo.GetSubtitleLanguage(false);
    if (m_subLang.empty())
    {
      m_subLang = g_langInfo.GetAudioLanguage(false);
      if (m_subLang.empty())
        m_subLang = m_playedAudioLang;
    }

    if (m_isPrefHearingImp && m_isPrefForced)
      m_isPrefForced = false;
  };

  bool operator()(const SelectionStream& ss) const
  {
    if (ss.type_index == m_subStream)
      return false;

    if (m_isSubNone)
      return true;

    const bool isExternal = STREAM_SOURCE_MASK(ss.source) == STREAM_SOURCE_DEMUX_SUB ||
                            STREAM_SOURCE_MASK(ss.source) == STREAM_SOURCE_TEXT;
    const bool isCC = STREAM_SOURCE_MASK(ss.source) == STREAM_SOURCE_VIDEOMUX;

    if (isExternal && (ss.language.empty() || ss.language == "und"))
    {
      return false;
    }

    const bool isSameSubLang = g_LangCodeExpander.CompareISO639Codes(ss.language, m_subLang);

    if (m_isPrefHearingImp)
    {
      const int checkFlags = FLAG_ORIGINAL | FLAG_HEARING_IMPAIRED;
      if ((ss.flags & checkFlags) == checkFlags)
        return false;

      if ((ss.flags & StreamFlags::FLAG_HEARING_IMPAIRED) &&
          (isSameSubLang || (isCC && (ss.language.empty() || ss.language == "und"))))
      {
        return false;
      }
      if (isSameSubLang && (ss.flags & FLAG_FORCED) == 0)
        return false;

      return true;
    }

    if (m_isPrefOriginal)
    {
      if ((ss.flags & FLAG_ORIGINAL))
        return false;
    }
    else if (m_isPrefForced)
    {
      if ((ss.flags & StreamFlags::FLAG_FORCED) && isSameSubLang)
        return false;
      else
        return true;
    }

    if ((isSameSubLang || (isCC && (ss.language.empty() || ss.language == "und"))) &&
        (ss.flags & FLAG_FORCED) == 0 && (ss.flags & FLAG_HEARING_IMPAIRED) == 0)
    {
      return false;
    }

    return true;
  }
};

class PredicateAudioFilter
{
private:
  int currentAudioStream;
  bool preferStereo;
public:
  explicit PredicateAudioFilter(int audioStream, bool preferStereo)
    : currentAudioStream(audioStream)
    , preferStereo(preferStereo)
  {
  };
  bool operator()(const SelectionStream& lh, const SelectionStream& rh) const {
    PREDICATE_RETURN(lh.type_index == currentAudioStream
                     , rh.type_index == currentAudioStream);

    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

    if (!StringUtils::EqualsNoCase(settings->GetString(CSettings::SETTING_LOCALE_AUDIOLANGUAGE), "mediadefault"))
    {
      if (!StringUtils::EqualsNoCase(settings->GetString(CSettings::SETTING_LOCALE_AUDIOLANGUAGE), "original"))
      {
        std::string audio_language = g_langInfo.GetAudioLanguage(true);
        PREDICATE_RETURN(g_LangCodeExpander.CompareISO639Codes(audio_language, lh.language)
          , g_LangCodeExpander.CompareISO639Codes(audio_language, rh.language));
      }
      else
      {
        PREDICATE_RETURN(lh.flags & StreamFlags::FLAG_ORIGINAL,
          rh.flags & StreamFlags::FLAG_ORIGINAL);
      }

      bool hearingimp = settings->GetBool(CSettings::SETTING_ACCESSIBILITY_AUDIOHEARING);
      PREDICATE_RETURN(!hearingimp ? !(lh.flags & StreamFlags::FLAG_HEARING_IMPAIRED) : lh.flags & StreamFlags::FLAG_HEARING_IMPAIRED
                       , !hearingimp ? !(rh.flags & StreamFlags::FLAG_HEARING_IMPAIRED) : rh.flags & StreamFlags::FLAG_HEARING_IMPAIRED);

      bool visualimp = settings->GetBool(CSettings::SETTING_ACCESSIBILITY_AUDIOVISUAL);
      PREDICATE_RETURN(!visualimp ? !(lh.flags & StreamFlags::FLAG_VISUAL_IMPAIRED) : lh.flags & StreamFlags::FLAG_VISUAL_IMPAIRED
                       , !visualimp ? !(rh.flags & StreamFlags::FLAG_VISUAL_IMPAIRED) : rh.flags & StreamFlags::FLAG_VISUAL_IMPAIRED);
    }

    if (settings->GetBool(CSettings::SETTING_VIDEOPLAYER_PREFERDEFAULTFLAG))
    {
      PREDICATE_RETURN(lh.flags & StreamFlags::FLAG_DEFAULT,
                       rh.flags & StreamFlags::FLAG_DEFAULT);
    }

    if (preferStereo)
      PREDICATE_RETURN(lh.channels == 2,
                       rh.channels == 2);
    else
      PREDICATE_RETURN(lh.channels,
                       rh.channels);

    PREDICATE_RETURN(StreamUtils::GetCodecPriority(lh.codec),
                     StreamUtils::GetCodecPriority(rh.codec));

    PREDICATE_RETURN(lh.flags & StreamFlags::FLAG_DEFAULT,
                     rh.flags & StreamFlags::FLAG_DEFAULT);
    return false;
  };
};

class PredicateSubtitlePriority
{
private:
  std::string m_playedAudioLang;
  std::string m_subLang;
  bool m_isPrefOriginal;
  bool m_isPrefForced;
  bool m_isPrefHearingImp;
  PredicateSubtitleFilter m_filter;
  int m_subStream;

public:
  explicit PredicateSubtitlePriority(const std::string& lang, int stream)
    : m_playedAudioLang(lang), m_filter(lang, stream), m_subStream(stream)
  {
    auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    const std::string subLangSetting =
        settings->GetString(CSettings::SETTING_LOCALE_SUBTITLELANGUAGE);

    m_isPrefOriginal = StringUtils::EqualsNoCase(subLangSetting, "original");
    m_isPrefForced = StringUtils::EqualsNoCase(subLangSetting, "forced_only");
    m_isPrefHearingImp = settings->GetBool(CSettings::SETTING_ACCESSIBILITY_SUBHEARING);

    m_subLang = g_langInfo.GetSubtitleLanguage(false);
    if (m_subLang.empty())
    {
      m_subLang = g_langInfo.GetAudioLanguage(false);
      if (m_subLang.empty())
        m_subLang = m_playedAudioLang;
    }

    if (m_isPrefHearingImp && m_isPrefForced)
      m_isPrefForced = false;
  };

  bool relevant(const SelectionStream& ss) const { return !m_filter(ss); }

  bool operator()(const SelectionStream& lh, const SelectionStream& rh) const
  {
    PREDICATE_RETURN(lh.type_index == m_subStream, rh.type_index == m_subStream);

    const bool isLexternal = STREAM_SOURCE_MASK(lh.source) == STREAM_SOURCE_DEMUX_SUB ||
                             STREAM_SOURCE_MASK(lh.source) == STREAM_SOURCE_TEXT;
    const bool isRexternal = STREAM_SOURCE_MASK(rh.source) == STREAM_SOURCE_DEMUX_SUB ||
                             STREAM_SOURCE_MASK(rh.source) == STREAM_SOURCE_TEXT;

    PREDICATE_RETURN(isLexternal, isRexternal);

    const bool isLSameSubLang = g_LangCodeExpander.CompareISO639Codes(lh.language, m_subLang);
    const bool isRSameSubLang = g_LangCodeExpander.CompareISO639Codes(rh.language, m_subLang);

    if (m_isPrefHearingImp)
    {
      if (m_isPrefOriginal)
      {
        int checkFlags = FLAG_ORIGINAL | FLAG_HEARING_IMPAIRED | FLAG_DEFAULT;
        PREDICATE_RETURN((lh.flags & checkFlags) == checkFlags,
                         (rh.flags & checkFlags) == checkFlags);

        checkFlags = FLAG_ORIGINAL | FLAG_HEARING_IMPAIRED;
        PREDICATE_RETURN((lh.flags & checkFlags) == checkFlags,
                         (rh.flags & checkFlags) == checkFlags);
      }

      int checkFlags = FLAG_HEARING_IMPAIRED | FLAG_DEFAULT;
      PREDICATE_RETURN((lh.flags & checkFlags) == checkFlags && isLSameSubLang,
                       (rh.flags & checkFlags) == checkFlags && isRSameSubLang);

      checkFlags = FLAG_HEARING_IMPAIRED;
      PREDICATE_RETURN((lh.flags & checkFlags) == checkFlags && isLSameSubLang,
                       (rh.flags & checkFlags) == checkFlags && isRSameSubLang);
    }

    if (m_isPrefOriginal)
    {
      const bool isLincluded =
          (lh.flags & FLAG_FORCED) == 0 && (lh.flags & FLAG_HEARING_IMPAIRED) == 0;
      const bool isRincluded =
          (rh.flags & FLAG_FORCED) == 0 && (rh.flags & FLAG_HEARING_IMPAIRED) == 0;

      const int checkFlags = FLAG_ORIGINAL | FLAG_DEFAULT;
      PREDICATE_RETURN(isLincluded && (lh.flags & checkFlags) == checkFlags && isLSameSubLang,
                       isRincluded && (rh.flags & checkFlags) == checkFlags && isRSameSubLang);

      PREDICATE_RETURN(isLincluded && (lh.flags & FLAG_ORIGINAL) && isLSameSubLang,
                       isRincluded && (rh.flags & FLAG_ORIGINAL) && isRSameSubLang);

      PREDICATE_RETURN(isLincluded && (lh.flags & checkFlags) == checkFlags,
                       isRincluded && (rh.flags & checkFlags) == checkFlags);

      PREDICATE_RETURN(isLincluded && (lh.flags & FLAG_ORIGINAL),
                       isRincluded && (rh.flags & FLAG_ORIGINAL));
    }
    else if (m_isPrefForced)
    {
      const int checkFlags = FLAG_FORCED | FLAG_DEFAULT;
      PREDICATE_RETURN((lh.flags & checkFlags) == checkFlags && isLSameSubLang,
                       (rh.flags & checkFlags) == checkFlags && isRSameSubLang);

      PREDICATE_RETURN((lh.flags & FLAG_FORCED) && isLSameSubLang,
                       (rh.flags & FLAG_FORCED) && isRSameSubLang);
    }

    const bool isLincluded =
        (lh.flags & FLAG_FORCED) == 0 && (lh.flags & FLAG_HEARING_IMPAIRED) == 0;
    const bool isRincluded =
        (rh.flags & FLAG_FORCED) == 0 && (rh.flags & FLAG_HEARING_IMPAIRED) == 0;

    PREDICATE_RETURN(isLincluded && lh.flags & FLAG_DEFAULT && isLSameSubLang,
                     isRincluded && rh.flags & FLAG_DEFAULT && isRSameSubLang);
    PREDICATE_RETURN(isLincluded && isLSameSubLang, isRincluded && isRSameSubLang);

    if (!m_isPrefForced && isLincluded && (lh.language.empty() || lh.language == "und"))
    {
      return true;
    }
    return false;
  }
};

class PredicateVideoFilter
{
private:
  int currentVideoStream;
public:
  explicit PredicateVideoFilter(int videoStream) : currentVideoStream(videoStream)
  {
  };
  bool operator()(const SelectionStream& lh, const SelectionStream& rh) const {
    PREDICATE_RETURN(lh.type_index == currentVideoStream,
                     rh.type_index == currentVideoStream);

    PREDICATE_RETURN(lh.flags & StreamFlags::FLAG_DEFAULT,
                     rh.flags & StreamFlags::FLAG_DEFAULT);
    return false;
  }
};

void CSelectionStreams::Clear(StreamType type, StreamSource source)
{
  auto new_end = std::remove_if(m_Streams.begin(), m_Streams.end(),
                                [type, source](const SelectionStream &stream)
                                {
                                  return (type == STREAM_NONE || stream.type == type) &&
                                  (source == 0 || stream.source == source);
                                });
  m_Streams.erase(new_end, m_Streams.end());
}

SelectionStream& CSelectionStreams::Get(StreamType type, int index)
{
  return const_cast<SelectionStream&>(std::as_const(*this).Get(type, index));
}

const SelectionStream& CSelectionStreams::Get(StreamType type, int index) const
{
  int count = -1;
  for (size_t i = 0; i < m_Streams.size(); ++i)
  {
    if (m_Streams[i].type != type)
      continue;
    count++;
    if (count == index)
      return m_Streams[i];
  }
  return m_invalid;
}

std::vector<SelectionStream> CSelectionStreams::Get(StreamType type)
{
  std::vector<SelectionStream> streams;
  std::copy_if(m_Streams.begin(), m_Streams.end(), std::back_inserter(streams),
               [type](const SelectionStream &stream)
               {
                 return stream.type == type;
               });
  return streams;
}

bool CSelectionStreams::Get(StreamType type, StreamFlags flag, SelectionStream& out) const {
  for(size_t i=0;i<m_Streams.size();i++)
  {
    if(m_Streams[i].type != type)
      continue;
    if((m_Streams[i].flags & flag) != flag)
      continue;
    out = m_Streams[i];
    return true;
  }
  return false;
}

int CSelectionStreams::TypeIndexOf(StreamType type, int source, int64_t demuxerId, int id) const
{
  if (id < 0)
    return -1;

  auto it = std::find_if(m_Streams.begin(), m_Streams.end(),
    [&](const SelectionStream& stream) {return stream.type == type
    && stream.source == source && stream.id == id
    && stream.demuxerId == demuxerId;});

  if (it != m_Streams.end())
    return it->type_index;
  else
    return -1;
}

int CSelectionStreams::Source(StreamSource source, const std::string& filename)
{
  int index = source - 1;
  for (size_t i=0; i<m_Streams.size(); i++)
  {
    SelectionStream &s = m_Streams[i];
    if (STREAM_SOURCE_MASK(s.source) != source)
      continue;
    // if it already exists, return same
    if (s.filename == filename)
      return s.source;
    if (index < s.source)
      index = s.source;
  }
  // return next index
  return index + 1;
}

void CSelectionStreams::Update(SelectionStream& s)
{
  int index = TypeIndexOf(s.type, s.source, s.demuxerId, s.id);
  if(index >= 0)
  {
    SelectionStream& o = Get(s.type, index);
    s.type_index = o.type_index;
    o = s;
  }
  else
  {
    s.type_index = CountType(s.type);
    m_Streams.push_back(s);
  }
}

void CSelectionStreams::Update(const std::shared_ptr<CDVDInputStream>& input,
                               CDVDDemux* demuxer,
                               const std::string& filename2)
{
  if(input && input->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    std::shared_ptr<CDVDInputStreamNavigator> nav = std::static_pointer_cast<CDVDInputStreamNavigator>(input);
    std::string filename = nav->GetFileName();
    int source = Source(STREAM_SOURCE_NAV, filename);

    std::vector<CDemuxStream*> demuxStreams;
    if (demuxer)
      demuxStreams = demuxer->GetStreams();

    int count;
    count = nav->GetAudioStreamCount();
    for(int i=0;i<count;i++)
    {
      const auto stream =
          std::find_if(demuxStreams.begin(), demuxStreams.end(),
                       [i](const auto& stream)
                       { return stream->type == STREAM_AUDIO && stream->dvdNavId == i; });
      CDemuxStreamAudio* aStream =
          (stream != demuxStreams.end()) ? static_cast<CDemuxStreamAudio*>(*stream) : nullptr;

      SelectionStream s;
      s.source   = source;
      s.type     = STREAM_AUDIO;
      s.id       = i;
      s.flags    = StreamFlags::FLAG_NONE;
      s.filename = filename;

      AudioStreamInfo info = nav->GetAudioStreamInfo(i);
      s.name     = info.name;
      s.codec    = info.codecName;
      // additional/more reliable info from ffmpeg than IFO nav data
      if (aStream)
      {
        s.codecDesc = aStream->GetStreamType();
        s.channels = aStream->iChannels;
        s.bitrate = aStream->iBitRate;
      }
      else
      {
        s.codecDesc = info.codecDesc;
        s.channels = info.channels;
      }
      s.language = g_LangCodeExpander.ConvertToISO6392B(info.language);
      s.flags = info.flags;
      Update(s);
    }

    count = nav->GetSubTitleStreamCount();
    for(int i=0;i<count;i++)
    {
      SelectionStream s;
      s.source   = source;
      s.type     = STREAM_SUBTITLE;
      s.id       = i;
      s.filename = filename;
      s.channels = 0;

      SubtitleStreamInfo info = nav->GetSubtitleStreamInfo(i);
      s.name     = info.name;
      s.codec = info.codecName;
      s.flags = info.flags;
      s.language = g_LangCodeExpander.ConvertToISO6392B(info.language);
      Update(s);
    }

    VideoStreamInfo info = nav->GetVideoStreamInfo();
    for (int i = 1; i <= info.angles; i++)
    {
      SelectionStream s;
      s.source = source;
      s.type = STREAM_VIDEO;
      s.id = i;
      s.flags = StreamFlags::FLAG_NONE;
      s.filename = filename;
      s.channels = 0;
      s.aspect_ratio = info.videoAspectRatio;
      s.width = info.width;
      s.height = info.height;
      s.codec = info.codecName;
      s.name = StringUtils::Format("{} {}", g_localizeStrings.Get(38032), i);
      Update(s);
    }
  }
  else if(demuxer)
  {
    std::string filename = demuxer->GetFileName();
    int source;
    if(input) /* hack to know this is sub decoder */
      source = Source(STREAM_SOURCE_DEMUX, filename);
    else if (!filename2.empty())
      source = Source(STREAM_SOURCE_DEMUX_SUB, filename);
    else
      source = Source(STREAM_SOURCE_VIDEOMUX, filename);

    for (auto stream : demuxer->GetStreams())
    {
      /* skip streams with no type */
      if (stream->type == STREAM_NONE)
        continue;
      /* make sure stream is marked with right source */
      stream->source = source;

      SelectionStream s;
      s.source   = source;
      s.type     = stream->type;
      s.id       = stream->uniqueId;
      s.demuxerId = stream->demuxerId;
      s.language = g_LangCodeExpander.ConvertToISO6392B(stream->language);
      s.flags    = stream->flags;
      s.filename = demuxer->GetFileName();
      s.filename2 = filename2;
      s.name = stream->GetStreamName();
      s.codec    = demuxer->GetStreamCodecName(stream->demuxerId, stream->uniqueId);
      s.channels = 0; // Default to 0. Overwrite if STREAM_AUDIO below.
      if(stream->type == STREAM_VIDEO)
      {
        CDemuxStreamVideo* vstream = static_cast<CDemuxStreamVideo*>(stream);
        s.width = vstream->iWidth;
        s.height = vstream->iHeight;
        s.aspect_ratio = vstream->fAspect;
        s.stereo_mode = vstream->stereo_mode;
        s.bitrate = vstream->iBitRate;
        s.hdrType = vstream->hdr_type;
        s.dovi = vstream->dovi;
        s.fpsRate = static_cast<uint32_t>(vstream->iFpsRate);
        s.fpsScale = static_cast<uint32_t>(vstream->iFpsScale);
      }
      if(stream->type == STREAM_AUDIO)
      {
        s.codecDesc = static_cast<CDemuxStreamAudio*>(stream)->GetStreamType();
        s.channels = static_cast<CDemuxStreamAudio*>(stream)->iChannels;
        s.bitrate = static_cast<CDemuxStreamAudio*>(stream)->iBitRate;
      }
      Update(s);
    }
  }
  CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
  CServiceBroker::GetDataCacheCore().SignalVideoInfoChange();
  CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();
}

void CSelectionStreams::Update(const std::shared_ptr<CDVDInputStream>& input, CDVDDemux* demuxer)
{
  Update(input, demuxer, "");
}

int CSelectionStreams::CountTypeOfSource(StreamType type, StreamSource source) const
{
  return std::count_if(m_Streams.begin(), m_Streams.end(),
    [&](const SelectionStream& stream) {return (stream.type == type) && (stream.source == source);});
}

int CSelectionStreams::CountType(StreamType type) const
{
  return std::count_if(m_Streams.begin(), m_Streams.end(),
                       [&](const SelectionStream& stream) { return stream.type == type; });
}

//------------------------------------------------------------------------------
// main class
//------------------------------------------------------------------------------

void CVideoPlayer::SetAVChange(std::string from) const
{
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) &&
      !static_cast<CDVDInputStreamBluray*>(m_pInputStream.get())->IsOnFeaturePlaylist() &&
      !m_bdFeatureStable)
  {
    logM(LOGDEBUG, "VideoPlayer::SetAVChange suppressed [{}] - bluray non-feature or unsettled playlist",
         from);
    return;
  }

  CLog::Log(LOGDEBUG, "VideoPlayer::SetAVChange true [{}]", from);

  if (CServiceBroker::GetDataCacheCore().GetAVChange())
    return; // already set, do not allow set again until done.

  const uint64_t generation = CServiceBroker::GetDataCacheCore().NextAVChangeGeneration();
  CServiceBroker::GetDataCacheCore().SetAVChange(true);
  CServiceBroker::GetDataCacheCore().SetAVChangeExtended(true);

  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const int codecLogoTimeout = settings->GetSetting(CSettings::SETTING_COREELEC_CODECLOGO_TIMEOUT)
                                   ? settings->GetInt(CSettings::SETTING_COREELEC_CODECLOGO_TIMEOUT)
                                   : -1;
  const unsigned int timeout{(codecLogoTimeout >= 0 && codecLogoTimeout <= 30)
                                 ? static_cast<unsigned int>(codecLogoTimeout)
                                 : CServiceBroker::GetSettingsComponent()
                                       ->GetAdvancedSettings()
                                       ->m_guiAVChangeFlagTimeout};

  // Schedule set to false after the configured timeout in advanced settings - user can dial-in as preferred.
  CServiceBroker::GetJobManager()->Submit([from = std::move(from), timeout, generation]() {
    std::this_thread::sleep_for(std::chrono::seconds(timeout));
    if (!CServiceBroker::GetDataCacheCore().IsAVChangeGeneration(generation))
      return;
    CServiceBroker::GetDataCacheCore().SetAVChange(false);
    CLog::Log(LOGDEBUG, "VideoPlayer::SetAVChange false [{}] after [{}] seconds", from, timeout);
    std::this_thread::sleep_for(2s);
    if (!CServiceBroker::GetDataCacheCore().IsAVChangeGeneration(generation))
      return;
    CServiceBroker::GetDataCacheCore().SetAVChangeExtended(false);
  });
}

void CVideoPlayer::CreatePlayers()
{
  if (m_players_created)
    return;

  m_VideoPlayerVideo = new CVideoPlayerVideo(&m_clock, &m_overlayContainer, m_messenger, m_renderManager, *m_processInfo, m_messageQueueTimeSize);
  m_VideoPlayerAudio = new CVideoPlayerAudio(&m_clock, m_messenger, m_renderManager, *m_processInfo, m_messageQueueTimeSize);
  m_VideoPlayerSubtitle = new CVideoPlayerSubtitle(&m_overlayContainer, *m_processInfo);
  m_VideoPlayerTeletext = new CDVDTeletextData(*m_processInfo);
  m_VideoPlayerRadioRDS = new CDVDRadioRDSData(*m_processInfo);
  m_VideoPlayerAudioID3 = std::make_unique<CVideoPlayerAudioID3>(*m_processInfo);
  m_players_created = true;
}

void CVideoPlayer::DestroyPlayers()
{
  if (!m_players_created)
    return;

  delete m_VideoPlayerVideo;
  delete m_VideoPlayerAudio;
  delete m_VideoPlayerSubtitle;
  delete m_VideoPlayerTeletext;
  delete m_VideoPlayerRadioRDS;
  m_VideoPlayerAudioID3.reset();

  m_players_created = false;
}

CVideoPlayer::CVideoPlayer(IPlayerCallback& callback)
  : IPlayer(callback),
    CThread("VideoPlayer"),
    m_CurrentAudio(STREAM_AUDIO, VideoPlayer_AUDIO),
    m_CurrentVideo(STREAM_VIDEO, VideoPlayer_VIDEO),
    m_CurrentSubtitle(STREAM_SUBTITLE, VideoPlayer_SUBTITLE),
    m_CurrentTeletext(STREAM_TELETEXT, VideoPlayer_TELETEXT),
    m_CurrentRadioRDS(STREAM_RADIO_RDS, VideoPlayer_RDS),
    m_CurrentAudioID3(STREAM_AUDIO_ID3, VideoPlayer_ID3),
    m_messenger("player"),
    m_outboundEvents(std::make_unique<CJobQueue>(false, 1, CJob::PRIORITY_NORMAL)),
    m_pInputStream(nullptr),
    m_pDemuxer(nullptr),
    m_pSubtitleDemuxer(nullptr),
    m_pCCDemuxer(nullptr),
    m_renderManager(m_clock, this)
{
  m_players_created = false;

  m_dvd.Clear();
  m_State.Clear();
  m_bAbortRequest = false;
  m_offset_pts = 0.0;
  m_playSpeed = DVD_PLAYSPEED_NORMAL;
  m_streamPlayerSpeed = DVD_PLAYSPEED_NORMAL;
  m_caching = CACHESTATE_DONE;
  m_HasVideo = false;
  m_HasAudio = false;
  m_UpdateStreamDetails = false;

  m_messageQueueTimeSize = 16; // 16 seconds for buffer at max bit rate.

  m_SkipCommercials = true;

  m_processInfo.reset(CProcessInfo::CreateInstance());
  // if we have a gui, register the cache
  m_processInfo->SetDataCache(&CServiceBroker::GetDataCacheCore());
  m_processInfo->SetSpeed(1.0);
  m_processInfo->SetTempo(1.0);
  m_processInfo->SetFrameAdvance(false);

  CreatePlayers();

  m_displayLost = false;
  m_error = false;
  m_bCloseRequest = false;
  CServiceBroker::GetWinSystem()->Register(this);
}

class CVideoPlayerHwRenderThread : public CThread
{
public:
  explicit CVideoPlayerHwRenderThread(CRenderManager& renderManager)
    : CThread("AMLHwVidRender"), m_renderManager(renderManager)
  {
  }
  ~CVideoPlayerHwRenderThread() override { StopThread(); }

protected:
  void Process() override
  {
    while (!m_bStop)
    {
      if (!m_renderManager.AsyncVideoWorkerIteration())
        break;
    }
  }

private:
  CRenderManager& m_renderManager;
};

void CVideoPlayer::StartHwVideoRenderThread()
{
  std::unique_lock lock(m_hwRenderThreadSection);
  if (m_hwRenderThread || !m_asyncVideoRenderLatched.load(std::memory_order_relaxed))
    return;
  m_renderManager.SetAsyncVideoWorkerActive(true);
  m_hwRenderThread = std::make_unique<CVideoPlayerHwRenderThread>(m_renderManager);
  m_hwRenderThread->Create();
  m_asyncVideoWorkerLive.store(true, std::memory_order_relaxed);
  logM(LOGDEBUG, "async video-layer render worker started");
}

void CVideoPlayer::StopHwVideoRenderThread(bool unlatch)
{
  std::unique_lock lock(m_hwRenderThreadSection);
  if (unlatch)
    m_asyncVideoRenderLatched.store(false, std::memory_order_relaxed);
  if (!m_hwRenderThread)
    return;
  m_renderManager.RequestAsyncVideoWorkerStop();
  {
    CSingleExit exitlock(CServiceBroker::GetWinSystem()->GetGfxContext());
    m_hwRenderThread->StopThread(true);
  }
  m_hwRenderThread.reset();
  m_renderManager.SetAsyncVideoWorkerActive(false);
  m_asyncVideoWorkerLive.store(false, std::memory_order_relaxed);
  logM(LOGDEBUG, "async video-layer render worker stopped");
}

CVideoPlayer::~CVideoPlayer()
{
  CServiceBroker::GetWinSystem()->Unregister(this);

  CloseFile();
  DestroyPlayers();

  while (m_outboundEvents->IsProcessing())
  {
    CThread::Sleep(10ms);
  }
}

bool CVideoPlayer::OpenFile(const CFileItem& file, const CPlayerOptions &options)
{
  CLog::Log(LOGDEBUG, "VideoPlayer::OpenFile: {}", CURL::GetRedacted(file.GetPath()));

  BDSTAGE::Play();

  m_asyncVideoRenderLatched.store(CServiceBroker::GetSettingsComponent()
                                      ->GetAdvancedSettings()
                                      ->m_videoAsyncVideoLayerRender,
                                  std::memory_order_relaxed);

  CServiceBroker::GetDataCacheCore().SetAVChange(false);
  CServiceBroker::GetDataCacheCore().SetAVChangeExtended(false);

  if (IsRunning())
  {
    CDVDMsgOpenFile::FileParams params;
    params.m_item = file;
    params.m_options = options;
    params.m_item.SetMimeTypeForInternetFile();
    m_messenger.Put(std::make_shared<CDVDMsgOpenFile>(params), 1);

    return true;
  }

  m_item = file;
  m_playerOptions = options;

  m_processInfo->SetPlayTimes(0,0,0,0);
  m_bAbortRequest = false;
  m_error = false;
  m_bCloseRequest = false;
  m_brokenFileNotified = false;
  m_brokenFileStallStart = {};
  m_brokenFileStallBytes = -1;
  m_brokenFileStallStarveLogged = false;
  m_lastChapterSeekTarget = 0;
  m_renderManager.PreInit();

  Create();
  m_messenger.Init();

  m_callback.OnPlayBackStarted(m_item);

  return true;
}

bool CVideoPlayer::CloseFile(bool reopen)
{
  CLog::Log(LOGDEBUG, "CVideoPlayer::CloseFile()");

  if (m_CurrentVideo.id >= 0)
  {
    m_renderManager.ShowVideo(false);
    m_renderManager.Flush(true, false);
  }

  // set the abort request so that other threads can finish up
  m_bAbortRequest = true;
  m_bCloseRequest = true;

  // tell demuxer to abort
  if(m_pDemuxer)
    m_pDemuxer->Abort();

  if(m_pSubtitleDemuxer)
    m_pSubtitleDemuxer->Abort();

  if(m_pInputStream)
    m_pInputStream->Abort();

  StopHwVideoRenderThread(true);

  m_renderManager.UnInit();

  CLog::Log(LOGDEBUG, "VideoPlayer: waiting for threads to exit");

  // wait for the main thread to finish up
  // since this main thread cleans up all other resources and threads
  // we are done after the StopThread call
  {
    CSingleExit exitlock(CServiceBroker::GetWinSystem()->GetGfxContext());
    StopThread();
  }

  m_Edl.Clear();
  CServiceBroker::GetDataCacheCore().Reset();
  m_processInfo->SetDataCache(&CServiceBroker::GetDataCacheCore());

  m_HasVideo = false;
  m_HasAudio = false;

  CLog::Log(LOGDEBUG, "VideoPlayer: finished waiting");
  return true;
}

bool CVideoPlayer::IsPlaying() const
{
  return !m_bStop;
}

void CVideoPlayer::OnStartup()
{
  m_CurrentVideo.Clear();
  m_CurrentAudio.Clear();
  m_CurrentSubtitle.Clear();
  m_CurrentTeletext.Clear();
  m_CurrentRadioRDS.Clear();
  m_CurrentAudioID3.Clear();

  UTILS::FONT::ClearTemporaryFonts();
}

bool CVideoPlayer::OpenInputStream()
{
  m_menus.reset();
  if (m_pInputStream.use_count() > 1)
    throw std::runtime_error("m_pInputStream reference count is greater than 1");
  m_pInputStream.reset();

  m_subtitleSeekRecallFromFile = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
      CSettings::SETTING_COREELEC_SUBTITLES_RECALL_FROM_FILE);

  CLog::Log(LOGDEBUG, "Creating InputStream");

  m_pInputStream = CDVDFactoryInputStream::CreateInputStream(this, m_item, true);
  if (m_pInputStream == nullptr)
  {
    CLog::Log(LOGERROR, "CVideoPlayer::OpenInputStream - unable to create input stream for [{}]",
              CURL::GetRedacted(m_item.GetPath()));
    return false;
  }
  m_menus = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream);

  if (!m_pInputStream->Open())
  {
    CLog::Log(LOGERROR, "CVideoPlayer::OpenInputStream - error opening [{}]",
              CURL::GetRedacted(m_item.GetPath()));
    return false;
  }

  // find any available external subtitles for non dvd files
  if (!m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD) &&
      !m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER))
  {
    // find any available external subtitles
    std::vector<std::string> filenames;

    const bool isStrm = URIUtils::HasExtension(m_item.GetPath(), ".strm") ||
                        URIUtils::HasExtension(m_item.GetDynPath(), ".strm");
    const std::string& scanPath = isStrm ? m_item.GetPath() : m_item.GetDynPath();
    if (!URIUtils::IsUPnP(scanPath) &&
        !m_item.GetProperty("no-ext-subs-scan").asBoolean(false))
      CUtil::ScanForExternalSubtitles(scanPath, filenames);

    // load any subtitles from file item
    std::string key("subtitle:1");
    for (unsigned s = 1; m_item.HasProperty(key); key = StringUtils::Format("subtitle:{}", ++s))
      filenames.push_back(m_item.GetProperty(key).asString());

    for (unsigned int i=0;i<filenames.size();i++)
    {
      // if vobsub subtitle:
      if (URIUtils::HasExtension(filenames[i], ".idx"))
      {
        std::string strSubFile;
        if (CUtil::FindVobSubPair( filenames, filenames[i], strSubFile))
          AddSubtitleFile(filenames[i], strSubFile);
      }
      else
      {
        if (!CUtil::IsVobSub(filenames, filenames[i] ))
        {
          AddSubtitleFile(filenames[i]);
        }
      }
    } // end loop over all subtitle files
  }

  m_clock.Reset();
  m_dvd.Clear();

  return true;
}

bool CVideoPlayer::OpenDemuxStream()
{
  CloseDemuxer();

  CLog::Log(LOGDEBUG, "Creating Demuxer");

  int attempts = 10;
  while (!m_bStop && attempts-- > 0)
  {
    m_pDemuxer.reset(CDVDFactoryDemuxer::CreateDemuxer(m_pInputStream));
    if(!m_pDemuxer && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER))
    {
      continue;
    }
    else if(!m_pDemuxer && m_pInputStream->NextStream() != CDVDInputStream::NEXTSTREAM_NONE)
    {
      CLog::Log(LOGDEBUG, "{} - New stream available from input, retry open", __FUNCTION__);
      continue;
    }
    break;
  }

  if (!m_pDemuxer)
  {
    CLog::Log(LOGERROR, "{} - Error creating demuxer", __FUNCTION__);
    return false;
  }

  m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_DEMUX);
  m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_NAV);
  m_SelectionStreams.Update(m_pInputStream, m_pDemuxer.get());
  m_pDemuxer->GetPrograms(m_programs);
  UpdateContent();
  m_demuxerSpeed = DVD_PLAYSPEED_NORMAL;
  m_processInfo->SetStateRealtime(false);

#if defined(HAVE_LIBBLURAY)
  {
    const auto pBluray = std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream);
    m_VideoPlayerSubtitle->SetSSIF(pBluray ? pBluray->GetSSIF() : nullptr);
  }
#endif

  int64_t len = m_pInputStream->GetLength();
  int64_t tim = m_pDemuxer->GetStreamLength();
  if (len > 0 && tim > 0)
    m_pInputStream->SetReadRate(static_cast<uint32_t>(len * 1000 / tim));

  m_offset_pts = 0;

  return true;
}

void CVideoPlayer::CloseDemuxer()
{
  m_pDemuxer.reset();
  m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_DEMUX);

  CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
  CServiceBroker::GetDataCacheCore().SignalVideoInfoChange();
  CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();
}

void CVideoPlayer::OpenDefaultStreams(bool reset)
{
  // if input stream dictate, we will open later
  // unless we are loading a bluray playlist directly in which case set now
  const bool noBlurayMenu{m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) &&
                          m_State.menuType == MenuType::NONE};
  if (!noBlurayMenu && (m_dvd.iSelectedAudioStream >= 0 || m_dvd.iSelectedSPUStream >= 0))
    return;

  bool valid;

  // open video stream
  valid   = false;

  int videoStreamPref = m_processInfo->GetVideoSettings().m_VideoStream;
  int preferredVideoId = m_pDemuxer ? m_pDemuxer->GetPreferredVideoStream() : -1;
  if (preferredVideoId >= 0)
  {
    for (const auto& s : m_SelectionStreams.Get(STREAM_VIDEO))
    {
      if (s.id == preferredVideoId)
      {
        videoStreamPref = s.type_index;
        break;
      }
    }
  }
  PredicateVideoFilter vf(videoStreamPref);
  for (const auto &stream : m_SelectionStreams.Get(STREAM_VIDEO, vf))
  {
    if (OpenStream(m_CurrentVideo, stream.demuxerId, stream.id, stream.source, reset))
    {
      valid = true;
      break;
    }
  }
  if (!valid)
  {
    CloseStream(m_CurrentVideo, true);
    m_processInfo->ResetVideoCodecInfo();
  }

  // open audio stream
  valid = false;
  if (!m_playerOptions.videoOnly)
  {
    PredicateAudioFilter af(m_processInfo->GetVideoSettings().m_AudioStream, m_playerOptions.preferStereo);
    for (const auto &stream : m_SelectionStreams.Get(STREAM_AUDIO, af))
    {
      if(OpenStream(m_CurrentAudio, stream.demuxerId, stream.id, stream.source, reset))
      {
        valid = true;
        break;
      }
    }
  }

  if(!valid)
  {
    CloseStream(m_CurrentAudio, true);
    m_processInfo->ResetAudioCodecInfo();
  }

  // enable  or disable subtitles
  bool visible = m_processInfo->GetVideoSettings().m_SubtitleOn;

  // open subtitle stream
  SelectionStream as = m_SelectionStreams.Get(STREAM_AUDIO, GetAudioStream());
  PredicateSubtitlePriority psp(as.language, m_processInfo->GetVideoSettings().m_SubtitleStream);
  valid = false;
  // We need to close CC subtitles to avoid conflicts with external sub stream
  if (m_CurrentSubtitle.source == STREAM_SOURCE_VIDEOMUX)
    CloseStream(m_CurrentSubtitle, false);

  for (const auto &stream : m_SelectionStreams.Get(STREAM_SUBTITLE, psp))
  {
    if (OpenStream(m_CurrentSubtitle, stream.demuxerId, stream.id, stream.source))
    {
      valid = true;
      if(!psp.relevant(stream))
        visible = false;
      break;
    }
  }
  if(!valid)
    CloseStream(m_CurrentSubtitle, false);

  // only set subtitle visibility if state not stored by dvd navigator, because navigator will restore it (if visible)
  if (!std::dynamic_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream) ||
      m_playerOptions.state.empty())
  {
    // SetEnableStream only if not visible, when visible OpenStream already implied that stream is enabled
    if (valid && !visible)
      SetEnableStream(m_CurrentSubtitle, false);

    SetSubtitleVisibleInternal(visible);
  }

  // open teletext stream
  valid   = false;
  for (const auto &stream : m_SelectionStreams.Get(STREAM_TELETEXT))
  {
    if (OpenStream(m_CurrentTeletext, stream.demuxerId, stream.id, stream.source))
    {
      valid = true;
      break;
    }
  }
  if(!valid)
    CloseStream(m_CurrentTeletext, false);

  // open RDS stream
  valid   = false;
  for (const auto &stream : m_SelectionStreams.Get(STREAM_RADIO_RDS))
  {
    if (OpenStream(m_CurrentRadioRDS, stream.demuxerId, stream.id, stream.source))
    {
      valid = true;
      break;
    }
  }
  if(!valid)
    CloseStream(m_CurrentRadioRDS, false);

  // open ID3 stream
  valid = false;
  for (const auto& stream : m_SelectionStreams.Get(STREAM_AUDIO_ID3))
  {
    if (OpenStream(m_CurrentAudioID3, stream.demuxerId, stream.id, stream.source))
    {
      valid = true;
      break;
    }
  }
  if (!valid)
    CloseStream(m_CurrentAudioID3, false);

  // disable demux streams
  if (m_item.IsRemote() && m_pDemuxer)
  {
    for (auto &stream : m_SelectionStreams.m_Streams)
    {
      if (STREAM_SOURCE_MASK(stream.source) == STREAM_SOURCE_DEMUX)
      {
        if (stream.id != m_CurrentVideo.id && stream.id != m_CurrentAudio.id &&
            stream.id != m_CurrentSubtitle.id && stream.id != m_CurrentTeletext.id &&
            stream.id != m_CurrentRadioRDS.id && stream.id != m_CurrentAudioID3.id)
        {
          m_pDemuxer->EnableStream(stream.demuxerId, stream.id, false);
        }
      }
    }
  }

  if (noBlurayMenu)
    SynchronizeDemuxer();
}

bool CVideoPlayer::ReadPacket(DemuxPacket*& packet, CDemuxStream*& stream)
{

  // check if we should read from subtitle demuxer
  if (m_pSubtitleDemuxer && !m_subtitleDemuxerEof && m_VideoPlayerSubtitle->AcceptsData())
  {
    packet = m_pSubtitleDemuxer->Read();

    if (!packet)
      m_subtitleDemuxerEof = true;

    if(packet)
    {
      UpdateCorrection(packet, m_offset_pts);
      if (auto* menus = dynamic_cast<CDVDInputStream::IMenus*>(m_pInputStream.get()))
      {
        int seamGen = 0;
        double seamCur = 0.0;
        double seamPrev = 0.0;
        if (menus->GetSeamTimeOffsets(seamGen, seamCur, seamPrev) && seamCur != 0.0)
          UpdateCorrection(packet, seamCur * DVD_TIME_BASE);
      }
      if(packet->iStreamId < 0)
        return true;

      stream = m_pSubtitleDemuxer->GetStream(packet->demuxerId, packet->iStreamId);
      if (!stream)
      {
        CLog::Log(LOGERROR, "{} - Error demux packet doesn't belong to a valid stream",
                  __FUNCTION__);
        return false;
      }
      if (stream->source == STREAM_SOURCE_NONE)
      {
        m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_DEMUX_SUB);
        m_SelectionStreams.Update(nullptr, m_pSubtitleDemuxer.get());
        UpdateContent();
      }
      return true;
    }
  }

  // read a data frame from stream.
  if (m_pDemuxer)
    packet = m_pDemuxer->Read();

  if (packet)
  {
    // stream changed, update and open defaults
    if (packet->iStreamId == DMX_SPECIALID_STREAMCHANGE)
    {
      m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_DEMUX);
      m_SelectionStreams.Update(m_pInputStream, m_pDemuxer.get());
      m_pDemuxer->GetPrograms(m_programs);
      UpdateContent();
      OpenDefaultStreams(false);

      // reevaluate HasVideo/Audio, we may have switched from/to a radio channel
      if(m_CurrentVideo.id < 0)
        m_HasVideo = false;
      if(m_CurrentAudio.id < 0)
        m_HasAudio = false;

      return true;
    }

    UpdateCorrection(packet, m_offset_pts);

    if(packet->iStreamId < 0)
      return true;

    if(m_pDemuxer)
    {
      stream = m_pDemuxer->GetStream(packet->demuxerId, packet->iStreamId);
      if (!stream)
      {
        CLog::Log(LOGERROR, "{} - Error demux packet doesn't belong to a valid stream",
                  __FUNCTION__);
        return false;
      }
      if(stream->source == STREAM_SOURCE_NONE)
      {
        m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_DEMUX);
        m_SelectionStreams.Update(m_pInputStream, m_pDemuxer.get());
        UpdateContent();
      }
    }
    return true;
  }
  return false;
}

namespace
{
constexpr double MENU_DOMAIN_RAMP_RATE = 0.75;
constexpr double MENU_DOMAIN_RAMP_HEADROOM = 0.5;
constexpr double MENU_DOMAIN_EMPTY_QUEUE_SECONDS = 0.05;
constexpr double MENU_DOMAIN_AUDIO_LOW_SECONDS = 0.4;
constexpr auto MENU_DOMAIN_EVAL_INTERVAL = std::chrono::milliseconds(250);
constexpr auto MENU_DOMAIN_STARVE_SUSTAIN = std::chrono::milliseconds(500);
constexpr auto MENU_DOMAIN_RAMP_MAX_STEP = std::chrono::milliseconds(1000);
}

void CVideoPlayer::UpdateMenuDomainQueueDepth(bool segmentOpen)
{
  const double clamp = static_cast<double>(CServiceBroker::GetSettingsComponent()
                                               ->GetAdvancedSettings()
                                               ->m_videoMenuDomainQueueTimeSize);
  if (clamp <= 0.0 || clamp >= m_messageQueueTimeSize)
    return;

  bool menuDomain = false;
  bool readDataPhase = false;
#if defined(HAVE_LIBBLURAY)
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY))
  {
    if (const std::shared_ptr<CDVDInputStreamBluray> bluray =
            std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
    {
      menuDomain = bluray->IsMenuDomainSegment();
      readDataPhase = bluray->IsReadInDataPhase();
    }
  }
#endif

  if (!menuDomain)
  {
    m_menuDomainSegment = false;
    m_menuDomainClampPending = false;
    m_menuDomainFillPending = false;
    m_menuDomainRampCap = 0.0;
    m_menuDomainStarveStart = {};
    if (!m_menuDomainLowLatency)
      return;

    m_menuDomainLowLatency = false;
    m_VideoPlayerAudio->SetMaxTimeSize(m_messageQueueTimeSize);
    m_VideoPlayerVideo->SetMaxTimeSize(m_messageQueueTimeSize);
    logM(LOGDEBUG, "menudomain: leaving low-latency mode, queue read-ahead {:.1f}s",
         m_messageQueueTimeSize);
    return;
  }

  const bool domainEntered = !m_menuDomainSegment;
  m_menuDomainSegment = true;

  if (m_menuDomainLowLatency)
  {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_menuDomainEvalLast < MENU_DOMAIN_EVAL_INTERVAL)
      return;
    m_menuDomainEvalLast = now;

    const double videoSecs = m_VideoPlayerVideo->GetQueueTimeSize();
    const double audioSecs = m_VideoPlayerAudio->GetQueueTimeSize();

    if (m_menuDomainFillPending)
    {
      if (std::max(videoSecs, audioSecs) < clamp)
        return;
      m_menuDomainFillPending = false;
      m_menuDomainRampCap = 0.0;
      m_VideoPlayerAudio->SetMaxTimeSize(clamp);
      m_VideoPlayerVideo->SetMaxTimeSize(clamp);
      logM(LOGDEBUG, "menudomain: queue filled, entering low-latency mode, queue read-ahead {:.1f}s",
           clamp);
      return;
    }

    if (m_menuDomainRampCap > clamp)
    {
      const auto step = std::min<std::chrono::steady_clock::duration>(
          now - m_menuDomainRampLast, MENU_DOMAIN_RAMP_MAX_STEP);
      m_menuDomainRampLast = now;
      double next =
          m_menuDomainRampCap - std::chrono::duration<double>(step).count() * MENU_DOMAIN_RAMP_RATE;
      next = std::min(next, std::max(videoSecs, audioSecs) + MENU_DOMAIN_RAMP_HEADROOM);
      next = std::max(next, clamp);
      m_menuDomainRampCap = next;
      m_VideoPlayerAudio->SetMaxTimeSize(next);
      m_VideoPlayerVideo->SetMaxTimeSize(next);
      if (next <= clamp)
        logM(LOGDEBUG, "menudomain: engage ramp settled, queue read-ahead {:.1f}s", clamp);
      m_menuDomainStarveStart = {};
      return;
    }

    const bool starving =
        m_playSpeed == DVD_PLAYSPEED_NORMAL && readDataPhase && m_HasVideo &&
        m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_INSYNC &&
        videoSecs < MENU_DOMAIN_EMPTY_QUEUE_SECONDS &&
        (m_CurrentAudio.id < 0 || audioSecs < MENU_DOMAIN_AUDIO_LOW_SECONDS) &&
        m_VideoPlayerVideo->AcceptsData() && m_VideoPlayerAudio->AcceptsData();
    if (!starving)
    {
      m_menuDomainStarveStart = {};
      return;
    }
    if (m_menuDomainStarveStart == std::chrono::steady_clock::time_point{})
    {
      m_menuDomainStarveStart = now;
      return;
    }
    if (now - m_menuDomainStarveStart < MENU_DOMAIN_STARVE_SUSTAIN)
      return;

    m_menuDomainLowLatency = false;
    m_menuDomainRampCap = 0.0;
    m_menuDomainStarveStart = {};
    m_VideoPlayerAudio->SetMaxTimeSize(m_messageQueueTimeSize);
    m_VideoPlayerVideo->SetMaxTimeSize(m_messageQueueTimeSize);
    logM(LOGDEBUG,
         "menudomain: starvation release, queue read-ahead {:.1f}s until the next menu segment",
         m_messageQueueTimeSize);
    return;
  }

  if (segmentOpen || domainEntered)
    m_menuDomainClampPending = true;

  if (!m_menuDomainClampPending)
    return;

  if (m_HasVideo && m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_STARTING)
    return;

  m_menuDomainClampPending = false;
  m_menuDomainLowLatency = true;
  m_menuDomainEvalLast = std::chrono::steady_clock::now();
  m_menuDomainStarveStart = {};
  const double queuedSecs =
      std::max(m_VideoPlayerVideo->GetQueueTimeSize(), m_VideoPlayerAudio->GetQueueTimeSize());
  if (queuedSecs > clamp + MENU_DOMAIN_RAMP_HEADROOM)
  {
    m_menuDomainRampCap = queuedSecs + MENU_DOMAIN_RAMP_HEADROOM;
    m_menuDomainRampLast = m_menuDomainEvalLast;
    m_VideoPlayerAudio->SetMaxTimeSize(m_menuDomainRampCap);
    m_VideoPlayerVideo->SetMaxTimeSize(m_menuDomainRampCap);
    logM(LOGDEBUG,
         "menudomain: entering low-latency mode via ramp from {:.1f}s, queue read-ahead target "
         "{:.1f}s",
         m_menuDomainRampCap, clamp);
  }
  else if (queuedSecs >= clamp)
  {
    m_menuDomainRampCap = 0.0;
    m_VideoPlayerAudio->SetMaxTimeSize(clamp);
    m_VideoPlayerVideo->SetMaxTimeSize(clamp);
    logM(LOGDEBUG, "menudomain: entering low-latency mode, queue read-ahead {:.1f}s", clamp);
  }
  else
  {
    m_menuDomainFillPending = true;
    m_menuDomainRampCap = 0.0;
    logM(LOGDEBUG,
         "menudomain: deferring the clamp until the queue fills to {:.1f}s (currently {:.1f}s)",
         clamp, queuedSecs);
  }
}

bool CVideoPlayer::IsValidStream(const CCurrentStream& stream) const {
  if(stream.id<0)
    return true; // we consider non selected as valid

  int source = STREAM_SOURCE_MASK(stream.source);
  if(source == STREAM_SOURCE_TEXT)
    return true;
  if (source == STREAM_SOURCE_DEMUX_SUB)
  {
    CDemuxStream* st = m_pSubtitleDemuxer->GetStream(stream.demuxerId, stream.id);
    if(st == nullptr || st->disabled)
      return false;
    if(st->type != stream.type)
      return false;
    return true;
  }
  if (source == STREAM_SOURCE_DEMUX)
  {
    CDemuxStream* st = m_pDemuxer->GetStream(stream.demuxerId, stream.id);
    if(st == nullptr || st->disabled)
      return false;
    if(st->type != stream.type)
      return false;

    if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
    {
      if (stream.type == STREAM_AUDIO && st->dvdNavId != m_dvd.iSelectedAudioStream)
        return false;
      if(stream.type == STREAM_SUBTITLE && st->dvdNavId != m_dvd.iSelectedSPUStream)
        return false;
    }

    return true;
  }
  if (source == STREAM_SOURCE_VIDEOMUX)
  {
    CDemuxStream* st = m_pCCDemuxer->GetStream(stream.id);
    if (st == nullptr || st->disabled)
      return false;
    if (st->type != stream.type)
      return false;
    return true;
  }

  return false;
}

bool CVideoPlayer::IsBetterStream(const CCurrentStream& current, CDemuxStream* stream) const {
  // Do not reopen non-video streams if we're in video-only mode
  if (m_playerOptions.videoOnly && current.type != STREAM_VIDEO)
    return false;

  if(stream->disabled)
    return false;

  if (m_pInputStream && (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD) ||
                         m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY)))
  {
    int source_type;

    source_type = STREAM_SOURCE_MASK(current.source);
    if (source_type != STREAM_SOURCE_DEMUX &&
        source_type != STREAM_SOURCE_NONE)
      return false;

    source_type = STREAM_SOURCE_MASK(stream->source);
    if(source_type != STREAM_SOURCE_DEMUX ||
       stream->type != current.type ||
       (stream->uniqueId == current.id && stream->demuxerId == current.demuxerId))
      return false;

    if(current.type == STREAM_AUDIO && stream->dvdNavId == m_dvd.iSelectedAudioStream)
      return true;
    if(current.type == STREAM_SUBTITLE && stream->dvdNavId == m_dvd.iSelectedSPUStream)
      return true;
    if(current.type == STREAM_VIDEO &&
       (current.id < 0 || stream->demuxerId != current.demuxerId))
      return true;
  }
  else
  {
    if(stream->source == current.source &&
       stream->uniqueId == current.id &&
       stream->demuxerId == current.demuxerId)
      return false;

    if(stream->type != current.type)
      return false;

    if(current.type == STREAM_SUBTITLE)
      return false;

    if(current.id < 0)
      return true;
  }
  return false;
}

void CVideoPlayer::CheckBetterStream(CCurrentStream& current, CDemuxStream* stream)
{
  IDVDStreamPlayer* player = GetStreamPlayer(current.player);
  if (!IsValidStream(current) && (player == nullptr || player->IsStalled()))
    CloseStream(current, true);

  if (IsBetterStream(current, stream))
    OpenStream(current, stream->demuxerId, stream->uniqueId, stream->source);
}

void CVideoPlayer::Prepare()
{
  m_menuDomainSegment = false;
  m_menuDomainClampPending = false;
  m_menuDomainFillPending = false;
  m_menuDomainRampCap = 0.0;
  m_menuDomainStarveStart = {};
  if (m_menuDomainLowLatency)
  {
    m_menuDomainLowLatency = false;
    m_VideoPlayerAudio->SetMaxTimeSize(m_messageQueueTimeSize);
    m_VideoPlayerVideo->SetMaxTimeSize(m_messageQueueTimeSize);
  }
  CFFmpegLog::SetLogLevel(1);
  SetPlaySpeed(DVD_PLAYSPEED_NORMAL);
  m_processInfo->SetSpeed(1.0);
  m_processInfo->SetTempo(1.0);
  m_processInfo->SetFrameAdvance(false);
  m_State.Clear();
  m_CurrentVideo.hint.Clear();
  m_CurrentAudio.hint.Clear();
  m_CurrentSubtitle.hint.Clear();
  m_CurrentTeletext.hint.Clear();
  m_CurrentRadioRDS.hint.Clear();
  m_CurrentAudioID3.hint.Clear();
  m_SpeedState.Reset(DVD_NOPTS_VALUE);
  m_offset_pts = 0;
  m_CurrentAudio.lastdts = DVD_NOPTS_VALUE;
  m_CurrentVideo.lastdts = DVD_NOPTS_VALUE;

  IPlayerCallback *cb = &m_callback;
  CFileItem fileItem = m_item;
  m_outboundEvents->Submit([=]() {
    cb->RequestVideoSettings(fileItem);
  });

  if (!OpenInputStream())
  {
    m_bAbortRequest = true;
    m_error = true;
    return;
  }

  bool discStateRestored = false;
  if (std::shared_ptr<CDVDInputStream::IMenus> ptr =
          std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream))
  {
    CLog::Log(LOGDEBUG, "VideoPlayer: playing a file with menus");

    if (!m_playerOptions.state.empty() && !(m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) &&
                                            m_State.menuType == MenuType::NONE))
    {
      discStateRestored = ptr->SetState(m_playerOptions.state);
    }
    else if (std::shared_ptr<CDVDInputStreamNavigator> nav =
                 std::dynamic_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream))
    {
      nav->EnableSubtitleStream(m_processInfo->GetVideoSettings().m_SubtitleOn);
    }
  }

  if (!OpenDemuxStream())
  {
    m_bAbortRequest = true;
    m_error = true;
    return;
  }
  // give players a chance to reconsider now codecs are known
  CreatePlayers();

  if (!discStateRestored)
    OpenDefaultStreams();

  /*
   * Check to see if the demuxer should start at something other than time 0. This will be the case
   * if there was a start time specified as part of the "Start from where last stopped" (aka
   * auto-resume) feature or if there is an EDL cut or commercial break that starts at time 0.
   */
  EDL::Edit edit;
  int starttime = 0;
  if (m_playerOptions.starttime > 0 || m_playerOptions.startpercent > 0)
  {
    if (m_playerOptions.startpercent > 0 && m_pDemuxer)
    {
      int playerStartTime = static_cast<int>((static_cast<double>(
          m_pDemuxer->GetStreamLength() * (m_playerOptions.startpercent / 100.0))));
      starttime = m_Edl.GetTimeAfterRestoringCuts(playerStartTime);
    }
    else
    {
      starttime = m_Edl.GetTimeAfterRestoringCuts(
          static_cast<int>(m_playerOptions.starttime * 1000)); // s to ms
    }
    CLog::Log(LOGDEBUG, "{} - Start position set to last stopped position: {}", __FUNCTION__,
              starttime);
  }
  else if (m_Edl.InEdit(starttime, &edit))
  {
    // save last edit times
    m_Edl.SetLastEditTime(edit.start);
    m_Edl.SetLastEditActionType(edit.action);

    if (edit.action == EDL::Action::CUT)
    {
      starttime = edit.end;
      CLog::Log(LOGDEBUG, "{} - Start position set to end of first cut: {}", __FUNCTION__,
                starttime);
    }
    else if (edit.action == EDL::Action::COMM_BREAK)
    {
      if (m_SkipCommercials)
      {
        starttime = edit.end;
        CLog::Log(LOGDEBUG, "{} - Start position set to end of first commercial break: {}",
                  __FUNCTION__, starttime);
      }

      const std::shared_ptr<CAdvancedSettings> advancedSettings =
          CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();
      if (advancedSettings && advancedSettings->m_EdlDisplayCommbreakNotifications)
      {
        const std::string timeString =
            StringUtils::SecondsToTimeString(edit.end / 1000, TIME_FORMAT_MM_SS);
        CGUIDialogKaiToast::QueueNotification(g_localizeStrings.Get(25011), timeString);
      }
    }
  }

  if (starttime > 0)
  {
    double startpts = DVD_NOPTS_VALUE;
    if (m_pDemuxer)
    {
      if (m_pDemuxer->SeekTime(starttime, true, &startpts))
        CLog::Log(LOGDEBUG, "{} - starting demuxer from: {}", __FUNCTION__, starttime);
      else
        CLog::Log(LOGDEBUG, "{} - failed to start demuxing from: {}", __FUNCTION__, starttime);
    }

    if (m_pSubtitleDemuxer)
    {
      m_subtitleDemuxerEof = false;
      if (m_pSubtitleDemuxer->SeekTime(starttime, true, &startpts))
        CLog::Log(LOGDEBUG, "{} - starting subtitle demuxer from: {}", __FUNCTION__, starttime);
      else
        CLog::Log(LOGDEBUG, "{} - failed to start subtitle demuxing from: {}", __FUNCTION__,
                  starttime);
    }

    m_clock.Discontinuity(DVD_MSEC_TO_TIME(starttime));
  }

  UpdatePlayState(0);

  SetCaching(CACHESTATE_FLUSH);
}

void CVideoPlayer::Process()
{
  // Try to resolve the correct mime type. This can take some time, for example if a requested
  // item is located at a slow/not reachable remote source. So, do mime type detection in vp worker
  // thread, not directly when initializing the player to keep GUI responsible.
  m_item.SetMimeTypeForInternetFile();

  m_parseCaptions = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
      CSettings::SETTING_SUBTITLES_PARSECAPTIONS);

  CServiceBroker::GetWinSystem()->RegisterRenderLoop(this);

  Prepare();

  while (!m_bAbortRequest)
  {
    // check display lost
    if (m_displayLost)
    {
      CThread::Sleep(50ms);
      continue;
    }

    // check if in an edit (cut or commercial break) that should be automatically skipped
    CheckAutoSceneSkip();

    // handle messages send to this thread, like seek or demuxer reset requests
    HandleMessages();

    if (m_bAbortRequest)
      break;

    // should we open a new input stream?
    if (!m_pInputStream)
    {
      if (OpenInputStream() == false)
      {
        m_bAbortRequest = true;
        break;
      }
    }

    // should we open a new demuxer?
    if (!m_pDemuxer)
    {
      if (m_pInputStream->NextStream() == CDVDInputStream::NEXTSTREAM_NONE)
        break;

      if (m_pInputStream->IsEOF())
        break;

      if (OpenDemuxStream() == false)
      {
        m_bAbortRequest = true;
        break;
      }

      // on channel switch we don't want to close stream players at this
      // time. we'll get the stream change event later
      if (!m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER) ||
          !m_SelectionStreams.m_Streams.empty())
        OpenDefaultStreams();

#if defined(HAVE_LIBBLURAY)
      if (auto bluray = std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
      {
        if (bluray->IsInMenu())
          bluray->RequestMenuOverlayRepost();
      }
#endif

      UpdatePlayState(0);
    }

    // handle eventual seeks due to playspeed
    HandlePlaySpeed();

    // update player state
    UpdatePlayState(200);

    // make sure we run subtitle process here
    m_VideoPlayerSubtitle->UpdatePlaybackPosition(m_clock.GetClock() + m_State.time_offset - m_VideoPlayerVideo->GetSubtitleDelay(), m_State.time_offset);

    // tell demuxer if we want to fill buffers
    if (m_demuxerSpeed != DVD_PLAYSPEED_PAUSE)
    {
      int audioLevel = 90;
      int videoLevel = 90;
      bool fillBuffer = false;
      if (m_CurrentAudio.id >= 0)
        audioLevel = m_VideoPlayerAudio->GetLevel();
      if (m_CurrentVideo.id >= 0)
        videoLevel = m_VideoPlayerVideo->GetLevel();
      if (videoLevel < 85 && audioLevel < 85)
      {
        fillBuffer = true;
      }
      if (m_pDemuxer)
        m_pDemuxer->FillBuffer(fillBuffer);
    }

    UpdateMenuDomainQueueDepth(false);

    if (CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
        CServiceBroker::GetLogging().CanLogComponent(LOGAVTIMING))
    {
      static auto lastPaceLog = std::chrono::steady_clock::time_point{};
      const auto nowPaceLog = std::chrono::steady_clock::now();
      if (nowPaceLog - lastPaceLog >= std::chrono::seconds(1))
      {
        lastPaceLog = nowPaceLog;
        const double renderPts = m_renderManager.GetRenderPts();
        const double demuxDts = m_CurrentVideo.dts;
        const double sinceRenderMs = (demuxDts != DVD_NOPTS_VALUE && renderPts != DVD_NOPTS_VALUE)
                                         ? (demuxDts - renderPts) / 1000.0
                                         : -1.0;
        logComponentM(LOGDEBUG, LOGAVTIMING,
                      "demuxpace: sinceRenderMs={:.0f} demuxDts={:.3f} renderPts={:.3f} vlvl={} "
                      "alvl={} vacc={} aacc={} vqSec={:.2f} aqSec={:.2f} maxSec={:.1f} lowLat={} "
                      "pend={} vsync={}",
                      sinceRenderMs, demuxDts * 1e-6, renderPts * 1e-6,
                      m_VideoPlayerVideo->GetLevel(), m_VideoPlayerAudio->GetLevel(),
                      m_VideoPlayerVideo->AcceptsData(), m_VideoPlayerAudio->AcceptsData(),
                      m_VideoPlayerVideo->GetQueueTimeSize(),
                      m_VideoPlayerAudio->GetQueueTimeSize(),
                      m_VideoPlayerVideo->GetMaxTimeSizeSeconds(), m_menuDomainLowLatency,
                      m_menuDomainClampPending, static_cast<int>(m_CurrentVideo.syncState));
      }
    }

    // if the queues are full, no need to read more
    if ((!m_VideoPlayerAudio->AcceptsData() && m_CurrentAudio.id >= 0) ||
        (!m_VideoPlayerVideo->AcceptsData() && m_CurrentVideo.id >= 0))
    {
      if (m_playSpeed == DVD_PLAYSPEED_PAUSE &&
          m_demuxerSpeed != DVD_PLAYSPEED_PAUSE)
      {
        if (m_pDemuxer)
          m_pDemuxer->SetSpeed(DVD_PLAYSPEED_PAUSE);
        m_demuxerSpeed = DVD_PLAYSPEED_PAUSE;
      }
      CThread::Sleep(10ms);
      continue;
    }

    // adjust demuxer speed; some rtsp servers wants to know for i.e. ff
    // delay pause until queue is full
    if (m_playSpeed != DVD_PLAYSPEED_PAUSE &&
        m_demuxerSpeed != m_playSpeed)
    {
      if (m_pDemuxer)
        m_pDemuxer->SetSpeed(m_playSpeed);
      m_demuxerSpeed = m_playSpeed;
    }

    DemuxPacket* pPacket = nullptr;
    CDemuxStream *pStream = nullptr;
    ReadPacket(pPacket, pStream);
    if (pPacket && !pStream)
    {
      /* probably a empty packet, just free it and move on */
      CDVDDemuxUtils::FreeDemuxPacket(pPacket);
      continue;
    }

    if (!pPacket)
    {
      // when paused, demuxer could be be returning empty
      if (m_playSpeed == DVD_PLAYSPEED_PAUSE)
        continue;

      // check for a still frame state
      if (std::shared_ptr<CDVDInputStream::IMenus> pStream = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream))
      {
        // stills will be skipped
        if(m_dvd.state == DVDSTATE_STILL)
        {
          if (m_dvd.iDVDStillTime > 0ms)
          {
            const auto now = std::chrono::steady_clock::now();
            const auto duration = now - m_dvd.iDVDStillStartTime;

            if (duration >= m_dvd.iDVDStillTime)
            {
              m_dvd.iDVDStillTime = 0ms;
              m_dvd.iDVDStillStartTime = {};
              m_dvd.state = DVDSTATE_NORMAL;
              pStream->SkipStill();
              continue;
            }
          }
        }
      }

      // if there is another stream available, reopen demuxer
      CDVDInputStream::ENextStream next = m_pInputStream->NextStream();
      if(next == CDVDInputStream::NEXTSTREAM_OPEN)
      {
        CloseDemuxer();

        SetCaching(CACHESTATE_DONE);
        bool flushOldStreams = false;
        if (std::shared_ptr<CDVDInputStream::IMenus> menu =
                std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream))
          flushOldStreams = menu->ConsumeDiscontinuityFlush();
        bool videoKeepAlive = false;
        bool naturalChain = false;
#if defined(HAVE_LIBBLURAY)
        if (std::shared_ptr<CDVDInputStreamBluray> bluray =
                std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
        {
          videoKeepAlive = bluray->ConsumeVideoCompatBoundary();
          naturalChain = bluray->ConsumeNaturalChainBoundary();
        }
#endif
        if (naturalChain && CServiceBroker::GetSettingsComponent()
                                ->GetAdvancedSettings()
                                ->m_videoBdBoundaryDrain)
          DrainStreamsAtBoundary();
        if (videoKeepAlive)
        {
          logComponentM(LOGDEBUG, LOGVIDEO,
                        "VideoPlayer: next stream, video-compatible boundary - keeping stream players alive");
          m_bdStreamReuse = true;
          FlushBuffers(DVD_NOPTS_VALUE, false, true);
          continue;
        }
        logComponentM(LOGDEBUG, LOGVIDEO, "VideoPlayer: next stream, {}",
                      flushOldStreams ? "flush old streams (menu/discontinuity)"
                                      : "wait for old streams to be finished");
        m_bdStreamReuse = false;
        CloseStream(m_CurrentAudio, !flushOldStreams);
        CloseStream(m_CurrentVideo, !flushOldStreams);

        m_CurrentAudio.Clear();
        m_CurrentVideo.Clear();
        m_CurrentSubtitle.Clear();
        continue;
      }

      // input stream asked us to just retry
      if(next == CDVDInputStream::NEXTSTREAM_RETRY)
      {
        CThread::Sleep(100ms);
        continue;
      }

      if (!HandleReadPacketEndOfStream()) break;

      continue;
    }

    m_eofRenderWaitStart = {};

    // see if we can find something better to play
    CheckBetterStream(m_CurrentAudio,    pStream);
    if (!pPacket->isELPackage)
      CheckBetterStream(m_CurrentVideo, pStream);
    CheckBetterStream(m_CurrentSubtitle, pStream);
    CheckBetterStream(m_CurrentTeletext, pStream);
    CheckBetterStream(m_CurrentRadioRDS, pStream);
    CheckBetterStream(m_CurrentAudioID3, pStream);

    // demux video stream
    if (m_parseCaptions && CheckIsCurrent(m_CurrentVideo, pStream, pPacket))
    {
      if (m_pCCDemuxer)
      {
        bool first = true;
        while (!m_bAbortRequest)
        {
          DemuxPacket *pkt = m_pCCDemuxer->Read(first ? pPacket : nullptr);
          if (!pkt)
            break;

          first = false;
          if (m_pCCDemuxer->GetNrOfStreams() != m_SelectionStreams.CountTypeOfSource(STREAM_SUBTITLE, STREAM_SOURCE_VIDEOMUX))
          {
            m_SelectionStreams.Clear(STREAM_SUBTITLE, STREAM_SOURCE_VIDEOMUX);
            m_SelectionStreams.Update(nullptr, m_pCCDemuxer.get(), "");
            UpdateContent();
            OpenDefaultStreams(false);
          }
          CDemuxStream *pSubStream = m_pCCDemuxer->GetStream(pkt->iStreamId);
          if (pSubStream && m_CurrentSubtitle.id == pkt->iStreamId && m_CurrentSubtitle.source == STREAM_SOURCE_VIDEOMUX)
            ProcessSubData(pSubStream, pkt);
          else
            CDVDDemuxUtils::FreeDemuxPacket(pkt);
        }
      }
    }

    if (IsInMenuInternal())
    {
      if (const std::shared_ptr<CDVDInputStream::IMenus>& menu = m_menus)
      {
        double correction = menu->GetTimeStampCorrection();
        if (pPacket->dts != DVD_NOPTS_VALUE && pPacket->dts > correction)
          pPacket->dts -= correction;
        if (pPacket->pts != DVD_NOPTS_VALUE && pPacket->pts > correction)
          pPacket->pts -= correction;
      }
      if (m_dvd.syncClock)
      {
        m_clock.Discontinuity(pPacket->dts);
        m_dvd.syncClock = false;
      }
    }

    // process the packet
    ProcessPacket(pStream, pPacket);
  }
}

bool CVideoPlayer::IsWaitingForVideoDrainAtEof()
{
  const auto waitingForVideoEof = !m_VideoPlayerVideo->HasData() &&
                                  m_VideoPlayerVideo->IsInited() &&
                                  !m_VideoPlayerVideo->IsEOS();

  const bool waitingForRender =
    m_renderManager.HasFutureFrame(m_clock.GetClock()) ||
    m_VideoPlayerVideo->IsOutputPictureInFlight();

  if (waitingForRender)
  {
    const auto now = std::chrono::steady_clock::now();

    if (m_eofRenderWaitStart == std::chrono::steady_clock::time_point{})
    {
      m_eofRenderWaitStart = now;
      logM(LOGDEBUG, "EOF render-drain wait begins (future frame queued in renderer)");
    }
    else if ((now - m_eofRenderWaitStart) > kEofRenderWaitMax)
    {
      logM(LOGINFO, "EOF render-drain wait exceeded {}s, proceeding with exit",
        std::chrono::duration_cast<std::chrono::seconds>(kEofRenderWaitMax).count());
      m_eofRenderWaitStart = {};
      return m_VideoPlayerVideo->HasData() || waitingForVideoEof;
    }
  }
  else if (m_eofRenderWaitStart != std::chrono::steady_clock::time_point{})
  {
    logM(LOGDEBUG, "EOF render-drain wait completed after {}ms",
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_eofRenderWaitStart).count());
    m_eofRenderWaitStart = {};
  }

  return m_VideoPlayerVideo->HasData() || waitingForVideoEof || waitingForRender;
}

bool CVideoPlayer::HandleReadPacketEndOfStream()
{
  if (m_CurrentVideo.inited)
  {
    m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::VIDEO_DRAIN));
  }
  else if (m_VideoPlayerVideo->IsInited() && !m_VideoPlayerVideo->HasData())
  {
    LOG_THROTTLE_PERIODIC_GENERAL(LOGDEBUG, 1000, "forcing video EOS at EOF");
    m_VideoPlayerVideo->SetEOS(true);
  }

  m_CurrentAudio.inited = false;
  m_CurrentVideo.inited = false;
  m_CurrentSubtitle.inited = false;
  m_CurrentTeletext.inited = false;
  m_CurrentRadioRDS.inited = false;
  m_CurrentAudioID3.inited = false;

  SetCaching(CACHESTATE_DONE);

  if (m_VideoPlayerAudio->HasData() || IsWaitingForVideoDrainAtEof())
  {
    CThread::Sleep(100ms);
    return true;
  }

  if (!m_pInputStream->IsEOF())
    logM(LOGINFO, "demuxer ran dry before input stream reported EOF");

  return false;
}

bool CVideoPlayer::CheckIsCurrent(const CCurrentStream& current,
                                  CDemuxStream* stream,
                                  DemuxPacket* pkg)
{
  if(current.id == pkg->iStreamId &&
     current.demuxerId == stream->demuxerId &&
     current.source == stream->source &&
     current.type == stream->type)
    return true;
  else
    return false;
}

void CVideoPlayer::ProcessPacket(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  // process packet if it belongs to selected stream.
  // for dvd's don't allow automatic opening of streams*/

  if (CheckIsCurrent(m_CurrentAudio, pStream, pPacket))
    ProcessAudioData(pStream, pPacket);
  else if (CheckIsCurrent(m_CurrentVideo, pStream, pPacket))
    ProcessVideoData(pStream, pPacket);
  else if (CheckIsCurrent(m_CurrentSubtitle, pStream, pPacket))
    ProcessSubData(pStream, pPacket);
  else if (CheckIsCurrent(m_CurrentTeletext, pStream, pPacket))
    ProcessTeletextData(pStream, pPacket);
  else if (CheckIsCurrent(m_CurrentRadioRDS, pStream, pPacket))
    ProcessRadioRDSData(pStream, pPacket);
  else if (CheckIsCurrent(m_CurrentAudioID3, pStream, pPacket))
    ProcessAudioID3Data(pStream, pPacket);
  else if (pPacket->isELPackage)
  {
    LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000, "packet from enhancement layer: size:{:d} dts:{:.3f} pts:{:.3f} dur:{:.3f}ms",
      pPacket->iSize, pPacket->dts/DVD_TIME_BASE, pPacket->pts/DVD_TIME_BASE, pPacket->duration/1000.0);
    m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, false));
  }
  else
  {
    CDVDDemuxUtils::FreeDemuxPacket(pPacket); // free it since we won't do anything with it
  }
}

void CVideoPlayer::CheckStreamChanges(CCurrentStream& current, CDemuxStream* stream)
{
  if (current.stream  != (void*)stream
  ||  current.changes != stream->changes)
  {
    /* check so that dmuxer hints or extra data hasn't changed */
    /* if they have, reopen stream */

    if (current.hint != CDVDStreamInfo(*stream, true))
    {
      m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_DEMUX);
      m_SelectionStreams.Update(m_pInputStream, m_pDemuxer.get());
      UpdateContent();
      OpenDefaultStreams(false);
    }

    current.stream = (void*)stream;
    current.changes = stream->changes;
  }
}

void CVideoPlayer::ProcessAudioData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentAudio, pStream);

  bool checkcont = CheckContinuity(m_CurrentAudio, pPacket);
  UpdateTimestamps(m_CurrentAudio, pPacket);

  if (checkcont && (m_CurrentAudio.avsync == CCurrentStream::AV_SYNC_CHECK))
    m_CurrentAudio.avsync = CCurrentStream::AV_SYNC_NONE;

  bool drop = false;
  if (CheckPlayerInit(m_CurrentAudio))
    drop = true;

  /*
   * If CheckSceneSkip() returns true then demux point is inside an EDL cut and the packets are dropped.
   */
  EDL::Edit edit;
  if (CheckSceneSkip(m_CurrentAudio))
  {
    drop = true;
  }
  else if (m_Edl.InEdit(DVD_TIME_TO_MSEC(m_CurrentAudio.dts + m_offset_pts), &edit) &&
           edit.action == EDL::Action::MUTE)
  {
    drop = true;
  }

  m_VideoPlayerAudio->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));

  if (!drop)
    m_CurrentAudio.packets++;
}

void CVideoPlayer::ProcessVideoData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentVideo, pStream);
  bool checkcont = false;

  if( pPacket->iSize != 4) //don't check the EOF_SEQUENCE of stillframes
  {
    checkcont = CheckContinuity(m_CurrentVideo, pPacket);
    UpdateTimestamps(m_CurrentVideo, pPacket);
  }
  if (checkcont && (m_CurrentVideo.avsync == CCurrentStream::AV_SYNC_CHECK))
    m_CurrentVideo.avsync = CCurrentStream::AV_SYNC_NONE;

  bool drop = false;
  if (CheckPlayerInit(m_CurrentVideo))
    drop = true;

  if (CheckSceneSkip(m_CurrentVideo))
    drop = true;

  m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));

  if (!drop)
    m_CurrentVideo.packets++;
}

namespace
{
bool IsCachableTextSubtitle(AVCodecID codec)
{
  switch (codec)
  {
    case AV_CODEC_ID_TEXT:
    case AV_CODEC_ID_SUBRIP:
    case AV_CODEC_ID_SSA:
    case AV_CODEC_ID_ASS:
    case AV_CODEC_ID_MOV_TEXT:
    case AV_CODEC_ID_WEBVTT:
      return true;
    default:
      return false;
  }
}

DemuxPacket* CopySubtitlePacket(const DemuxPacket* src)
{
  DemuxPacket* copy = CDVDDemuxUtils::AllocateDemuxPacket(src->iSize);
  if (!copy)
    return nullptr;
  if (src->iSize > 0 && src->pData)
    std::memcpy(copy->pData, src->pData, src->iSize);
  copy->iSize = src->iSize;
  copy->pts = src->pts;
  copy->dts = src->dts;
  copy->duration = src->duration;
  copy->iStreamId = src->iStreamId;
  copy->demuxerId = src->demuxerId;
  copy->iGroupId = src->iGroupId;
  copy->m_ptsOffsetCorrection = src->m_ptsOffsetCorrection;
  return copy;
}

constexpr size_t SUBTITLE_SEEK_CACHE_MAX_BYTES = 32 * 1024 * 1024;
}

void CVideoPlayer::ProcessSubData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentSubtitle, pStream);

  bool checkcont = false;
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY))
    checkcont = CheckContinuity(m_CurrentSubtitle, pPacket);

  UpdateTimestamps(m_CurrentSubtitle, pPacket);

  if (checkcont)
  {
    m_CurrentVideo.avsync = CCurrentStream::AV_SYNC_NONE;
    m_CurrentAudio.avsync = CCurrentStream::AV_SYNC_NONE;
  }

  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) &&
      pPacket->pts == DVD_NOPTS_VALUE && pPacket->dts == DVD_NOPTS_VALUE)
  {
    pPacket->pts = m_clock.GetClock() + m_State.time_offset;
  }

  bool drop = false;
  if (CheckPlayerInit(m_CurrentSubtitle))
    drop = true;

  if (CheckSceneSkip(m_CurrentSubtitle))
    drop = true;

  bool suppressDup = false;
  if (!m_subtitleReinjectedPts.empty() && pPacket->pts != DVD_NOPTS_VALUE &&
      m_subtitleSeekCacheStreamId == m_CurrentSubtitle.id)
  {
    const double eps = DVD_MSEC_TO_TIME(1);
    auto it = std::find_if(m_subtitleReinjectedPts.begin(), m_subtitleReinjectedPts.end(),
                           [&](const std::pair<double, int>& p) {
                             return p.second == pPacket->iSize &&
                                    std::abs(p.first - pPacket->pts) < eps;
                           });
    if (it != m_subtitleReinjectedPts.end())
    {
      suppressDup = true;
      m_subtitleReinjectedPts.erase(it);
    }
  }

  if (STREAM_SOURCE_MASK(pStream->source) == STREAM_SOURCE_DEMUX &&
      IsCachableTextSubtitle(pStream->codec))
  {
    if (m_subtitleSeekCacheStreamId != m_CurrentSubtitle.id ||
        m_subtitleSeekCacheDemuxerId != m_CurrentSubtitle.demuxerId)
    {
      ClearSubtitleSeekCache();
      m_subtitleSeekCacheStreamId = m_CurrentSubtitle.id;
      m_subtitleSeekCacheDemuxerId = m_CurrentSubtitle.demuxerId;
    }
    CacheSubtitlePacket(pPacket);
  }

  if (suppressDup)
    CDVDDemuxUtils::FreeDemuxPacket(pPacket);
  else
    m_VideoPlayerSubtitle->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));

  if(m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
    m_VideoPlayerSubtitle->UpdateOverlayInfo(std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream), LIBDVDNAV_BUTTON_NORMAL);
}

void CVideoPlayer::ProcessTeletextData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentTeletext, pStream);

  UpdateTimestamps(m_CurrentTeletext, pPacket);

  bool drop = false;
  if (CheckPlayerInit(m_CurrentTeletext))
    drop = true;

  if (CheckSceneSkip(m_CurrentTeletext))
    drop = true;

  m_VideoPlayerTeletext->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));
}

void CVideoPlayer::ProcessRadioRDSData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentRadioRDS, pStream);

  UpdateTimestamps(m_CurrentRadioRDS, pPacket);

  bool drop = false;
  if (CheckPlayerInit(m_CurrentRadioRDS))
    drop = true;

  if (CheckSceneSkip(m_CurrentRadioRDS))
    drop = true;

  m_VideoPlayerRadioRDS->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));
}

void CVideoPlayer::ProcessAudioID3Data(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentAudioID3, pStream);

  UpdateTimestamps(m_CurrentAudioID3, pPacket);

  bool drop = false;
  if (CheckPlayerInit(m_CurrentAudioID3))
    drop = true;

  if (CheckSceneSkip(m_CurrentAudioID3))
    drop = true;

  m_VideoPlayerAudioID3->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));
}

void CVideoPlayer::ClearSubtitleSeekCache()
{
  for (auto& [pts, pkt] : m_subtitleSeekCache)
    CDVDDemuxUtils::FreeDemuxPacket(pkt);
  m_subtitleSeekCache.clear();
  m_subtitleSeekCovered.clear();
  m_subtitleReinjectedPts.clear();
  m_subtitleSeekNewRun = true;
  m_subtitleSeekCurRun = -1;
  m_subtitleSeekCacheStreamId = -1;
  m_subtitleSeekCacheDemuxerId = -1;
  m_subtitleSeekCacheBytes = 0;
  m_pSubtitleCatchupDemuxer.reset();
  m_pSubtitleCatchupInput.reset();
}

void CVideoPlayer::CacheSubtitlePacket(DemuxPacket* pPacket)
{
  if (!pPacket || pPacket->pts == DVD_NOPTS_VALUE)
    return;

  const double start = pPacket->pts;
  const double end = pPacket->duration > 0 ? start + pPacket->duration : start;

  const auto extendCoverage = [&]() {
    if (m_subtitleSeekNewRun || m_subtitleSeekCurRun < 0 ||
        m_subtitleSeekCurRun >= static_cast<int>(m_subtitleSeekCovered.size()))
    {
      m_subtitleSeekCovered.emplace_back(start, end);
      m_subtitleSeekCurRun = static_cast<int>(m_subtitleSeekCovered.size()) - 1;
      m_subtitleSeekNewRun = false;
    }
    else
    {
      auto& run = m_subtitleSeekCovered[m_subtitleSeekCurRun];
      run.first = std::min(run.first, start);
      run.second = std::max(run.second, end);
    }
  };

  const auto [sameStartBegin, sameStartEnd] = m_subtitleSeekCache.equal_range(start);
  for (auto it = sameStartBegin; it != sameStartEnd; ++it)
  {
    if (it->second->iSize == pPacket->iSize)
    {
      extendCoverage();
      return;
    }
  }

  if (m_subtitleSeekCacheBytes + static_cast<size_t>(pPacket->iSize) >
      SUBTITLE_SEEK_CACHE_MAX_BYTES)
  {
    if (m_subtitleSeekCacheBytes < SUBTITLE_SEEK_CACHE_MAX_BYTES)
      logM(LOGWARNING,
           "subtitle seek-recall cache hit {} MiB cap; not caching further",
           SUBTITLE_SEEK_CACHE_MAX_BYTES / (1024 * 1024));
    m_subtitleSeekCacheBytes = SUBTITLE_SEEK_CACHE_MAX_BYTES;
    return;
  }

  DemuxPacket* copy = CopySubtitlePacket(pPacket);
  if (!copy)
    return;
  m_subtitleSeekCacheBytes += static_cast<size_t>(copy->iSize);
  m_subtitleSeekCache.emplace(start, copy);
  extendCoverage();
}

std::vector<DemuxPacket*> CVideoPlayer::FindActiveSubtitlePackets(double pts)
{
  std::vector<DemuxPacket*> active;
  for (const auto& [start, pkt] : m_subtitleSeekCache)
  {
    if (start > pts)
      break;
    if (pkt->duration > 0 && start + pkt->duration > pts)
      active.push_back(pkt);
  }
  return active;
}

bool CVideoPlayer::IsSubtitlePtsCovered(double pts) const
{
  const double eps = DVD_MSEC_TO_TIME(200);
  for (const auto& [from, to] : m_subtitleSeekCovered)
  {
    if (pts >= from - eps && pts <= to + eps)
      return true;
  }
  return false;
}

void CVideoPlayer::ReinjectSubtitlePackets(const std::vector<DemuxPacket*>& packets)
{
  for (const DemuxPacket* src : packets)
  {
    DemuxPacket* copy = CopySubtitlePacket(src);
    if (!copy)
      continue;
    m_VideoPlayerSubtitle->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(copy, false));
    if (src->pts != DVD_NOPTS_VALUE)
      m_subtitleReinjectedPts.emplace_back(src->pts, src->iSize);
  }
}

void CVideoPlayer::FetchActiveSubtitleFromFile(double seekTimeMs, double targetPts, int streamId)
{
  if (!m_pSubtitleCatchupDemuxer)
  {
    auto input = CDVDFactoryInputStream::CreateInputStream(nullptr, m_item);
    if (!input || !input->Open())
      return;
    auto demux = std::make_shared<CDVDDemuxFFmpeg>();
    if (!demux->Open(input, false))
      return;
    m_pSubtitleCatchupInput = input;
    m_pSubtitleCatchupDemuxer = demux;
  }

  constexpr double CATCHUP_WINDOW_MS = 10000.0;
  double from = seekTimeMs - CATCHUP_WINDOW_MS;
  if (from < 0)
    from = 0;
  if (!m_pSubtitleCatchupDemuxer->SeekTime(from, true))
    return;

  constexpr int MAX_PACKETS = 20000;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
  for (int i = 0; i < MAX_PACKETS; ++i)
  {
    if (std::chrono::steady_clock::now() >= deadline)
    {
      logM(LOGDEBUG, "subtitle seek-recall fallback hit time budget");
      break;
    }

    DemuxPacket* pkt = m_pSubtitleCatchupDemuxer->Read();
    if (!pkt)
      break;

    UpdateCorrection(pkt, m_offset_pts);

    const bool isWanted = pkt->iStreamId == streamId && pkt->pts != DVD_NOPTS_VALUE;
    if (isWanted)
      CacheSubtitlePacket(pkt);

    const double ref = pkt->pts != DVD_NOPTS_VALUE ? pkt->pts : pkt->dts;
    const bool passedTarget = ref != DVD_NOPTS_VALUE && ref > targetPts;
    CDVDDemuxUtils::FreeDemuxPacket(pkt);
    if (passedTarget)
      break;
  }

  ReinjectSubtitlePackets(FindActiveSubtitlePackets(targetPts));
}

void CVideoPlayer::RecallSubtitlesAfterSeek(double startPts, double seekTimeMs)
{
  m_subtitleReinjectedPts.clear();

  if (m_CurrentSubtitle.id < 0 || startPts == DVD_NOPTS_VALUE ||
      m_subtitleSeekCacheStreamId != m_CurrentSubtitle.id)
    return;

  std::vector<DemuxPacket*> active = FindActiveSubtitlePackets(startPts);
  if (!active.empty())
  {
    logM(LOGDEBUG, "subtitle seek-recall hit ({} event(s)) at {:.3f}s",
         active.size(), startPts / DVD_TIME_BASE);
    ReinjectSubtitlePackets(active);
    return;
  }

  if (m_subtitleSeekRecallFromFile && !IsSubtitlePtsCovered(startPts) &&
      dynamic_cast<CDVDDemuxFFmpeg*>(m_pDemuxer.get()))
  {
    logM(LOGDEBUG, "subtitle seek-recall miss at {:.3f}s; reading from file",
         startPts / DVD_TIME_BASE);
    FetchActiveSubtitleFromFile(seekTimeMs, startPts, m_CurrentSubtitle.id);
  }
}

CacheInfo CVideoPlayer::GetCachingTimes()
{
  CacheInfo info{};

  if (!m_pInputStream || !m_pDemuxer)
    return info;

  XFILE::SCacheStatus status;
  if (!m_pInputStream->GetCacheStatus(&status))
    return info;

  const uint64_t& maxforward = status.maxforward;
  const uint64_t& cached = status.forward;
  const uint32_t& currate = status.currate;
  const uint32_t& maxrate = status.maxrate;
  const uint32_t& lowrate = status.lowrate;

  int64_t length = m_pInputStream->GetLength();
  int64_t remain = length - m_pInputStream->Seek(0, SEEK_CUR);

  if (length <= 0 || remain < 0)
    return info;

  double queueTime = GetQueueTime();
  double play_sbp = DVD_MSEC_TO_TIME(m_pDemuxer->GetStreamLength()) / length;
  double queued = 1000.0 * queueTime / play_sbp;

  info.level = 0.0;
  info.offset = (cached + queued) / length;
  info.time = 0.0;
  info.valid = true;

  if (currate == 0)
    return info;

  // estimated playback time of current cached bytes
  const double cacheTime = (static_cast<double>(cached) / currate) + (queueTime / 1000.0);

  // cache level as current forward bytes / max forward bytes [0.0 - 1.0]
  const double cacheLevel = (maxforward > 0) ? static_cast<double>(cached) / maxforward : 0.0;

  info.time = cacheTime;

  if (lowrate > 0)
  {
    // buffer is full & our read rate is too low
    CLog::Log(LOGDEBUG, "Readrate {} was too low with {} required", lowrate, maxrate);
    info.level = -1.0;
  }
  else
    info.level = cacheLevel;

  return info;
}

void CVideoPlayer::HandlePlaySpeed()
{
  const bool isInMenu = IsInMenuInternal();
  m_processInfo->SetInMenu(isInMenu);
  const bool tolerateStall =
      isInMenu || (m_CurrentVideo.hint.flags & StreamFlags::FLAG_STILL_IMAGES);

  if (tolerateStall && m_caching != CACHESTATE_DONE)
    SetCaching(CACHESTATE_DONE);

  if (m_caching == CACHESTATE_FULL)
  {
    CacheInfo cache = GetCachingTimes();
    if (cache.valid)
    {
      if (cache.level < 0.0)
      {
        CGUIDialogKaiToast::QueueNotification(g_localizeStrings.Get(21454), g_localizeStrings.Get(21455));
        SetCaching(CACHESTATE_INIT);
      }
      // Note: Previously used cache.level >= 1 would keep video stalled
      // event after cache was full
      // Talk link: https://github.com/xbmc/xbmc/pull/23760
      if (cache.time > m_messageQueueTimeSize)
        SetCaching(CACHESTATE_INIT);
    }
    else
    {
      if ((!m_VideoPlayerAudio->AcceptsData() && m_CurrentAudio.id >= 0) ||
          (!m_VideoPlayerVideo->AcceptsData() && m_CurrentVideo.id >= 0))
        SetCaching(CACHESTATE_INIT);
    }

    // if audio stream stalled, wait until demux queue filled 10%
    if (m_pInputStream->IsRealtime() &&
        (m_CurrentAudio.id < 0 || m_VideoPlayerAudio->GetLevel() > 10))
    {
      SetCaching(CACHESTATE_INIT);
    }
  }

  if (m_caching == CACHESTATE_INIT)
  {
    // if all enabled streams have been inited we are done
    if ((m_CurrentVideo.id >= 0 || m_CurrentAudio.id >= 0) &&
        (m_CurrentVideo.id < 0 || m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_STARTING) &&
        (m_CurrentAudio.id < 0 || m_CurrentAudio.syncState != IDVDStreamPlayer::SYNC_STARTING))
      SetCaching(CACHESTATE_PLAY);

    // handle exceptions
    if (m_CurrentAudio.id >= 0 && m_CurrentVideo.id >= 0)
    {
      if ((!m_VideoPlayerAudio->AcceptsData() || !m_VideoPlayerVideo->AcceptsData()) &&
          m_cachingTimer.IsTimePast())
      {
        SetCaching(CACHESTATE_DONE);
      }
    }
  }

  if (m_caching == CACHESTATE_PLAY)
  {
    // if all enabled streams have started playing we are done
    if ((m_CurrentVideo.id < 0 || !m_VideoPlayerVideo->IsStalled()) &&
        (m_CurrentAudio.id < 0 || !m_VideoPlayerAudio->IsStalled()))
      SetCaching(CACHESTATE_DONE);
  }

  if (m_caching == CACHESTATE_DONE)
  {
    if (m_playSpeed == DVD_PLAYSPEED_NORMAL && !tolerateStall)
    {
      // take action if audio or video stream is stalled
      if (((m_VideoPlayerAudio->IsStalled() && m_CurrentAudio.inited) ||
           (m_VideoPlayerVideo->IsStalled() && m_CurrentVideo.inited)) &&
          m_syncTimer.IsTimePast())
      {
        if (m_pInputStream->IsRealtime())
        {
          if ((m_CurrentAudio.id >= 0 && m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_INSYNC &&
               m_VideoPlayerAudio->IsStalled()) ||
              (m_CurrentVideo.id >= 0 && m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_INSYNC &&
               (m_VideoPlayerVideo->GetLevel() == 0)))
          {
            logComponentM(LOGDEBUG, LOGAVTIMING, "Stream stalled, start buffering. Audio: {} - Video: {}",
                      m_VideoPlayerAudio->GetLevel(), m_VideoPlayerVideo->GetLevel());

            if (m_VideoPlayerAudio->AcceptsData() && m_VideoPlayerVideo->AcceptsData())
              SetCaching(CACHESTATE_FULL);
            else
              FlushBuffers(DVD_NOPTS_VALUE, false, true);
          }
        }
        else
        {
          // start caching if audio and video are running dry
          // Use a lower threshold (5%) for video when HW decoding, because AML
          // consumes packets in bursts and the queue level naturally fluctuates.
          // The original 20% threshold causes false rebuffering with high bitrate content.
          int audioThreshold = 20;
          int videoThreshold = m_processInfo->IsVideoHwDecoder() ? 5 : 20;
          if ((m_VideoPlayerAudio->GetLevel() <= audioThreshold) ||
              (m_VideoPlayerVideo->GetLevel() <= videoThreshold))
          {
            SetCaching(CACHESTATE_FULL);
          }
          else if (m_CurrentAudio.id >= 0 && m_CurrentAudio.inited &&
                   m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_INSYNC &&
                   m_VideoPlayerAudio->GetLevel() == 0 &&
                   !m_VideoPlayerAudio->IsPassthrough())
          {
            // Only trigger re-sync for PCM audio. For passthrough (TrueHD, DTS-HD MA),
            // the audio queue naturally empties between codec output bursts — a momentary
            // level of 0 is normal, not a stall. Flushing here causes a destructive
            // seek loop with HD audio.
            logComponentM(LOGDEBUG, LOGAVTIMING,"CVideoPlayer::HandlePlaySpeed - audio stream stalled, triggering re-sync");
            FlushBuffers(DVD_NOPTS_VALUE, true, true);
            CDVDMsgPlayerSeek::CMode mode;
            mode.time = (int)GetUpdatedTime();
            mode.backward = false;
            mode.accurate = true;
            mode.sync = true;
            m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
          }
        }
      }
      // care for live streams
      else if (m_pInputStream->IsRealtime())
      {
        if (m_CurrentAudio.id >= 0 && m_clock.GetClock() > DVD_MSEC_TO_TIME(1000) && !IsPassthrough())
        {
          // Proportional speed adjustment for live streams instead of binary toggle.
          // Old logic toggled between -0.05 and 0.0 causing oscillation.
          // New logic: ramp linearly between the thresholds for smoother playback.
          int aq = m_VideoPlayerAudio->GetLevel();
          double currentAdjust = m_clock.GetSpeedAdjust();
          double adjust = -1.0; // sentinel: no change
          if (aq < 1 && currentAdjust >= 0)
          {
            adjust = -0.05;
            logComponentM(LOGDEBUG, LOGAVTIMING, "VideoPlayer:Speed adjust:{:.3f} aq:{:d}", adjust, aq);
          }
          else if (aq >= 1 && aq <= 4 && currentAdjust <= 0)
          {
            // Proportional ramp: at aq=1 use -0.0375, aq=2 use -0.025,
            // aq=3 use -0.0125, aq=4 use 0.0
            adjust = -0.05 * (1.0 - static_cast<double>(aq) / 4.0);
            // Clamp to avoid tiny negative values from float imprecision
            if (adjust > -0.001)
              adjust = 0.0;
            LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGAVTIMING, 1000, "VideoPlayer:Speed adjust:{:.3f} aq:{:d} (ramping)", adjust, aq);
          }
          else if (aq > 4 && currentAdjust < 0)
          {
            adjust = 0.0;
            logComponentM(LOGDEBUG, LOGAVTIMING, "VideoPlayer:Speed adjust:{:.3f} aq:{:d}", adjust, aq);
          }
          if (adjust != -1.0)
          {
            m_clock.SetSpeedAdjust(adjust);
          }
        }
      }
    }
  }

  const bool brokenFileGate =
      m_pDemuxer && m_pInputStream && !m_pInputStream->IsRealtime() &&
      m_playSpeed == DVD_PLAYSPEED_NORMAL && !tolerateStall &&
      m_caching == CACHESTATE_DONE &&
      m_CurrentAudio.inited && m_CurrentVideo.inited &&
      m_VideoPlayerAudio->IsStalled() && m_VideoPlayerVideo->IsStalled() &&
      CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
          CSettings::SETTING_COREELEC_AMLOGIC_DETECT_BROKEN_FILES);
  if (brokenFileGate)
  {
    const auto now = std::chrono::steady_clock::now();
    if (m_brokenFileStallStart == std::chrono::steady_clock::time_point{})
    {
      m_brokenFileStallStart = now;
      m_brokenFileStallBytes = m_pDemuxer->GetSourceReadBytes();
    }
    else if (now - m_brokenFileStallStart >= std::chrono::seconds(5))
    {
      const int64_t readBytes = m_pDemuxer->GetSourceReadBytes();
      if (readBytes >= 0 && m_brokenFileStallBytes >= 0 &&
          readBytes - m_brokenFileStallBytes >= CDVDDemux::BROKEN_SOURCE_MIN_SCAN_BYTES)
      {
        if (!m_brokenFileNotified)
        {
          m_brokenFileNotified = true;
          logM(LOGERROR, "audio and video both stalled for 5+ seconds during normal playback "
                         "while the demuxer keeps reading; treating source as broken - stopping playback");
          CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning,
                                                g_localizeStrings.Get(60628), g_localizeStrings.Get(60629),
                                                TOAST_DISPLAY_TIME * 2);
        }
        m_pDemuxer->MarkBroken();
      }
      else if (!m_brokenFileStallStarveLogged)
      {
        m_brokenFileStallStarveLogged = true;
        logM(LOGWARNING, "audio and video stalled for 5+ seconds without demuxer read progress; "
                         "treating as I/O starvation, not a broken file");
      }
    }
  }
  else
  {
    m_brokenFileStallStart = {};
    m_brokenFileStallBytes = -1;
    m_brokenFileStallStarveLogged = false;
  }

  // sync streams to clock
  if ((m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_WAITSYNC) ||
      (m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_WAITSYNC))
  {
    unsigned int threshold = 20;
    if (m_pInputStream->IsRealtime())
      threshold = 40;

    AUDIODELAY_LOG("VP.Sync.enter",
                   "audioState={} videoState={} audioPackets={} videoPackets={} "
                   "audioId={} videoId={} threshold={} audioLevel={} videoLevel={}",
                   static_cast<int>(m_CurrentAudio.syncState),
                   static_cast<int>(m_CurrentVideo.syncState),
                   m_CurrentAudio.packets,
                   m_CurrentVideo.packets,
                   m_CurrentAudio.id,
                   m_CurrentVideo.id,
                   threshold,
                   m_VideoPlayerAudio->GetLevel(),
                   m_VideoPlayerVideo->GetLevel());

    bool video = (m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_WAITSYNC) ||
                 (m_CurrentVideo.packets == 0 && m_CurrentAudio.packets > threshold) ||
                 (!m_VideoPlayerAudio->AcceptsData() && (m_VideoPlayerVideo->GetLevel() < 10));
    bool audio = m_CurrentAudio.id < 0 || (m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_WAITSYNC) ||
                 (m_CurrentAudio.packets == 0 && m_CurrentVideo.packets > threshold) ||
                 (!m_VideoPlayerVideo->AcceptsData() && (m_VideoPlayerAudio->GetLevel() < 10));

    if (m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_WAITSYNC &&
        (m_CurrentAudio.avsync == CCurrentStream::AV_SYNC_CONT ||
         m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_INSYNC))
    {
      logComponentM(LOGDEBUG, LOGAUDIO, "VideoPlayer::Sync - Audio - Waiting, clock: {:.3f}", m_clock.GetClock());
      AUDIODELAY_LOG("VP.Sync.branch",
                     "branch=AudioFastPath clock={:.3f} audioAvsync={} videoState={}",
                     m_clock.GetClock(),
                     static_cast<int>(m_CurrentAudio.avsync),
                     static_cast<int>(m_CurrentVideo.syncState));
      m_CurrentAudio.syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_CurrentAudio.avsync = CCurrentStream::AV_SYNC_NONE;
      m_VideoPlayerAudio->SendMessage(
          std::make_shared<CDVDMsgDouble>(CDVDMsg::GENERAL_RESYNC, m_clock.GetClock()), 1);
    }
    else if (m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_WAITSYNC &&
             (m_CurrentVideo.avsync == CCurrentStream::AV_SYNC_CONT ||
             m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_INSYNC))
    {
      logComponentM(LOGDEBUG, LOGVIDEO, "VideoPlayer::Sync - Video - Waiting, clock: {:.3f}", m_clock.GetClock());
      AUDIODELAY_LOG("VP.Sync.branch",
                     "branch=VideoFastPath clock={:.3f} videoAvsync={} audioState={}",
                     m_clock.GetClock(),
                     static_cast<int>(m_CurrentVideo.avsync),
                     static_cast<int>(m_CurrentAudio.syncState));
      m_CurrentVideo.syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_CurrentVideo.avsync = CCurrentStream::AV_SYNC_NONE;
      m_VideoPlayerVideo->SendMessage(
          std::make_shared<CDVDMsgDouble>(CDVDMsg::GENERAL_RESYNC, m_clock.GetClock()), 1);
    }
    else if (video && audio)
    {
      double clock = 0;
      const char* branchPicked = "None";
      const bool audioTimingValid = (m_CurrentAudio.id >= 0 && m_VideoPlayerAudio);
      const double freshAudioDelay = audioTimingValid ? m_VideoPlayerAudio->GetCurrentSinkDelay() : -1.0;
      const double audioClockNow = audioTimingValid ? m_VideoPlayerAudio->GetAudioClock() : 0.0;
      const bool willFlushAudio = audioTimingValid &&
                                  m_CurrentVideo.starttime != DVD_NOPTS_VALUE &&
                                  m_CurrentVideo.starttime > audioClockNow - freshAudioDelay + 0.5 * DVD_TIME_BASE;
      if (m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
        LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGAVTIMING, 1000, "VideoPlayer::Sync - Audio - pts: {:.3f}, cache: {:.3f}, totalcache: {:.3f}, packets:{:d} level:{:d}",
                             m_CurrentAudio.starttime / DVD_TIME_BASE, m_CurrentAudio.cachetime / DVD_TIME_BASE, m_CurrentAudio.cachetotal / DVD_TIME_BASE, m_CurrentAudio.packets, m_VideoPlayerAudio->GetLevel());
      if (m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
        LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGAVTIMING, 1000, "VideoPlayer::Sync - Video - pts: {:.3f}, cache: {:.3f}, totalcache: {:.3f}, packets:{:d} level:{:d}",
                             m_CurrentVideo.starttime / DVD_TIME_BASE, m_CurrentVideo.cachetime / DVD_TIME_BASE, m_CurrentVideo.cachetotal / DVD_TIME_BASE, m_CurrentVideo.packets, m_VideoPlayerVideo->GetLevel());

      const bool audioReady = m_CurrentAudio.id < 0 ||
                              (m_CurrentAudio.starttime != DVD_NOPTS_VALUE && m_CurrentAudio.packets > 0);
      const bool videoReady = m_CurrentVideo.starttime != DVD_NOPTS_VALUE && m_CurrentVideo.packets > 0;
      if (!(audioReady && videoReady) && !m_pInputStream->IsRealtime())
      {
        const auto now = std::chrono::steady_clock::now();
        if (m_avResyncDeferStart == std::chrono::steady_clock::time_point{})
          m_avResyncDeferStart = now;
        const auto waited = now - m_avResyncDeferStart;
        if (waited < kAvResyncDeferMax)
          return;
        if (!videoReady && (m_CurrentAudio.id < 0 || !audioReady))
        {
          if (m_State.dts == DVD_NOPTS_VALUE)
            return;
          if (waited < kAvResyncNoSourceDeferMax)
            return;
        }
        m_avResyncDeferStart = std::chrono::steady_clock::time_point{};
      }
      else
      {
        m_avResyncDeferStart = std::chrono::steady_clock::time_point{};
      }

      // LAV sync fix: When using LAV passthrough sync with both audio and video streams,
      // wait for video to have valid PTS before sending RESYNC to audio.
      // This prevents audio from syncing to a clock that doesn't account for video latency.
      // NOTE: We still process video RESYNC normally - only audio RESYNC is delayed.
      const int algoValue = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
                                        CSettings::SETTING_COREELEC_AMLOGIC_DV_AUDIO_SEAMLESSBRANCH);
      const bool enableLavStyle = ((algoValue == 3) || (algoValue == 5));
      const bool waitingForVideoPts = enableLavStyle && (m_CurrentVideo.id >= 0) && (m_CurrentVideo.starttime == DVD_NOPTS_VALUE);

      if (m_CurrentVideo.starttime != DVD_NOPTS_VALUE && m_CurrentVideo.packets > 0 &&
          m_playSpeed == DVD_PLAYSPEED_PAUSE)
      {
        clock = m_CurrentVideo.starttime;
        branchPicked = "Pause";
      }
      else if (m_CurrentAudio.starttime != DVD_NOPTS_VALUE && m_CurrentAudio.packets > 0)
      {
        if (m_pInputStream->IsRealtime())
        {
          clock = m_CurrentAudio.starttime - m_CurrentAudio.cachetime - DVD_MSEC_TO_TIME(1000);
          branchPicked = "DefaultRealtime";
        }
        else
        {
          clock = m_CurrentAudio.starttime - m_CurrentAudio.cachetime;
          branchPicked = "DefaultAudio";
        }

        if (m_CurrentVideo.starttime != DVD_NOPTS_VALUE && (m_CurrentVideo.packets > 0))
        {
          if (m_CurrentVideo.starttime > clock &&
              m_CurrentVideo.starttime <= m_CurrentAudio.starttime &&
              !m_pInputStream->IsRealtime())
          {
            const double videoAhead = m_CurrentVideo.starttime - clock;
            if (willFlushAudio || videoAhead <= m_CurrentAudio.cachetime * 0.7)
            {
              clock = m_CurrentVideo.starttime;
              branchPicked = "Override2a";
            }
          }
          else if (m_CurrentVideo.starttime - m_CurrentVideo.cachetotal < clock)
          {
            clock = m_CurrentVideo.starttime;
            branchPicked = "Override2b";
          }
          else if (m_CurrentVideo.starttime > m_CurrentAudio.starttime &&
                   !m_pInputStream->IsRealtime())
          {
            const int audioLevel = m_VideoPlayerAudio->GetLevel();
            const double audioTimeMs = m_messageQueueTimeSize * 1000.0 * audioLevel / 100.0;
            const double maxAudioTime = clock + DVD_MSEC_TO_TIME(audioTimeMs);
            if ((m_CurrentVideo.starttime - m_CurrentVideo.cachetotal) > maxAudioTime)
            {
              clock = maxAudioTime;
              branchPicked = "Override2cMaxAudio";
            }
            else
            {
              clock = m_CurrentVideo.starttime - m_CurrentVideo.cachetotal;
              branchPicked = "Override2cCacheTotal";
            }
          }
        }
      }
      else if (m_CurrentVideo.starttime != DVD_NOPTS_VALUE)
      {
        clock = m_CurrentVideo.starttime;
        branchPicked = "VideoOnly";
      }
      else if (m_State.dts != DVD_NOPTS_VALUE)
      {
        clock = m_State.dts;
        branchPicked = "FallbackDts";
      }

      AUDIODELAY_LOG("VP.Sync.branch",
                     "branch=FullResync subBranch={} clock={:.3f} audioStart={:.3f} videoStart={:.3f} "
                     "audioCachetime={:.3f} audioCacheTotal={:.3f} videoCacheTotal={:.3f} "
                     "freshAudioDelay={:.3f} audioClock={:.3f} willFlushAudio={} waitingForVideoPts={} enableLavStyle={}",
                     branchPicked,
                     clock,
                     (m_CurrentAudio.starttime == DVD_NOPTS_VALUE) ? -1.0 : m_CurrentAudio.starttime,
                     (m_CurrentVideo.starttime == DVD_NOPTS_VALUE) ? -1.0 : m_CurrentVideo.starttime,
                     m_CurrentAudio.cachetime,
                     m_CurrentAudio.cachetotal,
                     m_CurrentVideo.cachetotal,
                     freshAudioDelay,
                     audioClockNow,
                     willFlushAudio ? 1 : 0,
                     waitingForVideoPts ? 1 : 0,
                     enableLavStyle ? 1 : 0);
      m_clock.Discontinuity(clock);

      // Only send RESYNC to audio if video PTS is valid (LAV sync fix)
      // This prevents audio from syncing to garbage during video startup
      if (!waitingForVideoPts)
      {
        AUDIODELAY_LOG("VP.Sync.resyncAudio",
                       "sending GENERAL_RESYNC to audio clock={:.3f}",
                       clock);
        m_CurrentAudio.syncState = IDVDStreamPlayer::SYNC_INSYNC;
        m_CurrentAudio.avsync = CCurrentStream::AV_SYNC_NONE;
        m_VideoPlayerAudio->SendMessage(
            std::make_shared<CDVDMsgDouble>(CDVDMsg::GENERAL_RESYNC, clock), 1);
      }

      m_CurrentVideo.syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_CurrentVideo.avsync = CCurrentStream::AV_SYNC_NONE;
      m_VideoPlayerVideo->SendMessage(
          std::make_shared<CDVDMsgDouble>(CDVDMsg::GENERAL_RESYNC, clock), 1);

      SetCaching(CACHESTATE_DONE);
      UpdatePlayState(0);

      m_syncTimer.Set(3000ms);

      if (!m_State.streamsReady)
      {
        // m_subtitleSeekGate.Set(7000ms);
        if (m_playerOptions.fullscreen)
        {
          CServiceBroker::GetAppMessenger()->PostMsg(TMSG_SWITCHTOFULLSCREEN);
        }

        IPlayerCallback *cb = &m_callback;
        CFileItem fileItem = m_item;
        m_outboundEvents->Submit([=]() {
          cb->OnAVStarted(fileItem);
        });
        m_State.streamsReady = true;
      }
    }
    else
    {
      // exceptions for which stream players won't start properly
      // 1. videoplayer has not detected a keyframe within length of demux buffers
      if (m_CurrentAudio.id >= 0 && m_CurrentVideo.id >= 0 &&
          !m_VideoPlayerAudio->AcceptsData() &&
          m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_STARTING &&
          m_VideoPlayerVideo->IsStalled() &&
          m_CurrentVideo.packets > 10)
      {
        m_VideoPlayerAudio->AcceptsData();
        logComponentM(LOGWARNING, LOGAVTIMING, "VideoPlayer::Sync - stream player video does not start, flushing buffers");
        FlushBuffers(DVD_NOPTS_VALUE, true, true);
      }
    }
  }

  // handle ff/rw
  if (m_playSpeed != DVD_PLAYSPEED_NORMAL && m_playSpeed != DVD_PLAYSPEED_PAUSE)
  {
    if (isInMenu)
    {
      // this can't be done in menu
      SetPlaySpeed(DVD_PLAYSPEED_NORMAL);

    }
    else
    {
      bool check = true;
      const double currentPts = m_VideoPlayerVideo->GetCurrentPts();
      const bool playbackStalled = m_VideoPlayerVideo->IsPlaybackStalled();

      const int64_t nowAbsMs = DVD_TIME_TO_MSEC(m_clock.GetAbsoluteClock());

      // only check if we have video
      if (m_CurrentVideo.id < 0 || m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_INSYNC)
        check = false;
      // video message queue either initiated or already seen eof
      else if (m_CurrentVideo.inited == false && m_playSpeed >= 0)
        check = false;
      // Don't check too frequently (HandlePlaySpeed can spin many times within the same
      // playback millisecond, especially during rewind). Use monotonic absolute time.
      else if (m_SpeedState.lasttime > 0 && (nowAbsMs - m_SpeedState.lasttime) < 20)
        check = false;
      // skip if frame at screen has no valid timestamp
      else if (currentPts == DVD_NOPTS_VALUE)
        check = false;
      // skip if frame on screen has not changed while fast-forwarding.
      // If output has stalled, or the frame has been unchanged for "too long",
      // keep checking so trickplay can trigger catch-up seeks.
      else if ((m_playSpeed > 0) &&
               (m_SpeedState.lastpts == currentPts) &&
               (m_SpeedState.lastpts > m_State.dts))
      {
        if (!playbackStalled)
        {
          const double unchangedFor = m_clock.GetAbsoluteClock() - m_SpeedState.lastabstime;
          if (unchangedFor < DVD_MSEC_TO_TIME(250)) check = false;
        }
      }

      if (check)
      {
        m_SpeedState.lastpts  = currentPts;
        m_SpeedState.lasttime = nowAbsMs;
        m_SpeedState.lastabstime = m_clock.GetAbsoluteClock();

        double error;
        error  = m_clock.GetClock() - m_SpeedState.lastpts;
        error *= m_playSpeed / abs(m_playSpeed);
        const double absRawError = std::abs(error);

        // allow a bigger error when going ff, the faster we go
        // the the bigger is the error we allow
        if (m_playSpeed > DVD_PLAYSPEED_NORMAL)
        {
          double errorwin = static_cast<double>(m_playSpeed) / DVD_PLAYSPEED_NORMAL;
          if (errorwin > 8.0)
            errorwin = 8.0;
          error /= errorwin;
        }
        LOG_THROTTLE_PERIODIC(LOGDEBUG, LOGVIDEO, 1000, "CVideoPlayer::Process - ffd/rwd: lastpts:{:.3f} clock:{:.3f} lastseekpts:{:.3f} speed:{:d} error:{:.3f}",
          m_SpeedState.lastpts / 1000000.0, m_clock.GetClock() / 1000000.0, m_SpeedState.lastseekpts / 1000000.0, (int)m_playSpeed, error / 1000000.0);


        const double absScaledError = std::abs(error);
        const bool highSpeedFastForward = m_playSpeed > (DVD_PLAYSPEED_NORMAL * 2);
        const bool veryHighSpeedFastForward = m_playSpeed >= (DVD_PLAYSPEED_NORMAL * 8);
        const bool stalledFastForward = highSpeedFastForward && m_VideoPlayerVideo->IsPlaybackStalled();
        const bool allowRawErrorGate = stalledFastForward || veryHighSpeedFastForward;

        const int64_t sinceLastSeekMs = (m_SpeedState.lastseektime > 0)
                                          ? (nowAbsMs - m_SpeedState.lastseektime)
                                          : ((m_playSpeed < 0) ? 250 : 1000);

        bool doSeek = false;
        double seekErrorMs = 0.0;

        // Rewind needs periodic backward seeks to synthesize reverse playback.
        // Don't depend on the clock-vs-PTS error becoming large, as it can remain small
        // while the displayed PTS makes little/no backward progress.
        if (m_playSpeed < 0)
        {
          if (sinceLastSeekMs >= 250) doSeek = true;
        }
        else if ((absScaledError > DVD_MSEC_TO_TIME(1000)) ||
                 (allowRawErrorGate && (absRawError > DVD_MSEC_TO_TIME(1000))))
        {
          seekErrorMs = std::abs((m_clock.GetClock() - m_SpeedState.lastseekpts) / 1000);

          if (sinceLastSeekMs < 1000)
          { }
          else if (seekErrorMs > 1000 || (m_VideoPlayerVideo->IsPlaybackStalled() && seekErrorMs > 100))
          {
            doSeek = true;
          }
        }

        if (doSeek)
        {
          m_SpeedState.lastseekpts = m_clock.GetClock();
          m_SpeedState.lastseektime = nowAbsMs;

          double iTime = 0.0;
          if (m_playSpeed < 0)
          {
            double anchorPts = (currentPts != DVD_NOPTS_VALUE) ? currentPts : m_clock.GetClock();
            if (m_SpeedState.rewindTargetPts != DVD_NOPTS_VALUE)
              anchorPts = std::min(anchorPts, m_SpeedState.rewindTargetPts);

            const double speedFactor = static_cast<double>(std::abs(m_playSpeed)) / DVD_PLAYSPEED_NORMAL;
            double dtSec = static_cast<double>(sinceLastSeekMs) / 1000.0;
            if (dtSec < 0.25) dtSec = 0.25;
            if (dtSec > 2.0) dtSec = 2.0;

            double rewindStepSec = speedFactor * dtSec;
            if (rewindStepSec < 1.0) rewindStepSec = 1.0;
            if (rewindStepSec > 10.0) rewindStepSec = 10.0;

            const double rewindStep = DVD_SEC_TO_TIME(rewindStepSec);
            const double nextTargetPts = std::max(0.0, anchorPts - rewindStep);
            m_SpeedState.rewindTargetPts = nextTargetPts;
            iTime = (nextTargetPts + m_State.time_offset) / 1000;
          }
          else
          {
            iTime = (m_clock.GetClock() + m_State.time_offset + DVD_SEC_TO_TIME(1.0)) / 1000;
          }

          CDVDMsgPlayerSeek::CMode mode;
          mode.time = iTime;
          mode.backward = (m_playSpeed < 0);
          mode.accurate = false;
          mode.restore = false;
          mode.trickplay = true;
          mode.sync = false;
          m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
        }
      }
    }
  }

  // reset tempo
  if (!m_State.cantempo)
  {
    float currentTempo = m_processInfo->GetNewTempo();
    if (currentTempo != 1.0f)
    {
      SetTempo(1.0f);
    }
  }
}

void CVideoPlayer::QueueSubtitleSwitchSeek(const SelectionStream& stream)
{
  // if (!m_subtitleSeekGate.IsTimePast())
  //   return;

  const auto sourceMask = static_cast<StreamSource>(STREAM_SOURCE_MASK(stream.source));
  if (sourceMask != STREAM_SOURCE_DEMUX)
    return;

  CDVDMsgPlayerSeek::CMode mode;
  if ((m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_STARTING ||
       m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_STARTING) &&
      m_State.dts != DVD_NOPTS_VALUE)
    mode.time = static_cast<double>(DVD_TIME_TO_MSEC(m_State.dts + m_State.time_offset));
  else
    mode.time = static_cast<double>(GetUpdatedTime());

  mode.backward = true;
  mode.accurate = true;
  mode.trickplay = true;
  mode.sync = true;
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
}

bool CVideoPlayer::CheckPlayerInit(CCurrentStream& current)
{
  if (current.inited)
    return false;

  if (current.startpts != DVD_NOPTS_VALUE)
  {
    if(current.dts == DVD_NOPTS_VALUE)
    {
      CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayer::{} - Dropping packet type:{:d} dts:{:.3f} to get to start point at {:.3f}",
        __FUNCTION__, current.player,  current.dts / 1000000.0, current.startpts / 1000000.0);
      return true;
    }

    if ((current.startpts - current.dts) > DVD_SEC_TO_TIME(20))
    {
      CLog::Log(LOGDEBUG, "{} - too far to decode before finishing seek", __FUNCTION__);
      if(m_CurrentAudio.startpts != DVD_NOPTS_VALUE)
        m_CurrentAudio.startpts = current.dts;
      if(m_CurrentVideo.startpts != DVD_NOPTS_VALUE)
        m_CurrentVideo.startpts = current.dts;
      if(m_CurrentSubtitle.startpts != DVD_NOPTS_VALUE)
        m_CurrentSubtitle.startpts = current.dts;
      if(m_CurrentTeletext.startpts != DVD_NOPTS_VALUE)
        m_CurrentTeletext.startpts = current.dts;
      if(m_CurrentRadioRDS.startpts != DVD_NOPTS_VALUE)
        m_CurrentRadioRDS.startpts = current.dts;
      if (m_CurrentAudioID3.startpts != DVD_NOPTS_VALUE)
        m_CurrentAudioID3.startpts = current.dts;
    }

    if(current.dts < current.startpts)
    {
      CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayer::{} - dropping packet type:{:d} dts:{:.3f} to get to start point at {:.3f}",
        __FUNCTION__, current.player,  current.dts / 1000000.0, current.startpts / 1000000.0);
      return true;
    }
  }

  if (current.dts != DVD_NOPTS_VALUE)
  {
    current.inited = true;
    current.startpts = current.dts;
  }
  return false;
}

void CVideoPlayer::UpdateCorrection(DemuxPacket* pkt, double correction)
{
  pkt->m_ptsOffsetCorrection = correction;

  if(pkt->dts != DVD_NOPTS_VALUE)
    pkt->dts -= correction;
  if(pkt->pts != DVD_NOPTS_VALUE)
    pkt->pts -= correction;
}

void CVideoPlayer::UpdateTimestamps(CCurrentStream& current, DemuxPacket* pPacket)
{
  double dts = current.dts;
  /* update stored values */
  if(pPacket->dts != DVD_NOPTS_VALUE)
    dts = pPacket->dts;
  else if(pPacket->pts != DVD_NOPTS_VALUE)
    dts = pPacket->pts;

  /* calculate some average duration */
  if(pPacket->duration != DVD_NOPTS_VALUE)
    current.dur = pPacket->duration;
  else if(dts != DVD_NOPTS_VALUE && current.dts != DVD_NOPTS_VALUE)
    current.dur = 0.1 * (current.dur * 9 + (dts - current.dts));

  current.dts = dts;

  current.dispTime = pPacket->dispTime;
}

static void UpdateLimits(double& minimum, double& maximum, double dts)
{
  if(dts == DVD_NOPTS_VALUE)
    return;
  if(minimum == DVD_NOPTS_VALUE || minimum > dts) minimum = dts;
  if(maximum == DVD_NOPTS_VALUE || maximum < dts) maximum = dts;
}

bool CVideoPlayer::CheckContinuity(CCurrentStream& current, DemuxPacket* pPacket)
{
  if (m_playSpeed < DVD_PLAYSPEED_PAUSE)
    return false;

  if( pPacket->dts == DVD_NOPTS_VALUE || current.dts == DVD_NOPTS_VALUE)
    return false;

  double mindts = DVD_NOPTS_VALUE, maxdts = DVD_NOPTS_VALUE;
  UpdateLimits(mindts, maxdts, m_CurrentAudio.dts);
  UpdateLimits(mindts, maxdts, m_CurrentVideo.dts);
  UpdateLimits(mindts, maxdts, m_CurrentAudio.dts_end());
  UpdateLimits(mindts, maxdts, m_CurrentVideo.dts_end());
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY))
  {
    UpdateLimits(mindts, maxdts, m_CurrentSubtitle.dts);
    UpdateLimits(mindts, maxdts, m_CurrentSubtitle.dts_end());
  }

  /* if we don't have max and min, we can't do anything more */
  if( mindts == DVD_NOPTS_VALUE || maxdts == DVD_NOPTS_VALUE )
    return false;

  double correction = 0.0;
  if( pPacket->dts > maxdts + DVD_MSEC_TO_TIME(1000))
  {
    logComponentM(LOGDEBUG, LOGAVTIMING,
              "CVideoPlayer::CheckContinuity - resync forward :{}, prev:{:f}, curr:{:f}, diff:{:f}",
              current.type, current.dts, pPacket->dts, pPacket->dts - maxdts);
    correction = pPacket->dts - maxdts;
  }

  /* if it's large scale jump, correct for it after having confirmed the jump */
  if (pPacket->dts + DVD_MSEC_TO_TIME(500) < current.dts_end())
  {
    const bool isStreamStartReorderArtefact =
        current.type == STREAM_VIDEO &&
        current.hint.codec == AV_CODEC_ID_H264 &&
        current.packets < 64 && pPacket->dts < DVD_MSEC_TO_TIME(100);
    if (isStreamStartReorderArtefact)
    {
      CLog::Log(LOGDEBUG,
                "CVideoPlayer::CheckContinuity - stream-start reorder :{}, "
                "prev:{:f}, curr:{:f}, diff:{:f}",
                current.type, current.dts, pPacket->dts, pPacket->dts - current.dts);
    }
    else
    {
      CLog::Log(
          LOGDEBUG,
          "CVideoPlayer::CheckContinuity - resync backward :{}, prev:{:f}, curr:{:f}, diff:{:f}",
          current.type, current.dts, pPacket->dts, pPacket->dts - current.dts);
      correction = pPacket->dts - current.dts_end();
    }
  }
  else if(pPacket->dts < current.dts)
  {
    logComponentM(LOGDEBUG, LOGAVTIMING,
              "CVideoPlayer::CheckContinuity - wrapback :{}, prev:{:f}, curr:{:f}, diff:{:f}",
              current.type, current.dts, pPacket->dts, pPacket->dts - current.dts);
  }

  double lastdts = pPacket->dts;
  if(correction != 0.0)
  {
    // we want the dts values of two streams to close, or for one to be invalid (e.g. from a missing audio stream)
    double this_dts = pPacket->dts;
    double that_dts =
        current.type == STREAM_AUDIO ? m_CurrentVideo.lastdts : m_CurrentAudio.lastdts;

    if (m_CurrentAudio.id == -1 || m_CurrentVideo.id == -1 ||
       current.type == STREAM_SUBTITLE ||
       current.lastdts == DVD_NOPTS_VALUE ||
       fabs(this_dts - that_dts) < DVD_MSEC_TO_TIME(1000))
    {
      m_offset_pts += correction;
      UpdateCorrection(pPacket, correction);
      lastdts = pPacket->dts;
      logComponentM(LOGDEBUG, LOGAVTIMING, "CVideoPlayer::CheckContinuity - update correction: {:f}", correction);
      if (current.avsync == CCurrentStream::AV_SYNC_CHECK)
        current.avsync = CCurrentStream::AV_SYNC_CONT;
    }
    else
    {
      // not sure yet - flags the packets as unknown until we get confirmation on another audio/video packet
      pPacket->dts = DVD_NOPTS_VALUE;
      pPacket->pts = DVD_NOPTS_VALUE;
    }
  }
  else
  {
    if (current.avsync == CCurrentStream::AV_SYNC_CHECK)
      current.avsync = CCurrentStream::AV_SYNC_CONT;
  }
  current.lastdts = lastdts;
  return true;
}

bool CVideoPlayer::CheckSceneSkip(const CCurrentStream& current)
{
  if (!m_Edl.HasEdits())
    return false;

  if(current.dts == DVD_NOPTS_VALUE)
    return false;

  if(current.inited == false)
    return false;

  EDL::Edit edit;
  return m_Edl.InEdit(DVD_TIME_TO_MSEC(current.dts + m_offset_pts), &edit) &&
         edit.action == EDL::Action::CUT;
}

void CVideoPlayer::CheckAutoSceneSkip()
{
  if (!m_Edl.HasEdits())
    return;

  // Check that there is an audio and video stream.
  if((m_CurrentAudio.id < 0 || m_CurrentAudio.syncState != IDVDStreamPlayer::SYNC_INSYNC) ||
     (m_CurrentVideo.id < 0 || m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_INSYNC))
    return;

  // If there is a startpts defined for either the audio or video stream then VideoPlayer is still
  // still decoding frames to get to the previously requested seek point.
  if (m_CurrentAudio.inited == false ||
      m_CurrentVideo.inited == false)
    return;

  const int64_t clock = GetTime();

  const double correctClock = m_Edl.GetTimeAfterRestoringCuts(clock);
  EDL::Edit edit;
  if (!m_Edl.InEdit(correctClock, &edit))
  {
    // @note: Users are allowed to jump back into EDL commercial breaks
    // do not reset the last edit time if the last surpassed edit is a commercial break
    if (m_Edl.GetLastEditActionType() != EDL::Action::COMM_BREAK)
    {
      m_Edl.ResetLastEditTime();
    }
    return;
  }

  if (edit.action == EDL::Action::CUT)
  {
    if ((m_playSpeed > 0 && correctClock < (edit.start + 1000)) ||
        (m_playSpeed < 0 && correctClock < (edit.end - 1000)))
    {
      CLog::Log(LOGDEBUG, "{} - Clock in EDL cut [{} - {}]: {}. Automatically skipping over.",
                __FUNCTION__, CEdl::MillisecondsToTimeString(edit.start),
                CEdl::MillisecondsToTimeString(edit.end), CEdl::MillisecondsToTimeString(clock));

      // Seeking either goes to the start or the end of the cut depending on the play direction.
      int seek = m_playSpeed >= 0 ? edit.end : edit.start;
      if (m_Edl.GetLastEditTime() != seek)
      {
        CDVDMsgPlayerSeek::CMode mode;
        mode.time = seek;
        mode.backward = true;
        mode.accurate = true;
        mode.restore = false;
        mode.trickplay = false;
        mode.sync = true;
        m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));

        m_Edl.SetLastEditTime(seek);
        m_Edl.SetLastEditActionType(edit.action);
      }
    }
  }
  else if (edit.action == EDL::Action::COMM_BREAK)
  {
    // marker for commbreak may be inaccurate. allow user to skip into break from the back
    if (m_playSpeed >= 0 && m_Edl.GetLastEditTime() != edit.start && clock < edit.end - 1000)
    {
      const std::shared_ptr<CAdvancedSettings> advancedSettings =
          CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();
      if (advancedSettings && advancedSettings->m_EdlDisplayCommbreakNotifications)
      {
        const std::string timeString =
            StringUtils::SecondsToTimeString((edit.end - edit.start) / 1000, TIME_FORMAT_MM_SS);
        CGUIDialogKaiToast::QueueNotification(g_localizeStrings.Get(25011), timeString);
      }

      m_Edl.SetLastEditTime(edit.start);
      m_Edl.SetLastEditActionType(edit.action);

      if (m_SkipCommercials)
      {
        CLog::Log(LOGDEBUG,
                  "{} - Clock in commercial break [{} - {}]: {}. Automatically skipping to end of "
                  "commercial break",
                  __FUNCTION__, CEdl::MillisecondsToTimeString(edit.start),
                  CEdl::MillisecondsToTimeString(edit.end), CEdl::MillisecondsToTimeString(clock));

        CDVDMsgPlayerSeek::CMode mode;
        mode.time = edit.end;
        mode.backward = true;
        mode.accurate = true;
        mode.restore = false;
        mode.trickplay = false;
        mode.sync = true;
        m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
      }
    }
  }
}


void CVideoPlayer::SynchronizeDemuxer()
{
  if(IsCurrentThread())
    return;
  if(!m_messenger.IsInited())
    return;

  auto message = std::make_shared<CDVDMsgGeneralSynchronize>(500ms, SYNCSOURCE_PLAYER);
  m_messenger.Put(message);
  message->Wait(m_bStop, 0);
}

IDVDStreamPlayer* CVideoPlayer::GetStreamPlayer(unsigned int target) const {
  if(target == VideoPlayer_AUDIO)
    return m_VideoPlayerAudio;
  if(target == VideoPlayer_VIDEO)
    return m_VideoPlayerVideo;
  if(target == VideoPlayer_SUBTITLE)
    return m_VideoPlayerSubtitle;
  if(target == VideoPlayer_TELETEXT)
    return m_VideoPlayerTeletext;
  if(target == VideoPlayer_RDS)
    return m_VideoPlayerRadioRDS;
  if (target == VideoPlayer_ID3)
    return m_VideoPlayerAudioID3.get();
  return nullptr;
}

void CVideoPlayer::SendPlayerMessage(std::shared_ptr<CDVDMsg> pMsg, unsigned int target)
{
  IDVDStreamPlayer* player = GetStreamPlayer(target);
  if(player)
    player->SendMessage(std::move(pMsg), 0);
}

void CVideoPlayer::OnExit()
{
  CLog::Log(LOGDEBUG, "CVideoPlayer::OnExit()");

  // set event to inform openfile something went wrong in case openfile is still waiting for this event
  SetCaching(CACHESTATE_DONE);

  // close each stream
  if (!m_bAbortRequest)
    CLog::Log(LOGDEBUG, "VideoPlayer: eof, waiting for queues to empty");

  CFileItem fileItem(m_item);
  UpdateFileItemStreamDetails(fileItem);

  CloseStream(m_CurrentAudio, !m_bAbortRequest);
  CloseStream(m_CurrentVideo, !m_bAbortRequest);
  CloseStream(m_CurrentTeletext,!m_bAbortRequest);
  CloseStream(m_CurrentRadioRDS, !m_bAbortRequest);
  CloseStream(m_CurrentAudioID3, !m_bAbortRequest);
  // the generalization principle was abused for subtitle player. actually it is not a stream player like
  // video and audio. subtitle player does not run on its own thread, hence waitForBuffers makes
  // no sense here. waitForBuffers is abused to clear overlay container (false clears container)
  // subtitles are added from video player. after video player has finished, overlays have to be cleared.
  CloseStream(m_CurrentSubtitle, false);  // clear overlay container

  CServiceBroker::GetWinSystem()->UnregisterRenderLoop(this);

  IPlayerCallback *cb = &m_callback;
  CVideoSettings vs = m_processInfo->GetVideoSettings();
  m_outboundEvents->Submit([=]() {
    cb->StoreVideoSettings(fileItem, vs);
  });

  CBookmark bookmark;
  bookmark.totalTimeInSeconds = 0;
  bookmark.timeInSeconds = 0;
  if (m_State.startTime == 0)
  {
    bookmark.totalTimeInSeconds = m_State.timeMax / 1000;
    bookmark.timeInSeconds = m_State.time / 1000;
  }
  bookmark.player = m_name;
  bookmark.playerState = GetPlayerState();
  m_outboundEvents->Submit([=]() {
    cb->OnPlayerCloseFile(fileItem, bookmark);
  });

  // destroy objects
  m_renderManager.Flush(false, false);
  m_pDemuxer.reset();
  m_pSubtitleDemuxer.reset();
  m_subtitleDemuxerMap.clear();
  ClearSubtitleSeekCache();
  m_pCCDemuxer.reset();
  m_menus.reset();
  if (m_pInputStream.use_count() > 1)
    throw std::runtime_error("m_pInputStream reference count is greater than 1");
  m_pInputStream.reset();

  // clean up all selection streams
  m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_NONE);

  m_messenger.End();

  CFFmpegLog::ClearLogLevel();
  m_bStop = true;

  bool error = m_error;
  bool close = m_bCloseRequest;
  m_outboundEvents->Submit([=]() {
    if (close)
      cb->OnPlayBackStopped();
    else if (error)
      cb->OnPlayBackError();
    else
      cb->OnPlayBackEnded();
  });
}

void CVideoPlayer::HandleMessages()
{
  std::shared_ptr<CDVDMsg> pMsg = nullptr;

  while (m_messenger.Get(pMsg, 0ms) == MSGQ_OK)
  {
    if (pMsg->IsType(CDVDMsg::PLAYER_OPENFILE) &&
        m_messenger.GetPacketCount(CDVDMsg::PLAYER_OPENFILE) == 0)
    {
      CDVDMsgOpenFile& msg(*std::static_pointer_cast<CDVDMsgOpenFile>(pMsg));

      IPlayerCallback *cb = &m_callback;
      CFileItem fileItem(m_item);
      UpdateFileItemStreamDetails(fileItem);
      CVideoSettings vs = m_processInfo->GetVideoSettings();
      m_outboundEvents->Submit([=]() {
        cb->StoreVideoSettings(fileItem, vs);
      });

      CBookmark bookmark;
      bookmark.totalTimeInSeconds = 0;
      bookmark.timeInSeconds = 0;
      if (m_State.startTime == 0)
      {
        bookmark.totalTimeInSeconds = m_State.timeMax / 1000;
        bookmark.timeInSeconds = m_State.time / 1000;
      }
      bookmark.player = m_name;
      bookmark.playerState = GetPlayerState();
      m_outboundEvents->Submit([=]() {
        cb->OnPlayerCloseFile(fileItem, bookmark);
      });

      m_item = msg.GetItem();
      m_playerOptions = msg.GetOptions();

      m_processInfo->SetPlayTimes(0,0,0,0);

      m_outboundEvents->Submit([this]() {
        m_callback.OnPlayBackStarted(m_item);
      });

      FlushBuffers(DVD_NOPTS_VALUE, true, true);
      m_renderManager.Flush(false, false);
      m_pDemuxer.reset();
      m_pSubtitleDemuxer.reset();
      m_subtitleDemuxerMap.clear();
      ClearSubtitleSeekCache();
      m_pCCDemuxer.reset();
      m_menus.reset();
      if (m_pInputStream.use_count() > 1)
        throw std::runtime_error("m_pInputStream reference count is greater than 1");
      m_pInputStream.reset();

      m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_NONE);

      Prepare();
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SEEK) &&
        m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK) == 0 &&
        m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK_CHAPTER) == 0)
    {
      CDVDMsgPlayerSeek& msg(*std::static_pointer_cast<CDVDMsgPlayerSeek>(pMsg));

      if (!m_State.canseek)
      {
        m_processInfo->SetStateSeeking(false);
        continue;
      }

      // skip seeks if player has not finished the last seek
      if (m_CurrentVideo.id >= 0 &&
          m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_INSYNC)
      {
        double now = m_clock.GetAbsoluteClock();
        if (m_playSpeed == DVD_PLAYSPEED_NORMAL &&
            (now - m_State.lastSeek)/1000 < 2000 &&
            msg.GetTrickPlay())
        {
          m_processInfo->SetStateSeeking(false);
          continue;
        }
      }

      const ECacheState cacheStateBeforeSeek = m_caching;
      const bool shouldFlushCaching = !msg.GetTrickPlay();

      {
        std::unique_lock<CCriticalSection> lock(m_StateSection);
        m_lastChapterSeekTarget = 0;
      }

      if (shouldFlushCaching)
      {
        m_processInfo->SeekFinished(0);
        SetCaching(CACHESTATE_FLUSH);
      }

      double start = DVD_NOPTS_VALUE;
      const int64_t currentTime = GetTime();

      double time = msg.GetTime();
      if (msg.GetRelative())
        time = (m_clock.GetClock() + m_State.time_offset) / 1000l + time;

      time = msg.GetRestore() ? m_Edl.GetTimeAfterRestoringCuts(time) : time;

      // if input stream doesn't support ISeekTime, convert back to pts
      //! @todo
      //! After demuxer we add an offset to input pts so that displayed time and clock are
      //! increasing steadily. For seeking we need to determine the boundaries and offset
      //! of the desired segment. With the current approach calculated time may point
      //! to nirvana
      if (m_pInputStream->GetIPosTime() == nullptr)
        time -= m_State.time_offset/1000l;

      time = static_cast<double>(NormalizeTarget(std::lround(time), currentTime, m_processInfo->GetMaxTime()));

      logComponentM(LOGDEBUG, LOGVIDEO, "demuxer seek to: {:f}", time);
      if (m_pDemuxer && m_pDemuxer->SeekTime(time, msg.GetBackward(), &start))
      {
        logComponentM(LOGDEBUG, LOGVIDEO, "demuxer seek to: {:f}, success", time);
        if (m_pSubtitleDemuxer)
        {
          m_subtitleDemuxerEof = false;
          if (!m_pSubtitleDemuxer->SeekTime(time, msg.GetBackward()))
          {
            logComponentM(LOGDEBUG, LOGVIDEO,
                          "subtitle demuxer backward-seek to {:f} failed; retrying forward",
                          time);
            if (!m_pSubtitleDemuxer->SeekTime(time, false))
            {
              logComponentM(LOGDEBUG, LOGVIDEO,
                            "subtitle demuxer forward-seek to {:f} also failed; falling back "
                            "to SeekTime(0)",
                            time);
              if (!m_pSubtitleDemuxer->SeekTime(0, false))
                logComponentM(LOGDEBUG, LOGVIDEO, "failed to seek subtitle demuxer: {:f}", time);
            }
          }
        }
        // dts after successful seek
        if (start == DVD_NOPTS_VALUE)
          start = DVD_MSEC_TO_TIME(time) - m_State.time_offset;

        m_State.dts = start;
        m_State.lastSeek = m_clock.GetAbsoluteClock();

        FlushBuffers(start, msg.GetAccurate(), msg.GetSync());
        logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayer::HandleMessages: flush buffers: dts:{:.3f} lastSeek:{:.3f} clock:{:.3f}", start / 1000000., m_State.lastSeek / 1000000.0, m_clock.GetClock() / 1000000.0);

        m_subtitleSeekNewRun = true;
        RecallSubtitlesAfterSeek(start, time);
      }
      else if (m_pDemuxer)
      {
        logComponentM(LOGDEBUG, LOGVIDEO, "seek [{}] ms failed; keeping playback pos", std::lround(time));
        if (shouldFlushCaching) SetCaching(cacheStateBeforeSeek);
      }

      // set flag to indicate we have finished a seeking request
      if(!msg.GetTrickPlay())
      {
        m_processInfo->SeekFinished(0);
      }

      // dvd's will issue a HOP_CHANNEL that we need to skip
      if(m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
        m_dvd.state = DVDSTATE_SEEK;

      m_processInfo->SetStateSeeking(false);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SEEK_CHAPTER) &&
             m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK) == 0 &&
             m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK_CHAPTER) == 0)
    {
      const ECacheState cacheStateBeforeSeek = m_caching;
      m_processInfo->SeekFinished(0);
      SetCaching(CACHESTATE_FLUSH);

      CDVDMsgPlayerSeekChapter& msg(*std::static_pointer_cast<CDVDMsgPlayerSeekChapter>(pMsg));
      double start = DVD_NOPTS_VALUE;
      int offset = 0;

      const int64_t chapterStartMs = GetChapterPos(msg.GetChapter()) * 1000;
      const int64_t currentTimeMs = GetTime();
      const int64_t clampedMs =
          NormalizeTarget(chapterStartMs, currentTimeMs, m_processInfo->GetMaxTime());
      if (chapterStartMs > 0 && clampedMs != chapterStartMs)
      {
        SetCaching(cacheStateBeforeSeek);
        if (currentTimeMs < clampedMs)
        {
          logComponentM(LOGDEBUG, LOGVIDEO,
                        "chapter {} starts {}ms inside the EOF guard zone, seeking to {}ms instead",
                        msg.GetChapter(), chapterStartMs, clampedMs);
          CDVDMsgPlayerSeek::CMode mode;
          mode.time = static_cast<double>(clampedMs);
          mode.backward = true;
          mode.accurate = true;
          mode.trickplay = false;
          mode.sync = true;
          m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
        }
        else
        {
          logComponentM(LOGDEBUG, LOGVIDEO,
                        "chapter {} starts inside the EOF guard zone and playback is already "
                        "past the clamp point; keeping playback pos",
                        msg.GetChapter());
        }
      }
      // This should always be the case.
      else if(m_pDemuxer && m_pDemuxer->SeekChapter(msg.GetChapter(), &start))
      {
        FlushBuffers(start, true, true);
        m_subtitleSeekNewRun = true;
        RecallSubtitlesAfterSeek(start, (start + m_State.time_offset) / 1000.0);
        int64_t beforeSeek = GetTime();
        offset = DVD_TIME_TO_MSEC(start) - static_cast<int>(beforeSeek);
        m_callback.OnPlayBackSeekChapter(msg.GetChapter());
      }
      else if (m_pInputStream)
      {
        CDVDInputStream::IChapter* pChapter = m_pInputStream->GetIChapter();
        if (pChapter && pChapter->SeekChapter(msg.GetChapter()))
        {
          FlushBuffers(start, true, true);
          m_subtitleSeekNewRun = true;
          RecallSubtitlesAfterSeek(start, (start + m_State.time_offset) / 1000.0);
          int64_t beforeSeek = GetTime();
          offset = DVD_TIME_TO_MSEC(start) - static_cast<int>(beforeSeek);
          m_callback.OnPlayBackSeekChapter(msg.GetChapter());
        }
        else
        {
          logComponentM(LOGDEBUG, LOGVIDEO, "chapter seek failed; keeping playback pos");
          SetCaching(cacheStateBeforeSeek);
        }
      }
      else
      {
        SetCaching(cacheStateBeforeSeek);
      }
      m_processInfo->SeekFinished(offset);
    }
    else if (pMsg->IsType(CDVDMsg::DEMUXER_RESET))
    {
      m_CurrentAudio.stream = nullptr;
      m_CurrentVideo.stream = nullptr;
      m_CurrentSubtitle.stream = nullptr;

      // we need to reset the demuxer, probably because the streams have changed
      if(m_pDemuxer)
        m_pDemuxer->Reset();
      if(m_pSubtitleDemuxer)
      {
        m_pSubtitleDemuxer->Reset();
        m_subtitleDemuxerEof = false;
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_AUDIOSTREAM))
    {
      auto pMsg2 = std::static_pointer_cast<CDVDMsgPlayerSetAudioStream>(pMsg);

      SelectionStream& st = m_SelectionStreams.Get(STREAM_AUDIO, pMsg2->GetStreamId());
      if(st.source != STREAM_SOURCE_NONE)
      {
        if(st.source == STREAM_SOURCE_NAV && m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
        {
          std::shared_ptr<CDVDInputStreamNavigator> pStream = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);
          if(pStream->SetActiveAudioStream(st.id))
          {
            m_dvd.iSelectedAudioStream = -1;
            CloseStream(m_CurrentAudio, false);
            CDVDMsgPlayerSeek::CMode mode;
            mode.time = (int)GetUpdatedTime();
            mode.backward = true;
            mode.accurate = true;
            mode.trickplay = true;
            mode.sync = true;
            m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
          }
        }
        else
        {
          CloseStream(m_CurrentAudio, false);
          OpenStream(m_CurrentAudio, st.demuxerId, st.id, st.source);
          AdaptForcedSubtitles();

          if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) && m_pDemuxer)
          {
            CDemuxStream* ds = m_pDemuxer->GetStream(st.demuxerId, st.id);
            if (ds)
              std::static_pointer_cast<CDVDInputStreamBluray>(m_pInputStream)
                ->EnableStream(BLURAY_AUDIO_STREAM, ds->dvdNavId, true);
          }

          CDVDMsgPlayerSeek::CMode mode;
          mode.time = (int)GetUpdatedTime();
          mode.backward = true;
          mode.accurate = true;
          mode.trickplay = true;
          mode.sync = true;
          m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
        }
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_VIDEOSTREAM))
    {
      auto pMsg2 = std::static_pointer_cast<CDVDMsgPlayerSetVideoStream>(pMsg);

      SelectionStream& st = m_SelectionStreams.Get(STREAM_VIDEO, pMsg2->GetStreamId());
      if (st.source != STREAM_SOURCE_NONE)
      {
        if (st.source == STREAM_SOURCE_NAV && m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
        {
          std::shared_ptr<CDVDInputStreamNavigator> pStream = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);
          if (pStream->SetAngle(st.id))
          {
            m_dvd.iSelectedVideoStream = st.id;

            CDVDMsgPlayerSeek::CMode mode;
            mode.time = (int)GetUpdatedTime();
            mode.backward = true;
            mode.accurate = true;
            mode.trickplay = true;
            mode.sync = true;
            m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
          }
        }
        else
        {
          CloseStream(m_CurrentVideo, false);
          OpenStream(m_CurrentVideo, st.demuxerId, st.id, st.source);
          CDVDMsgPlayerSeek::CMode mode;
          mode.time = (int)GetUpdatedTime();
          mode.backward = true;
          mode.accurate = true;
          mode.trickplay = true;
          mode.sync = true;
          m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
        }
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_SUBTITLESTREAM))
    {
      auto pMsg2 = std::static_pointer_cast<CDVDMsgPlayerSetSubtitleStream>(pMsg);

      SelectionStream& st = m_SelectionStreams.Get(STREAM_SUBTITLE, pMsg2->GetStreamId());
      if(st.source != STREAM_SOURCE_NONE)
      {
        if(st.source == STREAM_SOURCE_NAV && m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
        {
          std::shared_ptr<CDVDInputStreamNavigator> pStream = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);
          if(pStream->SetActiveSubtitleStream(st.id))
          {
            m_dvd.iSelectedSPUStream = -1;
            CloseStream(m_CurrentSubtitle, false);
          }
        }
        else
        {
          CloseStream(m_CurrentSubtitle, false);
          if (OpenStream(m_CurrentSubtitle, st.demuxerId, st.id, st.source))
          {
            QueueSubtitleSwitchSeek(st);
            if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) && m_pDemuxer)
            {
              CDemuxStream* ds = m_pDemuxer->GetStream(st.demuxerId, st.id);
              if (ds)
                std::static_pointer_cast<CDVDInputStreamBluray>(m_pInputStream)
                  ->EnableStream(BLURAY_PG_TEXTST_STREAM, ds->dvdNavId, true);
            }
          }
        }
        // aml_reset_from_subtitle_change();
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_SUBTITLESTREAM_VISIBLE))
    {
      bool isVisible = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;

      SetEnableStream(m_CurrentSubtitle, isVisible);

      if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) && m_pDemuxer && m_CurrentSubtitle.id >= 0)
      {
        CDemuxStream* ds = m_pDemuxer->GetStream(m_CurrentSubtitle.demuxerId, m_CurrentSubtitle.id);
        if (ds)
          std::static_pointer_cast<CDVDInputStreamBluray>(m_pInputStream)
            ->EnableStream(BLURAY_PG_TEXTST_STREAM, ds->dvdNavId, isVisible);
      }

      SetSubtitleVisibleInternal(isVisible);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_PROGRAM))
    {
      auto msg = std::static_pointer_cast<CDVDMsgInt>(pMsg);
      if (m_pDemuxer)
      {
        m_pDemuxer->SetProgram(msg->m_value);
        FlushBuffers(DVD_NOPTS_VALUE, false, true);
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_STATE))
    {
      SetCaching(CACHESTATE_FLUSH);

      auto pMsgPlayerSetState = std::static_pointer_cast<CDVDMsgPlayerSetState>(pMsg);

      if (std::shared_ptr<CDVDInputStream::IMenus> ptr = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream))
      {
        if(ptr->SetState(pMsgPlayerSetState->GetState()))
        {
          m_dvd.state = DVDSTATE_NORMAL;
          m_dvd.iDVDStillStartTime = {};
          m_dvd.iDVDStillTime = 0ms;
        }
      }

      m_processInfo->SeekFinished(0);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
    {
      FlushBuffers(DVD_NOPTS_VALUE, true, true);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      const auto msg = std::static_pointer_cast<CDVDMsgPlayerSetSpeed>(pMsg);
      const int speed = msg->GetSpeed();

      // correct our current clock, as it would start going wrong otherwise
      if (m_State.timestamp > 0)
      {
        double offset;
        offset = m_clock.GetAbsoluteClock() - m_State.timestamp;
        offset *= m_playSpeed / DVD_PLAYSPEED_NORMAL;
        offset = DVD_TIME_TO_MSEC(offset);
        if (offset > 1000)
          offset = 1000;
        if (offset < -1000)
          offset = -1000;
        m_State.time += offset;
        m_State.timestamp = m_clock.GetAbsoluteClock();
      }

      if (speed != DVD_PLAYSPEED_PAUSE && m_playSpeed != DVD_PLAYSPEED_PAUSE && speed != m_playSpeed)
      {
        m_callback.OnPlayBackSpeedChanged(speed / DVD_PLAYSPEED_NORMAL);
        m_processInfo->SeekFinished(0);
      }

      if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER) && speed != m_playSpeed)
      {
        std::shared_ptr<CInputStreamPVRBase> pvrinputstream = std::static_pointer_cast<CInputStreamPVRBase>(m_pInputStream);
        pvrinputstream->Pause(speed == 0);
      }

      const bool isTempoSpeed = msg->IsTempo();
      const bool wasFFRW =
          (m_playSpeed != DVD_PLAYSPEED_NORMAL && m_playSpeed != DVD_PLAYSPEED_PAUSE &&
           !m_processInfo->IsTempoAllowed(static_cast<float>(m_playSpeed) / DVD_PLAYSPEED_NORMAL));
      if ((speed == DVD_PLAYSPEED_NORMAL || isTempoSpeed) && wasFFRW)
      {
        double iTime = m_VideoPlayerVideo->GetCurrentPts();
        if (iTime == DVD_NOPTS_VALUE)
          iTime = m_Edl.GetTimeAfterRestoringCuts(static_cast<double>(GetUpdatedTime()));
        else
          iTime = (iTime + m_State.time_offset) / 1000;

        CDVDMsgPlayerSeek::CMode mode;
        mode.time = iTime;
        mode.backward = m_playSpeed < 0;
        mode.accurate = true;
        mode.trickplay = true;
        mode.sync = true;
        mode.restore = false;
        m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
      }

      if ((speed == DVD_PLAYSPEED_NORMAL) &&
          (m_playSpeed == DVD_PLAYSPEED_PAUSE) &&
          m_State.canseek && m_pInputStream && !m_pInputStream->IsRealtime() &&
          m_CurrentVideo.id >= 0 &&
          (m_CurrentVideo.hint.codec == AV_CODEC_ID_VC1 ||
           m_CurrentVideo.hint.codec == AV_CODEC_ID_WMV3))
      {
        const int audioAlgoValue = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
            CSettings::SETTING_COREELEC_AMLOGIC_DV_AUDIO_SEAMLESSBRANCH);
        if (audioAlgoValue == 0 || audioAlgoValue == 4 || audioAlgoValue == 5)
        {
          double iTime = (m_clock.GetClock() + m_State.time_offset) / 1000;
          CDVDMsgPlayerSeek::CMode mode;
          mode.time = static_cast<int>(iTime);
          mode.backward = true;
          mode.accurate = true;
          mode.trickplay = true;
          mode.sync = true;
          mode.restore = false;
          m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
        }
      }

      if (isTempoSpeed)
        m_processInfo->SetTempo(static_cast<float>(speed) / DVD_PLAYSPEED_NORMAL);
      else
        m_processInfo->SetSpeed(static_cast<float>(speed) / DVD_PLAYSPEED_NORMAL);

      m_processInfo->SetFrameAdvance(false);

      if (speed < DVD_PLAYSPEED_PAUSE)
      {
        if (m_playSpeed >= DVD_PLAYSPEED_PAUSE)
        {
          double seedPts = m_VideoPlayerVideo->GetCurrentPts();
          if (seedPts == DVD_NOPTS_VALUE)
            seedPts = m_clock.GetClock();
          m_SpeedState.rewindTargetPts = seedPts;
        }
      }
      else
      {
        m_SpeedState.rewindTargetPts = DVD_NOPTS_VALUE;
      }

      m_playSpeed = speed;

      m_caching = CACHESTATE_DONE;
      m_clock.SetSpeed(speed);
      m_VideoPlayerAudio->SetSpeed(speed);
      m_VideoPlayerVideo->SetSpeed(speed);
      m_streamPlayerSpeed = speed;
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_FRAME_ADVANCE))
    {
      if (m_playSpeed == DVD_PLAYSPEED_PAUSE)
      {
        int frames = std::static_pointer_cast<CDVDMsgInt>(pMsg)->m_value;
        double time = DVD_TIME_BASE / static_cast<double>(m_processInfo->GetVideoFps()) * frames;
        m_processInfo->SetFrameAdvance(true);
        m_clock.Advance(time);
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_GUI_ACTION))
      OnAction(std::static_pointer_cast<CDVDMsgType<CAction>>(pMsg)->m_value);
    else if (pMsg->IsType(CDVDMsg::PLAYER_STARTED))
    {
      SStartMsg& msg = std::static_pointer_cast<CDVDMsgType<SStartMsg>>(pMsg)->m_value;
      if (msg.player == VideoPlayer_AUDIO)
      {
        m_CurrentAudio.syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
        m_CurrentAudio.cachetime = msg.cachetime;
        m_CurrentAudio.cachetotal = msg.cachetotal;
        m_CurrentAudio.starttime = msg.timestamp;
      }
      if (msg.player == VideoPlayer_VIDEO)
      {
        m_CurrentVideo.syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
        m_CurrentVideo.cachetime = msg.cachetime;
        m_CurrentVideo.cachetotal = msg.cachetotal;
        m_CurrentVideo.starttime = msg.timestamp;
      }
      logComponentM(LOGDEBUG, LOGVIDEO, "player started {}", msg.player);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_REPORT_STATE))
    {
      SStateMsg& msg = std::static_pointer_cast<CDVDMsgType<SStateMsg>>(pMsg)->m_value;
      if (msg.player == VideoPlayer_AUDIO)
      {
        m_CurrentAudio.syncState = msg.syncState;
      }
      if (msg.player == VideoPlayer_VIDEO)
      {
        m_CurrentVideo.syncState = msg.syncState;
      }
      logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayer::HandleMessages - player {} reported state: {}", msg.player,
                msg.syncState);
    }
    else if (pMsg->IsType(CDVDMsg::SUBTITLE_ADDFILE))
    {
      int id = AddSubtitleFile(std::static_pointer_cast<CDVDMsgType<std::string>>(pMsg)->m_value);
      if (id >= 0)
      {
        SetSubtitle(id);
        SetSubtitleVisibleInternal(true);
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if (std::static_pointer_cast<CDVDMsgGeneralSynchronize>(pMsg)->Wait(100ms, SYNCSOURCE_PLAYER))
        logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayer - CDVDMsg::GENERAL_SYNCHRONIZE");
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_AVCHANGE))
    {
      CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
      CServiceBroker::GetDataCacheCore().SignalVideoInfoChange();
      CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();
      IPlayerCallback *cb = &m_callback;
      m_outboundEvents->Submit([=]() {
        cb->OnAVChange();
      });
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_ABORT))
    {
      logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayer - CDVDMsg::PLAYER_ABORT");
      m_bAbortRequest = true;
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_UPDATE_STREAM_DETAILS))
      m_UpdateStreamDetails = true;
  }
}

void CVideoPlayer::SetCaching(ECacheState state)
{
  if(state == CACHESTATE_FLUSH)
  {
    CacheInfo cache = GetCachingTimes();
    if (cache.valid)
      state = CACHESTATE_FULL;
    else
      state = CACHESTATE_INIT;
  }

  if(m_caching == state)
    return;

  CLog::Log(LOGDEBUG, LOGVIDEO, "CVideoPlayer::SetCaching - caching state {:d} clock:{:.3f} start pts:{:.3f}",
    state, m_clock.GetClock() / 1000000.0,
    m_CurrentVideo.starttime == DVD_NOPTS_VALUE ? -1.0 : m_CurrentVideo.starttime / 1000000.0);
  if (state == CACHESTATE_FULL ||
      state == CACHESTATE_INIT)
  {
    m_clock.SetSpeed(DVD_PLAYSPEED_PAUSE);

    m_VideoPlayerAudio->SetSpeed(DVD_PLAYSPEED_PAUSE);
    m_VideoPlayerVideo->SetSpeed(DVD_PLAYSPEED_PAUSE);
    m_streamPlayerSpeed = DVD_PLAYSPEED_PAUSE;

    m_cachingTimer.Set(5000ms);
  }

  if (state == CACHESTATE_PLAY ||
     (state == CACHESTATE_DONE && m_caching != CACHESTATE_PLAY))
  {
    m_clock.SetSpeed(m_playSpeed);
    m_VideoPlayerAudio->SetSpeed(m_playSpeed);
    m_VideoPlayerVideo->SetSpeed(m_playSpeed);
    m_streamPlayerSpeed = m_playSpeed;
  }
  m_caching = state;

  m_clock.SetSpeedAdjust(0);
}

void CVideoPlayer::SetPlaySpeed(int speed)
{
  if (IsPlaying())
  {
    UpdateAudioPassthroughForSpeed(static_cast<double>(speed) / DVD_PLAYSPEED_NORMAL);
    CDVDMsgPlayerSetSpeed::SpeedParams params = { speed, false };
    m_messenger.Put(std::make_shared<CDVDMsgPlayerSetSpeed>(params));
  }
  else
  {
    m_playSpeed = speed;
    m_streamPlayerSpeed = speed;
  }
}

bool CVideoPlayer::CanPause() const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return m_State.canpause;
}

void CVideoPlayer::Pause()
{
  // toggle between pause and normal speed
  if (m_processInfo->GetNewSpeed() == 0)
  {
    SetSpeed(1);
  }
  else
  {
    SetSpeed(0);
  }
}

bool CVideoPlayer::HasVideo() const
{
  return m_HasVideo;
}

bool CVideoPlayer::HasAudio() const
{
  return m_HasAudio;
}

bool CVideoPlayer::HasRDS() const
{
  return m_CurrentRadioRDS.id >= 0;
}

bool CVideoPlayer::HasID3() const
{
  return m_CurrentAudioID3.id >= 0;
}

bool CVideoPlayer::IsPassthrough() const
{
  return m_VideoPlayerAudio->IsPassthrough();
}

bool CVideoPlayer::CanSeek() const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return m_State.canseek;
}

void CVideoPlayer::Seek(bool bPlus, bool bLargeStep, bool bChapterOverride)
{
  if (!m_State.canseek)
    return;

  if (bLargeStep && bChapterOverride && GetChapter() > 0 && GetChapterCount() > 1)
  {
    if (!bPlus)
    {
      SeekChapter(GetPreviousChapter());
      return;
    }
    else
    {
      const int nextChapter = GetNextChapter();
      if (nextChapter <= GetChapterCount())
      {
        SeekChapter(nextChapter);
        return;
      }
    }
  }

  int64_t seekTarget;
  const std::shared_ptr<CAdvancedSettings> advancedSettings = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();
  if (advancedSettings->m_videoUseTimeSeeking && m_processInfo->GetMaxTime() > 2000 * advancedSettings->m_videoTimeSeekForwardBig)
  {
    if (bLargeStep)
      seekTarget = bPlus ? advancedSettings->m_videoTimeSeekForwardBig :
                           advancedSettings->m_videoTimeSeekBackwardBig;
    else
      seekTarget = bPlus ? advancedSettings->m_videoTimeSeekForward :
                           advancedSettings->m_videoTimeSeekBackward;
    seekTarget *= 1000;
    seekTarget += GetTime();
  }
  else
  {
    int percent;
    if (bLargeStep)
      percent = bPlus ? advancedSettings->m_videoPercentSeekForwardBig : advancedSettings->m_videoPercentSeekBackwardBig;
    else
      percent = bPlus ? advancedSettings->m_videoPercentSeekForward : advancedSettings->m_videoPercentSeekBackward;
    seekTarget = static_cast<int64_t>(m_processInfo->GetMaxTime() * (GetPercentage() + percent) / 100);
  }

  bool restore = true;

  const int64_t time = GetTime();
  if(g_application.CurrentFileItem().IsStack() &&
     (seekTarget > m_processInfo->GetMaxTime() || seekTarget < 0))
  {
    const int64_t target = NormalizeTarget(seekTarget, time, m_processInfo->GetMaxTime());
    g_application.SeekTime((target - time) * 0.001 + g_application.GetTime());
    // warning, don't access any VideoPlayer variables here as
    // the VideoPlayer object may have been destroyed
    return;
  }

  seekTarget = NormalizeTarget(seekTarget, time, m_processInfo->GetMaxTime());

  CDVDMsgPlayerSeek::CMode mode;
  mode.time = static_cast<double>(seekTarget);
  mode.backward = !bPlus;
  mode.accurate = false;
  mode.restore = restore;
  mode.trickplay = false;
  mode.sync = true;

  m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
  SynchronizeDemuxer();
  m_callback.OnPlayBackSeek(seekTarget, seekTarget - time);
}

bool CVideoPlayer::SeekScene(bool bPlus)
{
  if (!m_Edl.HasSceneMarker())
    return false;

  /*
   * There is a 5 second grace period applied when seeking for scenes backwards. If there is no
   * grace period applied it is impossible to go backwards past a scene marker.
   */
  int64_t clock = GetTime();
  if (!bPlus && clock > 5 * 1000) // 5 seconds
    clock -= 5 * 1000;

  int iScenemarker;
  if (m_Edl.GetNextSceneMarker(bPlus, clock, &iScenemarker))
  {
    const int64_t target = NormalizeTarget(iScenemarker, GetTime(), m_processInfo->GetMaxTime());

    CDVDMsgPlayerSeek::CMode mode;
    mode.time = static_cast<double>(target);
    mode.backward = !bPlus;
    mode.accurate = false;
    mode.restore = false;
    mode.trickplay = false;
    mode.sync = true;

    m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
    SynchronizeDemuxer();
    return true;
  }
  return false;
}

void CVideoPlayer::GetGeneralInfo(std::string& strGeneralInfo)
{
  if (!m_bStop)
  {
    double apts = m_VideoPlayerAudio->GetCurrentPts();
    double vpts = m_VideoPlayerVideo->GetCurrentPts();
    double dDiff = 0;

    if (apts != DVD_NOPTS_VALUE && vpts != DVD_NOPTS_VALUE)
      dDiff = (apts - vpts) / DVD_TIME_BASE;

    std::string strBuf;
    std::unique_lock<CCriticalSection> lock(m_StateSection);
    if (m_State.cache_bytes >= 0)
    {
      strBuf += StringUtils::Format("forward: {} / {:2.0f}% / {:6.3f}s / {:.3f}%",
                                    StringUtils::SizeToString(m_State.cache_bytes),
                                    m_State.cache_level * 100.0, m_State.cache_time,
                                    m_State.cache_offset * 100.0);
    }

    strGeneralInfo = StringUtils::Format("Player: a/v:{: 6.3f}, {}", dDiff, strBuf);
  }
}

void CVideoPlayer::SeekPercentage(float iPercent)
{
  int64_t iTotalTime = m_processInfo->GetMaxTime();

  if (!iTotalTime)
    return;

  SeekTime((int64_t)(iTotalTime * iPercent / 100));
}

float CVideoPlayer::GetPercentage()
{
  int64_t iTotalTime = m_processInfo->GetMaxTime();

  if (!iTotalTime)
    return 0.0f;

  return GetTime() * 100 / (float)iTotalTime;
}

float CVideoPlayer::GetCachePercentage() const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return (float) (m_State.cache_offset * 100); // NOTE: Percentage returned is relative
}

void CVideoPlayer::SetAVDelay(float fValue)
{
  m_processInfo->GetVideoSettingsLocked().SetAudioDelay(fValue);
  const int delayMs = static_cast<int>(fValue * 1000.0f);
  m_renderManager.SetDelay(delayMs);
  AUDIODELAY_LOG("VP.SetAVDelay", "source=SetAVDelay delayMs={} fValue={:.4f}", delayMs, fValue);
}

float CVideoPlayer::GetAVDelay()
{
  return static_cast<float>(m_renderManager.GetDelay()) / 1000.0f;
}

void CVideoPlayer::SetSubTitleDelay(float fValue)
{
  m_processInfo->GetVideoSettingsLocked().SetSubtitleDelay(fValue);
  m_VideoPlayerVideo->SetSubtitleDelay(static_cast<double>(-fValue) * DVD_TIME_BASE);
}

float CVideoPlayer::GetSubTitleDelay()
{
  return (float) -m_VideoPlayerVideo->GetSubtitleDelay() / DVD_TIME_BASE;
}

bool CVideoPlayer::GetSubtitleVisible() const
{
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    std::shared_ptr<CDVDInputStreamNavigator> pStream = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);
    return pStream->IsSubtitleStreamEnabled();
  }

  return m_VideoPlayerVideo->IsSubtitleEnabled();
}

void CVideoPlayer::SetSubtitleVisible(bool bVisible)
{
  m_messenger.Put(
      std::make_shared<CDVDMsgBool>(CDVDMsg::PLAYER_SET_SUBTITLESTREAM_VISIBLE, bVisible));
  m_processInfo->GetVideoSettingsLocked().SetSubtitleVisible(bVisible);
  if (!bVisible)
    aml_dv_set_subtitles(false);
}

void CVideoPlayer::SetEnableStream(CCurrentStream& current, bool isEnabled) const {
  if (m_pDemuxer && STREAM_SOURCE_MASK(current.source) == STREAM_SOURCE_DEMUX)
    m_pDemuxer->EnableStream(current.demuxerId, current.id, isEnabled);
}

void CVideoPlayer::SetSubtitleVisibleInternal(bool bVisible) const {
  m_VideoPlayerVideo->EnableSubtitle(bVisible);

  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
    std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream)->EnableSubtitleStream(bVisible);

  CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();
  if (!bVisible)
    aml_dv_set_subtitles(false);
}

void CVideoPlayer::SetSubtitleVerticalPosition(int value, bool save)
{
  m_processInfo->GetVideoSettingsLocked().SetSubtitleVerticalPosition(value, save);
  m_renderManager.SetSubtitleVerticalPosition(value, save);
}

std::shared_ptr<TextCacheStruct_t> CVideoPlayer::GetTeletextCache()
{
  if (m_CurrentTeletext.id < 0)
    return nullptr;

  return m_VideoPlayerTeletext->GetTeletextCache();
}

bool CVideoPlayer::HasTeletextCache() const
{
  return m_CurrentTeletext.id >= 0;
}

void CVideoPlayer::LoadPage(int p, int sp, unsigned char* buffer)
{
  if (m_CurrentTeletext.id < 0)
      return;

  return m_VideoPlayerTeletext->LoadPage(p, sp, buffer);
}

void CVideoPlayer::SeekTime(int64_t iTime)
{
  const int64_t currentTime = GetTime();
  const int64_t target = NormalizeTarget(iTime, currentTime, m_processInfo->GetMaxTime());

  int64_t seekOffset = target - currentTime;

  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const bool fastSeek =
      settings && settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_FAST_SEEK);

  CDVDMsgPlayerSeek::CMode mode;
  mode.time = static_cast<double>(target);
  mode.backward = true;
  mode.accurate = !fastSeek;
  mode.trickplay = false;
  mode.sync = true;

  m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
  SynchronizeDemuxer();
  m_callback.OnPlayBackSeek(target, seekOffset);
  m_processInfo->SeekFinished(seekOffset);
}

bool CVideoPlayer::SeekTimeRelative(int64_t iTime)
{
  const int64_t currentTime = GetTime();
  const int64_t unclampedTime = currentTime + iTime;
  const int64_t abstime = NormalizeTarget(unclampedTime, currentTime, m_processInfo->GetMaxTime());

  // if the file has EDL cuts we can't rely on m_clock for relative seeks
  // EDL cuts remove time from the original file, hence we might seek to
  // positions too far from the current m_clock position. Seek to absolute
  // time instead
  if (m_Edl.HasCuts() || (abstime != unclampedTime))
  {
    SeekTime(abstime);
    return true;
  }

  CDVDMsgPlayerSeek::CMode mode;
  mode.time = (int)iTime;
  mode.relative = true;
  mode.backward = (iTime < 0) ? true : false;
  mode.accurate = false;
  mode.trickplay = false;
  mode.sync = true;

  m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
  m_processInfo->SetStateSeeking(true);

  m_callback.OnPlayBackSeek(abstime, iTime);
  m_processInfo->SeekFinished(iTime);
  return true;
}

// return the time in milliseconds
int64_t CVideoPlayer::GetTime() const {
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return llrint(m_State.time);
}

void CVideoPlayer::SetSpeed(float speed)
{
  // can't rewind in menu as seeking isn't possible
  // forward is fine
  if (speed < 0 && IsInMenu())
    return;

  if (!CanSeek() && !CanPause())
    return;

  int iSpeed = static_cast<int>(speed * DVD_PLAYSPEED_NORMAL);

  if (!CanSeek())
  {
    if ((iSpeed != DVD_PLAYSPEED_NORMAL) && (iSpeed != DVD_PLAYSPEED_PAUSE))
      return;
  }

  float currentSpeed = m_processInfo->GetNewSpeed();
  m_processInfo->SetNewSpeed(speed);
  if (iSpeed != currentSpeed)
  {
    if (iSpeed == DVD_PLAYSPEED_NORMAL)
      m_callback.OnPlayBackResumed();
    else if (iSpeed == DVD_PLAYSPEED_PAUSE)
      m_callback.OnPlayBackPaused();

    if (iSpeed == DVD_PLAYSPEED_NORMAL)
    {
      float currentTempo = m_processInfo->GetNewTempo();
      if (currentTempo != 1.0f)
      {
        SetTempo(currentTempo);
        return;
      }
    }
    SetPlaySpeed(iSpeed);
  }
}

void CVideoPlayer::SetTempo(float tempo)
{
  tempo = floor(tempo * 100.0f + 0.5f) / 100.0f;
  if (m_processInfo->IsTempoAllowed(tempo))
  {
    UpdateAudioPassthroughForSpeed(tempo);
    int speed = tempo * DVD_PLAYSPEED_NORMAL;
    CDVDMsgPlayerSetSpeed::SpeedParams params = { speed, true };
    m_messenger.Put(std::make_shared<CDVDMsgPlayerSetSpeed>(params));

    m_processInfo->SetNewTempo(tempo);
  }
}

void CVideoPlayer::FrameAdvance(int frames)
{
  float currentSpeed = m_processInfo->GetNewSpeed();
  if (currentSpeed != DVD_PLAYSPEED_PAUSE)
    return;

  m_messenger.Put(std::make_shared<CDVDMsgInt>(CDVDMsg::PLAYER_FRAME_ADVANCE, frames));
}

void CVideoPlayer::WaitAsyncMainPace()
{
  m_renderManager.WaitAsyncMainPace();
}

uint64_t CVideoPlayer::GetVisibleOverlaySetSignature(bool& animated) const
{
  return m_renderManager.GetVisibleOverlaySetSignature(animated);
}

bool CVideoPlayer::SupportsTempo() const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return m_State.cantempo;
}

bool CVideoPlayer::CanTempo()
{
  auto comp = CServiceBroker::GetSettingsComponent();
  if (!comp)
    return false;
  auto settings = comp->GetSettings();
  if (!settings)
    return false;

  return settings->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK) ||
         settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_USE_DISPLAY_AS_CLOCK);
}

void CVideoPlayer::UpdateAudioPassthroughForSpeed(double speed)
{
  if (!m_VideoPlayerAudio)
    return;

  const bool isNonNormalSpeed = (speed != 1.0 && speed != 0.0);

  if (isNonNormalSpeed && m_VideoPlayerAudio->IsPassthrough() && !m_bPassthroughTempFallback)
  {
    CLog::Log(LOGINFO, "CVideoPlayer::UpdateAudioPassthroughForSpeed: non-normal speed ({:.2f}x), temporarily disabling passthrough for audio tempo", speed);
    m_bPassthroughTempFallback = true;
    m_VideoPlayerAudio->SetAllowPassthrough(false);
  }
  else if (!isNonNormalSpeed && m_bPassthroughTempFallback)
  {
    CLog::Log(LOGINFO, "CVideoPlayer::UpdateAudioPassthroughForSpeed: returning to normal speed, restoring passthrough");
    m_bPassthroughTempFallback = false;
    m_VideoPlayerAudio->SetAllowPassthrough(true);
  }
}


bool CVideoPlayer::OpenStream(CCurrentStream& current, int64_t demuxerId, int iStream, int source, bool reset /*= true*/)
{
  CDemuxStream* stream = nullptr;
  CDVDStreamInfo hint;

  CLog::Log(LOGDEBUG, "Opening stream: {} source: {}", iStream, source);

  if(STREAM_SOURCE_MASK(source) == STREAM_SOURCE_DEMUX_SUB)
  {
    int index = m_SelectionStreams.TypeIndexOf(current.type, source, demuxerId, iStream);
    if (index < 0)
      return false;
    const SelectionStream& st = m_SelectionStreams.Get(current.type, index);

    CLog::Log(LOGDEBUG, "Opening Subtitle file: {}", CURL::GetRedacted(st.filename));
    m_pSubtitleDemuxer.reset();
    const auto demux = m_subtitleDemuxerMap.find(demuxerId);
    if (demux == m_subtitleDemuxerMap.end())
    {
      CLog::Log(LOGDEBUG, "No demuxer found for file {}", CURL::GetRedacted(st.filename));
      return false;
    }

    m_pSubtitleDemuxer = demux->second;
    m_subtitleDemuxerEof = false;

    double pts = m_VideoPlayerVideo->GetCurrentPts();
    if(pts == DVD_NOPTS_VALUE)
      pts = m_CurrentVideo.dts;
    if(pts == DVD_NOPTS_VALUE)
      pts = 0;
    pts += m_offset_pts;
    const double backfillPts = std::max(0.0, pts - DVD_SEC_TO_TIME(SUBTITLE_DEMUX_BACKFILL_SECONDS));
    const int seek_ms = static_cast<int>(1000.0 * backfillPts / static_cast<double>(DVD_TIME_BASE));
    logComponentM(LOGDEBUG, LOGVIDEO,
                  "subtitle demuxer open external: file={} demuxerId={} streamId={} "
                  "backfillPts={:.6f} seek_ms={}",
                  CURL::GetRedacted(st.filename), demuxerId, iStream,
                  backfillPts / DVD_TIME_BASE, seek_ms);
    if (!m_pSubtitleDemuxer->SeekTime(seek_ms))
      logM(LOGDEBUG, "failed to start subtitle demuxing from: {:f}", backfillPts);
    stream = m_pSubtitleDemuxer->GetStream(demuxerId, iStream);
    if(!stream || stream->disabled)
      return false;

    m_pSubtitleDemuxer->EnableStream(demuxerId, iStream, true);

    hint.Assign(*stream, true);
  }
  else if(STREAM_SOURCE_MASK(source) == STREAM_SOURCE_TEXT)
  {
    int index = m_SelectionStreams.TypeIndexOf(current.type, source, demuxerId, iStream);
    if(index < 0)
      return false;

    hint.Clear();
    hint.filename = m_SelectionStreams.Get(current.type, index).filename;
    hint.fpsscale = m_CurrentVideo.hint.fpsscale;
    hint.fpsrate  = m_CurrentVideo.hint.fpsrate;
  }
  else if(STREAM_SOURCE_MASK(source) == STREAM_SOURCE_DEMUX)
  {
    if(!m_pDemuxer)
      return false;

    m_pDemuxer->OpenStream(demuxerId, iStream);

    stream = m_pDemuxer->GetStream(demuxerId, iStream);
    if (!stream || stream->disabled)
      return false;

    hint.Assign(*stream, true);

    if(m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
      hint.filename = "dvd";
    if (hint.fpsrate / hint.fpsscale > 200) {
      hint.fpsrate = 60000;
      hint.fpsscale = 1001;
    }
  }
  else if(STREAM_SOURCE_MASK(source) == STREAM_SOURCE_VIDEOMUX)
  {
    if(!m_pCCDemuxer)
      return false;

    stream = m_pCCDemuxer->GetStream(iStream);
    if(!stream || stream->disabled)
      return false;

    hint.Assign(*stream, false);
  }

  bool res;
  switch(current.type)
  {
    case STREAM_AUDIO:
      res = OpenAudioStream(hint, reset);
      break;
    case STREAM_VIDEO:
      res = OpenVideoStream(hint, reset);
      if (res)
        UpdateMenuDomainQueueDepth(true);
      break;
    case STREAM_SUBTITLE:
      hint.hdrType = m_CurrentVideo.hint.hdrType; // Set by Video Stream which is opened first
      res = OpenSubtitleStream(hint);
      break;
    case STREAM_TELETEXT:
      res = OpenTeletextStream(hint);
      break;
    case STREAM_RADIO_RDS:
      res = OpenRadioRDSStream(hint);
      break;
    case STREAM_AUDIO_ID3:
      res = OpenAudioID3Stream(hint);
      break;
    default:
      res = false;
      break;
  }

  if (res)
  {
    int oldId = current.id;
    current.id = iStream;
    current.demuxerId = demuxerId;
    current.source = source;
    current.hint = hint;
    current.stream = (void*)stream;
    current.lastdts = DVD_NOPTS_VALUE;
    if (oldId >= 0 && current.avsync != CCurrentStream::AV_SYNC_FORCE)
      current.avsync = CCurrentStream::AV_SYNC_CHECK;
    if(stream)
      current.changes = stream->changes;

    if (current.type == STREAM_VIDEO && m_CurrentSubtitle.id >= 0 &&
        m_CurrentSubtitle.hint.hdrType != hint.hdrType)
    {
      logM(LOGDEBUG,
           "CVideoPlayer::OpenStream - video hdr type changed, reopening subtitle stream for "
           "overlay reclassification");
      const int64_t subDemuxerId = m_CurrentSubtitle.demuxerId;
      const int subId = m_CurrentSubtitle.id;
      const int subSource = m_CurrentSubtitle.source;
      CloseStream(m_CurrentSubtitle, false);
      OpenStream(m_CurrentSubtitle, subDemuxerId, subId, subSource);
    }
  }
  else
  {
    if(stream)
    {
      /* mark stream as disabled, to disallow further attempts*/
      CLog::Log(LOGWARNING, "{} - Unsupported stream {}. Stream disabled.", __FUNCTION__,
                stream->uniqueId);
      stream->disabled = true;

      CCurrentStream failedStream = current;
      failedStream.id = iStream;
      failedStream.demuxerId = demuxerId;
      failedStream.source = source;
      SetEnableStream(failedStream, false);
    }
  }

  UpdateContentState();
  CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
  CServiceBroker::GetDataCacheCore().SignalVideoInfoChange();
  CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();

  return res;
}

bool CVideoPlayer::OpenAudioStream(CDVDStreamInfo& hint, bool reset)
{
  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentAudio.player);
  if(player == nullptr)
    return false;

  const bool reuse =
      m_bdStreamReuse && m_CurrentAudio.id >= 0 &&
      m_CurrentAudio.hint.Equal(hint, CDVDStreamInfo::COMPARE_ALL & ~CDVDStreamInfo::COMPARE_ID);
  if (reuse)
    logComponentM(LOGDEBUG, LOGVIDEO,
                  "CVideoPlayer::OpenAudioStream - BD stream reuse MATCH, keeping running decoder");

  if(!reuse && (m_CurrentAudio.id < 0 ||
     m_CurrentAudio.hint != hint))
  {
    if (!player->OpenStream(hint))
      return false;

    player->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, m_displayLost), 1);

    static_cast<IDVDStreamPlayerAudio*>(player)->SetSpeed(m_streamPlayerSpeed);
    m_CurrentAudio.syncState = IDVDStreamPlayer::SYNC_STARTING;
    m_CurrentAudio.packets = 0;
  }
  else if (reset)
    player->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_RESET), 0);

  m_HasAudio = true;

  static_cast<IDVDStreamPlayerAudio*>(player)->SendMessage(
      std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_REQUEST_STATE), 1);

  SetAVChange("OpenAudioStream");

  return true;
}

bool CVideoPlayer::OpenVideoStream(CDVDStreamInfo& hint, bool reset)
{
  m_eofRenderWaitStart = {};

  m_processInfo->SetVideoInterlaced((hint.codecOptions & CODEC_INTERLACED) == CODEC_INTERLACED);
  hint.bluray = m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY);
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    /* set aspect ratio as requested by navigator for dvd's */
    float aspect = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream)->GetVideoAspectRatio();
    if (aspect != 0.0f)
    {
      hint.aspect = static_cast<double>(aspect);
      hint.forced_aspect = true;
    }
    hint.dvd = true;
  }
  else if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER))
  {
    // set framerate if not set by demuxer
    if (hint.fpsrate == 0 || hint.fpsscale == 0)
    {
      int fpsidx = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_PVRPLAYBACK_FPS);
      if (fpsidx == 1)
      {
        hint.fpsscale = 1000;
        hint.fpsrate = 50000;
      }
      else if (fpsidx == 2)
      {
        hint.fpsscale = 1001;
        hint.fpsrate = 60000;
      }
    }
  }

  std::shared_ptr<CDVDInputStream::IMenus> pMenus = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream);
  if(pMenus && pMenus->IsInMenu())
    hint.stills = true;

  if (hint.stereo_mode.empty())
  {
    CGUIComponent *gui = CServiceBroker::GetGUI();
    if (gui != nullptr)
    {
      const CStereoscopicsManager &stereoscopicsManager = gui->GetStereoscopicsManager();
      hint.stereo_mode = stereoscopicsManager.DetectStereoModeByString(m_item.GetPath());

      if (!hint.stereo_mode.empty() && hint.width > 0 && hint.height > 0 && hint.aspect > 0.0)
        hint.aspect =
            StereoAspect::PerEyeAspect(hint.stereo_mode, hint.width, hint.height, hint.aspect);
    }
  }

  if (hint.flags & AV_DISPOSITION_ATTACHED_PIC)
    return false;

  // set desired refresh rate
  double openFramerate = 0.0;
  bool deferredRefresh = false;
  if (m_CurrentVideo.id < 0 && m_playerOptions.fullscreen &&
      CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenRoot() && hint.fpsrate != 0 &&
      hint.fpsscale != 0)
  {
    if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) != ADJUST_REFRESHRATE_OFF)
    {
      double framerate = DVD_TIME_BASE / CDVDCodecUtils::NormalizeFrameduration(
                                                   (double)DVD_TIME_BASE * hint.fpsscale /
                                                   (hint.fpsrate * (hint.interlaced ? 2 : 1)));

      const bool isVC1 = (hint.codec == AV_CODEC_ID_VC1 || hint.codec == AV_CODEC_ID_WMV3);
      if (hint.interlaced && framerate > 61.0)
      {
        framerate = DVD_TIME_BASE / CDVDCodecUtils::NormalizeFrameduration(
                                                     (double)DVD_TIME_BASE * hint.fpsscale /
                                                     hint.fpsrate);
        if (framerate > 61.0)
          framerate /= 2.0;
        m_processInfo->SetVideoInterlaced(true);
      }
      if (!isVC1 && hint.interlaced)
      {
        if (MathUtils::FloatEquals(25.0f, static_cast<float>(framerate), 0.01f))
        {
          framerate = 50.0;
          m_processInfo->SetVideoInterlaced(true);
        }
        if (MathUtils::FloatEquals(29.97f, static_cast<float>(framerate), 0.01f))
        {
          framerate = 60000.0 / 1001.0;
          m_processInfo->SetVideoInterlaced(true);
        }
      }
      if (isVC1)
        logComponentM(LOGDEBUG, LOGVIDEO,
          "CVideoPlayer::OpenVideoStream VC1 skip initial framerate doubling "
          "(hint.interlaced={} codecOptions=0x{:02x} framerate={:.3f}); "
          "scan type will be confirmed in FrameRateTracking",
          hint.interlaced, hint.codecOptions, framerate);
      m_processInfo->SetVideoFps(static_cast<float>(framerate));
      openFramerate = framerate;
      const double hintFramerate = DVD_TIME_BASE / CDVDCodecUtils::NormalizeFrameduration(
                                       (double)DVD_TIME_BASE * hint.fpsscale / hint.fpsrate);
      deferredRefresh =
          (hint.codec == AV_CODEC_ID_MPEG1VIDEO || hint.codec == AV_CODEC_ID_MPEG2VIDEO) &&
          (hint.codecOptions & CODEC_INTERLACED) && hintFramerate > 55.0 && hintFramerate < 61.0;
      if (!deferredRefresh)
        m_renderManager.TriggerUpdateResolution(framerate, hint.width, hint.height, hint.stereo_mode);
    }
  }

  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentVideo.player);
  if(player == nullptr)
    return false;

  bool reuse = false;
  if (m_bdStreamReuse && m_CurrentVideo.id >= 0)
  {
    CDVDStreamInfo cmp(hint);
    cmp.stills = m_CurrentVideo.hint.stills;
    reuse = m_CurrentVideo.hint.Equal(cmp, CDVDStreamInfo::COMPARE_ALL & ~CDVDStreamInfo::COMPARE_ID);
    logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayer::OpenVideoStream - BD stream reuse {}",
                  reuse ? "MATCH, reattaching running decoder" : "no format match, normal reopen");
  }
  m_bdStreamReuse = false;

  if(!reuse && (m_CurrentVideo.id < 0 ||
     m_CurrentVideo.hint != hint))
  {
    if (hint.codec == AV_CODEC_ID_MPEG2VIDEO || hint.codec == AV_CODEC_ID_H264)
      m_pCCDemuxer.reset();

    auto& dataCacheCore = CServiceBroker::GetDataCacheCore();
    dataCacheCore.SetVideoSourceHdrType(hint.hdrType);
    dataCacheCore.SetVideoSourceAdditionalHdrType(StreamHdrType::HDR_TYPE_NONE);
    dataCacheCore.SetVideoSourceDoViStreamInfo({});

    if (!player->OpenStream(hint))
      return false;

    if (deferredRefresh && m_processInfo->IsVideoHwDecoder())
      m_renderManager.TriggerUpdateResolution(openFramerate, hint.width, hint.height,
                                              hint.stereo_mode);

    player->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, m_displayLost), 1);

    const std::shared_ptr<CDVDInputStream::IExtentionStream>  pExt = std::dynamic_pointer_cast<CDVDInputStream::IExtentionStream>(m_pInputStream);
    if (pExt && !static_cast<IDVDStreamPlayerVideo*>(player)->SupportsExtention())
      pExt->DisableExtention();

    // look for any EDL files
    m_Edl.Clear();
    float fFramesPerSecond = 0.0f;
    if (m_CurrentVideo.hint.fpsscale > 0.0f)
      fFramesPerSecond = static_cast<float>(m_CurrentVideo.hint.fpsrate) / static_cast<float>(m_CurrentVideo.hint.fpsscale);
    m_Edl.ReadEditDecisionLists(m_item, fFramesPerSecond);
    CServiceBroker::GetDataCacheCore().SetEditList(m_Edl.GetEditList());
    CServiceBroker::GetDataCacheCore().SetCuts(m_Edl.GetCutMarkers());
    CServiceBroker::GetDataCacheCore().SetSceneMarkers(m_Edl.GetSceneMarkers());

    static_cast<IDVDStreamPlayerVideo*>(player)->SetSpeed(m_streamPlayerSpeed);
    m_CurrentVideo.syncState = IDVDStreamPlayer::SYNC_STARTING;
    m_CurrentVideo.packets = 0;
  }
  else if (reset)
    player->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_RESET), 0);

  m_HasVideo = true;

  static_cast<IDVDStreamPlayerVideo*>(player)->SendMessage(
      std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_REQUEST_STATE), 1);

  // open CC demuxer if video is mpeg2
  if ((hint.codec == AV_CODEC_ID_MPEG2VIDEO || hint.codec == AV_CODEC_ID_H264) && !m_pCCDemuxer)
  {
    m_pCCDemuxer = std::make_unique<CDVDDemuxCC>(hint.codec);
    m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_VIDEOMUX);
  }

  SetAVChange("OpenVideoStream");

  return true;
}

bool CVideoPlayer::OpenSubtitleStream(const CDVDStreamInfo& hint)
{
  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentSubtitle.player);
  if(player == nullptr)
    return false;

  if(m_CurrentSubtitle.id < 0 ||
     m_CurrentSubtitle.hint != hint)
  {
    if (!player->OpenStream(hint))
      return false;
  }

  return true;
}

void CVideoPlayer::AdaptForcedSubtitles()
{
  SelectionStream ss = m_SelectionStreams.Get(STREAM_SUBTITLE, GetSubtitle());
  if (ss.flags & StreamFlags::FLAG_FORCED)
  {
    SelectionStream as = m_SelectionStreams.Get(STREAM_AUDIO, GetAudioStream());
    bool isVisible = false;
    for (const auto &stream : m_SelectionStreams.Get(STREAM_SUBTITLE))
    {
      if (stream.flags & StreamFlags::FLAG_FORCED && g_LangCodeExpander.CompareISO639Codes(stream.language, as.language))
      {
        if (OpenStream(m_CurrentSubtitle, stream.demuxerId, stream.id, stream.source))
        {
          isVisible = true;
          break;
        }
      }
    }
    // SetEnableStream only if not visible, when visible OpenStream already implied that stream is enabled
    if (!isVisible)
      SetEnableStream(m_CurrentSubtitle, false);

    SetSubtitleVisibleInternal(isVisible);
  }
}

bool CVideoPlayer::OpenTeletextStream(CDVDStreamInfo& hint)
{
  if (!m_VideoPlayerTeletext->CheckStream(hint))
    return false;

  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentTeletext.player);
  if(player == nullptr)
    return false;

  if(m_CurrentTeletext.id < 0 ||
     m_CurrentTeletext.hint != hint)
  {
    if (!player->OpenStream(hint))
      return false;
  }

  return true;
}

bool CVideoPlayer::OpenRadioRDSStream(CDVDStreamInfo& hint)
{
  if (!m_VideoPlayerRadioRDS->CheckStream(hint))
    return false;

  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentRadioRDS.player);
  if(player == nullptr)
    return false;

  if(m_CurrentRadioRDS.id < 0 ||
     m_CurrentRadioRDS.hint != hint)
  {
    if (!player->OpenStream(hint))
      return false;
  }

  return true;
}

bool CVideoPlayer::OpenAudioID3Stream(CDVDStreamInfo& hint)
{
  if (!m_VideoPlayerAudioID3->CheckStream(hint))
    return false;

  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentAudioID3.player);
  if (player == nullptr)
    return false;

  if (m_CurrentAudioID3.id < 0 || m_CurrentAudioID3.hint != hint)
  {
    if (!player->OpenStream(hint))
      return false;
  }

  return true;
}

bool CVideoPlayer::CloseStream(CCurrentStream& current, bool bWaitForBuffers)
{
  if (current.id < 0)
    return false;

  CLog::Log(LOGDEBUG, "Closing stream player {}", current.player);

  if(bWaitForBuffers)
    SetCaching(CACHESTATE_DONE);

  SetEnableStream(current, false);

  IDVDStreamPlayer* player = GetStreamPlayer(current.player);
  if (player)
  {
    if ((current.type == STREAM_AUDIO && current.syncState != IDVDStreamPlayer::SYNC_INSYNC) ||
        (current.type == STREAM_VIDEO && current.syncState != IDVDStreamPlayer::SYNC_INSYNC) ||
        m_bAbortRequest)
      bWaitForBuffers = false;
    player->CloseStream(bWaitForBuffers);
  }

  current.Clear();
  return true;
}

void CVideoPlayer::FlushBuffers(double pts, bool accurate, bool sync)
{
  logComponentM(LOGDEBUG, LOGVIDEO, "CVideoPlayer::FlushBuffers - flushing buffers");

  double startpts;
  if (accurate)
    startpts = pts;
  else
    startpts = DVD_NOPTS_VALUE;

  double audioStartpts = startpts;
  if (accurate && m_CurrentVideo.id >= 0 && m_VideoPlayerVideo &&
      !m_VideoPlayerVideo->HonorsAccurateSeek())
  {
    audioStartpts = DVD_NOPTS_VALUE;
  }

  m_SpeedState.Reset(pts);

  if (sync)
  {
    m_CurrentAudio.inited = false;
    m_CurrentAudio.avsync = CCurrentStream::AV_SYNC_FORCE;
    m_CurrentAudio.starttime = DVD_NOPTS_VALUE;
    m_CurrentVideo.inited = false;
    m_CurrentVideo.avsync = CCurrentStream::AV_SYNC_FORCE;
    m_CurrentVideo.starttime = DVD_NOPTS_VALUE;
    m_CurrentSubtitle.inited = false;
    m_CurrentTeletext.inited = false;
    m_CurrentRadioRDS.inited  = false;

    if (m_pSubtitleDemuxer &&
        CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
            CSettings::SETTING_COREELEC_RESET_PTS_ON_SEEK))
      m_offset_pts = 0.0;
  }

  m_CurrentAudio.dts         = DVD_NOPTS_VALUE;
  m_CurrentAudio.startpts    = audioStartpts;
  m_CurrentAudio.packets = 0;

  m_CurrentVideo.dts         = DVD_NOPTS_VALUE;
  m_CurrentVideo.startpts    = startpts;
  m_CurrentVideo.packets = 0;

  m_CurrentSubtitle.dts      = DVD_NOPTS_VALUE;
  m_CurrentSubtitle.startpts = startpts;
  m_CurrentSubtitle.packets = 0;

  m_CurrentTeletext.dts      = DVD_NOPTS_VALUE;
  m_CurrentTeletext.startpts = startpts;
  m_CurrentTeletext.packets = 0;

  m_CurrentRadioRDS.dts      = DVD_NOPTS_VALUE;
  m_CurrentRadioRDS.startpts = startpts;
  m_CurrentRadioRDS.packets = 0;

  m_CurrentAudioID3.dts = DVD_NOPTS_VALUE;
  m_CurrentAudioID3.startpts = startpts;
  m_CurrentAudioID3.packets = 0;

  m_VideoPlayerAudio->Flush(sync);
  m_VideoPlayerVideo->Flush(sync);
  m_VideoPlayerSubtitle->Flush();
  m_VideoPlayerTeletext->Flush();
  m_VideoPlayerRadioRDS->Flush();
  m_VideoPlayerAudioID3->Flush();

  if (m_playSpeed == DVD_PLAYSPEED_NORMAL || m_playSpeed == DVD_PLAYSPEED_PAUSE ||
      m_processInfo->IsTempoAllowed(static_cast<float>(m_playSpeed) / DVD_PLAYSPEED_NORMAL))
  {
    // make sure players are properly flushed, should put them in stalled state
    unsigned int syncSources = 0;
    if (m_CurrentAudio.id >= 0)
      syncSources |= SYNCSOURCE_AUDIO;
    if (m_CurrentVideo.id >= 0)
      syncSources |= SYNCSOURCE_VIDEO;

    auto msg = std::make_shared<CDVDMsgGeneralSynchronize>(1s, syncSources);
    m_VideoPlayerAudio->SendMessage(msg, 1);
    m_VideoPlayerVideo->SendMessage(msg, 1);
    msg->Wait(m_bStop, 0);

    // purge any pending PLAYER_STARTED messages
    m_messenger.Flush(CDVDMsg::PLAYER_STARTED);

    // we should now wait for init cache
    SetCaching(CACHESTATE_FLUSH);
    if (sync)
    {
      m_CurrentAudio.syncState = IDVDStreamPlayer::SYNC_STARTING;
      m_CurrentVideo.syncState = IDVDStreamPlayer::SYNC_STARTING;
    }
  }

  if (pts != DVD_NOPTS_VALUE && sync && m_CurrentAudio.id < 0)
    m_clock.Discontinuity(pts);

  m_CurrentVideo.lastdts = DVD_NOPTS_VALUE;
  UpdatePlayState(0);

  m_demuxerSpeed = DVD_PLAYSPEED_NORMAL;
  if (m_pDemuxer)
    m_pDemuxer->SetSpeed(DVD_PLAYSPEED_NORMAL);
}

void CVideoPlayer::DrainStreamsAtBoundary()
{
  if (m_bAbortRequest || m_playSpeed != DVD_PLAYSPEED_NORMAL)
  {
    logComponentM(LOGDEBUG, LOGAVTIMING, "bddrain: skip reason={}",
                  m_bAbortRequest ? "abort" : "speed");
    return;
  }

  const bool videoActive =
      m_CurrentVideo.id >= 0 && m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_INSYNC;
  const bool audioActive =
      m_CurrentAudio.id >= 0 && m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_INSYNC;
  if (!videoActive && !audioActive)
  {
    logComponentM(LOGDEBUG, LOGAVTIMING, "bddrain: skip reason=nostream videoId={} audioId={}",
                  m_CurrentVideo.id, m_CurrentAudio.id);
    return;
  }

  const double videoSecs = videoActive ? m_VideoPlayerVideo->GetQueueTimeSize() : 0.0;
  const double audioSecs = audioActive ? m_VideoPlayerAudio->GetQueueTimeSize() : 0.0;
  if ((!videoActive || !m_VideoPlayerVideo->HasData()) &&
      (!audioActive || !m_VideoPlayerAudio->HasData()))
  {
    logComponentM(LOGDEBUG, LOGAVTIMING, "bddrain: skip reason=empty videoSec={:.2f} audioSec={:.2f}",
                  videoSecs, audioSecs);
    return;
  }

  if (videoActive)
    m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::VIDEO_DRAIN), 0);

  const auto ceiling = std::chrono::milliseconds(
      std::clamp(static_cast<int>(std::max(videoSecs, audioSecs) * 1000.0) + 3000, 8000, 30000));
  logComponentM(LOGDEBUG, LOGAVTIMING,
                "bddrain: enter videoSec={:.2f} audioSec={:.2f} sinkDelayMs={:.0f} ceilingMs={}",
                videoSecs, audioSecs,
                audioActive ? m_VideoPlayerAudio->GetCurrentSinkDelay() / 1000.0 : 0.0,
                ceiling.count());

  XbmcThreads::EndTime<> totalTimer(ceiling);
  XbmcThreads::EndTime<> stallTimer(1500ms);
  double lastVideoPts = videoActive ? m_VideoPlayerVideo->GetCurrentPts() : DVD_NOPTS_VALUE;
  double lastAudioPts = audioActive ? m_VideoPlayerAudio->GetCurrentPts() : DVD_NOPTS_VALUE;
  const char* reason = "drained";
  while (true)
  {
    if (m_bAbortRequest)
    {
      reason = "abort";
      break;
    }
    if (m_messenger.HasMessages())
    {
      reason = "message";
      break;
    }
    if (totalTimer.IsTimePast())
    {
      reason = "ceiling";
      break;
    }

    const bool videoBusy =
        videoActive && (m_VideoPlayerVideo->HasData() || !m_VideoPlayerVideo->IsEOS());
    const bool audioBusy = audioActive && m_VideoPlayerAudio->HasData();
    if (!videoBusy && !audioBusy)
      break;

    bool progressed = false;
    if (videoActive)
    {
      const double pts = m_VideoPlayerVideo->GetCurrentPts();
      if (pts != DVD_NOPTS_VALUE && pts != lastVideoPts)
      {
        lastVideoPts = pts;
        progressed = true;
      }
    }
    if (audioActive)
    {
      const double pts = m_VideoPlayerAudio->GetCurrentPts();
      if (pts != DVD_NOPTS_VALUE && pts != lastAudioPts)
      {
        lastAudioPts = pts;
        progressed = true;
      }
    }
    if (progressed)
      stallTimer.Set(1500ms);
    else if (stallTimer.IsTimePast())
    {
      reason = "stalled";
      break;
    }

    CThread::Sleep(25ms);
  }

  logComponentM(LOGDEBUG, LOGAVTIMING,
                "bddrain: exit reason={} videoSec={:.2f} audioSec={:.2f} sinkDelayMs={:.0f} "
                "videoEos={} elapsedMs={}",
                reason, videoActive ? m_VideoPlayerVideo->GetQueueTimeSize() : 0.0,
                audioActive ? m_VideoPlayerAudio->GetQueueTimeSize() : 0.0,
                audioActive ? m_VideoPlayerAudio->GetCurrentSinkDelay() / 1000.0 : 0.0,
                videoActive ? m_VideoPlayerVideo->IsEOS() : false,
                (totalTimer.GetInitialTimeoutValue() - totalTimer.GetTimeLeft()).count());
}

// since we call ffmpeg functions to decode, this is being called in the same thread as ::Process() is
int CVideoPlayer::OnDiscNavResult(void* pData, int iMessage)
{
  if (!m_pInputStream)
    return 0;

#if defined(HAVE_LIBBLURAY)
  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY))
  {
    switch (iMessage)
    {
    case BD_EVENT_MENU_OVERLAY:
      m_overlayContainer.ProcessAndAddOverlayIfValid(
          *static_cast<std::shared_ptr<CDVDOverlay>*>(pData));
      break;
    case BD_EVENT_MENU:
      // Interactive menu visible?
      if (*static_cast<uint32_t*>(pData) == false)
      {
        m_dvd.state = DVDSTATE_NORMAL;
        m_dvd.iDVDStillTime = 0ms;
        logComponentM(LOGDEBUG, LOGBLURAY, "BD_EVENT_MENU - libbluray leave menu (DVDSTATE_NORMAL)");
      }
      break;
    case BD_EVENT_PLAYLIST_STOP:
    {
      m_dvd.state = DVDSTATE_NORMAL;
      m_dvd.iDVDStillTime = 0ms;
      bool naturalChain = false;
      if (CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoBdBoundaryDrain)
      {
        if (std::shared_ptr<CDVDInputStreamBluray> bluray =
                std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
          naturalChain = bluray->IsNaturalChainBoundaryInFlight();
      }
      if (naturalChain)
        logM(LOGDEBUG,
             "BD_EVENT_PLAYLIST_STOP flush suppressed - natural chain boundary in flight");
      else
        m_messenger.Put(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_FLUSH));
      break;
    }
    case BD_EVENT_AUDIO_STREAM:
      m_dvd.iSelectedAudioStream = *static_cast<int*>(pData);
      break;

    case BD_EVENT_PG_TEXTST_STREAM:
      m_dvd.iSelectedSPUStream = *static_cast<int*>(pData);
      break;
    case BD_EVENT_PG_TEXTST:
    {
      bool enable = (*static_cast<int*>(pData) != 0);
      if (enable || !m_processInfo->GetVideoSettings().m_SubtitleOn)
        m_VideoPlayerVideo->EnableSubtitle(enable);
    }
    break;
    case BD_EVENT_STILL_TIME:
    {
      if (m_dvd.state != DVDSTATE_STILL)
      {
        // else notify the player we have received a still frame

        m_dvd.iDVDStillTime = std::chrono::milliseconds(*static_cast<int*>(pData));
        m_dvd.iDVDStillStartTime = std::chrono::steady_clock::now();

        if (m_dvd.iDVDStillTime > 0ms)
          m_dvd.iDVDStillTime *= 1000;

        /* adjust for the output delay in the video queue */
        std::chrono::milliseconds time = 0ms;
        if (m_CurrentVideo.stream && m_dvd.iDVDStillTime > 0ms)
        {
          time = std::chrono::milliseconds(
              static_cast<int>(m_VideoPlayerVideo->GetOutputDelay() / (DVD_TIME_BASE / 1000)));
          if (time < 10000ms && time > 0ms)
            m_dvd.iDVDStillTime += time;
        }
        m_dvd.state = DVDSTATE_STILL;
        logComponentM(LOGDEBUG, LOGBLURAY, "BD_EVENT_STILL_TIME - waiting {} msec, with delay of {} msec",
                  m_dvd.iDVDStillTime.count(), time.count());
      }
    }
    break;
    case BD_EVENT_STILL:
    {
      bool on = static_cast<bool>(*static_cast<int*>(pData));
      if (on && m_dvd.state != DVDSTATE_STILL)
      {
        m_dvd.state = DVDSTATE_STILL;
        m_dvd.iDVDStillStartTime = std::chrono::steady_clock::now();
        m_dvd.iDVDStillTime = 0ms;
        logComponentM(LOGDEBUG, LOGBLURAY, "CDVDPlayer::OnDVDNavResult - libbluray DVDSTATE_STILL start");
      }
      else if (!on && m_dvd.state == DVDSTATE_STILL)
      {
        m_dvd.state = DVDSTATE_NORMAL;
        m_dvd.iDVDStillStartTime = {};
        m_dvd.iDVDStillTime = 0ms;
        logComponentM(LOGDEBUG, LOGBLURAY, "CDVDPlayer::OnDVDNavResult - libbluray DVDSTATE_STILL end");
      }
    }
    break;
    case BD_EVENT_MENU_ERROR:
    {
      m_dvd.state = DVDSTATE_NORMAL;
      logComponentM(LOGDEBUG, LOGBLURAY, "CVideoPlayer::OnDiscNavResult - libbluray menu not supported (DVDSTATE_NORMAL)");
      CGUIDialogKaiToast::QueueNotification(g_localizeStrings.Get(25008), g_localizeStrings.Get(25009));
    }
    break;
    case BD_EVENT_ENC_ERROR:
    {
      m_dvd.state = DVDSTATE_NORMAL;
      logComponentM(LOGDEBUG, LOGBLURAY, "CVideoPlayer::OnDiscNavResult - libbluray the disc/file is encrypted and can't be played (DVDSTATE_NORMAL)");
      CGUIDialogKaiToast::QueueNotification(g_localizeStrings.Get(16026), g_localizeStrings.Get(29805));
    }
    break;
    case BD_EVENT_DISCONTINUITY:
      logComponentM(LOGDEBUG, LOGBLURAY,
                "CVideoPlayer::OnDiscNavResult - libbluray discontinuity detected (DEMUXER_RESET)");
      m_messenger.Put(std::make_shared<CDVDMsg>(CDVDMsg::DEMUXER_RESET));
      break;
    default:
      break;
    }

    return 0;
  }
#endif

  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    std::shared_ptr<CDVDInputStreamNavigator> pStream = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);

    switch (iMessage)
    {
    case DVDNAV_STILL_FRAME:
      {
        //CLog::Log(LOGDEBUG, "DVDNAV_STILL_FRAME");

        dvdnav_still_event_t *still_event = static_cast<dvdnav_still_event_t*>(pData);
        // should wait the specified time here while we let the player running
        // after that call dvdnav_still_skip(m_dvdnav);

        if (m_dvd.state != DVDSTATE_STILL)
        {
          // else notify the player we have received a still frame

          if(still_event->length < 0xff)
            m_dvd.iDVDStillTime = std::chrono::seconds(still_event->length);
          else
            m_dvd.iDVDStillTime = 0ms;

          m_dvd.iDVDStillStartTime = std::chrono::steady_clock::now();

          /* adjust for the output delay in the video queue */
          std::chrono::milliseconds time = 0ms;
          if (m_CurrentVideo.stream && m_dvd.iDVDStillTime > 0ms)
          {
            time = std::chrono::milliseconds(
                static_cast<int>(m_VideoPlayerVideo->GetOutputDelay() / (DVD_TIME_BASE / 1000)));
            if (time < 10000ms && time > 0ms)
              m_dvd.iDVDStillTime += time;
          }
          m_dvd.state = DVDSTATE_STILL;
          logComponentM(LOGDEBUG, LOGBLURAY, "DVDNAV_STILL_FRAME - waiting {} sec, with delay of {} msec",
                    still_event->length, time.count());
        }
        return NAVRESULT_HOLD;
      }
      break;
    case DVDNAV_SPU_CLUT_CHANGE:
      {
        m_VideoPlayerSubtitle->SendMessage(
            std::make_shared<CDVDMsgSubtitleClutChange>((uint8_t*)pData));
      }
      break;
    case DVDNAV_SPU_STREAM_CHANGE:
      {
        dvdnav_spu_stream_change_event_t* event = static_cast<dvdnav_spu_stream_change_event_t*>(pData);

        int iStream = event->physical_wide;
        bool visible = !(iStream & 0x80);

        SetSubtitleVisibleInternal(visible);

        if (iStream >= 0)
          m_dvd.iSelectedSPUStream = (iStream & ~0x80);
        else
          m_dvd.iSelectedSPUStream = -1;

        m_CurrentSubtitle.stream = nullptr;
      }
      break;
    case DVDNAV_AUDIO_STREAM_CHANGE:
      {
        dvdnav_audio_stream_change_event_t* event = static_cast<dvdnav_audio_stream_change_event_t*>(pData);
        // Tell system what audiostream should be opened by default
        m_dvd.iSelectedAudioStream = event->physical;
        m_CurrentAudio.stream = nullptr;
      }
      break;
    case DVDNAV_HIGHLIGHT:
      {
        //dvdnav_highlight_event_t* pInfo = (dvdnav_highlight_event_t*)pData;
        int iButton = pStream->GetCurrentButton();
        logComponentM(LOGDEBUG, LOGBLURAY, "DVDNAV_HIGHLIGHT: Highlight button {}", iButton);
        m_VideoPlayerSubtitle->UpdateOverlayInfo(std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream), LIBDVDNAV_BUTTON_NORMAL);
      }
      break;
    case DVDNAV_VTS_CHANGE:
      {
        //dvdnav_vts_change_event_t* vts_change_event = (dvdnav_vts_change_event_t*)pData;
        logComponentM(LOGDEBUG, LOGBLURAY, "DVDNAV_VTS_CHANGE");

        //Make sure we clear all the old overlays here, or else old forced items are left.
        m_overlayContainer.Clear();

        //Force an aspect ratio that is set in the dvdheaders if available
        m_CurrentVideo.hint.aspect = static_cast<double>(pStream->GetVideoAspectRatio());
        if( m_VideoPlayerVideo->IsInited() )
          m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgDouble>(
              CDVDMsg::VIDEO_SET_ASPECT, m_CurrentVideo.hint.aspect));

        m_SelectionStreams.Clear(STREAM_NONE, STREAM_SOURCE_NAV);
        m_SelectionStreams.Update(m_pInputStream, m_pDemuxer.get());
        UpdateContent();

        return NAVRESULT_HOLD;
      }
      break;
    case DVDNAV_CELL_CHANGE:
      {
        //dvdnav_cell_change_event_t* cell_change_event = (dvdnav_cell_change_event_t*)pData;
        logComponentM(LOGDEBUG, LOGBLURAY, "DVDNAV_CELL_CHANGE");

        if (m_dvd.state != DVDSTATE_STILL)
          m_dvd.state = DVDSTATE_NORMAL;
      }
      break;
    case DVDNAV_NAV_PACKET:
      {
          //pci_t* pci = (pci_t*)pData;

          // this should be possible to use to make sure we get
          // seamless transitions over these boundaries
          // if we remember the old vobunits boundaries
          // when a packet comes out of demuxer that has
          // pts values outside that boundary, it belongs
          // to the new vobunit, which has new timestamps
          UpdatePlayState(0);
      }
      break;
    case DVDNAV_HOP_CHANNEL:
      {
        // This event is issued whenever a non-seamless operation has been executed.
        // Applications with fifos should drop the fifos content to speed up responsiveness.
        logComponentM(LOGDEBUG, LOGBLURAY, "DVDNAV_HOP_CHANNEL");
        if(m_dvd.state == DVDSTATE_SEEK)
          m_dvd.state = DVDSTATE_NORMAL;
        else
        {
          bool sync = !IsInMenuInternal();
          FlushBuffers(DVD_NOPTS_VALUE, false, sync);
          m_dvd.syncClock = true;
          m_dvd.state = DVDSTATE_NORMAL;
          if (m_pDemuxer)
            m_pDemuxer->Flush();
        }

        return NAVRESULT_ERROR;
      }
      break;
    case DVDNAV_STOP:
      {
        logComponentM(LOGDEBUG, LOGBLURAY, "DVDNAV_STOP");
        m_dvd.state = DVDSTATE_NORMAL;
      }
      break;
    case DVDNAV_ERROR:
      {
        logComponentM(LOGDEBUG, LOGBLURAY, "DVDNAV_ERROR");
        m_dvd.state = DVDSTATE_NORMAL;
        CGUIDialogKaiToast::QueueNotification(g_localizeStrings.Get(16026),
                                              g_localizeStrings.Get(16029));
      }
      break;
    default:
    {}
      break;
    }
  }
  return NAVRESULT_NOP;
}

void CVideoPlayer::GetVideoResolution(unsigned int &width, unsigned int &height)
{
  RESOLUTION_INFO res = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
  width = res.iWidth;
  height = res.iHeight;
}

bool CVideoPlayer::OnAction(const CAction &action)
{
#define THREAD_ACTION(action) \
  do \
  { \
    if (!IsCurrentThread()) \
    { \
      m_messenger.Put( \
          std::make_shared<CDVDMsgType<CAction>>(CDVDMsg::GENERAL_GUI_ACTION, action)); \
      return true; \
    } \
  } while (false)

  std::shared_ptr<CDVDInputStream::IMenus> pMenus = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream);
  if (pMenus)
  {
    switch (action.GetID())
    {
      case ACTION_MOVE_LEFT:
      case ACTION_MOVE_RIGHT:
      case ACTION_MOVE_UP:
      case ACTION_MOVE_DOWN:
      case ACTION_SELECT_ITEM:
      case ACTION_NAV_BACK:
      case ACTION_PREVIOUS_MENU:
      case ACTION_SHOW_VIDEOMENU:
      case ACTION_NEXT_ITEM:
      case ACTION_PREV_ITEM:
        logComponentM(LOGDEBUG, LOGBLURAY,
                      "CVideoPlayer::OnAction id={} name='{}' pMenus->IsInMenu()={} cachedInMenu={} dvdState={} thread={}",
                      action.GetID(), action.GetName(),
                      pMenus->IsInMenu(), m_State.isInMenu, static_cast<int>(m_dvd.state),
                      IsCurrentThread() ? "player" : "gui");
        break;
      default:
        break;
    }
    if (m_dvd.state == DVDSTATE_STILL && m_dvd.iDVDStillTime != 0ms &&
        pMenus->GetTotalButtons() == 0)
    {
      switch(action.GetID())
      {
        case ACTION_NEXT_ITEM:
        case ACTION_MOVE_RIGHT:
        case ACTION_MOVE_UP:
        case ACTION_SELECT_ITEM:
          {
            THREAD_ACTION(action);
            /* this will force us out of the stillframe */
            logComponentM(LOGDEBUG, LOGVIDEO, "{} - User asked to exit stillframe", __FUNCTION__);
            m_dvd.iDVDStillStartTime = {};
            m_dvd.iDVDStillTime = 1ms;
          }
          return true;
      }
    }


    switch (action.GetID())
    {
/* this code is disabled to allow switching playlist items (dvdimage "stacks") */
#if 0
    case ACTION_PREV_ITEM:  // SKIP-:
      {
        THREAD_ACTION(action);
        logComponentM(LOGDEBUG, LOGVIDEO, " - pushed prev");
        pMenus->OnPrevious();
        m_processInfo->SeekFinished(0);
        return true;
      }
      break;
    case ACTION_NEXT_ITEM:  // SKIP+:
      {
        THREAD_ACTION(action);
        logComponentM(LOGDEBUG, LOGVIDEO, " - pushed next");
        pMenus->OnNext();
        m_processInfo->SeekFinished(0);
        return true;
      }
      break;
#endif
    case ACTION_SHOW_VIDEOMENU:   // start button
      {
        THREAD_ACTION(action);
        CDVDInputStream::IMenus::MenuCall menuCall = CDVDInputStream::IMenus::MenuCall::Auto;
        const std::string& menuArg = action.GetName();
        if (menuArg == "popup")
          menuCall = CDVDInputStream::IMenus::MenuCall::Popup;
        else if (menuArg == "top")
          menuCall = CDVDInputStream::IMenus::MenuCall::Top;
        logComponentM(LOGDEBUG, LOGVIDEO, "Trying to go to the menu ({})",
                      menuArg.empty() ? "auto" : menuArg.c_str());
        if (pMenus->OnMenu(menuCall))
        {
          if (m_playSpeed == DVD_PLAYSPEED_PAUSE)
          {
            SetPlaySpeed(DVD_PLAYSPEED_NORMAL);
            m_callback.OnPlayBackResumed();
          }

          // send a message to everyone that we've gone to the menu
          CGUIMessage msg(GUI_MSG_VIDEO_MENU_STARTED, 0, 0);
          CServiceBroker::GetGUI()->GetWindowManager().SendThreadMessage(msg);
        }
        return true;
      }
      break;
    case ACTION_TELETEXT_RED:
    case ACTION_TELETEXT_GREEN:
    case ACTION_TELETEXT_YELLOW:
    case ACTION_TELETEXT_BLUE:
      THREAD_ACTION(action);
      if (pMenus->OnColorKey(action.GetID() - ACTION_TELETEXT_RED))
        return true;
      break;
    }

    if (pMenus->IsInMenu())
    {
      switch (action.GetID())
      {
      case ACTION_NEXT_ITEM:
      {
        THREAD_ACTION(action);
        logComponentM(LOGDEBUG, LOGVIDEO, " - pushed next in menu, stream will decide");
        const int nextChapter = GetNextChapter();
        if (pMenus->CanSeek() && GetChapterCount() > 0 && nextChapter <= GetChapterCount())
          SeekChapter(nextChapter);
        else
          pMenus->OnNext();

        m_processInfo->SeekFinished(0);
        return true;
      }
      case ACTION_PREV_ITEM:
        THREAD_ACTION(action);
        logComponentM(LOGDEBUG, LOGVIDEO, " - pushed prev in menu, stream will decide");
        if (pMenus->CanSeek() && GetChapterCount() > 0 && GetChapter() > 0)
          m_messenger.Put(std::make_shared<CDVDMsgPlayerSeekChapter>(GetPreviousChapter()));
        else
          pMenus->OnPrevious();

        m_processInfo->SeekFinished(0);
        return true;
      case ACTION_PREVIOUS_MENU:
      case ACTION_NAV_BACK:
        {
          logComponentM(LOGDEBUG, LOGVIDEO, " - menu back");
          pMenus->OnBack();
        }
        break;
      case ACTION_MOVE_LEFT:
        {
          logComponentM(LOGDEBUG, LOGVIDEO, " - move left");
          pMenus->OnLeft();
        }
        break;
      case ACTION_MOVE_RIGHT:
        {
          logComponentM(LOGDEBUG, LOGVIDEO, " - move right");
          pMenus->OnRight();
        }
        break;
      case ACTION_MOVE_UP:
        {
          logComponentM(LOGDEBUG, LOGVIDEO, " - move up");
          pMenus->OnUp();
        }
        break;
      case ACTION_MOVE_DOWN:
        {
          logComponentM(LOGDEBUG, LOGVIDEO, " - move down");
          pMenus->OnDown();
        }
        break;

      case ACTION_MOUSE_MOVE:
      case ACTION_MOUSE_LEFT_CLICK:
        {
          CRect rs, rd, rv;
          m_renderManager.GetVideoRect(rs, rd, rv);
          CPoint pt(action.GetAmount(), action.GetAmount(1));
          if (!rd.PtInRect(pt))
            return false; // out of bounds
          THREAD_ACTION(action);
          // convert to video coords...
          pt -= CPoint(rd.x1, rd.y1);
          pt.x *= rs.Width() / rd.Width();
          pt.y *= rs.Height() / rd.Height();
          pt += CPoint(rs.x1, rs.y1);
          if (action.GetID() == ACTION_MOUSE_LEFT_CLICK)
          {
            if (pMenus->OnMouseClick(pt))
              return true;
            else
            {
              CServiceBroker::GetAppMessenger()->PostMsg(
                  TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                  static_cast<void*>(new CAction(ACTION_TRIGGER_OSD)));
              return false;
            }
          }
          return pMenus->OnMouseMove(pt);
        }
        break;
      case ACTION_SELECT_ITEM:
        {
          logComponentM(LOGDEBUG, LOGVIDEO, " - button select");
          // show button pushed overlay
          if(m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
            m_VideoPlayerSubtitle->UpdateOverlayInfo(std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream), LIBDVDNAV_BUTTON_CLICKED);

          pMenus->ActivateButton();
        }
        break;
      case REMOTE_0:
      case REMOTE_1:
      case REMOTE_2:
      case REMOTE_3:
      case REMOTE_4:
      case REMOTE_5:
      case REMOTE_6:
      case REMOTE_7:
      case REMOTE_8:
      case REMOTE_9:
        {
          THREAD_ACTION(action);
          // Offset from key codes back to button number
          int button = action.GetID() - REMOTE_0;
          logComponentM(LOGDEBUG, LOGVIDEO, " - button pressed {}", button);
          pMenus->SelectButton(button);
        }
       break;
      default:
        return false;
        break;
      }
      return true; // message is handled
    }
  }

  pMenus.reset();

  StreamHdrType hdrType = CServiceBroker::GetDataCacheCore().GetVideoHdrType();

  switch (action.GetID())
  {
    case ACTION_NEXT_ITEM:
    {
      const int nextChapter = GetNextChapter();
      if (GetChapter() > 0 && nextChapter <= GetChapterCount())
      {
        SeekChapter(nextChapter);
        m_processInfo->SeekFinished(0);
        return true;
      }
      else if (SeekScene(true))
        return true;
      else
        break;
    }
    case ACTION_PREV_ITEM:
      if (GetChapter() > 0)
      {
        m_messenger.Put(std::make_shared<CDVDMsgPlayerSeekChapter>(GetPreviousChapter()));
        m_processInfo->SeekFinished(0);
        return true;
      }
      else if (SeekScene(false))
        return true;
      else
        break;
    case ACTION_TOGGLE_COMMSKIP:
      m_SkipCommercials = !m_SkipCommercials;
      CGUIDialogKaiToast::QueueNotification(g_localizeStrings.Get(25011),
                                            g_localizeStrings.Get(m_SkipCommercials ? 25013 : 25012));
      break;
    case ACTION_PLAYER_DEBUG:
      m_renderManager.ToggleDebug();
      break;
    case ACTION_PLAYER_DEBUG_VIDEO:
      m_renderManager.ToggleDebugVideo();
      break;

    case ACTION_PLAYER_PROCESS_INFO:
      if (CServiceBroker::GetGUI()->GetWindowManager().GetActiveWindow() != WINDOW_DIALOG_PLAYER_PROCESS_INFO)
      {
        CServiceBroker::GetGUI()->GetWindowManager().ActivateWindow(WINDOW_DIALOG_PLAYER_PROCESS_INFO);
        return true;
      }
      break;

    case ACTION_VS10_ORIGINAL:
      if (hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
        aml_dv_set_vs10_mode(DOLBY_VISION_OUTPUT_MODE_IPT, hdrType);
      else
        aml_dv_set_vs10_mode(DOLBY_VISION_OUTPUT_MODE_BYPASS, hdrType);
      return true;
    case ACTION_VS10_SDR:
      aml_dv_set_vs10_mode(DOLBY_VISION_OUTPUT_MODE_SDR10, hdrType, true);
      return true;
    case ACTION_VS10_HDR10:
      aml_dv_set_vs10_mode(DOLBY_VISION_OUTPUT_MODE_HDR10, hdrType, true);
      return true;
    case ACTION_VS10_DV:
      aml_dv_set_vs10_mode(DOLBY_VISION_OUTPUT_MODE_IPT, hdrType);
      return true;
  }

  // return false to inform the caller we didn't handle the message
  return false;
}

bool CVideoPlayer::IsInMenuInternal() const
{
  if (m_menus)
  {
    if (m_dvd.state == DVDSTATE_STILL)
      return true;
    else
      return m_menus->IsInMenu();
  }
  return false;
}


bool CVideoPlayer::IsInMenu() const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return m_State.isInMenu;
}

MenuType CVideoPlayer::GetSupportedMenuType() const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return m_State.menuType;
}

std::string CVideoPlayer::GetPlayerState()
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return m_State.player_state;
}

bool CVideoPlayer::SetPlayerState(const std::string& state)
{
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSetState>(state));
  return true;
}

int CVideoPlayer::GetChapterCount() const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return m_State.chapters.size();
}

int CVideoPlayer::GetChapter() const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return m_State.chapter;
}

void CVideoPlayer::GetChapterName(std::string& strChapterName, int chapterIdx) const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  if (chapterIdx == -1 && m_State.chapter > 0 && m_State.chapter <= (int) m_State.chapters.size())
    strChapterName = m_State.chapters[m_State.chapter - 1].first;
  else if (chapterIdx > 0 && chapterIdx <= (int) m_State.chapters.size())
    strChapterName = m_State.chapters[chapterIdx - 1].first;
}

int CVideoPlayer::SeekChapter(int iChapter)
{
  const int currentChapter = GetChapter();
  if (currentChapter > 0)
  {
    if (iChapter < 0)
      iChapter = 0;
    if (iChapter > GetChapterCount())
    {
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "SeekChapter target={} > GetChapterCount()={} - rejected",
                    iChapter, GetChapterCount());
      return 0;
    }

    {
      std::unique_lock<CCriticalSection> lock(m_StateSection);
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "SeekChapter target={} prev m_State.chapter={} - optimistic update",
                    iChapter, m_State.chapter);
      m_State.chapter = iChapter;
      m_lastChapterSeekTarget = iChapter;
    }

    m_messenger.Put(std::make_shared<CDVDMsgPlayerSeekChapter>(iChapter));
    SynchronizeDemuxer();
  }
  else
  {
    logComponentM(LOGDEBUG, LOGVIDEO,
                  "SeekChapter target={} skipped - GetChapter()={}",
                  iChapter, currentChapter);
  }

  return 0;
}

int64_t CVideoPlayer::GetChapterPos(int chapterIdx) const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  if (chapterIdx > 0 && chapterIdx <= (int) m_State.chapters.size())
    return m_State.chapters[chapterIdx - 1].second;

  return -1;
}

int CVideoPlayer::GetPreviousChapter()
{
  const int64_t timeMs = GetTime();
  const int cachedChapter = GetChapter();

  int actualChapter = 0;
  int64_t actualChapterPosSec = -1;
  {
    std::unique_lock<CCriticalSection> lock(m_StateSection);
    for (size_t i = 0; i < m_State.chapters.size(); ++i)
    {
      const int64_t startSec = m_State.chapters[i].second;
      const int64_t startMs = startSec * 1000;
      const int64_t endMs =
          (i + 1 < m_State.chapters.size()) ? m_State.chapters[i + 1].second * 1000
                                            : std::numeric_limits<int64_t>::max();
      if (timeMs >= startMs && timeMs < endMs)
      {
        actualChapter = static_cast<int>(i + 1);
        actualChapterPosSec = startSec;
        break;
      }
    }
  }

  if (actualChapter == 0)
    actualChapter = cachedChapter;

  if (cachedChapter > 0 && actualChapter > cachedChapter + 1)
  {
    logComponentM(LOGDEBUG, LOGVIDEO,
                  "GetPreviousChapter time={}ms cachedChapter={} actualChapter={} "
                  "rejected (mid-seek clock ahead of current) -> {}",
                  timeMs, cachedChapter, actualChapter, cachedChapter - 1);
    return cachedChapter - 1;
  }

  const int result =
      (actualChapter > 0 && (timeMs < (actualChapterPosSec + 15) * 1000))
          ? actualChapter - 1
          : actualChapter;

  logComponentM(LOGDEBUG, LOGVIDEO,
                "GetPreviousChapter time={}ms cachedChapter={} actualChapter={} "
                "actualChapterPos={}s threshold=15s -> {}",
                timeMs, cachedChapter, actualChapter, actualChapterPosSec, result);

  return result;
}

int CVideoPlayer::GetNextChapter()
{
  const int64_t timeMs = GetTime();
  const int cachedChapter = GetChapter();

  int actualChapter = 0;
  int lastSeekTarget = 0;
  int64_t nextStartMs = -1;
  {
    std::unique_lock<CCriticalSection> lock(m_StateSection);
    lastSeekTarget = m_lastChapterSeekTarget;
    for (size_t i = 0; i < m_State.chapters.size(); ++i)
    {
      const int64_t startMs = m_State.chapters[i].second * 1000;
      const int64_t endMs =
          (i + 1 < m_State.chapters.size()) ? m_State.chapters[i + 1].second * 1000
                                            : std::numeric_limits<int64_t>::max();
      if (timeMs >= startMs && timeMs < endMs)
      {
        actualChapter = static_cast<int>(i + 1);
        if (i + 1 < m_State.chapters.size())
          nextStartMs = m_State.chapters[i + 1].second * 1000;
        break;
      }
    }
  }

  if (actualChapter == 0)
    actualChapter = cachedChapter;

  if (cachedChapter > 0 && actualChapter > cachedChapter + 1)
  {
    logComponentM(LOGDEBUG, LOGVIDEO,
                  "GetNextChapter time={}ms cachedChapter={} actualChapter={} "
                  "rejected (mid-seek clock ahead of current) -> {}",
                  timeMs, cachedChapter, actualChapter, cachedChapter + 1);
    return cachedChapter + 1;
  }

  if (actualChapter > 0 && lastSeekTarget == actualChapter + 1 && nextStartMs >= 0 &&
      nextStartMs - timeMs <= 20000)
  {
    logComponentM(LOGDEBUG, LOGVIDEO,
                  "GetNextChapter time={}ms cachedChapter={} actualChapter={} "
                  "landing zone of seek target {} -> {}",
                  timeMs, cachedChapter, actualChapter, lastSeekTarget, actualChapter + 2);
    return actualChapter + 2;
  }

  logComponentM(LOGDEBUG, LOGVIDEO,
                "GetNextChapter time={}ms cachedChapter={} actualChapter={} -> {}",
                timeMs, cachedChapter, actualChapter, actualChapter + 1);
  return actualChapter + 1;
}

void CVideoPlayer::AddSubtitle(const std::string& strSubPath)
{
  m_messenger.Put(
      std::make_shared<CDVDMsgType<std::string>>(CDVDMsg::SUBTITLE_ADDFILE, strSubPath));
}

bool CVideoPlayer::IsCaching() const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return !m_State.isInMenu && m_State.caching;
}

int CVideoPlayer::GetCacheLevel() const
{
  std::unique_lock<CCriticalSection> lock(m_StateSection);
  return (int)(m_State.cache_level * 100);
}

double CVideoPlayer::GetQueueTime() const {
  int a = m_VideoPlayerAudio->GetLevel();
  int v = m_VideoPlayerVideo->GetLevel();
  return std::max(a, v) * m_messageQueueTimeSize * 1000.0 / 100.0;
}

int CVideoPlayer::AddSubtitleFile(const std::string& filename, const std::string& subfilename)
{
  std::string ext = URIUtils::GetExtension(filename);
  std::string vobsubfile = subfilename;
  if (ext == ".idx" || ext == ".sup")
  {
    std::shared_ptr<CDVDDemux> pDemux;
    if (ext == ".idx")
    {
      if (vobsubfile.empty())
      {
        // find corresponding .sub (e.g. in case of manually selected .idx sub)
        vobsubfile = CUtil::GetVobSubSubFromIdx(filename);
        if (vobsubfile.empty())
          return -1;
      }

      auto pDemuxVobsub = std::make_shared<CDVDDemuxVobsub>();
      if (!pDemuxVobsub->Open(filename, STREAM_SOURCE_NONE, vobsubfile))
        return -1;

      m_SelectionStreams.Update(nullptr, pDemuxVobsub.get(), vobsubfile);
      pDemux = pDemuxVobsub;
    }
    else // .sup file
    {
      CFileItem item(filename, false);
      std::shared_ptr<CDVDInputStream> pInput;
      pInput = CDVDFactoryInputStream::CreateInputStream(nullptr, item);
      if (!pInput || !pInput->Open())
        return -1;

      auto pDemuxFFmpeg = std::make_shared<CDVDDemuxFFmpeg>();
      if (!pDemuxFFmpeg->Open(pInput, false))
        return -1;

      m_SelectionStreams.Update(nullptr, pDemuxFFmpeg.get(), filename);
      pDemux = pDemuxFFmpeg;
    }

    ExternalStreamInfo info =
        CUtil::GetExternalStreamDetailsFromFilename(m_item.GetDynPath(), filename);

    for (auto sub : pDemux->GetStreams())
    {
      if (sub->type != STREAM_SUBTITLE)
        continue;

      int index = m_SelectionStreams.TypeIndexOf(STREAM_SUBTITLE,
        m_SelectionStreams.Source(STREAM_SOURCE_DEMUX_SUB, filename),
        sub->demuxerId, sub->uniqueId);
      SelectionStream& stream = m_SelectionStreams.Get(STREAM_SUBTITLE, index);

      if (stream.name.empty())
        stream.name = info.name;

      if (stream.language.empty())
        stream.language = info.language;

      if (static_cast<StreamFlags>(info.flag) != StreamFlags::FLAG_NONE)
        stream.flags = static_cast<StreamFlags>(info.flag);
    }

    UpdateContent();
    // the demuxer id is unique
    m_subtitleDemuxerMap[pDemux->GetDemuxerId()] = pDemux;
    return m_SelectionStreams.TypeIndexOf(
        STREAM_SUBTITLE, m_SelectionStreams.Source(STREAM_SOURCE_DEMUX_SUB, filename),
        pDemux->GetDemuxerId(), 0);
  }

  if(ext == ".sub")
  {
    // if this looks like vobsub file (i.e. .idx found), add it as such
    std::string vobsubidx = CUtil::GetVobSubIdxFromSub(filename);
    if (!vobsubidx.empty())
      return AddSubtitleFile(vobsubidx, filename);
  }

  SelectionStream s;
  s.source   = m_SelectionStreams.Source(STREAM_SOURCE_TEXT, filename);
  s.type     = STREAM_SUBTITLE;
  s.id       = 0;
  s.filename = filename;
  ExternalStreamInfo info = CUtil::GetExternalStreamDetailsFromFilename(m_item.GetDynPath(), filename);
  s.name = info.name;
  s.language = info.language;
  if (static_cast<StreamFlags>(info.flag) != StreamFlags::FLAG_NONE)
    s.flags = static_cast<StreamFlags>(info.flag);

  m_SelectionStreams.Update(s);
  UpdateContent();
  return m_SelectionStreams.TypeIndexOf(STREAM_SUBTITLE, s.source, s.demuxerId, s.id);
}

void CVideoPlayer::UpdatePlayState(double timeout)
{
  const double now = m_clock.GetAbsoluteClock();

  SPlayerState state;
  {
    std::unique_lock lock(m_StateSection);
    if (m_State.timestamp != 0 && m_State.timestamp + DVD_MSEC_TO_TIME(timeout) > now)
      return;
    state = m_State;
  }

  m_parseCaptions = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
      CSettings::SETTING_SUBTITLES_PARSECAPTIONS);

  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY))
  {
    auto* bluray = static_cast<CDVDInputStreamBluray*>(m_pInputStream.get());
    const bool feature = bluray->IsFeaturePlaylistActive();
    if (feature)
    {
      if (m_bdFeatureActiveSince == 0.0)
        m_bdFeatureActiveSince = now;
    }
    else
    {
      m_bdFeatureActiveSince = 0.0;
      if (m_bdFeatureTagsFired)
      {
        CServiceBroker::GetDataCacheCore().NextAVChangeGeneration();
        CServiceBroker::GetDataCacheCore().SetAVChange(false);
        CServiceBroker::GetDataCacheCore().SetAVChangeExtended(false);
        logM(LOGDEBUG, "VideoPlayer::SetAVChange cancelled - bluray left the feature playlist");
      }
      m_bdFeatureTagsFired = false;
    }
    const double featureHoldMs = bluray->IsOnFeaturePlaylist() ? 2000.0 : 8000.0;
    m_bdFeatureStable = m_bdFeatureActiveSince != 0.0 &&
                        now - m_bdFeatureActiveSince >= DVD_MSEC_TO_TIME(featureHoldMs);
    if (m_bdFeatureStable && !m_bdFeatureTagsFired)
    {
      m_bdFeatureTagsFired = true;
      SetAVChange("FeatureStart");
    }
  }

  state.dts = DVD_NOPTS_VALUE;
  if (m_CurrentVideo.dts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentVideo.dts;
  else if (m_CurrentAudio.dts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentAudio.dts;
  else if (m_CurrentVideo.startpts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentVideo.startpts;
  else if (m_CurrentAudio.startpts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentAudio.startpts;

  state.startTime = 0;
  state.timeMin = 0;

  std::shared_ptr<CDVDInputStream::IMenus> pMenu = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream);



  if (m_pDemuxer)
  {
    if (IsInMenuInternal() && pMenu && !pMenu->CanSeek())
      state.chapter = 0;
    else
      state.chapter = m_pDemuxer->GetChapter();

    state.chapters.clear();
    if (m_pDemuxer->GetChapterCount() > 0)
    {
      for (int i = 0, ie = m_pDemuxer->GetChapterCount(); i < ie; ++i)
      {
        std::string name;
        m_pDemuxer->GetChapterName(name, i + 1);
        state.chapters.emplace_back(name, m_pDemuxer->GetChapterPos(i + 1));
      }
    }
    CServiceBroker::GetDataCacheCore().SetChapters(state.chapters);

    state.time = m_clock.GetClock(false) * 1000 / DVD_TIME_BASE;
    state.timeMax = m_pDemuxer->GetStreamLength();
  }

  state.canpause = false;
  state.canseek = false;
  state.cantempo = false;
  state.isInMenu = false;
  state.menuType = MenuType::NONE;

  if (m_pInputStream)
  {
    CDVDInputStream::IChapter* pChapter = m_pInputStream->GetIChapter();
    if (pChapter)
    {
      if (IsInMenuInternal() && pMenu && !pMenu->CanSeek())
        state.chapter = 0;
      else
        state.chapter = pChapter->GetChapter();

      const int chapterCount = pChapter->GetChapterCount();
      bool chaptersChanged = static_cast<int>(state.chapters.size()) != chapterCount;
      for (int i = 0; !chaptersChanged && i < chapterCount; ++i)
        chaptersChanged = state.chapters[i].second != pChapter->GetChapterPos(i + 1);
      if (chaptersChanged)
      {
        state.chapters.clear();
        for (int i = 0; i < chapterCount; ++i)
        {
          std::string name;
          pChapter->GetChapterName(name, i + 1);
          state.chapters.emplace_back(name, pChapter->GetChapterPos(i + 1));
        }
        CServiceBroker::GetDataCacheCore().SetChapters(state.chapters);
      }
    }

    CDVDInputStream::ITimes* pTimes = m_pInputStream->GetITimes();
    CDVDInputStream::IDisplayTime* pDisplayTime = m_pInputStream->GetIDisplayTime();

    CDVDInputStream::ITimes::Times times;
    if (pTimes && pTimes->GetTimes(times))
    {
      state.startTime = times.startTime;
      state.time = (m_clock.GetClock(false) - times.ptsStart) * 1000 / DVD_TIME_BASE;
      state.timeMax = (times.ptsEnd - times.ptsStart) * 1000 / DVD_TIME_BASE;
      state.timeMin = (times.ptsBegin - times.ptsStart) * 1000 / DVD_TIME_BASE;
      state.time_offset = -times.ptsStart;
    }
    else if (pDisplayTime && pDisplayTime->GetTotalTime() > 0)
    {
      if (state.dts != DVD_NOPTS_VALUE)
      {
        int dispTime = 0;
        if (m_CurrentVideo.id >= 0 && m_CurrentVideo.dispTime)
          dispTime = m_CurrentVideo.dispTime;
        else if (m_CurrentAudio.dispTime)
          dispTime = m_CurrentAudio.dispTime;

        state.time_offset = DVD_MSEC_TO_TIME(dispTime) - state.dts;
      }
      state.time += state.time_offset * 1000 / DVD_TIME_BASE;
      state.timeMax = pDisplayTime->GetTotalTime();
    }
    else
    {
      state.time_offset = 0;
    }

    if (pMenu)
    {
      if (!pMenu->GetState(state.player_state))
        state.player_state = "";

      if (m_dvd.state == DVDSTATE_STILL)
      {
        const auto now = std::chrono::steady_clock::now();
        const auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_dvd.iDVDStillStartTime);
        state.time = duration.count();
        state.timeMax = m_dvd.iDVDStillTime.count();
        state.isInMenu = true;
      }
      else if (IsInMenuInternal())
      {
        state.time = pDisplayTime->GetTime();
        state.isInMenu = true;
        if (!pMenu->CanSeek())
          state.time_offset = 0;
      }
      state.menuType = pMenu->GetSupportedMenuType();
    }

    state.canpause = m_pInputStream->CanPause();

    bool realtime = m_pInputStream->IsRealtime();

    state.cantempo = CanTempo() && !realtime;

    m_processInfo->SetStateRealtime(realtime);
  }

  if (m_Edl.HasCuts())
  {
    state.time = static_cast<double>(m_Edl.GetTimeWithoutCuts(state.time));
    state.timeMax = state.timeMax - static_cast<double>(m_Edl.GetTotalCutTime());
  }

  if (m_caching > CACHESTATE_DONE && m_caching < CACHESTATE_PLAY)
    state.caching = true;
  else
    state.caching = false;

  double queueTime = GetQueueTime();
  CacheInfo cache = GetCachingTimes();

  if (cache.valid)
  {
    state.cache_level = std::max(0.0, std::min(1.0, cache.level));
    state.cache_offset = cache.offset;
    state.cache_time = cache.time;
  }
  else
  {
    state.cache_level = std::min(1.0, queueTime / (m_messageQueueTimeSize * 1000.0));
    state.cache_offset = queueTime / state.timeMax;
    state.cache_time = queueTime / 1000.0;
  }

  XFILE::SCacheStatus status;
  if (m_pInputStream && m_pInputStream->GetCacheStatus(&status))
  {
    state.cache_bytes = status.forward;
    if(state.timeMax)
      state.cache_bytes += m_pInputStream->GetLength() * (int64_t)(queueTime / state.timeMax);
  }
  else
    state.cache_bytes = 0;

  state.timestamp = m_clock.GetAbsoluteClock();

  if (state.timeMax <= 0)
  {
    state.timeMax = state.time;
    state.timeMin = state.time;
  }
  if (state.timeMin == state.timeMax)
  {
    state.canseek = false;
    state.cantempo = false;
  }
  else
  {
    state.canseek = true;
    state.canpause = true;
  }

  m_processInfo->SetPlayTimes(state.startTime, state.time, state.timeMin, state.timeMax);

  std::unique_lock<CCriticalSection> lock(m_StateSection);
  m_State = state;
}

int64_t CVideoPlayer::GetUpdatedTime()
{
  UpdatePlayState(0);
  return llrint(m_State.time);
}

void CVideoPlayer::SetDynamicRangeCompression(long drc)
{
  m_processInfo->GetVideoSettingsLocked().SetVolumeAmplification(static_cast<float>(drc) / 100);
  m_VideoPlayerAudio->SetDynamicRangeCompression(drc);
}

CVideoSettings CVideoPlayer::GetVideoSettings() const
{
  return m_processInfo->GetVideoSettings();
}

void CVideoPlayer::SetVideoSettings(CVideoSettings& settings)
{
  m_processInfo->SetVideoSettings(settings);
  m_renderManager.SetVideoSettings(settings);
  const int settingsDelayMs = static_cast<int>(settings.m_AudioDelay * 1000.0f);
  m_renderManager.SetDelay(settingsDelayMs);
  AUDIODELAY_LOG("VP.SetAVDelay",
                 "source=SetVideoSettings delayMs={} audioDelay={:.4f}",
                 settingsDelayMs,
                 settings.m_AudioDelay);
  m_renderManager.SetSubtitleVerticalPosition(settings.m_subtitleVerticalPosition,
                                              settings.m_subtitleVerticalPositionSave);
  m_VideoPlayerVideo->EnableSubtitle(settings.m_SubtitleOn);
  m_VideoPlayerVideo->SetSubtitleDelay(static_cast<int>(-settings.m_SubtitleDelay * DVD_TIME_BASE));
}

void CVideoPlayer::FrameMove()
{
  m_renderManager.FrameMove();
  if (m_asyncVideoRenderLatched.load(std::memory_order_relaxed))
  {
    const bool live = m_asyncVideoWorkerLive.load(std::memory_order_relaxed);
    if (!live && m_renderManager.CanRunAsyncVideoWorker())
      StartHwVideoRenderThread();
    else if (live && m_renderManager.IsConfigured() && !m_renderManager.CanRunAsyncVideoWorker())
      StopHwVideoRenderThread(false);
  }
}

void CVideoPlayer::Render(bool clear, uint32_t alpha, bool gui)
{
  if (!gui && m_asyncVideoWorkerLive.load(std::memory_order_relaxed))
    return;
  m_renderManager.Render(clear, 0, alpha, gui);
}

void CVideoPlayer::FlushRenderer()
{
  m_renderManager.Flush(true, true);
}

void CVideoPlayer::PreInitRenderer()
{
  m_renderManager.PreInit();
}

void CVideoPlayer::UnInitRenderer()
{
  m_renderManager.UnInit();
}

void CVideoPlayer::SetRenderViewMode(int mode, float zoom, float par, float shift, bool stretch)
{
  m_processInfo->GetVideoSettingsLocked().SetViewMode(mode, zoom, par, shift, stretch);
  m_renderManager.SetVideoSettings(m_processInfo->GetVideoSettings());
  m_renderManager.SetViewMode(mode);
}

float CVideoPlayer::GetRenderAspectRatio() const
{
  return m_renderManager.GetAspectRatio();
}

void CVideoPlayer::TriggerUpdateResolution()
{
  std::string stereomode;
  m_renderManager.TriggerUpdateResolution(0, 0, 0, stereomode);
}

void CVideoPlayer::TriggerUpdateResolutionHdr(StreamHdrType hdrType)
{
  m_renderManager.TriggerUpdateResolutionHdr(hdrType);
}

bool CVideoPlayer::IsRenderingVideo() const
{
  return m_renderManager.IsConfigured();
}

bool CVideoPlayer::Supports(EINTERLACEMETHOD method) const
{
  if (!m_processInfo)
    return false;
  return m_processInfo->Supports(method);
}

EINTERLACEMETHOD CVideoPlayer::GetDeinterlacingMethodDefault() const
{
  if (!m_processInfo)
    return EINTERLACEMETHOD::VS_INTERLACEMETHOD_NONE;
  return m_processInfo->GetDeinterlacingMethodDefault();
}

bool CVideoPlayer::Supports(ESCALINGMETHOD method) const
{
  return m_renderManager.Supports(method);
}

bool CVideoPlayer::Supports(ERENDERFEATURE feature) const
{
  return m_renderManager.Supports(feature);
}

unsigned int CVideoPlayer::RenderCaptureAlloc()
{
  return m_renderManager.AllocRenderCapture();
}

void CVideoPlayer::RenderCapture(unsigned int captureId, unsigned int width, unsigned int height, int flags)
{
  m_renderManager.StartRenderCapture(captureId, width, height, flags);
}

void CVideoPlayer::RenderCaptureRelease(unsigned int captureId)
{
  m_renderManager.ReleaseRenderCapture(captureId);
}

bool CVideoPlayer::RenderCaptureGetPixels(unsigned int captureId, unsigned int millis, uint8_t *buffer, unsigned int size)
{
  return m_renderManager.RenderCaptureGetPixels(captureId, millis, buffer, size);
}

void CVideoPlayer::VideoParamsChange()
{
  m_messenger.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));
}

void CVideoPlayer::GetDebugInfo(std::string &audio, std::string &video, std::string &general)
{
  audio = m_VideoPlayerAudio->GetPlayerInfo();
  video = m_VideoPlayerVideo->GetPlayerInfo();
  GetGeneralInfo(general);
}

void CVideoPlayer::UpdateClockSync(bool enabled)
{
  m_processInfo->SetRenderClockSync(enabled);
}

void CVideoPlayer::UpdateRenderInfo(CRenderInfo &info)
{
  m_processInfo->UpdateRenderInfo(info);
}

void CVideoPlayer::UpdateRenderBuffers(int queued, int discard, int free)
{
  m_processInfo->UpdateRenderBuffers(queued, discard, free);
}

void CVideoPlayer::UpdateGuiRender(bool gui)
{
  m_processInfo->SetGuiRender(gui);
}

void CVideoPlayer::UpdateVideoRender(bool video)
{
  m_processInfo->SetVideoRender(video);
}

// IDispResource interface
void CVideoPlayer::OnLostDisplay()
{
  CLog::Log(LOGDEBUG, "VideoPlayer: OnLostDisplay received");
  m_VideoPlayerAudio->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, true), 1);
  m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, true), 1);
  m_clock.Pause(true);
  m_displayLost = true;
  // Causes https://github.com/xbmc/xbmc/issues/15447
  // FlushRenderer();
}

void CVideoPlayer::OnResetDisplay()
{
  if (!m_displayLost)
    return;

  CLog::Log(LOGDEBUG, "VideoPlayer: OnResetDisplay received");
  m_VideoPlayerAudio->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, false), 1);
  m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, false), 1);
  m_clock.Pause(false);
  m_displayLost = false;
  m_VideoPlayerAudio->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_DISPLAY_RESET), 1);
  m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_DISPLAY_RESET), 1);
}

std::string CVideoPlayer::GetDvProfileString(const AVDOVIDecoderConfigurationRecord& dovi)
{
  if (dovi.dv_profile == 0)
    return {};

  std::string profile = std::to_string(static_cast<int>(dovi.dv_profile)) + "." +
                        std::to_string(static_cast<int>(dovi.dv_bl_signal_compatibility_id));

  if (!dovi.el_present_flag)
    return profile;

  auto& dataCacheCore = CServiceBroker::GetDataCacheCore();
  DOVIELType elType = dataCacheCore.GetVideoSourceDoViStreamInfo().dovi_el_type;
  if (elType == DOVIELType::TYPE_NONE)
    elType = dataCacheCore.GetVideoDoViStreamInfo().dovi_el_type;

  if (elType == DOVIELType::TYPE_FEL)
    return profile + " FEL";
  if (elType == DOVIELType::TYPE_MEL)
    return profile + " MEL";

  return profile + " EL";
}

std::string CVideoPlayer::GetObjectAudioProfile(const std::string& codecName)
{
  return StreamUtils::GetCodecDetail(codecName);
}

void CVideoPlayer::UpdateFileItemStreamDetails(CFileItem& item)
{
  if (!m_UpdateStreamDetails)
    return;
  m_UpdateStreamDetails = false;

  CLog::Log(LOGDEBUG, "CVideoPlayer: updating file item stream details with available streams");

  VideoStreamInfo videoInfo;
  AudioStreamInfo audioInfo;
  SubtitleStreamInfo subtitleInfo;
  CVideoInfoTag* info = item.GetVideoInfoTag();
  GetVideoStreamInfo(CURRENT_STREAM, videoInfo);

  auto& dataCacheCore = CServiceBroker::GetDataCacheCore();
  const StreamHdrType sourceHdrType = dataCacheCore.GetVideoSourceHdrType();
  const StreamHdrType additionalHdrType = dataCacheCore.GetVideoSourceAdditionalHdrType();
  if (videoInfo.hdrType == StreamHdrType::HDR_TYPE_NONE)
    videoInfo.hdrType = sourceHdrType;
  else if (sourceHdrType == StreamHdrType::HDR_TYPE_HDR10PLUS &&
           videoInfo.hdrType != StreamHdrType::HDR_TYPE_HDR10PLUS)
  {
    if (videoInfo.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
      videoInfo.hdrTypeAlt = StreamHdrType::HDR_TYPE_HDR10PLUS;
    else
      videoInfo.hdrType = StreamHdrType::HDR_TYPE_HDR10PLUS;
  }
  if (additionalHdrType != StreamHdrType::HDR_TYPE_NONE && additionalHdrType != videoInfo.hdrType)
    videoInfo.hdrTypeAlt = additionalHdrType;

  const CStreamDetails previous = info->m_streamDetails;
  const bool previousIsSameVideo = (previous.GetVideoCodec(1) == videoInfo.codecName &&
                                    previous.GetVideoWidth(1) == videoInfo.width);
  if (previousIsSameVideo && videoInfo.hdrType != StreamHdrType::HDR_TYPE_NONE)
  {
    if (videoInfo.dvProfile.empty())
      videoInfo.dvProfile = previous.GetVideoDvProfile();
    if (videoInfo.hdrTypeAlt == StreamHdrType::HDR_TYPE_NONE)
      videoInfo.hdrTypeAlt = CStreamDetails::StringToHdrType(previous.GetVideoHdrTypeAlt());
  }
  if (videoInfo.hdrTypeAlt == videoInfo.hdrType)
    videoInfo.hdrTypeAlt = StreamHdrType::HDR_TYPE_NONE;

  info->m_streamDetails.SetStreams(videoInfo, m_processInfo->GetMaxTime() / 1000, audioInfo,
                                   subtitleInfo);

  //grab all the audio and subtitle info and save it

  for (int i = 0; i < GetAudioStreamCount(); i++)
  {
    GetAudioStreamInfo(i, audioInfo);
    audioInfo.objects = (i == GetAudioStream()) ? m_processInfo->GetAudioObjectCount() : -1;
    audioInfo.objectChannels =
        (i == GetAudioStream()) ? m_processInfo->GetAudioObjectChannels() : -1;
    audioInfo.bedChannels = (i == GetAudioStream()) ? m_processInfo->GetAudioBedChannels() : -1;
    audioInfo.profile = GetObjectAudioProfile(audioInfo.codecName);
    const bool matchesPrevious =
        previous.GetAudioCodec(i + 1) == StreamUtils::GetCanonicalCodecName(audioInfo.codecName) &&
        previous.GetAudioChannels(i + 1) == audioInfo.channels;
    if (audioInfo.profile.empty() && matchesPrevious)
      audioInfo.profile = previous.GetAudioProfile(i + 1);
    if (audioInfo.objects < 0 && matchesPrevious)
      audioInfo.objects = previous.GetAudioObjects(i + 1);
    if (audioInfo.objectChannels < 0 && matchesPrevious)
      audioInfo.objectChannels = previous.GetAudioObjectChannels(i + 1);
    if (audioInfo.bedChannels < 0 && matchesPrevious)
      audioInfo.bedChannels = previous.GetAudioBedChannels(i + 1);
    info->m_streamDetails.AddStream(new CStreamDetailAudio(audioInfo));
  }

  for (int i = 0; i < GetSubtitleCount(); i++)
  {
    GetSubtitleStreamInfo(i, subtitleInfo);
    info->m_streamDetails.AddStream(new CStreamDetailSubtitle(subtitleInfo));
  }
}

//------------------------------------------------------------------------------
// content related methods
//------------------------------------------------------------------------------

void CVideoPlayer::UpdateContent()
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);
  m_content.m_selectionStreams = m_SelectionStreams;
  m_content.m_programs = m_programs;
}

void CVideoPlayer::UpdateContentState()
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);

  m_content.m_videoIndex = m_SelectionStreams.TypeIndexOf(STREAM_VIDEO, m_CurrentVideo.source,
                                                      m_CurrentVideo.demuxerId, m_CurrentVideo.id);
  m_content.m_audioIndex = m_SelectionStreams.TypeIndexOf(STREAM_AUDIO, m_CurrentAudio.source,
                                                      m_CurrentAudio.demuxerId, m_CurrentAudio.id);
  m_content.m_subtitleIndex = m_SelectionStreams.TypeIndexOf(STREAM_SUBTITLE, m_CurrentSubtitle.source,
                                                         m_CurrentSubtitle.demuxerId, m_CurrentSubtitle.id);

  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD) && m_content.m_videoIndex == -1 &&
      m_content.m_audioIndex == -1)
  {
    std::shared_ptr<CDVDInputStreamNavigator> nav =
          std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);

    m_content.m_videoIndex = m_SelectionStreams.TypeIndexOf(STREAM_VIDEO, STREAM_SOURCE_NAV, -1,
                                                            nav->GetActiveAngle());
    m_content.m_audioIndex = m_SelectionStreams.TypeIndexOf(STREAM_AUDIO, STREAM_SOURCE_NAV, -1,
                                                            nav->GetActiveAudioStream());

    // only update the subtitle index in libdvdnav if the subtitle is provided by the dvd itself,
    // i.e. for external subtitles the index is always greater than the subtitlecount in dvdnav
    if (m_content.m_subtitleIndex < nav->GetSubTitleStreamCount())
    {
      m_content.m_subtitleIndex = m_SelectionStreams.TypeIndexOf(
          STREAM_SUBTITLE, STREAM_SOURCE_NAV, -1, nav->GetActiveSubtitleStream());
    }
  }

  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) && m_State.menuType == MenuType::NATIVE)
  {
    // Update settings with changes made in bluray menu
    CVideoSettings settings{m_processInfo->GetVideoSettings()};
    settings.m_AudioStream = m_content.m_audioIndex;
    settings.m_SubtitleStream = m_content.m_subtitleIndex;
    m_processInfo->SetVideoSettings(settings);
  }
}

void CVideoPlayer::GetVideoStreamInfo(int streamId, VideoStreamInfo& info) const
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);

  if (streamId == CURRENT_STREAM)
    streamId = m_content.m_videoIndex;

  if (streamId < 0 || streamId > GetVideoStreamCount() - 1)
  {
    info.valid = false;
    return;
  }

  const SelectionStream& s = m_content.m_selectionStreams.Get(STREAM_VIDEO, streamId);
  if (s.language.length() > 0)
    info.language = s.language;

  if (s.name.length() > 0)
    info.name = s.name;

  m_renderManager.GetVideoRect(info.SrcRect, info.DestRect, info.VideoRect);

  info.valid = true;
  info.bitrate = s.bitrate;
  info.width = s.width;
  info.height = s.height;
  info.codecName = s.codec;
  info.videoAspectRatio = s.aspect_ratio;
  info.stereoMode = s.stereo_mode;
  info.flags = s.flags;
  info.hdrType = s.hdrType;
  if (info.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION && s.dovi.dv_profile != 0)
  {
    info.hdrDetail = std::to_string(static_cast<int>(s.dovi.dv_profile));
    if (s.dovi.dv_profile == 8)
    {
      info.hdrDetail +=
          "." + std::to_string(static_cast<int>(s.dovi.dv_bl_signal_compatibility_id));
      if (s.dovi.dv_bl_signal_compatibility_id == 4)
        info.hdrTypeAlt = StreamHdrType::HDR_TYPE_HLG;
    }
  }
  else
  {
    info.hdrDetail.clear();
    info.hdrTypeAlt = StreamHdrType::HDR_TYPE_NONE;
  }
  info.dvProfile = GetDvProfileString(s.dovi);
  info.fpsRate = s.fpsRate;
  info.fpsScale = s.fpsScale;
}

int CVideoPlayer::GetVideoStreamCount() const
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);
  return m_content.m_selectionStreams.CountType(STREAM_VIDEO);
}

int CVideoPlayer::GetVideoStream() const
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);
  return m_content.m_videoIndex;
}

void CVideoPlayer::SetVideoStream(int iStream)
{
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSetVideoStream>(iStream));
  m_processInfo->GetVideoSettingsLocked().SetVideoStream(iStream);
  SynchronizeDemuxer();
}

void CVideoPlayer::GetAudioStreamInfo(int index, AudioStreamInfo& info) const
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);

  if (index == CURRENT_STREAM)
    index = m_content.m_audioIndex;

  if (index < 0 || index > GetAudioStreamCount() - 1)
  {
    info.valid = false;
    return;
  }

  const SelectionStream& s = m_content.m_selectionStreams.Get(STREAM_AUDIO, index);
  info.language = s.language;
  info.name = s.name;

  if (s.type == STREAM_NONE)
    info.name += " (Invalid)";

  info.valid = true;
  info.bitrate = s.bitrate;
  info.channels = s.channels;
  info.codecName = s.codec;
  info.codecDesc = s.codecDesc;
  info.flags = s.flags;
}

int CVideoPlayer::GetAudioStreamCount() const
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);
  return m_content.m_selectionStreams.CountType(STREAM_AUDIO);
}

int CVideoPlayer::GetAudioStream()
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);
  return m_content.m_audioIndex;
}

void CVideoPlayer::SetAudioStream(int iStream)
{
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSetAudioStream>(iStream));
  m_processInfo->GetVideoSettingsLocked().SetAudioStream(iStream);
  SynchronizeDemuxer();
}

void CVideoPlayer::GetSubtitleStreamInfo(int index, SubtitleStreamInfo& info) const
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);

  if (index == CURRENT_STREAM)
    index = m_content.m_subtitleIndex;

  if (index < 0 || index > GetSubtitleCount() - 1)
  {
    info.valid = false;
    info.language.clear();
    info.flags = StreamFlags::FLAG_NONE;
    return;
  }

  const SelectionStream& s = m_content.m_selectionStreams.Get(STREAM_SUBTITLE, index);
  info.name = s.name;

  if (s.type == STREAM_NONE)
    info.name += "(Invalid)";

  info.language = s.language;
  info.codecName = s.codec;
  info.flags = s.flags;
  info.isExternal = STREAM_SOURCE_MASK(s.source) == STREAM_SOURCE_DEMUX_SUB ||
  STREAM_SOURCE_MASK(s.source) == STREAM_SOURCE_TEXT;
}

void CVideoPlayer::SetSubtitle(int iStream)
{
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSetSubtitleStream>(iStream));
  m_processInfo->GetVideoSettingsLocked().SetSubtitleStream(iStream);
}

int CVideoPlayer::GetSubtitleCount() const
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);
  return m_content.m_selectionStreams.CountType(STREAM_SUBTITLE);
}

int CVideoPlayer::GetSubtitle()
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);
  return m_content.m_subtitleIndex;
}

int CVideoPlayer::GetPrograms(std::vector<ProgramInfo>& programs)
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);
  programs = m_programs;
  return programs.size();
}

void CVideoPlayer::SetProgram(int progId)
{
  m_messenger.Put(std::make_shared<CDVDMsgInt>(CDVDMsg::PLAYER_SET_PROGRAM, progId));
}

int CVideoPlayer::GetProgramsCount() const
{
  std::unique_lock<CCriticalSection> lock(m_content.m_section);
  return m_programs.size();
}

void CVideoPlayer::SetUpdateStreamDetails()
{
  m_messenger.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_SET_UPDATE_STREAM_DETAILS));
}
