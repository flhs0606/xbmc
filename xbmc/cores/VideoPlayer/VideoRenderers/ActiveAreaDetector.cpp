/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ActiveAreaDetector.h"

#include "FileItem.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "application/Application.h"
#include "filesystem/Directory.h"
#include "filesystem/File.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

using namespace KODI::VIDEORENDERER;

namespace
{

int DetectAvioRead(void* opaque, uint8_t* buf, int size)
{
  auto* file = static_cast<XFILE::CFile*>(opaque);
  int ret = file->Read(buf, size);
  return (ret == 0) ? AVERROR_EOF : ret;
}

int64_t DetectAvioSeek(void* opaque, int64_t pos, int whence)
{
  auto* file = static_cast<XFILE::CFile*>(opaque);
  if (whence == AVSEEK_SIZE)
    return file->GetLength();
  return file->Seek(pos, whence & ~AVSEEK_FORCE);
}

}

namespace KODI
{
namespace VIDEORENDERER
{

struct ActiveAreaScanContext
{
  XFILE::CFile file;
  AVIOContext* avioCtx{nullptr};
  AVFormatContext* fmtCtx{nullptr};
  int videoIdx{-1};
  std::string scanPath;
  bool broken{false};

  ~ActiveAreaScanContext() { Close(); }

  void Close()
  {
    if (fmtCtx)
      avformat_close_input(&fmtCtx);
    if (avioCtx)
    {
      av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    file.Close();
    videoIdx = -1;
    scanPath.clear();
    broken = false;
  }

  bool Open(const std::string& path, std::atomic<bool>* abortFlag, const char*& reason)
  {
    Close();
    if (!file.Open(path, XFILE::READ_NO_CACHE))
    {
      logM(LOGWARNING, "failed to open {}", path);
      reason = "open-fail";
      return false;
    }
    const int bufSize = 32768;
    auto* avioBuf = static_cast<uint8_t*>(av_malloc(bufSize));
    if (!avioBuf)
    {
      Close();
      reason = "alloc";
      return false;
    }
    avioCtx = avio_alloc_context(avioBuf, bufSize, 0, &file,
                                 DetectAvioRead, nullptr, DetectAvioSeek);
    if (!avioCtx)
    {
      av_free(avioBuf);
      Close();
      reason = "alloc";
      return false;
    }
    fmtCtx = avformat_alloc_context();
    if (!fmtCtx)
    {
      Close();
      reason = "alloc";
      return false;
    }
    fmtCtx->pb = avioCtx;
    fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (abortFlag)
    {
      fmtCtx->interrupt_callback.callback = +[](void* opaque) -> int {
        auto* flag = static_cast<std::atomic<bool>*>(opaque);
        return (flag && flag->load(std::memory_order_acquire)) ? 1 : 0;
      };
      fmtCtx->interrupt_callback.opaque = abortFlag;
    }
    if (avformat_open_input(&fmtCtx, path.c_str(), nullptr, nullptr) < 0)
    {
      Close();
      reason = "open-input";
      return false;
    }
    bool headerComplete = false;
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++)
    {
      const AVCodecParameters* par = fmtCtx->streams[i]->codecpar;
      if (par->codec_type == AVMEDIA_TYPE_VIDEO && par->codec_id != AV_CODEC_ID_NONE &&
          par->width > 0 && par->height > 0)
      {
        headerComplete = true;
        break;
      }
    }
    if ((!headerComplete || fmtCtx->duration <= 0) &&
        avformat_find_stream_info(fmtCtx, nullptr) < 0)
    {
      Close();
      reason = "stream-info";
      return false;
    }
    for (unsigned i = 0; i < fmtCtx->nb_streams; i++)
    {
      if (videoIdx < 0 && fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        videoIdx = static_cast<int>(i);
      else
        fmtCtx->streams[i]->discard = AVDISCARD_ALL;
    }
    if (videoIdx < 0)
    {
      Close();
      reason = "no-video";
      return false;
    }
    scanPath = path;
    logComponentM(LOGDEBUG, LOGVIDEO,
                  "ActiveAreaScan context opened for {} (streams {}, duration {}, probe {})",
                  path.substr(0, path.find('?')), fmtCtx->nb_streams, fmtCtx->duration,
                  (headerComplete && fmtCtx->duration > 0) ? "skipped" : "run");
    return true;
  }
};

}
}

namespace
{

constexpr uint32_t kCommonAspectRatiosX1000[] = {
    1333, 1667, 1778, 1850, 1896, 2000, 2200, 2350, 2390, 2400, 2550, 2760};

bool ParseAspectRatio(const std::string& s, uint32_t& outX1000)
{
  if (s == "auto" || s.empty())
    return false;
  if (s == "16:9") { outX1000 = 1778; return true; }
  if (s == "1.85:1") { outX1000 = 1850; return true; }
  if (s == "2.20:1") { outX1000 = 2200; return true; }
  if (s == "2.35:1") { outX1000 = 2350; return true; }
  if (s == "2.39:1") { outX1000 = 2390; return true; }
  return false;
}

struct ScanResult
{
  bool valid{false};
  bool variableAR{false};
  uint16_t topPx{0};
  uint16_t bottomPx{0};
  uint16_t frameWidth{0};
  uint16_t frameHeight{0};
  const char* reason{""};
};

std::string ResolveMainClipUnder(const std::string& root)
{
  if (root.empty())
    return "";

  const std::string streamDir = URIUtils::AddFileToFolder(root, "BDMV", "STREAM");
  CFileItemList items;
  if (!XFILE::CDirectory::GetDirectory(streamDir, items, ".m2ts", XFILE::DIR_FLAG_NO_FILE_DIRS))
    return "";

  std::string bestPath;
  int64_t bestSize = -1;
  for (int i = 0; i < items.Size(); ++i)
  {
    const CFileItemPtr& it = items[i];
    if (!it || it->m_bIsFolder)
      continue;
    if (it->m_dwSize > bestSize)
    {
      bestSize = it->m_dwSize;
      bestPath = it->GetPath();
    }
  }
  return bestPath;
}

ScanResult ScanFileForActiveArea(const std::string& filePath,
                                 double singleSampleRatio,
                                 std::atomic<bool>* abortFlag,
                                 ActiveAreaScanContext& ctx)
{
  ScanResult out;

  if (filePath.empty() ||
      filePath.compare(0, 9, "plugin://") == 0 ||
      filePath.compare(0, 6, "pvr://") == 0 ||
      filePath.compare(0, 9, "bluray://") == 0 ||
      URIUtils::HasExtension(filePath, ".iso|.img"))
  {
    // logM(LOGWARNING, "skipping unsupported source (empty/plugin/pvr/bluray/iso): {}", filePath);
    out.reason = "unsupported-source";
    return out;
  }

  std::string scanPath = filePath;
  if (filePath.compare(0, 9, "bluray://") == 0)
  {
    scanPath = ResolveMainClipUnder(CURL(filePath).GetHostName());
    if (scanPath.empty())
    {
      logM(LOGWARNING, "no scannable .m2ts under {}", filePath);
      out.reason = "no-bluray-clip";
      return out;
    }
    logM(LOGDEBUG, "bluray source resolved to {}", scanPath);
  }
  else if (URIUtils::HasExtension(filePath, ".iso|.img"))
  {
    CURL udfUrl("udf://");
    udfUrl.SetHostName(filePath);
    scanPath = ResolveMainClipUnder(udfUrl.Get());
    if (scanPath.empty())
    {
      logM(LOGWARNING, "no scannable .m2ts under {}", filePath);
      out.reason = "no-iso-clip";
      return out;
    }
    logM(LOGDEBUG, "iso source resolved to {}", scanPath);
  }

  AVFormatContext* fmtCtx = nullptr;
  AVCodecContext* codecCtx = nullptr;
  AVFrame* frame = nullptr;
  AVPacket pkt;
  int videoIdx = -1;
  bool anyRead = false;
  uint16_t detTop = 0, detBottom = 0;

  if (ctx.fmtCtx && (ctx.broken || ctx.scanPath != scanPath))
    ctx.Close();
  if (!ctx.fmtCtx && !ctx.Open(scanPath, abortFlag, out.reason))
    return out;
  fmtCtx = ctx.fmtCtx;
  videoIdx = ctx.videoIdx;

  {
    const AVCodec* codec = avcodec_find_decoder(fmtCtx->streams[videoIdx]->codecpar->codec_id);
    if (!codec) { out.reason = "no-decoder"; goto cleanup; }

    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) { out.reason = "alloc"; goto cleanup; }

    avcodec_parameters_to_context(codecCtx, fmtCtx->streams[videoIdx]->codecpar);
    codecCtx->thread_count = 2;
    codecCtx->skip_frame = AVDISCARD_NONREF;
    codecCtx->skip_loop_filter = AVDISCARD_ALL;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
    {
      out.reason = "codec-open";
      goto cleanup;
    }
  }

  frame = av_frame_alloc();
  if (!frame) { out.reason = "alloc"; goto cleanup; }
  av_init_packet(&pkt);

  {
    int seekPercents[7] = {5, 18, 32, 46, 60, 74, 88};
    int numSeeks = 7;
    const int64_t tsBase =
        (fmtCtx->start_time != AV_NOPTS_VALUE) ? fmtCtx->start_time : 0;
    int64_t singleSampleSeekUs = -1;
    if (singleSampleRatio >= 0.0)
    {
      double r = singleSampleRatio;
      if (r < 0.01) r = 0.01;
      if (r > 0.99) r = 0.99;
      singleSampleSeekUs = static_cast<int64_t>(fmtCtx->duration * r);
      numSeeks = 1;
    }
    const int maxRetries = 8;
    const int agreeTolerance = 5;
    uint16_t samples_top[7] = {}, samples_bottom[7] = {};
    int validSamples = 0;
    int lastWidth = 0, lastHeight = 0;

    auto pickBest = [](uint16_t* v, int n) -> uint16_t {
      uint16_t best = v[0];
      int bestCount = 0;
      for (int i = 0; i < n; i++)
      {
        int count = 0;
        for (int j = 0; j < n; j++)
          if (v[j] == v[i]) count++;
        if (count > bestCount) { bestCount = count; best = v[i]; }
      }
      if (bestCount >= 2) return best;
      std::sort(v, v + n);
      return v[n / 2];
    };

    auto countSupport = [agreeTolerance](uint16_t* v, int n, uint16_t picked) -> int {
      int support = 0;
      for (int i = 0; i < n; i++)
        if (std::abs(static_cast<int>(v[i]) - static_cast<int>(picked)) <= agreeTolerance)
          support++;
      return support;
    };

    for (int s = 0; s < numSeeks && validSamples < numSeeks; s++)
    {
      if (abortFlag && abortFlag->load(std::memory_order_acquire))
        { out.reason = "abort"; goto cleanup; }
      bool usable = false;

      for (int retry = 0; retry <= maxRetries && !usable; retry++)
      {
        if (abortFlag && abortFlag->load(std::memory_order_acquire))
          { out.reason = "abort"; goto cleanup; }
        int64_t seekTargetUs = -1;
        if (singleSampleSeekUs >= 0)
        {
          seekTargetUs = singleSampleSeekUs + static_cast<int64_t>(retry) * 500000;
          if (fmtCtx->duration > 0 && seekTargetUs >= fmtCtx->duration)
            break;
        }
        else
        {
          int seekPct = seekPercents[s] + retry;
          if (seekPct > 90) break;
          if (fmtCtx->duration > 0)
            seekTargetUs = fmtCtx->duration * seekPct / 100;
        }

        if (fmtCtx->duration > 0)
        {
          avcodec_flush_buffers(codecCtx);
          if (av_seek_frame(fmtCtx, -1, tsBase + seekTargetUs, AVSEEK_FLAG_BACKWARD) < 0)
            continue;
        }
        else
        {
          const int64_t fileSize = avio_size(fmtCtx->pb);
          int bytePct = (singleSampleRatio >= 0.0)
                            ? static_cast<int>(singleSampleRatio * 100.0) + retry * 5
                            : seekPercents[s] + retry;
          if (bytePct < 1)
            bytePct = 1;
          if (bytePct > 90)
            break;
          if (fileSize > 0)
          {
            avcodec_flush_buffers(codecCtx);
            if (av_seek_frame(fmtCtx, -1, fileSize * bytePct / 100,
                              AVSEEK_FLAG_BYTE) < 0 &&
                (s > 0 || retry > 0))
              break;
          }
          else if (s > 0 || retry > 0)
            break;
        }

        bool gotFrame = false;
        bool seenKey = false;
        for (int vidPkts = 0; vidPkts < 200 && !gotFrame;)
        {
          const int rd = av_read_frame(fmtCtx, &pkt);
          anyRead = true;
          if (rd < 0)
          {
            if (rd != AVERROR_EOF)
              ctx.broken = true;
            break;
          }
          if (pkt.stream_index != videoIdx) { av_packet_unref(&pkt); continue; }
          vidPkts++;
          if (!seenKey && !(pkt.flags & AV_PKT_FLAG_KEY)) { av_packet_unref(&pkt); continue; }
          seenKey = true;
          avcodec_send_packet(codecCtx, &pkt);
          av_packet_unref(&pkt);
          if (avcodec_receive_frame(codecCtx, frame) == 0 && frame->decode_error_flags == 0)
            gotFrame = true;
        }

        if (!gotFrame || !frame->data[0] || frame->width < 64 || frame->height < 64)
          continue;

        lastWidth = frame->width;
        lastHeight = frame->height;
        usable = true;
      }

      if (!usable) continue;

      const int stride = frame->linesize[0];
      const uint8_t* yData = frame->data[0];
      const bool isP010 = (frame->format == AV_PIX_FMT_P010LE ||
                           frame->format == AV_PIX_FMT_P010BE);
      const bool is10bit = isP010 ||
                           frame->format == AV_PIX_FMT_YUV420P10LE ||
                           frame->format == AV_PIX_FMT_YUV420P10BE;
      const int shift = isP010 ? 8 : (is10bit ? 2 : 0);
      const int sampleW = std::min(64, lastWidth / 2);
      const int sampleStartX = lastWidth / 2 - sampleW / 2;
      const int stripW = std::min(32, lastWidth / 8);
      const int leftStartX = std::max(0, lastWidth / 8 - stripW / 2);
      const int rightStartX = std::min(lastWidth - stripW, lastWidth - lastWidth / 8 - stripW / 2);

      auto getY = [&](int row, int col) -> uint32_t {
        if (is10bit)
          return reinterpret_cast<const uint16_t*>(yData + row * stride)[col] >> shift;
        return yData[row * stride + col];
      };

      auto stripAvg = [&](int row, int startX, int w) -> uint32_t {
        uint32_t sum = 0;
        for (int i = 0; i < w; i++)
          sum += getY(row, startX + i);
        return sum / static_cast<uint32_t>(w);
      };

      uint32_t topRowAvg = 0, botRowAvg = 0, contentAvg = 0;
      for (int i = 0; i < sampleW; i++) topRowAvg += getY(0, sampleStartX + i);
      for (int i = 0; i < sampleW; i++) botRowAvg += getY(lastHeight - 1, sampleStartX + i);
      for (int i = 0; i < sampleW; i++) contentAvg += getY(lastHeight / 2, sampleStartX + i);
      topRowAvg /= sampleW;
      botRowAvg /= sampleW;
      contentAvg /= sampleW;
      const uint32_t borderAvg = std::min(topRowAvg, botRowAvg);
      constexpr uint32_t kAbsBlackMax = 26;
      if (contentAvg <= kAbsBlackMax)
        continue;
      const uint32_t scanThreshold =
          std::min((borderAvg + contentAvg) / 2, kAbsBlackMax);

      auto rowIsBar = [&](int row) -> bool {
        if (stripAvg(row, sampleStartX, sampleW) > scanThreshold)
          return false;
        if (stripAvg(row, leftStartX, stripW) > scanThreshold)
          return false;
        if (stripAvg(row, rightStartX, stripW) > scanThreshold)
          return false;
        return true;
      };

      uint16_t sTop = 0, sBottom = 0;
      for (int row = 0; row < lastHeight / 2; row++)
      {
        if (!rowIsBar(row)) { sTop = static_cast<uint16_t>(row); break; }
      }
      for (int row = lastHeight - 1; row >= lastHeight / 2; row--)
      {
        if (!rowIsBar(row))
        {
          sBottom = static_cast<uint16_t>(lastHeight - 1 - row);
          break;
        }
      }

      const uint16_t rawTop = sTop;
      const uint16_t rawBot = sBottom;
      {
        const bool topTrust = topRowAvg < scanThreshold;
        const bool botTrust = botRowAvg < scanThreshold;
        if (topTrust && botTrust)
        {
          const uint16_t agreed = std::min(sTop, sBottom);
          sTop = agreed;
          sBottom = agreed;
        }
        else if (topTrust && !botTrust)
        {
          sBottom = sTop;
        }
        else if (botTrust && !topTrust)
        {
          sTop = sBottom;
        }
      }

      samples_top[validSamples] = sTop;
      samples_bottom[validSamples] = sBottom;
      validSamples++;
    }

    if (validSamples == 0)
    {
      if (!anyRead)
        ctx.broken = true;
      out.reason = "no-valid-samples";
      goto cleanup;
    }

    if (singleSampleRatio < 0.0)
    {
      if (validSamples >= 2)
      {
        constexpr int kVariableSpreadPx = 10;
        uint16_t minT = samples_top[0], maxT = samples_top[0];
        uint16_t minB = samples_bottom[0], maxB = samples_bottom[0];
        for (int i = 1; i < validSamples; i++)
        {
          minT = std::min(minT, samples_top[i]);
          maxT = std::max(maxT, samples_top[i]);
          minB = std::min(minB, samples_bottom[i]);
          maxB = std::max(maxB, samples_bottom[i]);
        }
        if (maxT - minT > kVariableSpreadPx || maxB - minB > kVariableSpreadPx)
          out.variableAR = true;
      }
      const int minUsable = (numSeeks + 1) / 2;
      if (validSamples < minUsable)
      {
        out.reason = "few-samples";
        goto cleanup;
      }

      detTop = pickBest(samples_top, validSamples);
      detBottom = pickBest(samples_bottom, validSamples);

      {
        int required = (validSamples + 1) / 2;
        if (required < 2) required = 2;
        const int topSupport = countSupport(samples_top, validSamples, detTop);
        const int botSupport = countSupport(samples_bottom, validSamples, detBottom);
        const bool topOk = topSupport >= required;
        const bool botOk = botSupport >= required;
        const bool corroborated = !topOk && !botOk &&
                                  topSupport >= 2 && botSupport >= 2 &&
                                  std::abs(static_cast<int>(detTop) - static_cast<int>(detBottom))
                                      <= agreeTolerance;
        if (corroborated)
        {
          const uint16_t agreed = (topSupport >= botSupport) ? detTop : detBottom;
          detTop = detBottom = agreed;
        }
        else if (!topOk && !botOk && validSamples >= 3)
        {
          out.reason = "support-gate";
          goto cleanup;
        }
        else if (topOk && !botOk) detBottom = detTop;
        else if (botOk && !topOk) detTop = detBottom;
      }

      if ((detTop > 0 || detBottom > 0) && !out.variableAR)
      {
        const uint16_t minSignificant = static_cast<uint16_t>(lastHeight / 40);
        if (detTop >= minSignificant || detBottom >= minSignificant)
        {
          for (int i = 0; i < validSamples; i++)
          {
            if (samples_top[i] <= agreeTolerance || samples_bottom[i] <= agreeTolerance)
            {
              out.reason = "outlier-guard";
              goto cleanup;
            }
          }
        }
      }
    }
    else
    {
      detTop = samples_top[0];
      detBottom = samples_bottom[0];
    }

    if (detTop || detBottom)
    {
      const uint32_t activeH = lastHeight - detTop - detBottom;
      if (activeH > 0)
      {
        const uint32_t arX1000 = (static_cast<uint32_t>(lastWidth) * 1000) / activeH;
        const uint32_t frameAR = (static_cast<uint32_t>(lastWidth) * 1000) / lastHeight;
        if (arX1000 >= 1200 && arX1000 <= 2900 && arX1000 >= frameAR)
        {
          uint32_t bestAR = arX1000, bestDelta = UINT32_MAX;
          for (auto ar : kCommonAspectRatiosX1000)
          {
            if (ar < arX1000) continue;
            const uint32_t delta = ar - arX1000;
            if (delta < bestDelta) { bestDelta = delta; bestAR = ar; }
          }
          if (bestDelta == UINT32_MAX || bestDelta * 100 > arX1000 * 5)
            bestAR = arX1000;
          uint32_t snapH = (static_cast<uint32_t>(lastWidth) * 1000 + bestAR / 2) / bestAR;
          if (snapH > static_cast<uint32_t>(lastHeight)) snapH = lastHeight;
          const uint16_t tb = static_cast<uint16_t>((lastHeight - snapH) / 2);
          detTop = tb;
          detBottom = tb;
        }
        else
        {
          detTop = 0;
          detBottom = 0;
          out.reason = "ar-sanity-zero";
        }
      }
    }

    out.valid = true;
    out.topPx = detTop;
    out.bottomPx = detBottom;
    out.frameWidth = static_cast<uint16_t>(lastWidth);
    out.frameHeight = static_cast<uint16_t>(lastHeight);
  }

cleanup:
  if (frame) av_frame_free(&frame);
  if (codecCtx) avcodec_free_context(&codecCtx);
  if (ctx.broken)
  {
    logComponentM(LOGDEBUG, LOGVIDEO, "ActiveAreaScan context dropped for {} after read error",
                  ctx.scanPath.substr(0, ctx.scanPath.find('?')));
    ctx.Close();
  }
  return out;
}

}

CActiveAreaDetector::CActiveAreaDetector() : m_scanCtx(std::make_unique<ActiveAreaScanContext>())
{
}

CActiveAreaDetector::~CActiveAreaDetector()
{
  Stop();
}

void CActiveAreaDetector::Start(const std::string& filePath, bool runInitialScan, bool startSuspended)
{
  Stop();

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_filePath = filePath;
  }
  m_suspended.store(startSuspended, std::memory_order_release);
  m_initialScanPending = false;
  m_overlayGen.store(0, std::memory_order_release);
  m_overlayGenSeen = 0;
  m_scanRetryOffset = 0;
  m_consecFailedScans = 0;
  m_havePending = false;
  m_pendingTopPx = 0;
  m_pendingBottomPx = 0;
  m_pendingMatchCount = 0;
  m_stableMatchCount = 0;
  m_variableAR = false;
  m_periodicEnabled.store(true, std::memory_order_release);
  m_abort.store(false, std::memory_order_release);
  m_stable.store(false, std::memory_order_release);
  m_l5Available.store(false, std::memory_order_release);
  m_everL5.store(false, std::memory_order_release);
  m_l5LastState.store(~0ULL, std::memory_order_release);
  m_l5TransitionScans.store(0, std::memory_order_release);
  m_detectedArea.store(0, std::memory_order_relaxed);
  m_stop.store(false, std::memory_order_release);
  m_runInitialScan = runInitialScan;

  m_thread = std::thread(&CActiveAreaDetector::ThreadProc, this);
}

void CActiveAreaDetector::Stop()
{
  m_abort.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop.store(true, std::memory_order_release);
  }
  m_cv.notify_all();
  if (m_thread.joinable())
    m_thread.join();
  m_scanCtx->Close();
  m_stable.store(false, std::memory_order_release);
  m_detectedArea.store(0, std::memory_order_relaxed);
}

void CActiveAreaDetector::SetSuspended(bool suspended)
{
  if (m_suspended.exchange(suspended, std::memory_order_acq_rel) == suspended)
    return;
  if (!suspended)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cv.notify_all();
  }
}

void CActiveAreaDetector::SetManualAspect(const std::string& aspect)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_manualAspect = aspect;
}

void CActiveAreaDetector::SetPeriodicRescan(bool enabled, int intervalSec)
{
  m_periodicEnabled.store(enabled, std::memory_order_release);
  if (intervalSec < 1) intervalSec = 1;
  if (intervalSec > 30) intervalSec = 30;
  m_periodicIntervalSec.store(intervalSec, std::memory_order_release);
  m_cv.notify_all();
}

void CActiveAreaDetector::NotifyBitmapOverlaySeen()
{
  m_overlayGen.fetch_add(1, std::memory_order_acq_rel);
  m_periodicEnabled.store(true, std::memory_order_release);
  m_cv.notify_all();
}

void CActiveAreaDetector::SetL5Available(bool available)
{
  m_l5Available.store(available, std::memory_order_release);
}

void CActiveAreaDetector::NotifyL5Transition(bool available, uint16_t topPx, uint16_t bottomPx)
{
  if (available)
    m_everL5.store(true, std::memory_order_release);
  const uint64_t packed = (static_cast<uint64_t>(available) << 32) |
                          (static_cast<uint64_t>(topPx) << 16) | bottomPx;
  const uint64_t last = m_l5LastState.load(std::memory_order_acquire);
  if (last == packed)
    return;
  m_l5LastState.store(packed, std::memory_order_release);
  if (last == ~0ULL)
    return;
  constexpr int kL5TransitionBurst = 3;
  m_l5TransitionScans.store(kL5TransitionBurst, std::memory_order_release);
  m_periodicEnabled.store(true, std::memory_order_release);
  m_cv.notify_all();
}

bool CActiveAreaDetector::ApplyManualAspect(int frameWidth,
                                            int frameHeight,
                                            int& topPx,
                                            int& bottomPx) const
{
  uint32_t arX1000 = 0;
  std::string aspect;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    aspect = m_manualAspect;
  }
  if (!ParseAspectRatio(aspect, arX1000))
    return false;
  if (frameWidth <= 0 || frameHeight <= 0)
    return false;

  const uint32_t frameAR = (static_cast<uint32_t>(frameWidth) * 1000) / frameHeight;
  if (arX1000 <= frameAR)
  {
    topPx = 0;
    bottomPx = 0;
    return true;
  }
  uint32_t snapH = (static_cast<uint32_t>(frameWidth) * 1000 + arX1000 / 2) / arX1000;
  if (snapH > static_cast<uint32_t>(frameHeight))
    snapH = frameHeight;
  const int barTotal = frameHeight - static_cast<int>(snapH);
  topPx = barTotal / 2;
  bottomPx = barTotal - topPx;
  return true;
}

bool CActiveAreaDetector::TryGetActiveArea(int frameWidth,
                                           int frameHeight,
                                           int& topPx,
                                           int& bottomPx) const
{
  if (ApplyManualAspect(frameWidth, frameHeight, topPx, bottomPx))
    return true;

  if (!m_stable.load(std::memory_order_acquire))
    return false;

  const uint64_t area = m_detectedArea.load(std::memory_order_acquire);
  const int detH = static_cast<int>((area >> 32) & 0xFFFF);
  if (detH <= 0)
    return false;

  const int detTop = static_cast<int>(area & 0xFFFF);
  const int detBot = static_cast<int>((area >> 16) & 0xFFFF);

  if (detTop == 0 && detBot == 0)
  {
    topPx = 0;
    bottomPx = 0;
    return true;
  }

  const float yScale = static_cast<float>(frameHeight) / static_cast<float>(detH);
  topPx = static_cast<int>(detTop * yScale + 0.5f);
  bottomPx = static_cast<int>(detBot * yScale + 0.5f);
  return true;
}

void CActiveAreaDetector::RunInitialScanIfNeeded()
{
  if (m_l5Available.load(std::memory_order_acquire))
    return;
  if (RunOneScan(false) && !m_stop.load(std::memory_order_acquire))
    RunOneScan(true);
  m_lastScanTime = std::chrono::steady_clock::now();
}

void CActiveAreaDetector::ThreadProc()
{
  if (!m_runInitialScan)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this]
              { return m_stop.load(std::memory_order_acquire) ||
                       m_overlayGen.load(std::memory_order_acquire) != m_overlayGenSeen ||
                       m_l5TransitionScans.load(std::memory_order_acquire) > 0; });
  }
  if (m_stop.load(std::memory_order_acquire))
    return;
  m_overlayGenSeen = m_overlayGen.load(std::memory_order_acquire);

  if (m_suspended.load(std::memory_order_acquire))
    m_initialScanPending = true;
  else
    RunInitialScanIfNeeded();

  constexpr auto debounceMs = std::chrono::milliseconds(1000);
  constexpr int kHeartbeatIntervalSec = 15;
  constexpr int kInitialLockIntervalSec = 1;

  while (!m_stop.load(std::memory_order_acquire))
  {
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      const uint32_t lastSeen = m_overlayGenSeen;
      auto pred = [this, lastSeen]
      {
        return m_stop.load(std::memory_order_acquire) ||
               m_overlayGen.load(std::memory_order_acquire) != lastSeen ||
               m_l5TransitionScans.load(std::memory_order_acquire) > 0;
      };
      int lockIntervalSec = kInitialLockIntervalSec;
      if (m_consecFailedScans >= 20)
        lockIntervalSec = 60;
      else if (m_consecFailedScans >= 5)
        lockIntervalSec = kHeartbeatIntervalSec;
      int intervalSec = !m_stable.load(std::memory_order_acquire)
                            ? lockIntervalSec
                            : (m_periodicEnabled.load(std::memory_order_acquire)
                                   ? m_periodicIntervalSec.load(std::memory_order_acquire)
                                   : kHeartbeatIntervalSec);
      constexpr int kVariableArIntervalSec = 2;
      if (m_variableAR && !m_everL5.load(std::memory_order_acquire) &&
          m_consecFailedScans == 0 && intervalSec > kVariableArIntervalSec)
        intervalSec = kVariableArIntervalSec;
      if (m_havePending && m_consecFailedScans == 0 && intervalSec > kInitialLockIntervalSec)
        intervalSec = kInitialLockIntervalSec;
      m_cv.wait_for(lock, std::chrono::seconds(intervalSec), pred);
      m_overlayGenSeen = m_overlayGen.load(std::memory_order_acquire);
    }

    if (m_stop.load(std::memory_order_acquire))
      return;

    if (m_suspended.load(std::memory_order_acquire))
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait(lock, [this] {
        return m_stop.load(std::memory_order_acquire) ||
               !m_suspended.load(std::memory_order_acquire);
      });
      continue;
    }

    if (m_initialScanPending)
    {
      m_initialScanPending = false;
      RunInitialScanIfNeeded();
      continue;
    }

    const bool l5Scan = m_l5TransitionScans.load(std::memory_order_acquire) > 0;
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastScanTime < debounceMs)
    {
      if (!l5Scan)
        continue;
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait_for(lock, debounceMs - (now - m_lastScanTime),
                    [this] { return m_stop.load(std::memory_order_acquire); });
      if (m_stop.load(std::memory_order_acquire))
        return;
      now = std::chrono::steady_clock::now();
    }
    if (!l5Scan && !m_havePending && !m_variableAR &&
        m_l5Available.load(std::memory_order_acquire) &&
        now - m_lastScanTime < std::chrono::seconds(kHeartbeatIntervalSec))
      continue;
    const int failBackoffSec =
        (m_consecFailedScans >= 20) ? 60
                                    : ((m_consecFailedScans >= 5) ? kHeartbeatIntervalSec : 0);
    if (failBackoffSec > 0 && now - m_lastScanTime < std::chrono::seconds(failBackoffSec))
    {
      m_l5TransitionScans.store(0, std::memory_order_release);
      continue;
    }

    if (l5Scan)
      m_l5TransitionScans.fetch_sub(1, std::memory_order_acq_rel);
    RunOneScan(true, l5Scan);
    m_lastScanTime = std::chrono::steady_clock::now();
  }
}

bool CActiveAreaDetector::RunOneScan(bool periodic, bool l5Transition)
{
  std::string filePath;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    filePath = m_filePath;
  }
  if (filePath.empty())
    return false;

  double singleSampleRatio = -1.0;
  bool posTrusted = true;
  if (periodic)
  {
    const double t = g_application.GetTime();
    const double total = g_application.GetTotalTime();
    if (total > 0.0)
    {
      singleSampleRatio = t / total;
      posTrusted = (t / total) >= 0.011;
    }
    else
    {
      singleSampleRatio = 0.5;
    }
    singleSampleRatio += m_scanRetryOffset * 0.01;
    const double minRatio = (total > 40.0) ? (2.0 / total) : 0.05;
    if (singleSampleRatio < minRatio) singleSampleRatio = minRatio;
    if (singleSampleRatio > 0.95)
      singleSampleRatio = 0.05 + std::fmod(singleSampleRatio - 0.05, 0.90);
  }

  ScanResult result = ScanFileForActiveArea(filePath, singleSampleRatio, &m_abort, *m_scanCtx);
  if (result.variableAR)
    m_variableAR = true;
  logComponentM(LOGDEBUG, LOGVIDEO,
                "ActiveAreaScan periodic={} l5trans={} varAR={} valid={} top={} bot={} w={} h={} "
                "l5avail={} reason={}",
                periodic, l5Transition, m_variableAR, result.valid, result.topPx, result.bottomPx,
                result.frameWidth, result.frameHeight,
                m_l5Available.load(std::memory_order_acquire), result.reason);
  if (!result.valid)
  {
    m_scanRetryOffset = (m_scanRetryOffset + 5) % 90;
    m_consecFailedScans++;
    return false;
  }
  m_scanRetryOffset = 0;
  m_consecFailedScans = 0;

  if (!posTrusted)
  {
    const uint64_t earlyArea = m_detectedArea.load(std::memory_order_relaxed);
    const int curSum = static_cast<int>(earlyArea & 0xFFFF) +
                       static_cast<int>((earlyArea >> 16) & 0xFFFF);
    const int newSum = static_cast<int>(result.topPx) + static_cast<int>(result.bottomPx);
    if (newSum <= curSum)
      return true;
  }

  constexpr int kHysteresisTolerancePx = 10;
  const uint64_t curArea = m_detectedArea.load(std::memory_order_relaxed);
  const uint16_t curTop = static_cast<uint16_t>(curArea & 0xFFFF);
  const uint16_t curBot = static_cast<uint16_t>((curArea >> 16) & 0xFFFF);
  const bool stable = m_stable.load(std::memory_order_acquire);
  const int dTop = std::abs(static_cast<int>(result.topPx) - static_cast<int>(curTop));
  const int dBot = std::abs(static_cast<int>(result.bottomPx) - static_cast<int>(curBot));
  const bool matchesPublished =
      stable && dTop <= kHysteresisTolerancePx && dBot <= kHysteresisTolerancePx;

  constexpr int kStabilityThreshold = 5;

  if (matchesPublished)
  {
    m_havePending = false;
    m_pendingMatchCount = 0;
    if (++m_stableMatchCount >= kStabilityThreshold && !m_variableAR)
      m_periodicEnabled.store(false, std::memory_order_release);
  }
  else
  {
    bool matchesPending = false;
    if (m_havePending)
    {
      const int pTop = std::abs(static_cast<int>(result.topPx) -
                                static_cast<int>(m_pendingTopPx));
      const int pBot = std::abs(static_cast<int>(result.bottomPx) -
                                static_cast<int>(m_pendingBottomPx));
      matchesPending = pTop <= kHysteresisTolerancePx && pBot <= kHysteresisTolerancePx;
    }

    bool publishNow = !stable || l5Transition;
    if (!publishNow)
      publishNow = static_cast<int>(result.topPx) + static_cast<int>(result.bottomPx) >
                   static_cast<int>(curTop) + static_cast<int>(curBot) + kHysteresisTolerancePx;
    if (!publishNow && matchesPending)
    {
      m_pendingMatchCount++;
      publishNow = m_pendingMatchCount >= 1;
    }

    if (publishNow)
    {
      if (stable)
        m_variableAR = true;
      m_detectedArea.store(static_cast<uint64_t>(result.topPx) |
                               (static_cast<uint64_t>(result.bottomPx) << 16) |
                               (static_cast<uint64_t>(result.frameHeight) << 32),
                           std::memory_order_release);
      m_stable.store(true, std::memory_order_release);
      m_havePending = false;
      m_pendingMatchCount = 0;
      m_stableMatchCount = 0;
      m_l5TransitionScans.store(0, std::memory_order_release);
      m_periodicEnabled.store(true, std::memory_order_release);
    }
    else if (!matchesPending)
    {
      m_pendingTopPx = result.topPx;
      m_pendingBottomPx = result.bottomPx;
      m_havePending = true;
      m_pendingMatchCount = 0;
      if (!m_periodicEnabled.load(std::memory_order_acquire))
      {
        m_periodicEnabled.store(true, std::memory_order_release);
        m_stableMatchCount = 0;
      }
    }
  }
  return true;
}
