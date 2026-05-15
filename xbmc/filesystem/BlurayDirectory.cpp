/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#include "BlurayDirectory.h"

#include "File.h"
#include "FileItem.h"
#include "LangInfo.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "cores/VideoPlayer/DVDInputStreams/BlurayIsoCache.h"
#include "filesystem/BlurayCallback.h"
#include "filesystem/Directory.h"
#include "filesystem/DirectoryFactory.h"
#include "guilib/LocalizeStrings.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/LangCodeExpander.h"
#include "utils/RegExp.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"
#include "video/VideoInfoTag.h"

#include <array>
#include <cassert>
#include <climits>
#include <memory>
#include <stdlib.h>
#include <string>

#include <libbluray/bluray-version.h>
#include <libbluray/bluray.h>
#include <libbluray/filesystem.h>
#include <libbluray/log_control.h>

namespace XFILE
{

#define MAIN_TITLE_LENGTH_PERCENT 70 /** Minimum length of main titles, based on longest title */

CBlurayDirectory::~CBlurayDirectory()
{
  Dispose();
}

void CBlurayDirectory::Dispose()
{
  if (m_isoCache)
  {
    m_isoCache->Stop();
    m_isoCache.reset();
  }
  m_isoFile.reset();

  if(m_bd)
  {
    bd_close(m_bd);
    m_bd = nullptr;
  }
}

std::string CBlurayDirectory::GetBlurayTitle()
{
  return GetDiscInfoString(DiscInfo::TITLE);
}

std::string CBlurayDirectory::GetBlurayID()
{
  return GetDiscInfoString(DiscInfo::ID);
}

std::string CBlurayDirectory::GetDiscInfoString(DiscInfo info)
{
  switch (info)
  {
  case XFILE::CBlurayDirectory::DiscInfo::TITLE:
  {
    if (!m_blurayInitialized)
      return "";
    const BLURAY_DISC_INFO* disc_info = bd_get_disc_info(m_bd);
    if (!disc_info || !disc_info->bluray_detected)
      return "";

    std::string title = "";

#if (BLURAY_VERSION > BLURAY_VERSION_CODE(1,0,0))
    title = disc_info->disc_name ? disc_info->disc_name : "";
#endif

    return title;
  }
  case XFILE::CBlurayDirectory::DiscInfo::ID:
  {
    if (!m_blurayInitialized)
      return "";

    const BLURAY_DISC_INFO* disc_info = bd_get_disc_info(m_bd);
    if (!disc_info || !disc_info->bluray_detected)
      return "";

    std::string id = "";

#if (BLURAY_VERSION > BLURAY_VERSION_CODE(1,0,0))
    id = disc_info->udf_volume_id ? disc_info->udf_volume_id : "";

    if (id.empty())
    {
      id = HexToString(disc_info->disc_id, 20);
    }
#endif

    return id;
  }
  default:
    break;
  }

  return "";
}

std::shared_ptr<CFileItem> CBlurayDirectory::GetTitle(const BLURAY_TITLE_INFO* title,
                                                      const std::string& label) const {
  std::string buf;
  std::string chap;
  CFileItemPtr item(new CFileItem("", false));
  CURL path(m_url);
  buf = StringUtils::Format("BDMV/PLAYLIST/{:05}.mpls", title->playlist);
  path.SetFileName(buf);
  item->SetPath(path.Get());
  int duration = (int)(title->duration / 90000);
  item->GetVideoInfoTag()->SetDuration(duration);
  item->GetVideoInfoTag()->m_iTrack = title->playlist;
  buf = StringUtils::Format(label, title->playlist);
  item->m_strTitle = buf;
  item->SetLabel(buf);
  chap = StringUtils::Format(g_localizeStrings.Get(25007), title->chapter_count,
                             StringUtils::SecondsToTimeString(duration));
  item->SetLabel2(chap);
  item->m_dwSize = 0;
  item->SetArt("icon", "DefaultVideo.png");
  for(unsigned int i = 0; i < title->clip_count; ++i)
    item->m_dwSize += title->clips[i].pkt_count * 192;

  return item;
}

void CBlurayDirectory::GetTitles(bool main, CFileItemList &items)
{
  std::vector<BLURAY_TITLE_INFO*> titleList;
  uint64_t minDuration = 0;

  // Searching for a user provided list of playlists.
  if (main)
    titleList = GetUserPlaylists();

  if (!main || titleList.empty())
  {
    uint32_t numTitles = bd_get_titles(m_bd, TITLES_RELEVANT, 0);

    for (uint32_t i = 0; i < numTitles; i++)
    {
      BLURAY_TITLE_INFO* t = bd_get_title_info(m_bd, i, 0);

      if (!t)
      {
        CLog::Log(LOGDEBUG, "CBlurayDirectory - unable to get title {}", i);
        continue;
      }

      if (main && t->duration > minDuration)
          minDuration = t->duration;

      titleList.emplace_back(t);
    }
  }

  minDuration = minDuration * MAIN_TITLE_LENGTH_PERCENT / 100;

  for (auto& title : titleList)
  {
    if (title->duration < minDuration)
      continue;

    items.Add(GetTitle(title, main ? g_localizeStrings.Get(25004) /* Main Title */ : g_localizeStrings.Get(25005) /* Title */));
    bd_free_title_info(title);
  }
}

void CBlurayDirectory::GetRoot(CFileItemList &items)
{
    GetTitles(true, items);

    CURL path(m_url);
    CFileItemPtr item;

    path.SetFileName(URIUtils::AddFileToFolder(m_url.GetFileName(), "titles"));
    item = std::make_shared<CFileItem>();
    item->SetPath(path.Get());
    item->m_bIsFolder = true;
    item->SetLabel(g_localizeStrings.Get(25002) /* All titles */);
    item->SetArt("icon", "DefaultVideoPlaylists.png");
    items.Add(item);

    const BLURAY_DISC_INFO* disc_info = bd_get_disc_info(m_bd);
    if (disc_info && disc_info->no_menu_support)
    {
      CLog::Log(LOGDEBUG, "CBlurayDirectory::GetRoot - no menu support, skipping menu entry");
      return;
    }

    path.SetFileName("menu");
    item = std::make_shared<CFileItem>();
    item->SetPath(path.Get());
    item->m_bIsFolder = false;
    item->SetLabel(g_localizeStrings.Get(25003) /* Menus */);
    item->SetArt("icon", "DefaultProgram.png");
    items.Add(item);
}

bool CBlurayDirectory::GetDirectory(const CURL& url, CFileItemList &items)
{
  Dispose();
  m_url = url;
  std::string root = m_url.GetHostName();
  std::string file = m_url.GetFileName();
  URIUtils::RemoveSlashAtEnd(file);
  URIUtils::RemoveSlashAtEnd(root);

  if (!InitializeBluray(root))
    return false;

  if(file == "root")
    GetRoot(items);
  else if(file == "root/titles")
    GetTitles(false, items);
  else
  {
    CURL url2 = GetUnderlyingCURL(url);
    CDirectory::CHints hints;
    hints.flags = m_flags;
    if (!CDirectory::GetDirectory(url2, items, hints))
      return false;
  }

  items.AddSortMethod(SortByTrackNumber,  554, LABEL_MASKS("%L", "%D", "%L", ""));    // FileName, Duration | Foldername, empty
  items.AddSortMethod(SortBySize,         553, LABEL_MASKS("%L", "%I", "%L", "%I"));  // FileName, Size | Foldername, Size

  return true;
}

CURL CBlurayDirectory::GetUnderlyingCURL(const CURL& url)
{
  assert(url.IsProtocol("bluray"));
  std::string host = url.GetHostName();
  const std::string& filename = url.GetFileName();
  return CURL(host.append(filename));
}

bool CBlurayDirectory::InitializeBluray(const std::string &root)
{
  bd_set_debug_handler(CBlurayCallback::bluray_logger);
  bd_set_debug_mask(DBG_CRIT | DBG_BLURAY | DBG_NAV);

  m_bd = bd_init();

  if (!m_bd)
  {
    CLog::Log(LOGERROR, "CBlurayDirectory::InitializeBluray - failed to initialize libbluray");
    return false;
  }

  std::string langCode;
  g_LangCodeExpander.ConvertToISO6392T(g_langInfo.GetDVDMenuLanguage(), langCode);
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_MENU_LANG, langCode.c_str());

  m_realPath = root;

  auto fileHandler = CDirectoryFactory::Create(CURL{root});
  if (fileHandler)
    m_realPath = fileHandler->ResolveMountPoint(root);

  // Check if the root is a disc image (ISO) and use stream mode with LRU cache
  CURL rootUrl(root);
  bool isDiscImage = false;
  std::string isoPath;

  if (rootUrl.IsProtocol("udf"))
  {
    isoPath = rootUrl.GetHostName();
    CFileItem isoItem(isoPath, false);
    isDiscImage = isoItem.IsDiscImage();
  }

  if (isDiscImage)
  {
    m_isoFile = std::make_shared<CFile>();
    if (!m_isoFile->Open(isoPath))
    {
      CLog::Log(LOGERROR, "CBlurayDirectory::InitializeBluray - failed to open ISO file {}",
                CURL::GetRedacted(isoPath));
      return false;
    }

    const int64_t sourceLength = m_isoFile->GetLength();
    if (sourceLength > 0)
    {
      const auto adv = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();
      CBlurayIsoCache::Config cacheConfig{};
      cacheConfig.pageSize = adv->m_blurayIsoCachePageSize;
      cacheConfig.maxBytes = adv->m_blurayIsoCacheMaxBytes;

      m_isoCache = std::make_shared<CBlurayIsoCache>(
          sourceLength,
          [this](int64_t offset, uint8_t* buffer, size_t size) {
            return ReadRaw(offset, buffer, size);
          },
          cacheConfig);
      m_isoCache->Start();
    }

    if (!bd_open_stream(m_bd, this, ReadBlockCallback))
    {
      CLog::Log(LOGERROR, "CBlurayDirectory::InitializeBluray - failed to open {} in stream mode",
                CURL::GetRedacted(root));
      return false;
    }
  }
  else
  {
    if (!bd_open_files(m_bd, &m_realPath, CBlurayCallback::dir_open, CBlurayCallback::file_open))
    {
      CLog::Log(LOGERROR, "CBlurayDirectory::InitializeBluray - failed to open {}",
                CURL::GetRedacted(root));
      return false;
    }
  }
  m_blurayInitialized = true;

  return true;
}

std::string CBlurayDirectory::HexToString(const uint8_t *buf, int count)
{
  std::array<char, 42> tmp;

  for (int i = 0; i < count; i++)
  {
    sprintf(tmp.data() + (i * 2), "%02x", buf[i]);
  }

  return std::string(std::begin(tmp), std::end(tmp));
}

std::vector<BLURAY_TITLE_INFO*> CBlurayDirectory::GetUserPlaylists() const {
  // ★ Skip disc.inf for HTTP ISO: disc.inf almost never exists (99.9% of ISOs),
  // and CFile::Open would create a new HTTP connection consuming the CDN token
  // which can cause 403 → crash. Let GetTitles fallback to bd_get_titles() API.
  if (m_isoFile || m_isoCache)
    return {};

  std::string root = m_url.GetHostName();
  std::string discInfPath = URIUtils::AddFileToFolder(root, "disc.inf");
  std::vector<BLURAY_TITLE_INFO*> userTitles;
  CFile file;
  char buffer[1025];

  if (file.Open(discInfPath))
  {
    CLog::Log(LOGDEBUG, "CBlurayDirectory::GetTitles - disc.inf found");

    CRegExp pl(true);
    if (!pl.RegComp("(\\d+)"))
    {
      file.Close();
      return userTitles;
    }

    uint8_t maxLines = 100;
    while ((maxLines > 0) && file.ReadString(buffer, 1024))
    {
      maxLines--;
      if (StringUtils::StartsWithNoCase(buffer, "playlists"))
      {
        int pos = 0;
        while ((pos = pl.RegFind(buffer, static_cast<unsigned int>(pos))) >= 0)
        {
          std::string playlist = pl.GetMatch(0);
          uint32_t len = static_cast<uint32_t>(playlist.length());

          if (len <= 5)
          {
            unsigned long int plNum = strtoul(playlist.c_str(), nullptr, 10);

            BLURAY_TITLE_INFO* t = bd_get_playlist_info(m_bd, static_cast<uint32_t>(plNum), 0);
            if (t)
              userTitles.emplace_back(t);
          }

          if (static_cast<int64_t>(pos) + static_cast<int64_t>(len) > INT_MAX)
            break;
          else
            pos += len;
        }
      }
    }
    file.Close();
  }
  return userTitles;
}

int CBlurayDirectory::ReadBlockCallback(void* handle, void* buf, int lba, int num_blocks)
{
  auto* self = static_cast<CBlurayDirectory*>(handle);
  if (!self || !self->m_isoFile)
    return -1;

  if (self->m_isoCache)
    return self->m_isoCache->ReadBlocks(static_cast<uint8_t*>(buf), lba, num_blocks);

  // Fallback: direct read
  int64_t offset = static_cast<int64_t>(lba) * 2048;
  int64_t size = static_cast<int64_t>(num_blocks) * 2048;
  ssize_t ret = self->ReadRaw(offset, static_cast<uint8_t*>(buf), static_cast<size_t>(size));
  return ret > 0 ? static_cast<int>(ret / 2048) : -1;
}

int64_t CBlurayDirectory::ReadRaw(int64_t offset, uint8_t* buffer, size_t size)
{
  if (!m_isoFile || !buffer || size == 0)
    return -1;

  if (size > static_cast<size_t>(std::numeric_limits<int>::max()))
    return -1;

  std::lock_guard lock(m_isoReadLock);

  if (m_isoFile->Seek(offset, SEEK_SET) != offset)
    return -1;

  size_t totalRead = 0;
  while (totalRead < size)
  {
    int chunk = m_isoFile->Read(buffer + totalRead, static_cast<int>(size - totalRead));
    if (chunk < 0)
      return -1;
    if (chunk == 0)
      break;
    totalRead += static_cast<size_t>(chunk);
  }

  return static_cast<int64_t>(totalRead > 0 ? totalRead : -1);
}

} /* namespace XFILE */
