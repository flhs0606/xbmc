/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#include "BlurayIsoRegistry.h"

#include "utils/log.h"

namespace
{
constexpr const char* LOG_TAG = "CBlurayIsoRegistry";
}

CBlurayIsoRegistry& CBlurayIsoRegistry::Get()
{
  static CBlurayIsoRegistry instance;
  return instance;
}

std::shared_ptr<BlurayIsoSharedHandle> CBlurayIsoRegistry::CreatePending(
    const std::string& isoPath)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  auto it = m_handles.find(isoPath);
  if (it != m_handles.end())
  {
    //有人已经为此路径注册了一个句柄（目录浏览和播放打开之间的竞争）-缓冲引用并将其交还给调用者，以便调用者检测到这一点并跳过自己的打开。
    ++it->second->refCount;
    return it->second;
  }

  auto handle = std::make_shared<BlurayIsoSharedHandle>();
  handle->isoPath = isoPath;
  handle->refCount = 1;
  m_handles.emplace(isoPath, handle);
  return handle;
}

void CBlurayIsoRegistry::Publish(const std::string& isoPath,
                                  std::shared_ptr<BlurayIsoSharedHandle> handle)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_handles[isoPath] = std::move(handle);
}

std::shared_ptr<BlurayIsoSharedHandle> CBlurayIsoRegistry::TryAcquire(
    const std::string& isoPath)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_handles.find(isoPath);
  if (it == m_handles.end())
    return nullptr;

  ++it->second->refCount;
  CLog::Log(LOGDEBUG, "{}::{} - reusing shared ISO handle for {} (refCount={})", LOG_TAG,
            __FUNCTION__, isoPath, it->second->refCount);
  return it->second;
}

bool CBlurayIsoRegistry::Release(const std::string& isoPath)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_handles.find(isoPath);
  if (it == m_handles.end())
    return false;

  --it->second->refCount;
  CLog::Log(LOGDEBUG, "{}::{} - release shared ISO handle for {} (refCount={})", LOG_TAG,
            __FUNCTION__, isoPath, it->second->refCount);

  if (it->second->refCount <= 0)
  {
    m_handles.erase(it);
    //最后一个引用-调用者必须实际关闭bd/isoFile/isoCache
    return true;
  }
  return false;
}