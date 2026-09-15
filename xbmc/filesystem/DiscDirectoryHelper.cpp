/*
 *  Copyright (C) 2005-2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#include "DiscDirectoryHelper.h"

#include "FileItem.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "guilib/LocalizeStrings.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/RegExp.h"
#include "utils/StreamUtils.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

using namespace XFILE;
using namespace std::chrono_literals;

using PlaylistMapEntry = std::pair<unsigned int, PlaylistInformation>;
using PlaylistVector = std::vector<std::pair<unsigned int, PlaylistInformation>>;
using PlaylistVectorEntry = std::pair<unsigned int, PlaylistInformation>;


void CDiscDirectoryHelper::Reset()
{
}

CDiscDirectoryHelper::CDiscDirectoryHelper(StreamDetailsProvider getStreamDetails)
  : m_getStreamDetails(std::move(getStreamDetails))
{
}

namespace
{
std::string_view GetTitlesJobDescription(GetTitle job)
{
  switch (job)
  {
    case GetTitle::SINGLE:
      return "single playlist";
    case GetTitle::MAIN:
      return "main playlist(s)";
    case GetTitle::EPISODES:
      return "episode playlists";
    case GetTitle::ALL:
      return "all playlists";
  }
  return "unknown";
}

void LogMoviePlaylist(std::string_view prefix, const PlaylistInformation& playlist)
{
  CLog::LogF(
      LOGDEBUG, "{} playlist {}, duration {}, chapters {}, clips {}, langs {}, subs {}", prefix,
      playlist.playlist,
      StringUtils::SecondsToTimeString(static_cast<int>(
          std::chrono::duration_cast<std::chrono::seconds>(playlist.duration).count())),
      playlist.chapters.size(), playlist.clips.size(), playlist.languages,
      fmt::join(playlist.pgStreams |
                    std::views::transform([](const auto& stream) { return stream.language; }),
                ","));
}

void LogMoviePlaylists(std::string_view prefix, const std::vector<PlaylistInformation>& playlists)
{
  if (playlists.empty())
  {
    CLog::LogF(LOGDEBUG, "{} none", prefix);
    return;
  }
  for (const auto& playlist : playlists)
    LogMoviePlaylist(prefix, playlist);
}

/*!
 * \brief Removes the playlists matching shouldRemove, logging each removal and the reason for it.
 */
template<typename Predicate>
void RemovePlaylists(std::vector<PlaylistInformation>& playlists,
                     std::string_view reason,
                     Predicate shouldRemove)
{
  for (const auto& playlist : playlists | std::views::filter(shouldRemove))
    LogMoviePlaylist(fmt::format("Rejected ({}) -", reason), playlist);

  std::erase_if(playlists, shouldRemove);
}

void InitialiseMoviePlaylistSearch(std::vector<PlaylistInformation>& playlists,
                                   const PlaylistMap& playlistMap,
                                   GetTitle job,
                                   int mainPlaylist)
{
  playlists.reserve(playlistMap.size());
  std::ranges::transform(playlistMap | std::views::values, std::back_inserter(playlists),
                         [](const PlaylistInformation& information) { return information; });

  CLog::LogF(LOGDEBUG, "*** Movie Search Start ***");
  CLog::LogF(LOGDEBUG, "Looking for {} - main playlist {}", GetTitlesJobDescription(job),
             mainPlaylist >= 0 ? std::to_string(mainPlaylist) : "unknown");
  LogMoviePlaylists("Candidate -", playlists);
}

//! \brief The sorted durations of a playlist's clips, or none at all if any of them is unknown.
std::vector<std::chrono::milliseconds> GetSortedClipDurations(const PlaylistInformation& playlist,
                                                              const ClipMap& clips)
{
  std::vector<std::chrono::milliseconds> durations;
  durations.reserve(playlist.clips.size());
  for (const unsigned int clip : playlist.clips)
  {
    const auto it{clips.find(clip)};
    if (it == clips.end() || it->second.duration <= 0ms)
      return {};
    durations.emplace_back(it->second.duration);
  }
  std::ranges::sort(durations);
  return durations;
}

/*!
 * \brief Whether two playlists present the same content, whatever streams they expose.
 *
 * Playlists assembling the same content do not have to reference the same clips in the same order.
 * Discs hide the movie among copies of it, either by giving each copy its own copies of the short
 * clips joining the movie's longer ones (eg. Avatar (2009), whose playlists 800 and 801 share the
 * movie's clips but each use their own copies of the clips joining them), or by playing the same
 * clips in a different order (eg. John Wick: Chapter 3 - Parabellum (2019), whose 385 copies of the
 * movie are 385 orderings of the same 19 clips). Playlists of the same overall length, cut into the
 * same number of chapters and into clips of the same durations, therefore present the same content
 * however they reference and order those clips - provided they have a clip in common, without which
 * unrelated titles that happen to run the same length would be taken for copies of one another.
 */
bool IsSamePresentation(const PlaylistInformation& a,
                        const std::vector<std::chrono::milliseconds>& aClipDurations,
                        const PlaylistInformation& b,
                        const std::vector<std::chrono::milliseconds>& bClipDurations)
{
  if (a.duration != b.duration)
    return false;
  if (a.clips == b.clips)
    return true;
  if (aClipDurations.size() < 2 || a.chapters.size() != b.chapters.size() ||
      aClipDurations != bClipDurations)
    return false;

  const std::set<unsigned int> aClips{a.clips.begin(), a.clips.end()};
  return std::ranges::any_of(b.clips,
                             [&aClips](unsigned int clip) { return aClips.contains(clip); });
}

/*!
 * \brief Whether the playlist presents the content picture-in-picture.
 *
 * \note Kodi does not play the secondary video stream.
 */
bool IsPictureInPicturePresentation(const PlaylistInformation& playlist)
{
  return playlist.hasSecondaryVideo;
}

/*!
 * \brief The playlist presenting the movie most fully of those of (near) the longest length.
 *
 * Playlists within MOVIE_EQUAL_LENGTH_TOLERANCE of one another are the same movie presented
 * differently rather than separate editions of it, so the longest is not necessarily the best -
 * a sing along wrapping the movie in a bumper runs a second or two longer while offering fewer
 * streams. Only the streams are compared, as holding the same content in more chapters makes a
 * playlist no fuller a presentation of it.
 */
const PlaylistInformation& GetBestMoviePlaylist(const std::vector<PlaylistInformation>& playlists)
{
  const auto offersMoreStreams{[](const PlaylistInformation& a, const PlaylistInformation& b)
                               {
                                 if (a.audioStreams.size() != b.audioStreams.size())
                                   return a.audioStreams.size() > b.audioStreams.size();
                                 return a.pgStreams.size() > b.pgStreams.size();
                               }};

  // A picture-in-picture presentation is only the best a disc offers when it is all it offers
  const bool anyFeature{
      std::ranges::any_of(playlists, std::not_fn(IsPictureInPicturePresentation))};
  auto candidates{
      std::views::filter(playlists, [anyFeature](const PlaylistInformation& playlist)
                         { return !anyFeature || !IsPictureInPicturePresentation(playlist); })};

  const auto longest{std::ranges::max_element(candidates, {}, &PlaylistInformation::duration)};
  const PlaylistInformation* best{&*longest};
  for (const auto& playlist : candidates)
  {
    if (std::chrono::abs(playlist.duration - longest->duration) <= MOVIE_EQUAL_LENGTH_TOLERANCE &&
        offersMoreStreams(playlist, *best))
      best = &playlist;
  }
  return *best;
}

bool IsRicherPresentation(const PlaylistInformation& a, const PlaylistInformation& b)
{
  if (a.audioStreams.size() != b.audioStreams.size())
    return a.audioStreams.size() > b.audioStreams.size();
  if (a.pgStreams.size() != b.pgStreams.size())
    return a.pgStreams.size() > b.pgStreams.size();
  if (a.chapters.size() != b.chapters.size())
    return a.chapters.size() > b.chapters.size();
  return a.playlist < b.playlist;
}

/*!
 * \brief Discards the copies a disc holds of the same presentation, keeping the fullest of each.
 *
 * Discs present the same content through several playlists both to expose different sets of streams
 * (eg. 28 Days Later) and to hide the movie among copies of itself (eg. John Wick: Chapter 3 -
 * Parabellum (2019), which holds 385 copies). Only one of each set of copies is a candidate.
 *
 * The playlist the disc names as the main one (from disc.inf) is always the copy kept, as the rest
 * of the search identifies the movie by it.
 */
void RemoveDuplicateMoviePlaylists(std::vector<PlaylistInformation>& playlists,
                                   const ClipMap& clips,
                                   GetTitle job,
                                   int mainPlaylist)
{
  if (job == GetTitle::ALL || playlists.size() < 2)
    return;

  // The clip durations are gathered up front, as a disc can hold hundreds of copies of the movie
  std::vector<std::vector<std::chrono::milliseconds>> clipDurations;
  clipDurations.reserve(playlists.size());
  std::ranges::transform(playlists, std::back_inserter(clipDurations),
                         [&clips](const PlaylistInformation& playlist)
                         { return GetSortedClipDurations(playlist, clips); });

  std::unordered_set<unsigned int> duplicatePlaylists;
  for (size_t i = 0; i + 1 < playlists.size(); ++i)
  {
    if (duplicatePlaylists.contains(playlists[i].playlist))
      continue;

    for (size_t j = i + 1; j < playlists.size(); ++j)
    {
      if (duplicatePlaylists.contains(playlists[j].playlist) ||
          !IsSamePresentation(playlists[i], clipDurations[i], playlists[j], clipDurations[j]))
        continue;

      // The main playlist (from disc.inf) is the copy kept, however fully the other presents the
      // content
      const bool firstIsMain{std::cmp_equal(playlists[i].playlist, mainPlaylist)};
      const bool secondIsMain{std::cmp_equal(playlists[j].playlist, mainPlaylist)};
      const bool keepFirst{firstIsMain ||
                           (!secondIsMain && IsRicherPresentation(playlists[i], playlists[j]))};
      const PlaylistInformation& duplicate{keepFirst ? playlists[j] : playlists[i]};
      const PlaylistInformation& kept{keepFirst ? playlists[i] : playlists[j]};
      const bool keptIsMain{keepFirst ? firstIsMain : secondIsMain};
      LogMoviePlaylist(keptIsMain
                           ? fmt::format("Rejected (main playlist {} presents the same content) -",
                                         kept.playlist)
                           : fmt::format(
                                 "Rejected (playlist {} presents the same content as fully) -",
                                 kept.playlist),
                       duplicate);
      duplicatePlaylists.emplace(duplicate.playlist);

      if (!keepFirst)
        break; // playlists[i] is itself a duplicate, so cannot stand in for the ones after it
    }
  }

  std::erase_if(playlists, [&duplicatePlaylists](const PlaylistInformation& playlist)
                { return duplicatePlaylists.contains(playlist.playlist); });
}

bool FilterMoviePlaylists(std::vector<PlaylistInformation>& playlists, GetTitle job)
{
  if (job != GetTitle::ALL)
  {
    // Remove all playlists less than MIN_MOVIE_DURATION
    RemovePlaylists(playlists, "shorter than minimum movie duration",
                    [](const PlaylistInformation& playlist)
                    { return playlist.duration < MIN_MOVIE_DURATION; });
  }
  if (playlists.empty())
    CLog::LogF(LOGDEBUG, "No playlists of at least minimum movie duration");

  return !playlists.empty();
}

//! \brief The playlist's video resolution, or 0 if not known.
int GetPlaylistHeight(const PlaylistInformation& playlist)
{
  if (playlist.videoStreams.empty())
    return 0;
  return std::ranges::max(playlist.videoStreams | std::views::transform(&VideoStreamInfo::height));
}

//!  \brief The audio languages the playlist offers, empty if not known.
std::set<std::string> GetPlaylistLanguages(const PlaylistInformation& playlist)
{
  if (playlist.languages.empty())
    return {};
  const std::vector<std::string> languages{StringUtils::Split(playlist.languages, ",")};
  return {languages.begin(), languages.end()};
}

/*!
 * \brief Discards playlists of a lower resolution than another candidate.
 *
 * The movie is always presented at the disc's highest resolution, as is every version of it,
 * whereas extras are often standard definition.
 *
 * Playlists without video stream information are neither discarded nor
 * used for comparison.
 */
void FilterMoviePlaylistsByResolution(std::vector<PlaylistInformation>& playlists,
                                      GetTitle job,
                                      int mainPlaylist)
{
  if (job == GetTitle::ALL || playlists.size() < 2)
    return;

  std::map<unsigned int, int> heights;
  for (const auto& playlist : playlists)
  {
    const int height{GetPlaylistHeight(playlist)};
    if (height > 0)
      heights[playlist.playlist] = height;
  }

  if (heights.empty())
    return;

  const int highest{std::ranges::max(heights | std::views::values)};
  CLog::LogF(LOGDEBUG, "Highest resolution of any playlist is {}", highest);

  RemovePlaylists(playlists, "lower resolution than another playlist",
                  [&heights, highest, mainPlaylist](const PlaylistInformation& playlist)
                  {
                    // Remove if not main playlist (from disc.inf) and lower resolution than another playlist
                    const auto it{heights.find(playlist.playlist)};
                    return it != heights.end() && it->second < highest &&
                           std::cmp_not_equal(playlist.playlist, mainPlaylist);
                  });
}

void GetMainMoviePlaylists(std::vector<PlaylistInformation>& playlists,
                           GetTitle job,
                           int mainPlaylist)
{
  // If any playlists have more than one chapter, discard those without
  if (job != GetTitle::ALL && std::ranges::any_of(playlists, [](const PlaylistInformation& p)
                                                  { return p.chapters.size() > 1; }))
  {
    RemovePlaylists(playlists, "single chapter",
                    [mainPlaylist](const PlaylistInformation& p) {
                      return p.chapters.size() <= 1 && std::cmp_not_equal(p.playlist, mainPlaylist);
                    });
  }

  if (playlists.empty())
    return;

  if (job == GetTitle::SINGLE && mainPlaylist >= 0)
  {
    const auto it{std::ranges::find(playlists, mainPlaylist, &PlaylistInformation::playlist)};
    if (it != playlists.end())
    {
      CLog::LogF(LOGDEBUG, "Using playlist {} from disc information (disc.inf)", mainPlaylist);
      playlists = {std::move(*it)};
      return;
    }
  }

  const auto it{std::ranges::max_element(playlists, {}, &PlaylistInformation::duration)};
  if (job == GetTitle::SINGLE)
  {
    // Single longest playlist. Where playlists are of (near) identical length they are the same
    // movie presented differently rather than another edition of it (eg. a sing along, which wraps
    // the movie in a bumper and drops the other audio tracks), so the fullest presentation is used
    const PlaylistInformation& best{GetBestMoviePlaylist(playlists)};
    LogMoviePlaylist(&best == &*it ? "Using longest -" : "Using fullest of the longest -", best);
    playlists = {best};
    return;
  }

  if (job == GetTitle::MAIN)
  {
    // All playlists with duration of at least 70% of the longest title (to allow multiple editions
    // on same disc), plus any playlist presenting the movie as fully (resolution, languages) as
    // the longest one does
    const auto minimumDuration{it->duration * MAIN_TITLE_LENGTH_PERCENT / 100};
    const auto minimumEditionDuration{it->duration * MIN_EDITION_LENGTH_PERCENT / 100};
    const std::set<std::string> longestLanguages{GetPlaylistLanguages(*it)};
    const int longestHeight{GetPlaylistHeight(*it)};
    LogMoviePlaylist("Longest -", *it);
    CLog::LogF(LOGDEBUG, "Accepting playlists of at least {}% of the longest ({})",
               MAIN_TITLE_LENGTH_PERCENT,
               StringUtils::SecondsToTimeString(static_cast<int>(
                   std::chrono::duration_cast<std::chrono::seconds>(minimumDuration).count())));
    RemovePlaylists(
        playlists, "shorter than minimum percentage of longest playlist",
        [minimumDuration, minimumEditionDuration, &longestLanguages, longestHeight,
         mainPlaylist](const PlaylistInformation& playlist)
        {
          if (playlist.duration >= minimumDuration ||
              std::cmp_equal(playlist.playlist, mainPlaylist))
            return false;

          // Another edition of the movie can be considerably shorter than the longest one (eg. a
          // theatrical cut against an extended one), but offers at least the same languages and
          // resolution. An edition may have more languages than a longer one (more dubs having
          // been made of the theatrical release), so an exact match isn't required
          if (longestLanguages.empty() || playlist.duration < minimumEditionDuration)
            return true;

          const std::set<std::string> languages{GetPlaylistLanguages(playlist)};
          const int height{GetPlaylistHeight(playlist)};
          return !std::ranges::includes(languages, longestLanguages) ||
                 (height > 0 && longestHeight > 0 && height < longestHeight);
        });
  }
}

void EndMoviePlaylistSearch(const std::vector<PlaylistInformation>& playlists)
{
  LogMoviePlaylists("Selected -", playlists);
  CLog::LogF(LOGDEBUG, "*** Movie Search End ***");
}

/*!
 * \brief The languages of the streams the disc expects a player to start with.
 *
 * The audio and presentation graphic streams the playlist flags as its defaults - audio stream
 * number 1 and presentation graphic stream number 1 of its longest play item - shown as
 * "eng | jpn". A playlist that flags no default, or whose default names no language,
 * contributes nothing.
 *
 * The flag is read rather than the first stream taken, so that this agrees with the stream
 * VideoPlayer is told is the default even where the two would otherwise differ - a playlist
 * described from its clip rather than its play item flags no default at all.
 */
std::string GetDefaultStreamLanguages(const PlaylistInformation& information)
{
  const auto isDefault{[](const auto& stream)
                       { return (stream.flags & StreamFlags::FLAG_DEFAULT) != 0; }};

  std::vector<std::string> languages;
  if (const auto audio{std::ranges::find_if(information.audioStreams, isDefault)};
      audio != information.audioStreams.cend() && !audio->language.empty())
    languages.emplace_back(audio->language);

  if (const auto pg{std::ranges::find_if(information.pgStreams, isDefault)};
      pg != information.pgStreams.cend() && !pg->language.empty())
    languages.emplace_back(pg->language);

  return StringUtils::Join(languages, " | ");
}

std::shared_ptr<CFileItem> GenerateMovieItem(const CURL& url,
                                             unsigned int playlist,
                                             unsigned int mainPlaylist,
                                             const PlaylistInformation& information)
{
  CURL path{url};
  std::string buf{fmt::format("BDMV/PLAYLIST/{:05}.mpls", playlist)};
  path.SetFileName(buf);
  const auto item{std::make_shared<CFileItem>(path.Get(), false)};

  // Get clips
  const std::chrono::milliseconds duration{information.duration};

  CVideoInfoTag* itemTag{item->GetVideoInfoTag()};
  itemTag->SetDuration(static_cast<int>(duration.count() / 1000));
  item->SetProperty("bluray_playlist", playlist);

  buf = fmt::format(fmt::runtime(playlist == mainPlaylist
                                     ? g_localizeStrings.Get(25004) /* Main Title */
                                     : g_localizeStrings.Get(25005) /* Title */),
                    playlist);
  item->m_strTitle = buf;
  item->SetLabel(buf);

  std::string label2{
      fmt::format(fmt::runtime(g_localizeStrings.Get(25007)), information.chapters.size(),
                  StringUtils::SecondsToTimeString(static_cast<int>(duration.count() / 1000)))};

  // The streams a playlist starts on are what tells playlists offering the same content apart
  if (const std::string languages{GetDefaultStreamLanguages(information)}; !languages.empty())
    label2 += " | " + languages;

  item->SetLabel2(label2);

  item->m_dwSize = 0;
  item->SetArt("icon", "DefaultVideo.png");

  return item;
}

void AddStreamDetails(const StreamDetailsProvider& getStreamDetails,
                      const CFileItemList& allTitles,
                      unsigned int playlist,
                      CFileItem& item)
{
  if (!getStreamDetails)
    return;

  if (!allTitles.Contains(item.GetPath()))
  {
    CLog::LogF(LOGDEBUG, "Playlist {} not found in disc titles", playlist);
    return;
  }

  getStreamDetails(playlist, item);
}

void PopulateMovieFileItems(
    const CURL& url,
    CFileItemList& items,
    int mainPlaylist,
    const CFileItemList& allTitles, // FileItem for each playlist on the disc (no stream details)
    const std::vector<PlaylistInformation>& playlists,
    const StreamDetailsProvider& getStreamDetails)
{
  // Sort by duration (putting mainPlaylist first if present, and any picture-in-picture
  // presentation last however long it runs)
  auto sortedPlaylists = playlists;
  std::ranges::sort(sortedPlaylists,
                    [mainPlaylist](const PlaylistInformation& a, const PlaylistInformation& b)
                    {
                      const bool aIsMain{std::cmp_equal(a.playlist, mainPlaylist)};
                      const bool bIsMain{std::cmp_equal(b.playlist, mainPlaylist)};

                      if (aIsMain || bIsMain)
                        return aIsMain && !bIsMain;

                      const bool aIsPiP{IsPictureInPicturePresentation(a)};
                      const bool bIsPiP{IsPictureInPicturePresentation(b)};
                      if (aIsPiP != bIsPiP)
                        return bIsPiP;

                      if (a.duration != b.duration)
                        return a.duration > b.duration;

                      return a.playlist < b.playlist;
                    });

  // The first item becomes the default version, so the best playlist leads. Of playlists of (near)
  // identical length the longest is not necessarily the fullest presentation of the movie
  if (std::ranges::none_of(sortedPlaylists, [mainPlaylist](const PlaylistInformation& playlist)
                           { return std::cmp_equal(playlist.playlist, mainPlaylist); }))
  {
    const auto best{std::ranges::find(sortedPlaylists,
                                      GetBestMoviePlaylist(sortedPlaylists).playlist,
                                      &PlaylistInformation::playlist)};
    std::rotate(sortedPlaylists.begin(), best, std::next(best));
  }

  for (const auto& playlist : sortedPlaylists)
  {
    const auto newItem{GenerateMovieItem(url, playlist.playlist, mainPlaylist, playlist)};
    if (!newItem)
    {
      CLog::LogF(LOGDEBUG, "Failed to generate FileItem for playlist {}", playlist.playlist);
      continue;
    }

    AddStreamDetails(getStreamDetails, allTitles, playlist.playlist, *newItem);

    items.Add(newItem);
  }
}

} // namespace

bool CDiscDirectoryHelper::GetMoviePlaylists(const CURL& url,
                                             CFileItemList& items,
                                             const CFileItemList& allTitles,
                                             int mainPlaylist,
                                             GetTitle job,
                                             const ClipMap& clips,
                                             const PlaylistMap& playlistMap)
{
  items.Clear();
  Reset();

  // Checks
  if (playlistMap.empty() || clips.empty())
    return false;

  std::vector<PlaylistInformation> playlists;
  InitialiseMoviePlaylistSearch(playlists, playlistMap, job, mainPlaylist);
  RemoveDuplicateMoviePlaylists(playlists, clips, job, mainPlaylist);
  if (!FilterMoviePlaylists(playlists, job))
  {
    EndMoviePlaylistSearch(playlists);
    return false;
  }
  FilterMoviePlaylistsByResolution(playlists, job, mainPlaylist);
  GetMainMoviePlaylists(playlists, job, mainPlaylist);
  PopulateMovieFileItems(url, items, mainPlaylist, allTitles, playlists, m_getStreamDetails);
  EndMoviePlaylistSearch(playlists);

  return !items.IsEmpty();
}

void CDiscDirectoryHelper::AddRootOptions(const CURL& url,
                                          CFileItemList& items,
                                          AllTitles allTitlesType,
                                          AddMenuAndAllTitlesOptions addMenuAndAllTitlesOptions)
{
  if (addMenuAndAllTitlesOptions & AddMenuAndAllTitlesOptions::ADD_ALL_TITLES)
  {
    CURL path{url};
    if (allTitlesType == AllTitles::MOVIES)
      path.SetFileName(URIUtils::AddFileToFolder("root", "titles", "all"));
    else if (allTitlesType == AllTitles::EPISODES)
      path.SetFileName(URIUtils::AddFileToFolder("root", "titles", "episodes", "all"));

    auto item{std::make_shared<CFileItem>(path.Get(), true)};
    item->SetLabel(
        g_localizeStrings.Get(25002) /* All titles */);
    item->SetArt("icon", "DefaultVideoPlaylists.png");
    items.Add(item);
  }

  if (addMenuAndAllTitlesOptions & AddMenuAndAllTitlesOptions::ADD_MENU)
  {
    CURL path{url};
    path.SetFileName("menu");
    auto item{std::make_shared<CFileItem>(path.Get(), false)};
    item->SetLabel(
        g_localizeStrings.Get(25003) /* Menu */);
    item->SetArt("icon", "DefaultProgram.png");
    items.Add(item);
  }
}
