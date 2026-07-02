/*
 *  Copyright (C) 2025 Team CoreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "IFile.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef void CURL_HANDLE;
typedef void CURLM;
struct curl_slist;

class CURL;

namespace XFILE
{

/**
 * High-performance HTTP streaming engine for ISO disc images.
 *
 * Features:
 * - Background worker thread with RingBuffer for continuous prefetch
 * - Global LRU block cache (CCurlFileLRUCache) shared across file instances
 * - Deferred close: worker stays alive ~200ms for rapid open/close cycles
 * - Transparent 302 redirect handling with effective URL tracking
 * - Cross-domain credential protection for redirected URLs
 */
class CCurlFileEngine
{
public:
  CCurlFileEngine();
  ~CCurlFileEngine();

  // --- Lifecycle ---
  bool Open(const CURL& url);
  void Close();

  // --- I/O ---
  ssize_t Read(void* buffer, size_t size);
  int64_t Seek(int64_t position, int whence);

  // --- Queries ---
  int64_t GetPosition() const { return m_logicalPos; }
  int64_t GetLength() const { return m_totalSize; }
  bool IsRangeSupported() const { return m_supportRange; }
  bool IsIsoFile() const { return m_isIso; }
  time_t GetModTime() const { return m_modTime; }
  const std::string& GetFileUrl() const { return m_fileUrl; }

  // --- ISO deferred close ---
  void ResetForReuse();

  // --- Configuration ---
  void SetBufferSize(size_t bytes) { m_ringBufferSize = bytes; }

  // --- Stat (HEAD-only info fetch, no worker) ---
  int Stat(const CURL& url, struct __stat64* buffer);

private:
  // --- Worker ---
  void StartWorker();
  void WorkerLoop();
  void StopWorker();

  // --- HTTP helpers ---
  static std::string GetExtensionFromUrl(const std::string& url);
  static std::string FixDavProtocol(const std::string& url);
  static std::string ExtractHost(const std::string& url);
  static bool EqualsNoCase(const std::string& a, const std::string& b);

  void UpdateEffectiveUrl(CURL_HANDLE* curl, const std::string& originalUrl);
  void SetupBaseCurlOptions(CURL_HANDLE* curl, const std::string& targetUrl);
  void SetupStatHeadOptions(CURL_HANDLE* curl, const std::string& targetUrl);
  void SetupWorkerDownloadOptions(CURL_HANDLE* curl, const std::string& targetUrl, int64_t start);
  bool DownloadRange(CURL_HANDLE* curl, int64_t start, int64_t length, std::vector<uint8_t>& buf);
  void LogStats(const char* reason);
  // Signal the worker to stop (sets flags + notifies cv) WITHOUT joining the
  // thread. The worker may be blocked in curl_easy_perform with its own
  // timeout; join on the call site would stall the caller for that long.
  // Use StopWorker() (which calls RequestStopWorker + joins) on shutdown.
  void RequestStopWorker();
  void InvalidateCdnAndFallBackToSource();

  // --- Parse Kodi URL protocol options (after '|') ---
  void ParseProtocolOptions(const CURL& url);

  // --- RingBuffer helpers ---

  // --- Callbacks ---
  static size_t WorkerWriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
  size_t HandleWorkerWrite(void* contents, size_t size);
  static size_t CacheWriteCallback(void* contents, size_t size, size_t nmemb, void* userp);

  // ------------------------------------------------------------------
  // State
  // ------------------------------------------------------------------
  std::string m_fileUrl;       // Original URL
  std::string m_effectiveUrl;  // After 302 redirects
  std::string m_userName;
  std::string m_password;
  std::string m_userAgent;
  std::string m_referer;
  std::string m_cookie;
  std::string m_httpAuth;      // any/anysafe/digest/ntlm/basic
  std::string m_cipherList;    // SSL cipher list
  std::string m_customRequest; // CURLOPT_CUSTOMREQUEST
  curl_slist* m_customHeaders = nullptr;

  // Protocol options
  int m_redirectLimit = 5;
  int m_connectTimeout = 0;
  bool m_seekable = true;
  bool m_failOnError = false;
  bool m_verifyPeer = true;

  // File info
  bool m_isIso = false;
  bool m_isDirectory = false;
  bool m_supportRange = true;
  int64_t m_totalSize = 0;
  int64_t m_logicalPos = 0;
  time_t m_modTime = 0;

  // Worker thread
  std::thread m_workerThread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_eof{false};
  std::atomic<bool> m_hasError{false};
  std::atomic<bool> m_abortTransfer{false};
  std::atomic<bool> m_triggerReset{false};
  std::atomic<int64_t> m_resetTargetPos{0};
  std::atomic<int64_t> m_downloadPos{0};
  std::atomic<int> m_cdnFallbackCount{0};     // CDN→original URL fallback guard counter
  std::chrono::steady_clock::time_point m_lastDiag{}; // Throttle for Read() wait-loop heartbeat (per-instance)

  // RingBuffer
  std::vector<uint8_t> m_ringBuffer;
  size_t m_ringBufferSize = 64 * 1024 * 1024; // 64MB default
  size_t m_rbHead = 0;
  size_t m_rbTail = 0;
  size_t m_rbAvailable = 0;
  std::atomic<int64_t> m_rbLogicalStart{0}; // Absolute byte position of data at m_rbTail
  std::mutex m_rbMutex;
  std::condition_variable m_rbCvReader;
  std::condition_variable m_rbCvWriter;

  // Network timeouts
  long m_netConnectTimeoutSec = 10;
  long m_netLowSpeedTimeSec = 15;
  long m_netWorkerLowSpeedTimeSec = 15;
  long m_netReadTimeoutSec = 20;
  long m_netRangeTotalTimeoutSec = 20;
  int m_netMaxRetries = 5;

  std::atomic<uint64_t> m_readRequests{0};
  std::atomic<uint64_t> m_requestedBytes{0};
  std::atomic<uint64_t> m_lruHits{0};
  std::atomic<uint64_t> m_lruMisses{0};
  std::atomic<uint64_t> m_lruStores{0};
  std::atomic<uint64_t> m_workerStarts{0};
  std::atomic<uint64_t> m_workerResets{0};
  std::atomic<uint64_t> m_downloadRangeRequests{0};
};

} // namespace XFILE

// Global CDN cache (used by CCurlFile for Stat)
bool TryStatCache(const std::string& origUrl, std::string& cdnUrl, int64_t& fileSize);
void ClearStatCache(const std::string& origUrl);

// Deferred close cache for ISO fast reopen (used by CCurlFile)
std::unique_ptr<XFILE::CCurlFileEngine> TryReuseEngine(const std::string& urlKey);
void CacheEngineForReuse(const std::string& urlKey, std::unique_ptr<XFILE::CCurlFileEngine> engine);
