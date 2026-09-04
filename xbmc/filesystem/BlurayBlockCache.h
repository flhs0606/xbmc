/*
 *  Copyright (C) 2025 Team CoreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * Global, per-source block LRU store shared by every Blu-ray ISO reader
 * (HTTP via CCurlFileEngine, or SMB/NFS/local via CBlurayIsoSession).
 *
 * Blocks are raw bytes of the disc image keyed by (source, block_num),
 * where a "source" is one image URL/path and "block_num" is the byte offset
 * divided by the (fixed) block size.
 *
 * To keep per-entry memory low the full source URL is stored only once in a
 * small registry; every cached block then carries a compact 64-bit source id
 * instead of a duplicated URL string. Opening the same source with a
 * different version (image length / mtime) transparently invalidates that
 * source's blocks, so cross-session reuse (e.g. Blu-ray picker -> player) is
 * automatic while stale data from a changed file is dropped.
 *
 * Block size and total capacity are configurable (one shared setting group),
 * defaults 1MB / 128MB. LRU eviction when capacity is exceeded.
 *
 * Thread-safe: single global mutex. Block data is returned via shared_ptr so
 * callers can read while the mutex is released.
 */
class CBlurayBlockCache
{
public:
  static CBlurayBlockCache& Instance();

  /** Look up a block. Returns nullptr on miss (or when the source's version
   *  no longer matches what was Put). */
  std::shared_ptr<std::vector<uint8_t>> Get(const std::string& url,
                                             int64_t version,
                                             int64_t blockNum);

  /** Insert a block. May evict the LRU entry if the cache is full. A Put for
   *  an unknown source registers it; a Put with a different version than the
   *  registered one purges that source's old blocks first. */
  void Put(const std::string& url,
           int64_t version,
           int64_t blockNum,
           const uint8_t* data,
           size_t size);

  /** Configure block size and total capacity (triggers full clear on block
   *  size change). */
  void SetLimits(size_t blockSize, size_t totalSize);

  /** Current block size in bytes. Block indices are relative to this. */
  size_t GetBlockSize();

  /** Clear everything (blocks + source registry). */
  void Clear();

private:
  CBlurayBlockCache() = default;
  ~CBlurayBlockCache() = default;
  CBlurayBlockCache(const CBlurayBlockCache&) = delete;
  CBlurayBlockCache& operator=(const CBlurayBlockCache&) = delete;

  struct SourceEntry
  {
    int64_t sourceId{0};
    int64_t version{0};
  };

  void PurgeSourceLocked(int64_t sourceId);

  struct Key
  {
    int64_t sourceId{0};
    int64_t blockNum{0};

    bool operator==(const Key& o) const;
  };

  struct KeyHash
  {
    size_t operator()(const Key& k) const;
  };

  std::mutex m_mutex;
  std::unordered_map<std::string, SourceEntry> m_sources;
  int64_t m_nextSourceId{0};

  std::list<Key> m_lruOrder;
  std::unordered_map<Key,
                     std::pair<std::list<Key>::iterator, std::shared_ptr<std::vector<uint8_t>>>,
                     KeyHash> m_blocks;

  size_t m_blockSize{1024 * 1024};
  size_t m_maxBlocks{64};
};
