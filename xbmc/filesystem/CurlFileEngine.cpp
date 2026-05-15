/*
 *  Copyright (C) 2025 Team CoreELEC
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "CurlFileEngine.h"
#include "CurlFileLRUCache.h"
#include "DllLibCurl.h"
#include "File.h"
#include "URL.h"
#include "ServiceBroker.h"
#include "filesystem/SpecialProtocol.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/CharsetConverter.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <map>
#include <vector>
#include <unordered_map>
#include <sys/stat.h>

using namespace XFILE;
using namespace XCURL;

// Global CDN URL dedup + file size cache (TTL: 30 min, vfs.stream.fast pattern)
static constexpr auto CDN_CACHE_TTL = std::chrono::minutes(30);

struct CdnCacheEntry {
  std::string cdnUrl;
  int64_t fileSize;
  std::chrono::steady_clock::time_point cachedAt;

  bool expired() const {
    return std::chrono::steady_clock::now() - cachedAt > CDN_CACHE_TTL;
  }
};

static std::mutex g_statMutex;
static std::unordered_map<std::string, CdnCacheEntry> g_cdnCache; // url -> CdnCacheEntry
static std::unordered_map<std::string, bool> g_statInFlight;

bool TryStatCache(const std::string& origUrl, std::string& cdnUrl, int64_t& fileSize)
{
  std::lock_guard<std::mutex> lk(g_statMutex);
  auto it = g_cdnCache.find(origUrl);
  if (it != g_cdnCache.end()) {
    if (it->second.expired()) {
      CLog::Log(LOGDEBUG, "{}::cache - CDN entry expired for {}", "CCurlFileEngine", origUrl);
      g_cdnCache.erase(it);
      return false;
    }
    cdnUrl = it->second.cdnUrl;
    fileSize = it->second.fileSize;
    return true;
  }
  return false;
}

static void SaveStatCache(const std::string& origUrl, const std::string& cdnUrl, int64_t fileSize)
{
  std::lock_guard<std::mutex> lk(g_statMutex);
  g_cdnCache[origUrl] = {cdnUrl, fileSize, std::chrono::steady_clock::now()};
}

// ★ Clear CDN cache entry when 4xx error indicates stale redirect token
void ClearStatCache(const std::string& origUrl)
{
  std::lock_guard<std::mutex> lk(g_statMutex);
  auto it = g_cdnCache.find(origUrl);
  if (it != g_cdnCache.end()) {
    CLog::Log(LOGDEBUG, "{}::cache - clearing CDN entry due to HTTP error: {}", "CCurlFileEngine", origUrl);
    g_cdnCache.erase(it);
  }
}

static bool TryLockStat(const std::string& origUrl)
{
  std::lock_guard<std::mutex> lk(g_statMutex);
  if (g_statInFlight.count(origUrl)) return false;
  g_statInFlight[origUrl] = true;
  return true;
}

static void UnlockStat(const std::string& origUrl)
{
  std::lock_guard<std::mutex> lk(g_statMutex);
  g_statInFlight.erase(origUrl);
}

// Global CURL handle pool (vfs.stream.fast pattern)
static std::vector<CURL_HANDLE*> g_handlePool;
static std::mutex g_handlePoolMutex;
static CURL_HANDLE* PoolGet() {
  std::lock_guard<std::mutex> lk(g_handlePoolMutex);
  if (!g_handlePool.empty()) { auto h = g_handlePool.back(); g_handlePool.pop_back(); return h; }
  return g_curlInterface.easy_init();
}
static void PoolReturn(CURL_HANDLE* h) {
  if (!h) return;
  std::lock_guard<std::mutex> lk(g_handlePoolMutex);
  g_curlInterface.easy_reset(h);
  if (g_handlePool.size() < 5) g_handlePool.push_back(h);
  else g_curlInterface.easy_cleanup(h);
}

namespace { constexpr const char* LOG_TAG="CCurlFileEngine"; static constexpr size_t LRU_BLOCK_SIZE=512*1024; static constexpr size_t LRU_TOTAL_SIZE=100*1024*1024; }

// =========================================================================
// ISO 延迟关闭缓存 (Deferred Close Cache)
// =========================================================================
// 蓝光 ISO 播放时 libbluray 快速 Open/Close 同一文件数十次。
// Close 时不销毁 Engine，放入缓存保持 Worker 运行 200ms。
// 下次 Open 同一 URL 直接复用。
// =========================================================================
static constexpr int ENGINE_CLOSE_DELAY_MS = 200;

struct CachedEngine {
  std::unique_ptr<CCurlFileEngine> engine;
  std::chrono::steady_clock::time_point expire_at;
};

static std::mutex g_engineCacheMutex;
static std::unordered_map<std::string, CachedEngine> g_engineCache;
static std::condition_variable g_engineCacheCv;
static std::thread g_engineCleanupThread;
static bool g_engineCleanupRunning = false;

static void EngineCleanupThreadFunc()
{
  std::unique_lock<std::mutex> lock(g_engineCacheMutex);
  while (g_engineCleanupRunning)
  {
    if (g_engineCache.empty()) { g_engineCacheCv.wait(lock); continue; }
    auto earliest = std::min_element(g_engineCache.begin(), g_engineCache.end(),
      [](const auto& a, const auto& b) { return a.second.expire_at < b.second.expire_at; });
    g_engineCacheCv.wait_until(lock, earliest->second.expire_at);
    auto now = std::chrono::steady_clock::now();
    for (auto it = g_engineCache.begin(); it != g_engineCache.end();)
    {
      if (now >= it->second.expire_at) {
        CLog::Log(LOGDEBUG, "{}::cache - deferred close expired, destroying engine for {}", LOG_TAG, it->first);
        it->second.engine->Close();
        it = g_engineCache.erase(it);
      } else ++it;
    }
  }
}

static void EnsureEngineCleanupThread()
{
  if (!g_engineCleanupRunning) {
    g_engineCleanupRunning = true;
    g_engineCleanupThread = std::thread(EngineCleanupThreadFunc);
  }
}

// Try to reuse a cached Engine for the same URL
std::unique_ptr<CCurlFileEngine> TryReuseEngine(const std::string& urlKey)
{
  std::lock_guard<std::mutex> lock(g_engineCacheMutex);
  auto it = g_engineCache.find(urlKey);
  if (it != g_engineCache.end()) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      it->second.expire_at - std::chrono::steady_clock::now()).count();
    auto engine = std::move(it->second.engine);
    g_engineCache.erase(it);
    engine->ResetForReuse();
    CLog::Log(LOGDEBUG, "{}::cache - reused engine (remaining {}ms)", LOG_TAG, remaining);
    return engine;
  }
  return nullptr;
}

// Put engine into deferred close cache
void CacheEngineForReuse(const std::string& urlKey, std::unique_ptr<CCurlFileEngine> engine)
{
  std::lock_guard<std::mutex> lock(g_engineCacheMutex);
  EnsureEngineCleanupThread();
  // Replace if same URL already cached
  auto it = g_engineCache.find(urlKey);
  if (it != g_engineCache.end()) { it->second.engine->Close(); g_engineCache.erase(it); }
  auto expire = std::chrono::steady_clock::now() + std::chrono::milliseconds(ENGINE_CLOSE_DELAY_MS);
  g_engineCache[urlKey] = {std::move(engine), expire};
  g_engineCacheCv.notify_one();
  CLog::Log(LOGDEBUG, "{}::cache - deferred close ({}ms grace)", LOG_TAG, ENGINE_CLOSE_DELAY_MS);
}

// =========================================================================
// Construction / Destruction
// =========================================================================

CCurlFileEngine::CCurlFileEngine()
{
  CCurlFileLRUCache::Instance().SetLimits(LRU_BLOCK_SIZE, LRU_TOTAL_SIZE);
}

CCurlFileEngine::~CCurlFileEngine()
{
  Close();
}

// =========================================================================
// URL Helpers
// =========================================================================

std::string CCurlFileEngine::GetExtensionFromUrl(const std::string& url)
{
  CURLU* h = curl_url(); if (!h) return {};
  std::string ext; char* path = nullptr;
  if (curl_url_set(h, CURLUPART_URL, url.c_str(), CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK &&
      curl_url_get(h, CURLUPART_PATH, &path, 0) == CURLUE_OK && path) {
    std::string p(path); size_t ls = p.rfind('/');
    std::string fn = (ls == std::string::npos) ? p : p.substr(ls + 1);
    size_t dp = fn.rfind('.'); if (dp != std::string::npos) { ext = fn.substr(dp+1); std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower); }
    curl_free(path);
  }
  curl_url_cleanup(h); return ext;
}

std::string CCurlFileEngine::FixDavProtocol(const std::string& url)
{
  std::string f = url;
  if (f.rfind("dav://", 0) == 0) f.replace(0, 6, "http://");
  else if (f.rfind("davs://", 0) == 0) f.replace(0, 7, "https://");
  return f;
}

std::string CCurlFileEngine::ExtractHost(const std::string& url)
{
  size_t p = url.find("://"); if (p == std::string::npos) return {};
  size_t s = p + 3, at = url.find('@', s), sl = url.find('/', s);
  if (at != std::string::npos && (sl == std::string::npos || at < sl)) s = at + 1;
  size_t e = url.find('/', s); return (e == std::string::npos) ? url.substr(s) : url.substr(s, e - s);
}

bool CCurlFileEngine::EqualsNoCase(const std::string& a, const std::string& b)
{
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}

// =========================================================================
// CURL Option Helpers
// =========================================================================

void CCurlFileEngine::SetupBaseCurlOptions(CURL_HANDLE* curl, const std::string& targetUrl)
{
  g_curlInterface.easy_setopt(curl, CURLOPT_URL, targetUrl.c_str());
  // ★ vfs.stream.fast: explicit HTTP/1.1 for max compatibility with CDNs
  g_curlInterface.easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  g_curlInterface.easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  g_curlInterface.easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  g_curlInterface.easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  g_curlInterface.easy_setopt(curl, CURLOPT_MAXREDIRS, m_redirectLimit);
  g_curlInterface.easy_setopt(curl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
  g_curlInterface.easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  g_curlInterface.easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 15L);
  g_curlInterface.easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 5L);
  g_curlInterface.easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
  g_curlInterface.easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
  g_curlInterface.easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
  g_curlInterface.easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_connectTimeout > 0 ? m_connectTimeout : m_netConnectTimeoutSec);
  g_curlInterface.easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  g_curlInterface.easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_netLowSpeedTimeSec);
  if (!m_userAgent.empty()) g_curlInterface.easy_setopt(curl, CURLOPT_USERAGENT, m_userAgent.c_str());
  else g_curlInterface.easy_setopt(curl, CURLOPT_USERAGENT, CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_userAgent.c_str());
  g_curlInterface.easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_verifyPeer ? 1L : 0L);
  g_curlInterface.easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_verifyPeer ? 2L : 0L);
  if (!m_cipherList.empty()) g_curlInterface.easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST, m_cipherList.c_str());
  // ★ vfs.stream.fast: don't send credentials to a different host (e.g. CDN after redirect)
  bool shouldSendAuth = true;
  {
    std::string hostOrigin = ExtractHost(m_fileUrl);
    std::string hostTarget = ExtractHost(targetUrl);
    if (!hostOrigin.empty() && !hostTarget.empty() && !EqualsNoCase(hostOrigin, hostTarget))
      shouldSendAuth = false;
  }
  if (!m_userName.empty() && shouldSendAuth) { g_curlInterface.easy_setopt(curl, CURLOPT_USERNAME, m_userName.c_str()); if (!m_password.empty()) g_curlInterface.easy_setopt(curl, CURLOPT_PASSWORD, m_password.c_str()); }
  bool authSet = false;
  if (!m_httpAuth.empty()) { if (m_httpAuth=="any") { g_curlInterface.easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY); authSet=true; } else if (m_httpAuth=="anysafe") { g_curlInterface.easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANYSAFE); authSet=true; } else if (m_httpAuth=="digest") { g_curlInterface.easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST); authSet=true; } else if (m_httpAuth=="ntlm") { g_curlInterface.easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_NTLM); authSet=true; } else if (m_httpAuth=="basic") { g_curlInterface.easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC); authSet=true; } }
  if (!authSet) { if (!m_userName.empty() && shouldSendAuth) g_curlInterface.easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC); else g_curlInterface.easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY); }
  if (!m_cookie.empty()) g_curlInterface.easy_setopt(curl, CURLOPT_COOKIE, m_cookie.c_str());
  if (!m_customRequest.empty()) g_curlInterface.easy_setopt(curl, CURLOPT_CUSTOMREQUEST, m_customRequest.c_str());
  if (m_failOnError) g_curlInterface.easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  if (m_customHeaders) g_curlInterface.easy_setopt(curl, CURLOPT_HTTPHEADER, m_customHeaders);
  std::string ca = CSpecialProtocol::TranslatePath(CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_caTrustFile);
  if (!ca.empty() && XFILE::CFile::Exists(ca)) g_curlInterface.easy_setopt(curl, CURLOPT_CAINFO, ca.c_str());
}

void CCurlFileEngine::SetupStatHeadOptions(CURL_HANDLE* curl, const std::string& targetUrl)
{
  SetupBaseCurlOptions(curl, targetUrl);
  // ★ vfs.stream.fast: FOLLOWLOCATION remains 1 (set in SetupBaseCurlOptions).
  // HEAD follows 302→CDN, response from CDN gives Content-Length.
  g_curlInterface.easy_setopt(curl, CURLOPT_NOBODY, 1L);
  g_curlInterface.easy_setopt(curl, CURLOPT_FILETIME, 1L);
}

void CCurlFileEngine::SetupWorkerDownloadOptions(CURL_HANDLE* curl, const std::string& targetUrl, int64_t start)
{
  SetupBaseCurlOptions(curl, targetUrl);
  g_curlInterface.easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_netWorkerLowSpeedTimeSec);
  g_curlInterface.easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  g_curlInterface.easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
      +[](void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        auto* e = static_cast<CCurlFileEngine*>(clientp);
        if (e && !e->m_running) return 1;
        if (e && e->m_abortTransfer.load()) return 1;
        return 0;
      });
  g_curlInterface.easy_setopt(curl, CURLOPT_XFERINFODATA, this);
  g_curlInterface.easy_setopt(curl, CURLOPT_WRITEFUNCTION, WorkerWriteCallback);
  g_curlInterface.easy_setopt(curl, CURLOPT_WRITEDATA, this);
  g_curlInterface.easy_setopt(curl, CURLOPT_BUFFERSIZE, 1L * 1024L * 1024L);
  // ★ vfs: no HEADERFUNCTION/HEADERDATA in worker
  if (m_supportRange) { std::string rs = std::to_string(start) + "-"; g_curlInterface.easy_setopt(curl, CURLOPT_RANGE, rs.c_str()); }
  else { g_curlInterface.easy_setopt(curl, CURLOPT_RANGE, NULL); g_curlInterface.easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)0); }
}

// =========================================================================
// Effective URL tracking (identical to vfs.stream.fast approach)
// =========================================================================

void CCurlFileEngine::UpdateEffectiveUrl(CURL_HANDLE* curl, const std::string& origUrl, const char* ctx)
{
  char* eff = nullptr;
  g_curlInterface.easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
  if (!eff) return;
  std::string effStr(eff);
  if (origUrl == effStr) return;

  if (m_effectiveUrl != effStr) {
    CLog::Log(LOGDEBUG, "{}::{} - {} redirect: {} -> {}", LOG_TAG, ctx, ctx, origUrl, effStr);
    m_effectiveUrl = effStr;
    int64_t newSize = 0;
    struct curl_header* h = nullptr;
    if (curl_easy_header(curl, "Content-Range", 0, CURLH_HEADER, -1, &h) == CURLHE_OK && h && h->value) {
      std::string cr(h->value); size_t sl = cr.find('/');
      if (sl != std::string::npos) { std::string t = cr.substr(sl + 1); if (t != "*" && !t.empty()) try { newSize = std::stoll(t); } catch(...) {} }
    }
    if (newSize <= 0) { long c = 0; g_curlInterface.easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &c);
      if (c == 200) { curl_off_t cl = -1; if (g_curlInterface.easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl) == CURLE_OK && cl > 0) newSize = (int64_t)cl; } }
    if (newSize > 0 && newSize != m_totalSize) { CLog::Log(LOGDEBUG, "{}::{} - redirect target size: {} (was {})", LOG_TAG, ctx, newSize, m_totalSize); m_totalSize = newSize; }
    SaveStatCache(m_fileUrl, effStr, m_totalSize > 0 ? m_totalSize : 0);
  }
}

// =========================================================================
// Parse Kodi URL protocol options
// =========================================================================

void CCurlFileEngine::ParseProtocolOptions(const CURL& url)
{
  m_referer.clear(); m_httpAuth.clear(); m_cookie.clear(); m_cipherList.clear(); m_customRequest.clear();
  m_connectTimeout = 0; m_redirectLimit = 5; m_seekable = true; m_failOnError = false; m_verifyPeer = true;
  if (m_customHeaders) { g_curlInterface.slist_free_all(m_customHeaders); m_customHeaders = nullptr; }

  std::map<std::string, std::string> opts; url.GetProtocolOptions(opts);
  for (const auto& [name, value] : opts) {
    std::string k = name; StringUtils::ToLower(k);
    if (k == "auth") { m_httpAuth = value; StringUtils::ToLower(m_httpAuth); if (m_httpAuth.empty()) m_httpAuth = "any"; }
    else if (k == "referer") m_referer = value;
    else if (k == "user-agent") m_userAgent = value;
    else if (k == "cookie") m_cookie = value;
    else if (k == "seekable" && value == "0") m_seekable = false;
    else if (k == "sslcipherlist") m_cipherList = value;
    else if (k == "connection-timeout") m_connectTimeout = strtol(value.c_str(), nullptr, 10);
    else if (k == "failonerror") m_failOnError = (value == "true");
    else if (k == "redirect-limit") m_redirectLimit = strtol(value.c_str(), nullptr, 10);
    else if (k == "verifypeer" && value == "false") m_verifyPeer = false;
    else { std::string hdr = (!k.empty() && k[0] == '!') ? name.substr(1) + ": " + value : name + ": " + value; m_customHeaders = g_curlInterface.slist_append(m_customHeaders, hdr.c_str()); }
  }
}

struct CWContext { std::vector<uint8_t>* buffer; size_t offset; size_t limit; };

size_t CCurlFileEngine::CacheWriteCallback(void* c, size_t s, size_t n, void* u) {
  auto* x = static_cast<CWContext*>(u); size_t t = s * n; if (!x || !x->buffer) return 0;
  if (x->offset + t > x->limit) t = x->limit - x->offset;
  if (t > 0) { memcpy(x->buffer->data() + x->offset, c, t); x->offset += t; } return t; }

bool CCurlFileEngine::DownloadRange(CURL_HANDLE* curl, int64_t start, int64_t length, std::vector<uint8_t>& buf)
{
  if (!curl) return false;
  buf.resize((size_t)length); int retries = 0; CURLcode res; long code = 0; char eb[CURL_ERROR_SIZE] = {};
  while (retries < m_netMaxRetries) {
    g_curlInterface.easy_reset(curl); eb[0] = 0; g_curlInterface.easy_setopt(curl, CURLOPT_ERRORBUFFER, eb);
    std::string tUrl = !m_effectiveUrl.empty() ? m_effectiveUrl : FixDavProtocol(m_fileUrl);
    SetupBaseCurlOptions(curl, tUrl);
    g_curlInterface.easy_setopt(curl, CURLOPT_RANGE, (StringUtils::Format("{}-{}", start, start+length-1)).c_str());
    g_curlInterface.easy_setopt(curl, CURLOPT_TIMEOUT, m_netRangeTotalTimeoutSec + retries*5);
    CWContext ctx{&buf, 0, (size_t)length};
    g_curlInterface.easy_setopt(curl, CURLOPT_WRITEFUNCTION, CacheWriteCallback); g_curlInterface.easy_setopt(curl, CURLOPT_WRITEDATA, &ctx); g_curlInterface.easy_setopt(curl, CURLOPT_FILETIME, 0L);
    res = g_curlInterface.easy_perform(curl); g_curlInterface.easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (res == CURLE_OK && (code == 206 || code == 200)) { buf.resize(ctx.offset); UpdateEffectiveUrl(curl, m_fileUrl, "DownloadRange"); return true; }
    CLog::Log(LOGDEBUG, "{}::DownloadRange - retry {}/{} range {}-{} code {} res {}{}{}", LOG_TAG, retries+1, m_netMaxRetries, start, start+length-1, code, (int)res, res!=CURLE_OK?" err=":"", res!=CURLE_OK?eb:"");
    ++retries; if (code == 416 || code == 406) { m_supportRange = false; return false; }
  }
  return false;
}

// =========================================================================
// Stat — HEAD request to discover file size & track 302 redirect
// (matches vfs.stream.fast: HEAD→302→CDN, save effective URL, Worker
//  uses CDN URL directly without burning the original sign token again)
// =========================================================================

int CCurlFileEngine::Stat(const CURL& url, struct __stat64* buffer)
{
  m_fileUrl     = url.Get();
  m_isIso       = (GetExtensionFromUrl(m_fileUrl) == "iso");
  m_userName    = url.GetUserName();
  m_password    = url.GetPassWord();

  /* Use global cache to avoid concurrent HEADs on same URL */
  std::string cdn;
  int64_t fs = 0;
  if (TryStatCache(m_fileUrl, cdn, fs)) {
    m_effectiveUrl = cdn;
    m_totalSize = fs > 0 ? fs : -1; m_modTime = 0; m_supportRange = true; m_seekable = true; m_isDirectory = false;
    if (buffer) { *buffer = {}; buffer->st_size = m_totalSize; buffer->st_mode = _S_IFREG; }
    return 0;
  }

  /* Only one concurrent Stat per URL */
  if (!TryLockStat(m_fileUrl)) {
    m_totalSize = -1; m_modTime = 0; m_supportRange = true; m_seekable = true; m_isDirectory = false;
    if (buffer) { *buffer = {}; buffer->st_size = -1; buffer->st_mode = _S_IFREG; }
    return 0;
  }

  std::string tUrl = FixDavProtocol(m_fileUrl);
  CURL_HANDLE* curl = PoolGet();
  if (!curl) { UnlockStat(m_fileUrl); return -1; }

  // ★ vfs.stream.fast: HEAD follows 302→CDN, extracts size + effective URL
  curl_easy_reset(curl);
  SetupStatHeadOptions(curl, tUrl);
  CURLcode res = g_curlInterface.easy_perform(curl);
  long code = 0; g_curlInterface.easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  int64_t fileSize = 0;
  if (res == CURLE_OK && code >= 200 && code < 400) {
    curl_off_t cl = -1; g_curlInterface.easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    if (cl >= 0) fileSize = (int64_t)cl;
    long ft = -1; g_curlInterface.easy_getinfo(curl, CURLINFO_FILETIME, &ft);
    if (ft > 0) m_modTime = (time_t)ft;
    m_supportRange = true;
  }
  UpdateEffectiveUrl(curl, m_fileUrl, "Stat");
  PoolReturn(curl);
  UnlockStat(m_fileUrl);

  m_totalSize = fileSize; m_isDirectory = false;
  if (!m_effectiveUrl.empty() && fileSize > 0) SaveStatCache(m_fileUrl, m_effectiveUrl, fileSize);
  if (buffer) { *buffer = {}; buffer->st_size = fileSize; buffer->st_mode = _S_IFREG; }
  CLog::Log(LOGDEBUG, "{}::Stat - size={} effective={}", LOG_TAG, fileSize, !m_effectiveUrl.empty()?m_effectiveUrl:"(none)");
  return 0;
}

// =========================================================================
// Open — Stat + Worker launch
// =========================================================================

bool CCurlFileEngine::Open(const CURL& url)
{
  m_fileUrl = url.Get();
  ParseProtocolOptions(url);

  m_isIso = true;
  m_userName = url.GetUserName();
  m_password = url.GetPassWord();

  m_totalSize = -1;
  m_seekable = true;
  m_supportRange = true;
  m_isFirstRead = true;

  // ★ vfs.stream.fast: Stat HEAD follows 302→CDN, gets size + m_effectiveUrl.
  // Worker then uses m_effectiveUrl (CDN) directly for GET with range.
  Stat(url, nullptr);

  CLog::Log(LOGDEBUG, "{}::Open - {} size={} cdn={}", LOG_TAG, url.GetRedacted(), m_totalSize, !m_effectiveUrl.empty()?m_effectiveUrl:"(none)");
  return true;
}

// =========================================================================
// Close / ResetForReuse
// =========================================================================

void CCurlFileEngine::Close() {
  StopWorker(); if (m_customHeaders) { g_curlInterface.slist_free_all(m_customHeaders); m_customHeaders = nullptr; }
  m_fileUrl.clear(); m_effectiveUrl.clear(); m_logicalPos = 0; m_totalSize = 0; m_isFirstRead = true;
}

void CCurlFileEngine::ResetForReuse() { m_logicalPos = 0; m_isFirstRead = true; m_abortTransfer = false; m_triggerReset = false; m_hasError = false; }

// =========================================================================
// Seek
// =========================================================================

// =========================================================================
// Seek — Lazy (vfs.stream.fast pattern)
// =========================================================================

int64_t CCurlFileEngine::Seek(int64_t pos, int whence)
{
  if (!m_supportRange) {
    if ((whence==SEEK_SET && pos==0) || (whence==SEEK_CUR && pos==0)) return m_logicalPos;
    return -1;
  }

  int64_t n = m_logicalPos;
  switch (whence) { case SEEK_SET: n=pos; break; case SEEK_CUR: n+=pos; break; case SEEK_END: n=(m_totalSize>0)?m_totalSize+pos:n; break; default: return -1; }
  if (n<0) n=0;
  if (m_totalSize>0 && n>m_totalSize) n=m_totalSize;
  if (n==m_logicalPos) return n;

  CLog::Log(LOGDEBUG,"{}::Seek - pos={} (was {})", LOG_TAG, n, m_logicalPos);
  m_logicalPos = n;
  // Lazy: all network work deferred to Read()
  return m_logicalPos;
}

// =========================================================================
// Read
// =========================================================================

// =========================================================================
// Read — vfs.stream.fast range-mode pattern
// =========================================================================
// For range-supported files: blocks extracted from RingBuffer into LRU.
// Teleport resets Worker when reader is far from download position.
// Never uses DownloadRange for immediate reads during playback.
// =========================================================================

ssize_t CCurlFileEngine::Read(void* buffer, size_t size)
{
  if (size==0) return 0;
  uint8_t* out = (uint8_t*)buffer;

  // --- Non-range mode: sequential ring buffer only ---
  if (!m_supportRange) {
    if (!m_workerThread.joinable()) StartWorker();
    std::unique_lock<std::mutex> lk(m_rbMutex);
    while (m_rbAvailable==0) {
      if (m_eof||m_hasError||!m_running) return m_hasError?-1:0;
      if (m_rbCvReader.wait_for(lk,std::chrono::seconds(60))==std::cv_status::timeout)
        { CLog::Log(LOGERROR,"{}::Read - timeout waiting for data",LOG_TAG); return -1; }
    }
    size_t rd=std::min(m_rbAvailable,size);
    size_t f=std::min(rd,m_ringBuffer.size()-m_rbTail);
    memcpy(out,m_ringBuffer.data()+m_rbTail,f);
    if (rd>f) memcpy(out+f,m_ringBuffer.data(),rd-f);
    m_rbTail=(m_rbTail+rd)%m_ringBuffer.size(); m_rbAvailable-=rd;
    m_logicalPos+=(int64_t)rd; m_rbCvWriter.notify_one(); return rd;
  }

  // ================================================================
  // Range mode (vfs.stream.fast pattern)
  // ================================================================
  int64_t blk = m_logicalPos / (int64_t)LRU_BLOCK_SIZE;

  // --- Step 1: LRU cache hit ---
  if (auto c=CCurlFileLRUCache::Instance().Get(m_fileUrl,m_modTime,blk)){
    int64_t bs=blk*(int64_t)LRU_BLOCK_SIZE;
    size_t off=(size_t)(m_logicalPos-bs),cs=std::min(size,c->size()-off);
    if (m_totalSize>0)cs=std::min(cs,(size_t)(m_totalSize-m_logicalPos));
    if (cs>0){memcpy(out,c->data()+off,cs);m_logicalPos+=(int64_t)cs;m_isFirstRead=false;return cs;}
  }

  // --- Step 2: Small file (<1MB) download entirely ---
  if (m_totalSize>0 && m_totalSize<=(int64_t)LRU_BLOCK_SIZE && m_isFirstRead) {
    m_isFirstRead=false;
    CURL_HANDLE* c=PoolGet(); if(c){
      std::vector<uint8_t> d((size_t)m_totalSize);
      if(DownloadRange(c,0,m_totalSize,d)){PoolReturn(c);CCurlFileLRUCache::Instance().Put(m_fileUrl,m_modTime,0,d.data(),d.size());
        size_t off=(size_t)m_logicalPos,cs=std::min(size,d.size()-off);
        if(cs>0){memcpy(out,d.data()+off,cs);m_logicalPos+=(int64_t)cs;return cs;}}
      PoolReturn(c);
    }
  }

  // --- Step 3: Lazy worker start ---
  if (!m_workerThread.joinable()){
    int64_t ap=(m_logicalPos/(int64_t)LRU_BLOCK_SIZE)*(int64_t)LRU_BLOCK_SIZE,sv=m_logicalPos;
    m_logicalPos=ap; StartWorker(); m_logicalPos=sv;
  }

  // --- Step 4: ISO tail + anchor prefetch (matches vfs) ---
  if (m_isFirstRead && m_isIso && m_totalSize>(int64_t)LRU_BLOCK_SIZE) {
    m_isFirstRead=false;
    int64_t lb=(m_totalSize-1)/(int64_t)LRU_BLOCK_SIZE;
    // Tail block
    if (lb!=blk && !CCurlFileLRUCache::Instance().Get(m_fileUrl,m_modTime,lb)){
      int64_t ls=m_totalSize-lb*(int64_t)LRU_BLOCK_SIZE;
      CLog::Log(LOGDEBUG,"{}::Read - ISO tail prefetch block#{}",LOG_TAG,lb);
      CURL_HANDLE* c=PoolGet();std::vector<uint8_t> d((size_t)ls);
      if(c&&DownloadRange(c,lb*(int64_t)LRU_BLOCK_SIZE,ls,d)){PoolReturn(c);CCurlFileLRUCache::Instance().Put(m_fileUrl,m_modTime,lb,d.data(),d.size());}
      else if(c)PoolReturn(c);
    }
    // ★ Anchor block: UDF sector 256 is at byte 524288, covered by block#0.
    //    Prefetch it while Worker is connecting — this serves initial UDF reads.
    if (blk!=0 && !CCurlFileLRUCache::Instance().Get(m_fileUrl,m_modTime,0)){
      CLog::Log(LOGDEBUG,"{}::Read - ISO anchor prefetch block#0",LOG_TAG);
      CURL_HANDLE* c=PoolGet();std::vector<uint8_t> d((size_t)LRU_BLOCK_SIZE);
      if(c&&DownloadRange(c,0,LRU_BLOCK_SIZE,d)){PoolReturn(c);CCurlFileLRUCache::Instance().Put(m_fileUrl,m_modTime,0,d.data(),d.size());}
      else if(c)PoolReturn(c);
    }
  } else { m_isFirstRead=false; }

  // --- Step 5: Wait for RingBuffer to contain needed block (vfs pattern) ---
  int64_t block_start = blk * (int64_t)LRU_BLOCK_SIZE;
  int64_t block_end = block_start + (int64_t)LRU_BLOCK_SIZE;
  if (m_totalSize>0) block_end = std::min(block_end, m_totalSize);

  while (m_running) {
    // ★ Re-check LRU: tail/anchor prefetch may have filled it
    if (auto c=CCurlFileLRUCache::Instance().Get(m_fileUrl,m_modTime,blk)){
      int64_t bs=blk*(int64_t)LRU_BLOCK_SIZE;
      size_t off=(size_t)(m_logicalPos-bs),cs=std::min((size_t)size,c->size()-off);
      if(m_totalSize>0)cs=std::min(cs,(size_t)(m_totalSize-m_logicalPos));
      if(cs>0){memcpy(out,c->data()+off,cs);m_logicalPos+=(int64_t)cs;return cs;}
    }

    std::unique_lock<std::mutex> lk(m_rbMutex);
    int64_t dp = m_downloadPos.load();
    int64_t buf_start = dp - (int64_t)m_rbAvailable;
    int64_t buf_end = dp;

    // Adjust for EOF
    int64_t effective_end = block_end;
    if (m_eof && dp < effective_end)
      effective_end = dp;
    if (m_totalSize>0 && effective_end > m_totalSize)
      effective_end = m_totalSize;

    // ★ Check if ring buffer covers the entire needed block
    if (effective_end > block_start && block_start >= buf_start && block_end <= buf_end) {
      size_t bsize = (size_t)(effective_end - block_start);
      size_t off_from_tail = (size_t)(block_start - buf_start);
      size_t rd_ptr = (m_rbTail + off_from_tail) % m_ringBuffer.size();

      // Extract block from ring buffer
      std::vector<uint8_t> block_data(bsize);
      size_t copied=0;
      while (copied<bsize) {
        size_t chunk=std::min(bsize-copied,m_ringBuffer.size()-rd_ptr);
        memcpy(block_data.data()+copied,m_ringBuffer.data()+rd_ptr,chunk);
        rd_ptr=(rd_ptr+chunk)%m_ringBuffer.size(); copied+=chunk;
      }

      // Drop consumed data from ring buffer (lazy pruning)
      size_t drop = (size_t)(block_start - buf_start);
      if (drop>0 && drop<=m_rbAvailable) {
        m_rbTail=(m_rbTail+drop)%m_ringBuffer.size(); m_rbAvailable-=drop;
        m_rbLogicalStart=block_start; m_rbCvWriter.notify_one();
      }

      lk.unlock();

      // Write block to LRU
      CCurlFileLRUCache::Instance().Put(m_fileUrl,m_modTime,blk,block_data.data(),bsize);

      // Copy requested data from block
      size_t off=(size_t)(m_logicalPos-block_start);
      size_t cs=std::min(size,bsize-off);
      if (m_totalSize>0) cs=std::min(cs,(size_t)(m_totalSize-m_logicalPos));
      if (cs>0){memcpy(out,block_data.data()+off,cs);m_logicalPos+=(int64_t)cs;return cs;}
      return 0;
    }

    // --- Teleport check (vfs pattern) ---
    bool need_reset=false;
    int64_t plan_limit = buf_end + (int64_t)m_ringBufferSize;

    if (block_start < buf_start) {
      CLog::Log(LOGDEBUG,"{}::Read - behind buffer, teleport to {}",LOG_TAG,block_start);
      need_reset=true;
    } else if (block_start > plan_limit || (block_start - buf_end) > 16*1024*1024) {
      CLog::Log(LOGDEBUG,"{}::Read - ahead/gap > 16MB (gap={}), teleport to {}",LOG_TAG,block_start-buf_end,block_start);
      need_reset=true;
    }

    if (need_reset) {
      m_resetTargetPos = block_start;
      m_triggerReset = true; m_abortTransfer = true;
      m_rbCvWriter.notify_all();
      m_hasError = false; m_eof = false;
    }

    // --- Drop stale data before reader position ---
    if (block_start > buf_start + (int64_t)m_rbAvailable) {
      // Reader is ahead of all buffered data, no point keeping it
    }
    if (block_start > buf_start && m_rbAvailable > 0) {
      size_t to_drop = std::min((size_t)(block_start - buf_start), m_rbAvailable);
      m_rbTail=(m_rbTail+to_drop)%m_ringBuffer.size(); m_rbAvailable-=to_drop;
      m_rbLogicalStart=block_start; m_rbCvWriter.notify_one();
    }

    // --- Check termination ---
    if (m_hasError && !need_reset) return -1;
    if (m_eof && block_start >= m_downloadPos.load()) return 0;

    // ★ Diagnostic: log position once per second
    static int diagSkip=0;
    if (++diagSkip % 10 == 0)
      CLog::Log(LOGDEBUG,"{}::Read - waiting block#{} pos={} buf=[{}-{}] avail={} dp={}",LOG_TAG,blk,m_logicalPos,buf_start,buf_end,(int64_t)m_rbAvailable,dp);

    // --- Wait for Worker to fill data ---
    if (m_rbCvReader.wait_for(lk, std::chrono::seconds(60)) == std::cv_status::timeout) {
      CLog::Log(LOGERROR,"{}::Read - timeout waiting for block#{} at pos={}",LOG_TAG,blk,m_logicalPos);
      return -1;
    }
    if (!m_running) return -1;
  }
  return 0;
}

// =========================================================================
// Worker Thread
// =========================================================================

void CCurlFileEngine::StartWorker(){if(m_workerThread.joinable()||m_running)return;if(m_ringBuffer.empty()){m_ringBuffer.resize(m_ringBufferSize);CLog::Log(LOGDEBUG,"{}::StartWorker - ring buffer resized to {}MB",LOG_TAG,m_ringBuffer.size()>>20);}m_running=true;m_eof=false;m_hasError=false;m_abortTransfer=false;m_triggerReset=false;m_rbHead=0;m_rbTail=0;m_rbAvailable=0;m_rbLogicalStart=m_logicalPos;m_downloadPos=m_logicalPos;m_workerThread=std::thread(&CCurlFileEngine::WorkerLoop,this);}
void CCurlFileEngine::StopWorker(){m_running=false;m_abortTransfer=true;{std::lock_guard<std::mutex> lk(m_rbMutex);m_rbCvWriter.notify_all();m_rbCvReader.notify_all();}if(m_workerThread.joinable())m_workerThread.join();m_ringBuffer.clear();m_rbHead=0;m_rbTail=0;m_rbAvailable=0;}
size_t CCurlFileEngine::WorkerWriteCallback(void* c,size_t s,size_t n,void* u){auto* e=(CCurlFileEngine*)u;return e?e->HandleWorkerWrite(c,s*n):0;}

size_t CCurlFileEngine::HandleWorkerWrite(void* c,size_t s){
  if(m_abortTransfer)return 0;
  std::unique_lock<std::mutex> lk(m_rbMutex);

  // ★ vfs.stream.fast pattern: block until space available (no discard)
  while(m_rbAvailable + s > m_ringBuffer.size()){
    if(m_abortTransfer || m_triggerReset || !m_running) return 0;
    m_rbCvWriter.wait(lk);
    if(!m_running) return 0;
    if(m_abortTransfer || m_triggerReset) return 0;
  }

  // Write data to ring buffer
  size_t rem=s;
  while(rem>0){
    size_t tw=std::min(rem,m_ringBuffer.size()-m_rbHead);
    memcpy(m_ringBuffer.data()+m_rbHead,(const uint8_t*)c+(s-rem),tw);
    m_rbHead=(m_rbHead+tw)%m_ringBuffer.size(); rem-=tw;
  }
  m_rbAvailable+=s; m_downloadPos+=(int64_t)s;
  m_rbCvReader.notify_one();
  return s;
}

void CCurlFileEngine::WorkerLoop()
{
  CLog::Log(LOGDEBUG, "{}::Worker - thread starting, url={}", LOG_TAG, m_fileUrl);

  CURL_HANDLE* curl = PoolGet();
  if (!curl) { CLog::Log(LOGERROR, "{}::Worker - PoolGet FAILED", LOG_TAG); m_hasError = true; m_running = false; return; }

  int retries = 0;
  char errbuf[CURL_ERROR_SIZE];

  while (m_running) {
    // ---- 1. Reset phase ----
    if (m_triggerReset) {
      CLog::Log(LOGDEBUG, "{}::Worker - reset to pos={}", LOG_TAG, m_resetTargetPos.load());
      retries = 0; m_triggerReset = false;
      { std::lock_guard<std::mutex> lk(m_rbMutex); m_rbHead=0; m_rbTail=0; m_rbAvailable=0; m_rbLogicalStart=m_resetTargetPos.load(); m_downloadPos=m_resetTargetPos.load(); m_eof=false; }
      m_hasError = false; m_abortTransfer = false;
    }

    // ---- 2. EOF check ----
    if (m_totalSize > 0 && m_downloadPos.load() >= m_totalSize) {
      if (!m_eof) { m_eof = true; { std::lock_guard<std::mutex> lk(m_rbMutex); m_rbCvReader.notify_all(); } }
      std::unique_lock<std::mutex> lk(m_rbMutex);
      m_rbCvWriter.wait(lk, [this]{ return m_triggerReset.load() || !m_running; });
      if (!m_running) break;
      continue;
    }

    // ---- 3. Download phase ----
    g_curlInterface.easy_reset(curl);
    errbuf[0] = 0; g_curlInterface.easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    // ★ Match vfs: verbose curl logging for debugging
    g_curlInterface.easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    // ★ vfs.stream.fast: Worker uses CDN URL from Stat. CDN supports range.
    // Stat already established a TCP connection to CDN which is cached
    // in the handle pool and will be reused here.
    std::string tUrl = !m_effectiveUrl.empty() ? FixDavProtocol(m_effectiveUrl) : FixDavProtocol(m_fileUrl);

    int64_t sp = m_downloadPos.load();
    SetupWorkerDownloadOptions(curl, tUrl, sp);

    CLog::Log(LOGDEBUG, "{}::Worker - connecting to {} (pos={})", LOG_TAG, tUrl, sp);
    // Log handle pointer to verify pool reuse
    CLog::Log(LOGDEBUG, "{}::Worker - handle=%p calling curl_easy_perform...", LOG_TAG, static_cast<void*>(curl));
    CURLcode res = g_curlInterface.easy_perform(curl);
    if (!m_running) break;

    long code = 0; g_curlInterface.easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    CLog::Log(LOGDEBUG, "{}::Worker - result: res={} code={} avail={} dp={}{}", LOG_TAG, (int)res, code, (int64_t)m_rbAvailable, m_downloadPos.load(), res!=CURLE_OK?StringUtils::Format(" err=%s",errbuf):"");

    // Update redirect cache after every connection attempt
    if (code > 0) UpdateEffectiveUrl(curl, m_fileUrl, "Worker");

    // Runtime range detection (vfs pattern)
    if (res == CURLE_OK && !m_supportRange) {
      bool sr = (code == 206);
      if (!sr) {
        struct curl_header* h = nullptr;
        if (curl_easy_header(curl, "Accept-Ranges", 0, CURLH_HEADER, -1, &h) == CURLHE_OK && h && h->value && std::string(h->value).find("bytes") != std::string::npos)
          sr = true;
      }
      if (sr) { m_supportRange = true; CLog::Log(LOGDEBUG, "{}::Worker - detected range support", LOG_TAG); }
    }

    // ---- 4. Result handling ----
    if (res == CURLE_ABORTED_BY_CALLBACK && m_abortTransfer) {
      continue; // Expected abort for large teleport
    }

    if (res == CURLE_WRITE_ERROR && (m_abortTransfer || m_triggerReset)) {
      continue; // Expected: write stopped due to reset signal
    }

    if (res == CURLE_OK) {
      retries = 0;
      if (m_totalSize == 0 && m_downloadPos.load() > 0) {
        m_totalSize = m_downloadPos.load();
        CLog::Log(LOGDEBUG, "{}::Worker - runtime size correction: {}", LOG_TAG, m_totalSize);
      }
      m_eof = true;
      { std::lock_guard<std::mutex> lk(m_rbMutex); m_rbCvReader.notify_all(); }
      // Wait for reset or close
      std::unique_lock<std::mutex> lk(m_rbMutex);
      m_rbCvWriter.wait(lk, [this]{ return m_triggerReset.load() || !m_running; });
      continue;
    }

    // Error handling
    if (res == CURLE_OPERATION_TIMEDOUT)
      CLog::Log(LOGWARNING, "{}::Worker - timeout, retry {}/{}", LOG_TAG, retries+1, m_netMaxRetries);
    else if (res == CURLE_WRITE_ERROR)
      CLog::Log(LOGDEBUG, "{}::Worker - write error (backpressure/teleport), retry {}/{}", LOG_TAG, retries, m_netMaxRetries);
    else
      CLog::Log(LOGDEBUG, "{}::Worker - error: {} ({}), retry {}/{}", LOG_TAG, g_curlInterface.easy_strerror(res), (int)res, retries, m_netMaxRetries);

    if (code >= 400 && code < 500 && code != 429) {
      CLog::Log(LOGWARNING, "{}::Worker - permanent HTTP error {}, stopping", LOG_TAG, code);
      ClearStatCache(m_fileUrl);  // ★ CDN token likely expired, clear global cache
      m_hasError = true; break;
    }

    // ★ Clear CDN URL only on non-write errors (write errors are expected during teleports)
    if (res != CURLE_WRITE_ERROR)
      m_effectiveUrl.clear();

    // Buffer back-pressure: if buffer is mostly full, wait for consumption
    {
      std::unique_lock<std::mutex> lk(m_rbMutex);
      size_t threshold = (size_t)(m_ringBuffer.size() * 0.9);
      if (m_rbAvailable > threshold && threshold > 0) {
        CLog::Log(LOGDEBUG, "{}::Worker - buffer full ({} > {}), waiting before retry", LOG_TAG, m_rbAvailable, threshold);
        m_rbCvWriter.wait(lk, [this, threshold]{ return m_rbAvailable < threshold || m_triggerReset.load() || !m_running; });
        if (m_triggerReset) continue;
        if (!m_running) break;
      }
    }

    retries++;
    if (retries > m_netMaxRetries) {
      m_hasError = true;
      { std::lock_guard<std::mutex> lk(m_rbMutex); m_rbCvReader.notify_all(); }
      // Wait for reset or close
      std::unique_lock<std::mutex> lk(m_rbMutex);
      m_rbCvWriter.wait(lk, [this]{ return m_triggerReset.load() || !m_running; });
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
  }
  PoolReturn(curl); m_running = false;
  { std::lock_guard<std::mutex> lk(m_rbMutex); m_rbCvReader.notify_all(); }
}
