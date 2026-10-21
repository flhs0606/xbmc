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
#include <memory>
#include <string>
#include <vector>

/**
 * Per-open Blu-ray ISO read session.
 *
 * Converts libbluray LBA requests into whole-block reads of the disc image
 * and serves them from the process-global CBlurayBlockCache (keyed by the
 * source URL/path so the Blu-ray picker (CBlurayDirectory) and the player
 * (CDVDInputStreamBluray) share blocks of the same image automatically).
 *
 * This class owns no block storage: hit/miss/eviction live in
 * CBlurayBlockCache. It only keeps per-session geometry (block size) and the
 * transport read callback.
 */
class CBlurayIsoSession
{
public:
  static constexpr size_t BLURAY_SECTOR_SIZE = 2048;

  struct Config
  {
    size_t blockSize{1024 * 1024};
    size_t maxBytes{128 * 1024 * 1024};
  };

  using ReadCallback = std::function<int64_t(int64_t offset, uint8_t* buffer, size_t size)>;
  // Block payload is exactly what CBlurayBlockCache stores (vector<uint8_t>).
  using BlockPtr = std::shared_ptr<std::vector<uint8_t>>;

  CBlurayIsoSession(std::string source,
                    int64_t sourceLength,
                    int64_t version,
                    ReadCallback readCallback,
                    Config config);
  ~CBlurayIsoSession();

  void Start();
  void Stop();
  int ReadBlocks(uint8_t* buffer, int lba, int numBlocks);

private:
  // Preload the UDF head/tail regions (head descriptors + tail anchor) into the
  // shared store. Called from Start() only — needs the store geometry set up.
  void WarmUp();
  BlockPtr GetBlock(int64_t blockIndex);
  BlockPtr ReadBlock(int64_t blockIndex);
  BlockPtr LoadBlock(int64_t blockIndex);
  void InsertBlock(int64_t blockIndex, BlockPtr block);
  void LogStats(const char* reason);

  bool IsValidBlockIndex(int64_t blockIndex) const;
  int64_t GetMaxBlockIndex() const;

  Config m_config;
  ReadCallback m_readCallback;
  std::string m_source; // shared-store key: actual opened image URL/path
  int64_t m_sourceLength{0};
  int64_t m_version{0}; // shared-store version (image length for now)
  size_t m_blockSize{1024 * 1024};
  bool m_started{false};

  std::atomic<uint64_t> m_readRequests{0};
  std::atomic<uint64_t> m_requestedBlocks{0};
  std::atomic<uint64_t> m_blockHits{0};
  std::atomic<uint64_t> m_blockMisses{0};
  std::atomic<uint64_t> m_syncBlockLoads{0};
};
