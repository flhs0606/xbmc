/*
*  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

extern "C"
{
#include <libbluray/bluray.h>
}

class CBlurayIsoCache;
namespace XFILE { class CFile; }

// 单个物理ISO URL的共享句柄，由CBlurayDirectory（浏览）和CDVDInputStreamBluray（播放）引用，
// 因此同一ISO永远不会同时打开两次（避免同一源上的双重CDN令牌消耗/并发bd_open_stream）。
struct BlurayIsoSharedHandle
{
  BLURAY* bd = nullptr;
  std::shared_ptr<XFILE::CFile> isoFile;
  std::shared_ptr<CBlurayIsoCache> isoCache;
  std::string isoPath;
  int refCount = 0;
};

class CBlurayIsoRegistry
{
public:
  static CBlurayIsoRegistry& Get();

  //由第一个打开者调用（通常是CBlurayDirectory）。返回一个空句柄，由调用者填写并通过Publish（）存储。
  std::shared_ptr<BlurayIsoSharedHandle> CreatePending(const std::string& isoPath);

  //在第一个打开器成功完成bd_open_stream后调用。
  void Publish(const std::string& isoPath, std::shared_ptr<BlurayIsoSharedHandle> handle);

  //在尝试自行打开之前，由任何后续的打开器调用（例如播放）。如果此路径不存在活动句柄，则返回nullptr。
  std::shared_ptr<BlurayIsoSharedHandle> TryAcquire(const std::string& isoPath);

  // 递减引用计数；调用者必须检查返回的布尔值 - true 表示这是最后一个引用，调用者（仍然持有共享句柄的人）负责实际关闭 bd/isoFile/isoCache。
  bool Release(const std::string& isoPath);

private:
  std::mutex m_mutex;
  std::unordered_map<std::string, std::shared_ptr<BlurayIsoSharedHandle>> m_handles;
};