/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

class CBlurayIsoCache
{
public:
  static constexpr size_t BLOCK_SIZE = 2048;

  struct Config
  {
    size_t pageSize{256 * 1024};
    size_t maxBytes{64 * 1024 * 1024};
  };

  using ReadCallback = std::function<int64_t(int64_t offset, uint8_t* buffer, size_t size)>;

  CBlurayIsoCache(int64_t sourceLength,
                  ReadCallback readCallback,
                  Config config);
  ~CBlurayIsoCache();

  void Start();
  void Stop();
  int ReadBlocks(uint8_t* buffer, int lba, int numBlocks);

private:
  struct Page
  {
    std::vector<uint8_t> data;
    size_t validBytes{0};
  };

  using PagePtr = std::shared_ptr<Page>;

  struct CacheSlot
  {
    std::list<int64_t>::iterator lruIt;
    PagePtr page;
  };

  PagePtr GetPage(int64_t pageIndex);
  PagePtr LoadPage(int64_t pageIndex);
  void InsertPage(int64_t pageIndex, PagePtr page);
  void TouchPageUnlocked(std::unordered_map<int64_t, CacheSlot>::iterator it);
  void TrimUnlocked();
  void LogStats(const char* reason);

  bool IsValidPageIndex(int64_t pageIndex) const;
  int64_t GetMaxPageIndex() const;

  Config m_config;
  ReadCallback m_readCallback;
  int64_t m_sourceLength{0};
  size_t m_maxPages{1};
  bool m_started{false};

  std::mutex m_cacheMutex;
  std::list<int64_t> m_lruOrder;
  std::unordered_map<int64_t, CacheSlot> m_pages;

  std::atomic<uint64_t> m_readRequests{0};
  std::atomic<uint64_t> m_requestedBlocks{0};
  std::atomic<uint64_t> m_pageHits{0};
  std::atomic<uint64_t> m_pageMisses{0};
  std::atomic<uint64_t> m_syncPageLoads{0};
  std::atomic<uint64_t> m_evictions{0};
};
