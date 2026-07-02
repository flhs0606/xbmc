/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

class CBlurayIsoCache
{
public:
  static constexpr size_t BLURAY_SECTOR_SIZE = 2048;

  struct Config
  {
    size_t blockSize{1024 * 1024};
    size_t maxBytes{128 * 1024 * 1024};
  };

  using ReadCallback = std::function<int64_t(int64_t offset, uint8_t* buffer, size_t size)>;

  CBlurayIsoCache(int64_t sourceLength,
                  ReadCallback readCallback,
                  Config config);
  ~CBlurayIsoCache();

  void Start();
  void Stop();
  void NotifySeek();
  int ReadBlocks(uint8_t* buffer, int lba, int numBlocks);

private:
  struct Block
  {
    std::vector<uint8_t> data;
    size_t validBytes{0};
  };

  using BlockPtr = std::shared_ptr<Block>;

  struct CacheSlot
  {
    std::list<int64_t>::iterator lruIt;
    BlockPtr block;
  };

  BlockPtr GetBlock(int64_t blockIndex);
  BlockPtr ReadBlock(int64_t blockIndex);
  BlockPtr LoadBlock(int64_t blockIndex);
  void InsertBlock(int64_t blockIndex, BlockPtr block);
  void TouchBlockUnlocked(std::unordered_map<int64_t, CacheSlot>::iterator it);
  void TrimUnlocked();
  void LogStats(const char* reason);

  bool IsValidBlockIndex(int64_t blockIndex) const;
  int64_t GetMaxBlockIndex() const;

  // Burst prefetch: background thread loads subsequent blocks to warm cache
  void PrefetchWorker();
  void SchedulePrefetch(int64_t fromBlock, int64_t toBlock);
  void ResetPrefetchLocked();
  bool IsPrefetchGenerationStale(uint64_t generation);
  void TrackDemandRead(int64_t firstBlock, int64_t lastBlock);

  static constexpr int64_t BURST_AHEAD = 4; // blocks to prefetch ahead
  static constexpr int64_t WARMUP_TAIL = 5; // conservative open-time metadata warmup
  static constexpr int64_t DISCONTINUITY_AHEAD = 128; // tolerate metadata jumps before clearing queue
  static constexpr int64_t DISCONTINUITY_BEHIND = 128;

  Config m_config;
  ReadCallback m_readCallback;
  int64_t m_sourceLength{0};
  size_t m_maxBlocks{1};
  bool m_started{false};

  std::mutex m_cacheMutex;
  std::list<int64_t> m_lruOrder;
  std::unordered_map<int64_t, CacheSlot> m_blocks;

  // Prefetch thread
  std::thread m_prefetchThread;
  std::atomic<bool> m_prefetchRunning{false};
  std::mutex m_prefetchMutex;
  std::condition_variable m_prefetchCv;
  std::deque<int64_t> m_prefetchQueue;
  uint64_t m_prefetchGeneration{0};
  std::optional<std::pair<int64_t, int64_t>> m_lastDemandRange;

  std::atomic<uint64_t> m_readRequests{0};
  std::atomic<uint64_t> m_requestedBlocks{0};
  std::atomic<uint64_t> m_blockHits{0};
  std::atomic<uint64_t> m_blockMisses{0};
  std::atomic<uint64_t> m_syncBlockLoads{0};
  std::atomic<uint64_t> m_evictions{0};
  std::atomic<uint64_t> m_prefetchBlockLoads{0};
  std::atomic<uint64_t> m_prefetchInvalidations{0};
  std::atomic<uint64_t> m_prefetchStaleDiscards{0};
};
