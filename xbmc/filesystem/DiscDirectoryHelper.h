/*
 *  Copyright (C) 2005-2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "Directory.h"
#include "video/VideoInfoTag.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class CFileItem;
class CFileItemList;
class CURL;
class CVideoInfoTag;

namespace XFILE
{
using namespace std::chrono_literals;

static constexpr int ALL_PLAYLISTS{-1};

enum class GetTitle : uint8_t
{
  SINGLE,
  MAIN,
  EPISODES,
  ALL
};

enum class SortTitles : uint8_t
{
  SORT_TITLES_NONE,
  SORT_TITLES_EPISODE,
  SORT_TITLES_MOVIE
};

enum class AddMenuAndAllTitlesOptions : uint8_t
{
  NONE = 0x00,
  ADD_MENU = 0x01,
  ADD_ALL_TITLES = 0x02
};

constexpr AddMenuAndAllTitlesOptions operator|(AddMenuAndAllTitlesOptions lhs,
                                               AddMenuAndAllTitlesOptions rhs)
{
  return static_cast<AddMenuAndAllTitlesOptions>(static_cast<uint8_t>(lhs) |
                                                 static_cast<uint8_t>(rhs));
}

constexpr bool operator&(AddMenuAndAllTitlesOptions lhs, AddMenuAndAllTitlesOptions rhs)
{
  return (static_cast<uint8_t>(lhs) & static_cast<uint8_t>(rhs)) != 0;
}

enum class ENCODING_TYPE : uint8_t
{
  // Video
  VIDEO_MPEG2 = 0x02,
  VIDEO_VC1 = 0xea,
  VIDEO_H264 = 0x1b,
  VIDEO_H264_MVC = 0x20,
  VIDEO_HEVC = 0x24,

  // Audio
  AUDIO_LPCM = 0x80,
  AUDIO_AC3 = 0x81,
  AUDIO_DTS = 0x82,
  AUDIO_TRUHD = 0x83,
  AUDIO_AC3PLUS = 0x84,
  AUDIO_DTSHD = 0x85,
  AUDIO_DTSHD_MASTER = 0x86,
  AUDIO_AC3PLUS_SECONDARY = 0xa1,
  AUDIO_DTSHD_SECONDARY = 0xa2,

  // Other
  SUB_PG = 0x90,
  SUB_IG = 0x91,
  SUB_TEXT = 0x92,
};

enum class ASPECT_RATIO : uint8_t
{
  RATIO_4_3 = 2,
  RATIO_16_9 = 3
};

struct PlaylistInformation
{
  unsigned int playlist{0};
  std::chrono::milliseconds duration{0ms};
  std::vector<unsigned int> clips;
  std::map<unsigned int, std::chrono::milliseconds> clipDuration;
  std::vector<std::chrono::milliseconds> chapters;
  std::vector<VideoStreamInfo> videoStreams;
  std::vector<AudioStreamInfo> audioStreams;
  std::vector<SubtitleStreamInfo> pgStreams;
  std::string languages;

  //! Whether the playlist carries a secondary video stream, ie. it presents the content
  //! picture-in-picture (see IsPictureInPicturePresentation)
  bool hasSecondaryVideo{false};

  void clear()
  {
    playlist = 0;
    duration = 0ms;
    clips.clear();
    clipDuration.clear();
    chapters.clear();
    videoStreams.clear();
    audioStreams.clear();
    pgStreams.clear();
    languages.clear();
    hasSecondaryVideo = false;
  }
};

struct ClipInfo
{
  std::chrono::milliseconds duration{0ms};
  std::vector<unsigned int> playlists;
};

using PlaylistMap = std::map<unsigned int, PlaylistInformation>;
using ClipMap = std::map<unsigned int, ClipInfo>;
// Movies
static constexpr std::chrono::milliseconds MIN_MOVIE_DURATION{30 * 60 * 1000}; // 30 minutes
static constexpr int MAIN_TITLE_LENGTH_PERCENT{70};
// A playlist offering the movie as fully as the longest one is accepted as another edition of it
// even when shorter than MAIN_TITLE_LENGTH_PERCENT. Editions do differ considerably in length (eg.
// Das Boot (1981), whose theatrical cut is barely half the length of the television one), but an
// extra offering the same languages and resolution as the movie is not an edition of it, however
// long it runs (eg. Fast X (2023), whose 35 minutes of deleted scenes accompany a 2 hour movie).
static constexpr int MIN_EDITION_LENGTH_PERCENT{40};
// Playlists within this of each other are the same movie presented differently rather than
// separate editions of it, which differ by minutes rather than seconds (eg. Snow White (2025),
// whose sing along wraps the movie in a one second bumper at each end)
static constexpr std::chrono::milliseconds MOVIE_EQUAL_LENGTH_TOLERANCE{10 * 1000}; // 10 seconds

/*!
 \brief Populates the stream details of item for the given title on the disc.
 Supplied by the disc's directory implementation.
 */
using StreamDetailsProvider = std::function<void(unsigned int title, CFileItem& item)>;

class CDiscDirectoryHelper
{
public:
  /*!
   * \brief Construct a helper that can describe the streams of the titles it returns.
   * \param getStreamDetails supplied by the disc's directory implementation. When empty the
   *        returned items carry no stream details.
   */
  explicit CDiscDirectoryHelper(StreamDetailsProvider getStreamDetails);

  CDiscDirectoryHelper(const CDiscDirectoryHelper&) = delete;
  CDiscDirectoryHelper& operator=(const CDiscDirectoryHelper&) = delete;

  /*!
   * \brief Populates a CFileItemList with the playlist(s) corresponding to the main movie.
   * \param url bluray:// url
   * \param items CFileItemList to populate
   * \param allTitles CFileItemList of all titles on the disc (populated by CBlurayDirectory). Used for streamdetails.
   * \param mainPlaylist the main playlist number (if known, from disc.inf), otherwise -1
   * \param job determines whether to get all possible movie playlists (ie. multiple versions) or just one (for initial library scan)
   * \param clips map of clips on disc (populated in CBlurayDirectory)
   * \param playlistMap map of playlists on disc (populated in CBlurayDirectory)
   * \return true if at least one playlist is found, otherwise false
   */
  bool GetMoviePlaylists(const CURL& url,
                         CFileItemList& items,
                         const CFileItemList& allTitles,
                         int mainPlaylist,
                         GetTitle job,
                         const ClipMap& clips,
                         const PlaylistMap& playlistMap);

  enum class AllTitles : bool
  {
    EPISODES,
    MOVIES
  };

  /*!
   * \brief Add All Titles and, if appropriate, Menu options to the CFileItemList
   * \param url bluray:// episode url
   * \param items CFileItemList to populate
   * \param allTitlesType Determines whether to add All Episodes or All Movies option
   * \param addMenuAndAllTitlesOptions whether to add Disc Menu and All Titles options
   */
  static void AddRootOptions(const CURL& url,
                             CFileItemList& items,
                             AllTitles allTitlesType,
                             AddMenuAndAllTitlesOptions addMenuAndAllTitlesOptions);

private:
  void Reset();
  //! Describes the streams of a title, supplied by the disc's directory implementation
  StreamDetailsProvider m_getStreamDetails;

};
} // namespace XFILE
