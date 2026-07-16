/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BlurayIsoCache.h"

#include "utils/log.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace
{
constexpr const char* LOG_TAG = "CBlurayIsoCache";
}

CBlurayIsoCache::CBlurayIsoCache(int64_t sourceLength, ReadCallback readCallback, Config config)
  : m_config(std::move(config)), m_readCallback(std::move(readCallback)), m_sourceLength(sourceLength)
{
  if (m_config.blockSize < BLURAY_SECTOR_SIZE)
    m_config.blockSize = BLURAY_SECTOR_SIZE;

  const size_t remainder = m_config.blockSize % BLURAY_SECTOR_SIZE;
  if (remainder != 0)
    m_config.blockSize += BLURAY_SECTOR_SIZE - remainder;

  if (m_config.maxBytes < m_config.blockSize)
    m_config.maxBytes = m_config.blockSize;

  m_maxBlocks = std::max<size_t>(1, m_config.maxBytes / m_config.blockSize);
}

CBlurayIsoCache::~CBlurayIsoCache()
{
  Stop();
}

void CBlurayIsoCache::Start()
{
  Stop();

  if (m_sourceLength <= 0 || !m_readCallback)
  {
    CLog::Log(LOGDEBUG, "{}::{} - disable cache, invalid source length {}", LOG_TAG, __FUNCTION__,
              m_sourceLength);
    return;
  }

  m_started = true;

  CLog::Log(LOGDEBUG,
            "{}::{} - cache config blockSize={} maxBytes={} maxBlocks={} prefetch={}",
            LOG_TAG, __FUNCTION__, m_config.blockSize, m_config.maxBytes, m_maxBlocks,
            m_config.prefetch ? "true" : "false");

  CLog::Log(LOGDEBUG, "{}::{} - Bluray ISO cache started for {} bytes source",
            LOG_TAG, __FUNCTION__, m_sourceLength);

  if (!m_config.prefetch)
  {
    CLog::Log(LOGDEBUG, "{}::{} - background prefetch disabled", LOG_TAG, __FUNCTION__);
    return;
  }

  // Launch burst prefetch background thread
  m_prefetchRunning = true;
  m_prefetchThread = std::thread(&CBlurayIsoCache::PrefetchWorker, this);

  // Conservative warmup for common UDF/BD metadata near the ISO tail. Keep this small so
  // foreground reads can still take over quickly on high-latency network filesystems.
  const int64_t maxBlock = GetMaxBlockIndex();
  if (maxBlock >= 0)
    SchedulePrefetch(std::max<int64_t>(0, maxBlock - WARMUP_TAIL + 1), maxBlock);
}

void CBlurayIsoCache::Stop()
{
  if (m_prefetchRunning.exchange(false))
  {
    {
      std::lock_guard<std::mutex> lk(m_prefetchMutex);
      m_prefetchQueue.clear();
    }
    m_prefetchCv.notify_one();
    if (m_prefetchThread.joinable())
      m_prefetchThread.join();
  }

  if (m_started)
    LogStats("stop");

  {
    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    m_blocks.clear();
    m_lruOrder.clear();
  }

  m_started = false;
}

int CBlurayIsoCache::ReadBlocks(uint8_t* buffer, int lba, int numBlocks)
{
  if (!buffer || lba < 0 || numBlocks <= 0)
    return -1;

  ++m_readRequests;
  m_requestedBlocks += static_cast<uint64_t>(numBlocks);

  const int64_t offset = static_cast<int64_t>(lba) * BLURAY_SECTOR_SIZE;
  const int64_t requestedBytes = static_cast<int64_t>(numBlocks) * BLURAY_SECTOR_SIZE;
  if (offset < 0 || requestedBytes <= 0 || offset >= m_sourceLength)
    return -1;

  const int64_t availableBytes = std::min<int64_t>(requestedBytes, m_sourceLength - offset);

  const int64_t firstBlock = offset / static_cast<int64_t>(m_config.blockSize);
  const int64_t lastBlock = (offset + availableBytes - 1) / static_cast<int64_t>(m_config.blockSize);

  if (m_config.prefetch)
    TrackDemandRead(firstBlock, lastBlock);

  int64_t copied = 0;
  for (int64_t blockIndex = firstBlock; blockIndex <= lastBlock; ++blockIndex)
  {
    BlockPtr block = GetBlock(blockIndex);
    if (!block)
    {
      ++m_blockMisses;
      block = LoadBlock(blockIndex);
    }
    else
      ++m_blockHits;

    if (!block)
      return copied > 0 ? static_cast<int>(copied / BLURAY_SECTOR_SIZE) : -1;

    const int64_t blockStart = blockIndex * static_cast<int64_t>(m_config.blockSize);
    const int64_t copyStart = std::max<int64_t>(offset, blockStart);
    const int64_t copyEnd = std::min<int64_t>(offset + availableBytes,
                                              blockStart + static_cast<int64_t>(block->validBytes));
    if (copyEnd <= copyStart)
      break;

    const size_t blockOffset = static_cast<size_t>(copyStart - blockStart);
    const size_t chunk = static_cast<size_t>(copyEnd - copyStart);
    std::memcpy(buffer + copied, block->data.data() + blockOffset, chunk);
    copied += static_cast<int64_t>(chunk);
  }

  if (copied <= 0)
    return -1;

  // ★ Burst prefetch: warm cache with next BURST_AHEAD blocks
  // Avoids serial NFS round-trips for subsequent sequential reads.
  if (m_prefetchRunning.load(std::memory_order_acquire))
  {
    const int64_t pfStart = lastBlock + 1;
    const int64_t pfEnd = std::min<int64_t>(lastBlock + BURST_AHEAD, GetMaxBlockIndex());
    if (pfStart <= pfEnd)
      SchedulePrefetch(pfStart, pfEnd);
  }

  return static_cast<int>(copied / BLURAY_SECTOR_SIZE);
}

CBlurayIsoCache::BlockPtr CBlurayIsoCache::GetBlock(int64_t blockIndex)
{
  std::lock_guard<std::mutex> lock(m_cacheMutex);
  auto it = m_blocks.find(blockIndex);
  if (it == m_blocks.end())
    return nullptr;

  TouchBlockUnlocked(it);
  return it->second.block;
}

CBlurayIsoCache::BlockPtr CBlurayIsoCache::ReadBlock(int64_t blockIndex)
{
  if (!IsValidBlockIndex(blockIndex) || !m_readCallback)
    return nullptr;

  auto block = std::make_shared<Block>();
  block->data.resize(m_config.blockSize);

  const int64_t offset = blockIndex * static_cast<int64_t>(m_config.blockSize);
  const size_t bytesToRead = static_cast<size_t>(
      std::min<int64_t>(static_cast<int64_t>(m_config.blockSize), m_sourceLength - offset));
  const int64_t bytesRead = m_readCallback(offset, block->data.data(), bytesToRead);
  if (bytesRead <= 0)
    return nullptr;

  if (static_cast<size_t>(bytesRead) > bytesToRead)
  {
    CLog::Log(LOGERROR, "{}::{} - callback returned {} bytes, expected at most {}",
              LOG_TAG, __FUNCTION__, bytesRead, bytesToRead);
    return nullptr;
  }

  block->validBytes = static_cast<size_t>(bytesRead);
  return block;
}

CBlurayIsoCache::BlockPtr CBlurayIsoCache::LoadBlock(int64_t blockIndex)
{
  auto block = ReadBlock(blockIndex);
  if (!block)
    return nullptr;

  InsertBlock(blockIndex, block);
  ++m_syncBlockLoads;

  return block;
}

void CBlurayIsoCache::InsertBlock(int64_t blockIndex, BlockPtr block)
{
  if (!block)
    return;

  std::lock_guard<std::mutex> lock(m_cacheMutex);
  auto it = m_blocks.find(blockIndex);
  if (it != m_blocks.end())
  {
    it->second.block = std::move(block);
    TouchBlockUnlocked(it);
    return;
  }

  m_lruOrder.push_front(blockIndex);
  m_blocks.emplace(blockIndex, CacheSlot{m_lruOrder.begin(), std::move(block)});
  TrimUnlocked();
}

void CBlurayIsoCache::TouchBlockUnlocked(std::unordered_map<int64_t, CacheSlot>::iterator it)
{
  if (it->second.lruIt != m_lruOrder.begin())
    m_lruOrder.splice(m_lruOrder.begin(), m_lruOrder, it->second.lruIt);
  it->second.lruIt = m_lruOrder.begin();
}

void CBlurayIsoCache::TrimUnlocked()
{
  while (m_blocks.size() > m_maxBlocks && !m_lruOrder.empty())
  {
    const int64_t blockIndex = m_lruOrder.back();
    m_lruOrder.pop_back();
    m_blocks.erase(blockIndex);
    ++m_evictions;
  }
}

void CBlurayIsoCache::LogStats(const char* reason)
{
  size_t cachedBlocks = 0;
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    cachedBlocks = m_blocks.size();
  }

  CLog::Log(LOGDEBUG,
            "{}::{} - {} detail: reads={} reqBlocks={} blockHits={} misses={} syncLoads={} evicts={} prefetchLoads={} prefetchInvalidations={} staleDiscards={} blocks={}",
            LOG_TAG, __FUNCTION__, reason, m_readRequests.load(), m_requestedBlocks.load(),
            m_blockHits.load(), m_blockMisses.load(), m_syncBlockLoads.load(),
            m_evictions.load(), m_prefetchBlockLoads.load(), m_prefetchInvalidations.load(),
            m_prefetchStaleDiscards.load(), cachedBlocks);
}

bool CBlurayIsoCache::IsValidBlockIndex(int64_t blockIndex) const
{
  return blockIndex >= 0 && blockIndex <= GetMaxBlockIndex();
}

int64_t CBlurayIsoCache::GetMaxBlockIndex() const
{
  if (m_sourceLength <= 0)
    return -1;
  return (m_sourceLength - 1) / static_cast<int64_t>(m_config.blockSize);
}

void CBlurayIsoCache::ResetPrefetchLocked()
{
  ++m_prefetchGeneration;
  m_prefetchQueue.clear();
  ++m_prefetchInvalidations;
}

bool CBlurayIsoCache::IsPrefetchGenerationStale(uint64_t generation)
{
  std::lock_guard<std::mutex> lk(m_prefetchMutex);
  if (generation == m_prefetchGeneration)
    return false;

  ++m_prefetchStaleDiscards;
  return true;
}

void CBlurayIsoCache::TrackDemandRead(int64_t firstBlock, int64_t lastBlock)
{
  std::lock_guard<std::mutex> lk(m_prefetchMutex);

  if (m_lastDemandRange)
  {
    const auto& [lastFirstBlock, lastLastBlock] = *m_lastDemandRange;
    if (firstBlock + DISCONTINUITY_BEHIND < lastFirstBlock ||
        firstBlock > lastLastBlock + DISCONTINUITY_AHEAD)
      ResetPrefetchLocked();
  }

  m_lastDemandRange = std::make_pair(firstBlock, lastBlock);
}

void CBlurayIsoCache::NotifySeek()
{
  if (!m_config.prefetch)
    return;

  {
    std::lock_guard<std::mutex> lk(m_prefetchMutex);
    ResetPrefetchLocked();
    m_lastDemandRange.reset();
  }
  m_prefetchCv.notify_one();
}

void CBlurayIsoCache::SchedulePrefetch(int64_t fromBlock, int64_t toBlock)
{
  // Deduplicate: only queue blocks not yet in cache or already pending.
  std::lock_guard<std::mutex> lk(m_prefetchMutex);
  for (int64_t p = fromBlock; p <= toBlock; ++p)
  {
    if (!IsValidBlockIndex(p))
      break;

    // Quick check: skip if already in cache (avoids wasted prefetch)
    {
      std::lock_guard<std::mutex> clk(m_cacheMutex);
      if (m_blocks.count(p))
        continue;
    }

    if (std::find(m_prefetchQueue.begin(), m_prefetchQueue.end(), p) == m_prefetchQueue.end())
      m_prefetchQueue.push_back(p);
  }
  if (!m_prefetchQueue.empty())
    m_prefetchCv.notify_one();
}

void CBlurayIsoCache::PrefetchWorker()
{
  CLog::Log(LOGDEBUG, "{}::PrefetchWorker - started", LOG_TAG);

  while (m_prefetchRunning.load(std::memory_order_acquire))
  {
    int64_t blockIndex = -1;
    uint64_t generation = 0;
    {
      std::unique_lock<std::mutex> lk(m_prefetchMutex);
      m_prefetchCv.wait(lk, [this] {
        return !m_prefetchQueue.empty() || !m_prefetchRunning.load(std::memory_order_acquire);
      });

      if (!m_prefetchRunning.load(std::memory_order_acquire))
        break;

      if (m_prefetchQueue.empty())
        continue;

      blockIndex = m_prefetchQueue.front();
      m_prefetchQueue.pop_front();
      generation = m_prefetchGeneration;
    }

    if (blockIndex < 0)
      continue;

    if (IsPrefetchGenerationStale(generation))
      continue;

    // Skip if already loaded (racy double-check)
    if (GetBlock(blockIndex))
      continue;

    // Load via the same read callback — callers are serialized by
    // their own ReadRaw lock so there is no seek/read interleaving.
    auto block = ReadBlock(blockIndex);
    if (!block)
      continue;

    if (IsPrefetchGenerationStale(generation))
      continue;

    InsertBlock(blockIndex, block);
    ++m_prefetchBlockLoads;
  }

  CLog::Log(LOGDEBUG, "{}::PrefetchWorker - stopped", LOG_TAG);
}
