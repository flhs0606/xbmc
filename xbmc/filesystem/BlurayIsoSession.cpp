/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BlurayIsoSession.h"

#include "BlurayBlockCache.h"

#include "utils/log.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace
{
constexpr const char* LOG_TAG = "CBlurayIsoSession";

// UDF keeps everything the mount/parse phase needs in two tiny regions: the
// Anchor Volume Descriptor Pointer sits at the very end of the image, the
// Volume/FileSet/metadata descriptors at the very start. libbluray+udfread
// therefore read head and tail in an interleaved, non-monotonic order
// (measured: 0 -> tail -> 0 -> tail -> 4M -> 2M -> 3M -> 1M), which is the
// worst case for a linear prefetch ring — every jump aborts the in-flight
// transfer and reconnects. Reading both regions once, in order, up front
// turns 8 scattered misses into 2 sequential transfers.
constexpr int64_t WARMUP_REGION_BYTES = 8 * 1024 * 1024;
}

CBlurayIsoSession::CBlurayIsoSession(std::string source,
                                     int64_t sourceLength,
                                     int64_t version,
                                     ReadCallback readCallback,
                                     Config config)
  : m_config(std::move(config)),
    m_readCallback(std::move(readCallback)),
    m_source(std::move(source)),
    m_sourceLength(sourceLength),
    m_version(version)
{
  if (m_config.blockSize < BLURAY_SECTOR_SIZE)
    m_config.blockSize = BLURAY_SECTOR_SIZE;

  if (m_config.maxBytes < m_config.blockSize)
    m_config.maxBytes = m_config.blockSize;

  m_blockSize = m_config.blockSize;
}

CBlurayIsoSession::~CBlurayIsoSession()
{
  Stop();
}

void CBlurayIsoSession::Start()
{
  Stop();

  if (m_sourceLength <= 0 || !m_readCallback || m_source.empty())
  {
    CLog::Log(LOGDEBUG, "{}::{} - disable cache, invalid source {} (length {}, callback {})",
              LOG_TAG, __FUNCTION__, m_source.empty() ? "(empty)" : "set", m_sourceLength,
              m_readCallback ? "set" : "none");
    return;
  }

  // Make the shared store use this session's geometry, then derive the block
  // size from the store so block indices are consistent across writers.
  CBlurayBlockCache::Instance().SetLimits(m_config.blockSize, m_config.maxBytes);
  m_blockSize = CBlurayBlockCache::Instance().GetBlockSize();

  m_started = true;

  CLog::Log(LOGDEBUG, "{}::{} - Bluray ISO session started for {} bytes source (blockSize={} maxBytes={})",
            LOG_TAG, __FUNCTION__, m_sourceLength, m_blockSize, m_config.maxBytes);

  WarmUp();
}

void CBlurayIsoSession::WarmUp()
{
  const int64_t blockSize = static_cast<int64_t>(m_blockSize);
  const int64_t maxBlock = GetMaxBlockIndex();
  if (maxBlock < 0)
    return;

  // Never spend more than a quarter of the store on warmup.
  const int64_t budget =
      std::min<int64_t>(WARMUP_REGION_BYTES, static_cast<int64_t>(m_config.maxBytes) / 4);
  const int64_t regionBlocks = std::max<int64_t>(1, budget / blockSize);

  const int64_t headLast = std::min<int64_t>(regionBlocks - 1, maxBlock);
  const int64_t tailFirst = std::max<int64_t>(maxBlock - regionBlocks + 1, headLast + 1);

  uint64_t loaded = 0;
  uint64_t present = 0;
  const auto warmRange = [&](int64_t first, int64_t last)
  {
    for (int64_t i = first; i <= last; ++i)
    {
      if (GetBlock(i))
      {
        // Already in the shared store (e.g. the picker warmed it before the
        // player opened the same image) — nothing to download.
        ++present;
        continue;
      }
      if (!LoadBlock(i))
        return false; // transport error: give up, normal demand reads will retry
      ++loaded;
    }
    return true;
  };

  if (warmRange(0, headLast))
    warmRange(tailFirst, maxBlock);

  CLog::Log(LOGDEBUG, "{}::{} - warmup head[0-{}] tail[{}-{}]: loaded={} alreadyCached={}",
            LOG_TAG, __FUNCTION__, headLast, tailFirst, maxBlock, loaded, present);
}

void CBlurayIsoSession::Stop()
{
  if (m_started)
    LogStats("stop");

  // Blocks live in the shared store and are intentionally NOT dropped here so
  // a later session on the same source (e.g. picker -> player) can reuse them.
  m_started = false;
}

int CBlurayIsoSession::ReadBlocks(uint8_t* buffer, int lba, int numBlocks)
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

  const int64_t blockSize = static_cast<int64_t>(m_blockSize);
  const int64_t firstBlock = offset / blockSize;
  const int64_t lastBlock = (offset + availableBytes - 1) / blockSize;

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

    const int64_t blockStart = blockIndex * blockSize;
    const int64_t copyStart = std::max<int64_t>(offset, blockStart);
    const int64_t copyEnd =
        std::min<int64_t>(offset + availableBytes, blockStart + static_cast<int64_t>(block->size()));
    if (copyEnd <= copyStart)
      break;

    const size_t blockOffset = static_cast<size_t>(copyStart - blockStart);
    const size_t chunk = static_cast<size_t>(copyEnd - copyStart);
    std::memcpy(buffer + copied, block->data() + blockOffset, chunk);
    copied += static_cast<int64_t>(chunk);
  }

  if (copied <= 0)
    return -1;

  return static_cast<int>(copied / BLURAY_SECTOR_SIZE);
}

CBlurayIsoSession::BlockPtr CBlurayIsoSession::GetBlock(int64_t blockIndex)
{
  // Hit/miss and LRU bookkeeping are owned by the shared store.
  return CBlurayBlockCache::Instance().Get(m_source, m_version, blockIndex);
}

CBlurayIsoSession::BlockPtr CBlurayIsoSession::ReadBlock(int64_t blockIndex)
{
  if (!IsValidBlockIndex(blockIndex) || !m_readCallback)
    return nullptr;

  auto block = std::make_shared<std::vector<uint8_t>>(m_blockSize);

  const int64_t offset = blockIndex * static_cast<int64_t>(m_blockSize);
  const size_t bytesToRead =
      static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(m_blockSize),
                                            m_sourceLength - offset));
  const int64_t bytesRead = m_readCallback(offset, block->data(), bytesToRead);
  if (bytesRead <= 0)
    return nullptr;

  if (static_cast<size_t>(bytesRead) > bytesToRead)
  {
    CLog::Log(LOGERROR, "{}::{} - callback returned {} bytes, expected at most {}",
              LOG_TAG, __FUNCTION__, bytesRead, bytesToRead);
    return nullptr;
  }

  block->resize(static_cast<size_t>(bytesRead));
  return block;
}

CBlurayIsoSession::BlockPtr CBlurayIsoSession::LoadBlock(int64_t blockIndex)
{
  auto block = ReadBlock(blockIndex);
  if (!block)
    return nullptr;

  InsertBlock(blockIndex, block);
  ++m_syncBlockLoads;

  return block;
}

void CBlurayIsoSession::InsertBlock(int64_t blockIndex, BlockPtr block)
{
  if (!block)
    return;

  CBlurayBlockCache::Instance().Put(m_source, m_version, blockIndex, block->data(), block->size());
}

void CBlurayIsoSession::LogStats(const char* reason)
{
  CLog::Log(LOGDEBUG,
            "{}::{} - {} detail: reads={} reqBlocks={} blockHits={} misses={} syncLoads={}",
            LOG_TAG, __FUNCTION__, reason, m_readRequests.load(), m_requestedBlocks.load(),
            m_blockHits.load(), m_blockMisses.load(), m_syncBlockLoads.load());
}

bool CBlurayIsoSession::IsValidBlockIndex(int64_t blockIndex) const
{
  return blockIndex >= 0 && blockIndex <= GetMaxBlockIndex();
}

int64_t CBlurayIsoSession::GetMaxBlockIndex() const
{
  if (m_sourceLength <= 0)
    return -1;
  return (m_sourceLength - 1) / static_cast<int64_t>(m_blockSize);
}
