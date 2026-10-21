/*
 *  Copyright (C) 2025 Team CoreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BlurayBlockCache.h"

#include <functional>

bool CBlurayBlockCache::Key::operator==(const Key& o) const
{
  return sourceId == o.sourceId && blockNum == o.blockNum;
}

size_t CBlurayBlockCache::KeyHash::operator()(const Key& k) const
{
  size_t h1 = std::hash<int64_t>{}(k.sourceId);
  size_t h2 = std::hash<int64_t>{}(k.blockNum);
  // combine hashes
  return h1 ^ (h2 * 11400714819323198485ULL);
}

CBlurayBlockCache& CBlurayBlockCache::Instance()
{
  static CBlurayBlockCache cache;
  return cache;
}

std::shared_ptr<std::vector<uint8_t>> CBlurayBlockCache::Get(const std::string& url,
                                                             int64_t version,
                                                             int64_t blockNum)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  auto src = m_sources.find(url);
  // Missing source or a stale version must not serve old bytes.
  if (src == m_sources.end() || src->second.version != version)
    return nullptr;

  Key key{src->second.sourceId, blockNum};
  auto it = m_blocks.find(key);
  if (it == m_blocks.end())
    return nullptr;
  // Move to front (MRU)
  m_lruOrder.splice(m_lruOrder.begin(), m_lruOrder, it->second.first);
  return it->second.second;
}

void CBlurayBlockCache::Put(const std::string& url,
                            int64_t version,
                            int64_t blockNum,
                            const uint8_t* data,
                            size_t size)
{
  std::lock_guard<std::mutex> lock(m_mutex);

  // Resolve (and if needed register) the source, purging stale blocks when
  // the same source is seen again with a different version.
  auto src = m_sources.find(url);
  int64_t sourceId;
  if (src == m_sources.end())
  {
    sourceId = ++m_nextSourceId;
    m_sources.emplace(url, SourceEntry{sourceId, version});
  }
  else
  {
    if (src->second.version != version)
    {
      PurgeSourceLocked(src->second.sourceId);
      src->second.version = version;
    }
    sourceId = src->second.sourceId;
  }

  Key key{sourceId, blockNum};
  auto it = m_blocks.find(key);
  if (it != m_blocks.end())
  {
    // Update existing
    it->second.second = std::make_shared<std::vector<uint8_t>>(data, data + size);
    m_lruOrder.splice(m_lruOrder.begin(), m_lruOrder, it->second.first);
    return;
  }

  // Evict if needed
  while (m_blocks.size() >= m_maxBlocks && !m_lruOrder.empty())
  {
    m_blocks.erase(m_lruOrder.back());
    m_lruOrder.pop_back();
  }

  m_lruOrder.push_front(key);
  m_blocks[key] = {m_lruOrder.begin(), std::make_shared<std::vector<uint8_t>>(data, data + size)};
}

void CBlurayBlockCache::PurgeSourceLocked(int64_t sourceId)
{
  for (auto li = m_lruOrder.begin(); li != m_lruOrder.end();)
  {
    if (li->sourceId == sourceId)
    {
      m_blocks.erase(*li);
      li = m_lruOrder.erase(li);
    }
    else
    {
      ++li;
    }
  }
}

void CBlurayBlockCache::SetLimits(size_t blockSize, size_t totalSize)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (blockSize == 0)
    blockSize = 1024 * 1024;
  if (m_blockSize != blockSize)
  {
    // Block size changed — old block numbers invalid, clear everything.
    // Source registry stays valid (ids are size-independent).
    m_lruOrder.clear();
    m_blocks.clear();
  }
  m_blockSize = blockSize;
  m_maxBlocks = totalSize / m_blockSize;
  if (m_maxBlocks == 0)
    m_maxBlocks = 1;
  // Trim excess
  while (m_blocks.size() > m_maxBlocks && !m_lruOrder.empty())
  {
    m_blocks.erase(m_lruOrder.back());
    m_lruOrder.pop_back();
  }
}

size_t CBlurayBlockCache::GetBlockSize()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_blockSize;
}

void CBlurayBlockCache::Clear()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_lruOrder.clear();
  m_blocks.clear();
  m_sources.clear();
  m_nextSourceId = 0;
}
