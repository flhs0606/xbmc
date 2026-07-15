/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "IDirectory.h"
#include "URL.h"

#include <memory>
#include <mutex>

#include "cores/VideoPlayer/DVDInputStreams/BlurayIsoRegistry.h"


class CBlurayIsoCache;
class CFileItem;
class CFileItemList;

namespace XFILE
{
class CFile;
}

typedef struct bluray BLURAY;
typedef struct bd_title_info BLURAY_TITLE_INFO;

namespace XFILE
{

class CBlurayDirectory : public IDirectory
{
public:
  CBlurayDirectory() = default;
  ~CBlurayDirectory() override;
  bool GetDirectory(const CURL& url, CFileItemList &items) override;

  bool InitializeBluray(const std::string &root);
  std::string GetBlurayTitle();
  std::string GetBlurayID();

private:
  enum class DiscInfo
  {
    TITLE,
    ID
  };

  void         Dispose();
  std::string  GetDiscInfoString(DiscInfo info);
  void         GetRoot  (CFileItemList &items);
  void         GetTitles(bool main, CFileItemList &items);
  std::vector<BLURAY_TITLE_INFO*> GetUserPlaylists() const;
  std::shared_ptr<CFileItem> GetTitle(const BLURAY_TITLE_INFO* title, const std::string& label) const;
  CURL         GetUnderlyingCURL(const CURL& url);
  std::string  HexToString(const uint8_t * buf, int count);
  static int   ReadBlockCallback(void* handle, void* buf, int lba, int num_blocks);
  int64_t      ReadRaw(int64_t offset, uint8_t* buffer, size_t size);
  CURL          m_url;
  BLURAY*       m_bd = nullptr;
  bool          m_blurayInitialized = false;
  std::string m_realPath;
  std::shared_ptr<CBlurayIsoCache> m_isoCache;
  std::shared_ptr<XFILE::CFile> m_isoFile;
  std::mutex m_isoReadLock;

  // 非空表示本实例复用了共享句柄
  std::shared_ptr<BlurayIsoSharedHandle> m_sharedHandle;
  // 注册表key，Dispose时用于Release
  std::string m_isoPathKey;
};

}
