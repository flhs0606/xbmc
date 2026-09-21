/*
 *      Initial code sponsored by: Voddler Inc (voddler.com)
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OverlayRenderer.h"

#include "OverlayRendererUtil.h"
#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlay.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayImage.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayLibass.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlaySpu.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_set>
#include <utility>

using namespace KODI;
using namespace OVERLAY;

COverlay::COverlay()
{
  m_x = 0.0f;
  m_y = 0.0f;
  m_width = 0.0f;
  m_height = 0.0f;
  m_type = TYPE_NONE;
  m_align = ALIGN_SCREEN;
  m_pos = POSITION_RELATIVE;
}

COverlay::~COverlay() = default;

unsigned int CRenderer::m_textureid = 1;

CRenderer::CRenderer()
{
  CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()->RegisterObserver(this);
  const auto subSettings = CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings();
  m_pgsVerticalMode.store(subSettings->GetPgsVerticalMode(), std::memory_order_release);
  m_pgsVerticalOffsetSteps.store(subSettings->GetPgsVerticalOffsetSteps(), std::memory_order_release);
  m_pgsBitmapZoom.store(subSettings->GetPgsBitmapZoom(), std::memory_order_release);
  m_alignOriginal.store(subSettings->GetAlignment() == SUBTITLES::Align::ORIGINAL,
                        std::memory_order_release);
  m_restrictToActiveArea.store(subSettings->GetRestrictToActiveArea(), std::memory_order_release);
}

CRenderer::~CRenderer()
{
  CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()->UnregisterObserver(this);
  Flush();
}

void CRenderer::AddOverlay(std::shared_ptr<CDVDOverlay> o, double pts, int index)
{
  std::unique_lock lock(m_section);

  SElement   e;
  e.pts = pts;
  e.overlay_dvd = std::move(o);
  m_buffers[index].push_back(e);

  m_overlayCount[index].fetch_add(1, std::memory_order_relaxed);
}

void CRenderer::Release(std::vector<SElement>& list)
{
  list.clear();
}

void CRenderer::UnInit()
{
  if (m_saveSubtitlePosition)
  {
    m_saveSubtitlePosition = false;
    CDisplaySettings::GetInstance().UpdateCalibrations();
    CServiceBroker::GetSettingsComponent()->GetSettings()->Save();
  }

  Flush();
}

void CRenderer::Flush()
{
  std::unique_lock lock(m_section);

  for (unsigned int i = 0; i < NUM_BUFFERS; ++i)
  {
    Release(m_buffers[i]);
    m_overlayCount[i].store(0, std::memory_order_relaxed);
  }
  m_buffersChanged.store(true, std::memory_order_relaxed);

  ReleaseCache();
  Reset();
}

void CRenderer::Reset()
{
  m_subtitlePosition = 0;
  m_subtitlePosResInfo = -1;
}

void CRenderer::Release(int idx)
{
  std::unique_lock lock(m_section);
  Release(m_buffers[idx]);

  m_overlayCount[idx].store(0, std::memory_order_relaxed);
  m_buffersChanged.store(true, std::memory_order_relaxed);
}

void CRenderer::ReleaseCache()
{
  m_textureCache.clear();
  m_textureid++;
}

void CRenderer::ReleaseUnused()
{
  std::unordered_set<unsigned int> usedTextureIds;
  for (const auto& buffer : m_buffers)
  {
    for (const auto& elem : buffer)
    {
      if (elem.overlay_dvd)
        usedTextureIds.insert(elem.overlay_dvd->m_textureid);
    }
  }

  for (auto it = m_textureCache.begin(); it != m_textureCache.end(); )
  {
    if (usedTextureIds.find(it->first) == usedTextureIds.end())
      it = m_textureCache.erase(it);
    else
      ++it;
  }
}

void CRenderer::Render(int idx, float depth)
{
  std::unique_lock lock(m_section);

  const bool pruneCache = m_buffersChanged.exchange(false, std::memory_order_relaxed);

  std::vector<SElement>& list = m_buffers[idx];

  std::vector<std::pair<std::shared_ptr<COverlay>, SRenderState>> items;
  items.reserve(list.size());

  struct Cand { float top; float bottom; float centerY; };
  std::array<Cand, 16> cands;
  std::size_t candCount = 0;
  float anchorY = 0.0f;
  bool haveCands = false;

  bool subVisible = false;
  bool subImageVisible = false;
  float subTop = 0.0f;
  float subBottom = 0.0f;

  for (auto it = list.begin(); it != list.end(); ++it)
  {
    if (!it->overlay_dvd)
      continue;
    std::shared_ptr<COverlay> o = Convert(*(it->overlay_dvd), it->pts);
    if (!o)
      continue;
    const SRenderState state = ComputeBaseState(o.get());
    if (IsSubtitleTextCandidate(o.get(), state) && candCount < cands.size())
    {
      const bool centerBased = (o->m_pos == COverlay::POSITION_RELATIVE);
      const float top = centerBased ? state.y - state.height * 0.5f : state.y;
      const float bottom = centerBased ? state.y + state.height * 0.5f : state.y + state.height;
      const float cY = (top + bottom) * 0.5f;
      cands[candCount++] = {top, bottom, cY};
      if (!haveCands || cY > anchorY)
      {
        anchorY = cY;
        haveCands = true;
      }
    }
    items.emplace_back(std::move(o), state);
  }

  float blockTop = 0.0f;
  float blockBottom = 0.0f;
  bool haveBlock = false;
  float clusterMinY = 0.0f;
  if (haveCands)
  {
    constexpr float kClusterFrac = 0.20f;
    clusterMinY = anchorY - m_rv.Height() * kClusterFrac;
    for (std::size_t i = 0; i < candCount; ++i)
    {
      if (cands[i].centerY < clusterMinY)
        continue;
      if (!haveBlock)
      {
        blockTop = cands[i].top;
        blockBottom = cands[i].bottom;
        haveBlock = true;
      }
      else
      {
        blockTop = std::min(blockTop, cands[i].top);
        blockBottom = std::max(blockBottom, cands[i].bottom);
      }
    }
  }

  m_pgsBitmapBlockShift = haveBlock ? ComputeBitmapBlockShift(blockTop, blockBottom) : 0.0f;
  m_pgsBitmapClusterMinY = clusterMinY;

  for (const auto& item : items)
  {
    const COverlay* ov = item.first.get();
    if (ov && !ov->m_isDiscMenuOverlay)
    {
      SRenderState st = item.second;
      if (m_pgsBitmapBlockShift != 0.0f && IsSubtitleTextCandidate(ov, st))
      {
        const bool cb = (ov->m_pos == COverlay::POSITION_RELATIVE);
        const float cY = cb ? st.y : (st.y + st.height * 0.5f);
        if (cY >= m_pgsBitmapClusterMinY)
          st.y += m_pgsBitmapBlockShift;
      }
      float t;
      float b;
      bool haveSpan = true;
      if (ov->m_pos == COverlay::POSITION_RELATIVE && !ov->m_isBitmapOverlay)
      {
        haveSpan = ov->m_glyphMaxY > ov->m_glyphMinY;
        t = m_rv.y1 + ov->m_glyphMinY * m_rv.Height();
        b = m_rv.y1 + ov->m_glyphMaxY * m_rv.Height();
      }
      else if (ov->m_pos == COverlay::POSITION_RELATIVE)
      {
        t = st.y - st.height * 0.5f;
        b = st.y + st.height * 0.5f;
      }
      else
      {
        t = st.y;
        b = st.y + st.height;
      }
      if (haveSpan)
      {
        if (!subVisible)
        {
          subTop = t;
          subBottom = b;
          subVisible = true;
        }
        else
        {
          subTop = std::min(subTop, t);
          subBottom = std::max(subBottom, b);
        }
        if (ov->m_isBitmapOverlay)
          subImageVisible = true;
      }
    }
    RenderOverlay(item.first.get(), item.second);
  }

  m_lastSubtitleVisible.store(subVisible, std::memory_order_relaxed);
  m_lastSubtitleImageVisible.store(subImageVisible, std::memory_order_relaxed);
  m_lastSubtitleTopPx.store(static_cast<int>(std::floor(subTop)), std::memory_order_relaxed);
  m_lastSubtitleBottomPx.store(static_cast<int>(std::ceil(subBottom)), std::memory_order_relaxed);

  if (pruneCache)
    ReleaseUnused();
}

SRenderState CRenderer::ComputeBaseState(const COverlay* o) const {
  SRenderState state;
  state.x = o->m_x;
  state.y = o->m_y;
  state.width = o->m_width;
  state.height = o->m_height;

  COverlay::EPosition pos = o->m_pos;
  COverlay::EAlign align = o->m_align;

  if (pos == COverlay::POSITION_RELATIVE)
  {
    float scale_x = 1.0;
    float scale_y = 1.0;
    float scale_w = 1.0;
    float scale_h = 1.0;

    if (align == COverlay::ALIGN_SCREEN || align == COverlay::ALIGN_SUBTITLE)
    {
      scale_x = m_rv.Width();
      scale_y = m_rv.Height();
      scale_w = scale_x;
      scale_h = scale_y;
    }
    else if (align == COverlay::ALIGN_SCREEN_AR)
    {
      // Align to screen by keeping aspect ratio to fit into the screen area
      float source_width = o->m_source_width > 0 ? o->m_source_width : m_rs.Width();
      float source_height = o->m_source_height > 0 ? o->m_source_height : m_rs.Height();
      float ratio = std::min<float>(m_rv.Width() / source_width, m_rv.Height() / source_height);
      scale_x = m_rv.Width();
      scale_y = m_rv.Height();
      scale_w = ratio;
      scale_h = ratio;
    }
    else if (align == COverlay::ALIGN_VIDEO)
    {
      scale_x = m_rs.Width();
      scale_y = m_rs.Height();
      scale_w = scale_x;
      scale_h = scale_y;
    }

    state.x *= scale_x;
    state.y *= scale_y;
    state.width *= scale_w;
    state.height *= scale_h;

    pos = COverlay::POSITION_ABSOLUTE;
  }

  if (pos == COverlay::POSITION_ABSOLUTE)
  {
    if (align == COverlay::ALIGN_SCREEN || align == COverlay::ALIGN_SCREEN_AR ||
        align == COverlay::ALIGN_SUBTITLE)
    {
      if (align == COverlay::ALIGN_SUBTITLE)
      {
        RESOLUTION_INFO resInfo = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
        state.x += m_rv.x1 + m_rv.Width() * 0.5f;
        state.y += m_rv.y1 + (resInfo.iSubtitles - resInfo.Overscan.top);
      }
      else
      {
        state.x += m_rv.x1;
        state.y += m_rv.y1;
      }
    }
    else if (align == COverlay::ALIGN_VIDEO)
    {
      float scale_x = m_rd.Width() / m_rs.Width();
      float scale_y = m_rd.Height() / m_rs.Height();

      state.x *= scale_x;
      state.y *= scale_y;
      state.width *= scale_x;
      state.height *= scale_y;

      state.x += m_rd.x1;
      state.y += m_rd.y1;
    }
  }

  state.x += GetStereoscopicDepth(o->m_pgsSubtitle && !o->m_isDiscMenuOverlay,
                                  o->m_3dSubtitleDepth);

  const int pgsZoom = m_pgsBitmapZoom.load(std::memory_order_acquire);

  if (o->m_isBitmapOverlay && !o->m_isDiscMenuOverlay && pgsZoom != 100)
  {
    const float zoom = static_cast<float>(pgsZoom) / 100.0f;
    if (o->m_pos == COverlay::POSITION_RELATIVE)
    {
      state.y += state.height * (1.0f - zoom) * 0.5f;
      state.width *= zoom;
      state.height *= zoom;
    }
    else
    {
      const float cx = state.x + state.width * 0.5f;
      state.y += state.height * (1.0f - zoom);
      state.width *= zoom;
      state.height *= zoom;
      state.x = cx - state.width * 0.5f;
    }
  }

  return state;
}

void CRenderer::Render(COverlay* o) const
{
  RenderOverlay(o, ComputeBaseState(o));
}

void CRenderer::RenderOverlay(COverlay* o, SRenderState state) const
{
  const float preShiftY = state.y;
  if (m_pgsBitmapBlockShift != 0.0f && IsSubtitleTextCandidate(o, state))
  {
    const bool centerBased = (o->m_pos == COverlay::POSITION_RELATIVE);
    const float centerY = centerBased ? state.y : (state.y + state.height * 0.5f);
    if (centerY >= m_pgsBitmapClusterMinY)
      state.y += m_pgsBitmapBlockShift;
  }

  if (o->m_isBitmapOverlay && CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
      CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO))
  {
    const int pgsMode = m_pgsVerticalMode.load(std::memory_order_acquire);
    static int s_bmPgs = -1, s_bmY = -100000, s_bmAA = -1;
    static std::chrono::steady_clock::time_point s_bmPrev{};
    const int yi = static_cast<int>(state.y);
    if (pgsMode != s_bmPgs || yi != s_bmY || m_activeAreaBottomOffsetPx != s_bmAA)
    {
      const auto bmNow = std::chrono::steady_clock::now();
      if (s_bmPrev.time_since_epoch().count() == 0 ||
          bmNow - s_bmPrev >= std::chrono::seconds(1))
      {
        s_bmPgs = pgsMode;
        s_bmY = yi;
        s_bmAA = m_activeAreaBottomOffsetPx;
        s_bmPrev = bmNow;
        logComponentM(LOGDEBUG, LOGVIDEO,
                      "BitmapPos pgsMode={} pos={} align={} m_aa={}/{} rd=(y1={:.0f} h={:.0f}) "
                      "rv=(y1={:.0f} y2={:.0f}) shift={:.0f} preY={:.0f} stateY={:.0f} stateH={:.0f}",
                      pgsMode, static_cast<int>(o->m_pos), static_cast<int>(o->m_align),
                      m_activeAreaTopOffsetPx.load(std::memory_order_relaxed),
                      m_activeAreaBottomOffsetPx.load(std::memory_order_relaxed), m_rd.y1,
                      m_rd.Height(), m_rv.y1, m_rv.y2, m_pgsBitmapBlockShift, preShiftY, state.y,
                      state.height);
      }
    }
  }

  o->Render(state);
}

bool CRenderer::IsSubtitleTextCandidate(const COverlay* o, const SRenderState& s) const
{
  if (!o->m_isBitmapOverlay)
    return false;
  if (o->m_isDiscMenuOverlay)
    return false;
  const float frameH = m_rv.Height();
  if (frameH <= 0.0f)
    return false;
  const int z = m_pgsBitmapZoom.load(std::memory_order_acquire);
  const float zoomFactor = z > 0 ? static_cast<float>(z) / 100.0f : 1.0f;
  if (s.height >= frameH * 0.15f * zoomFactor)
    return false;
  const bool centerBased = (o->m_pos == COverlay::POSITION_RELATIVE);
  const float centerY = centerBased ? s.y : (s.y + s.height * 0.5f);
  return centerY >= (m_rv.y1 + m_rv.y2) * 0.5f;
}

float CRenderer::ComputeBitmapBlockShift(float blockTop, float blockBottom) const
{
  if (m_rd.Height() <= 0.0f)
    return 0.0f;

  const int pgsMode = m_pgsVerticalMode.load(std::memory_order_acquire);
  if (pgsMode == 0)
  {
    if (!m_alignOriginal.load(std::memory_order_acquire) ||
        !m_restrictToActiveArea.load(std::memory_order_acquire))
      return 0.0f;
  }

  const float activeTop = m_rd.y1 + static_cast<float>(m_activeAreaTopOffsetPx);
  const float activeBottom =
      m_rd.y1 + m_rd.Height() - static_cast<float>(m_activeAreaBottomOffsetPx);
  constexpr float kBleedSafetyPx = 8.0f;

  float shift = 0.0f;
  switch (pgsMode)
  {
    case 0:
      if (blockBottom > activeBottom)
        shift = activeBottom - blockBottom;
      else if (blockTop < activeTop)
        shift = activeTop - blockTop;
      break;
    case 1:
      shift = (activeBottom - kBleedSafetyPx) - blockBottom;
      break;
    case 2:
      shift = (activeTop + kBleedSafetyPx) - blockTop;
      break;
    case 3:
      shift = (m_rv.y2 - kBleedSafetyPx) - blockBottom;
      break;
    case 4:
      shift = (m_rv.y1 + kBleedSafetyPx) - blockTop;
      break;
    case 5:
    {
      const int pgsSteps = m_pgsVerticalOffsetSteps.load(std::memory_order_acquire);
      const float refHeight = m_rv.Height() * 75.0f / 1080.0f;
      shift = -static_cast<float>(pgsSteps) * (refHeight * 0.125f);
      if (blockTop + shift < m_rv.y1)
        shift = m_rv.y1 - blockTop;
      if (blockBottom + shift > m_rv.y2)
        shift = m_rv.y2 - blockBottom;
      break;
    }
    default:
      break;
  }
  return shift;
}

bool CRenderer::HasOverlay(int idx)
{
  return m_overlayCount[idx].load(std::memory_order_relaxed) != 0;
}

int CRenderer::GetOverlayCount(int idx)
{
  return m_overlayCount[idx].load(std::memory_order_relaxed);
}

uint64_t CRenderer::GetOverlaySetSignature(int idx, bool& animated) const
{
  std::unique_lock lock(m_section);
  uint64_t signature = m_overlayRenderChangeGen.load(std::memory_order_relaxed);
  bool needsContinuousRedraw = false;
  for (const auto& element : m_buffers[idx])
  {
    signature = signature * 31 + reinterpret_cast<uintptr_t>(element.overlay_dvd.get());
    if (element.overlay_dvd)
    {
      signature = signature * 31 +
                  static_cast<uint64_t>(static_cast<int64_t>(element.overlay_dvd->iPTSStartTime));
      signature = signature * 31 +
                  static_cast<uint64_t>(static_cast<int64_t>(element.overlay_dvd->iPTSStopTime));
      if (element.overlay_dvd->IsOverlayType(DVDOVERLAY_TYPE_TEXT) ||
          element.overlay_dvd->IsOverlayType(DVDOVERLAY_TYPE_SSA))
      {
        const auto handler =
            static_cast<CDVDOverlayLibass*>(element.overlay_dvd.get())->GetLibassHandler();
        if (handler && handler->NeedsRerender(element.pts))
          needsContinuousRedraw = true;
      }
    }
  }
  animated = needsContinuousRedraw;
  return signature;
}

bool CRenderer::GetVisibleSubtitleSpan(int& topPx, int& botPx, bool& imageVisible) const
{
  topPx = m_lastSubtitleTopPx.load(std::memory_order_relaxed);
  botPx = m_lastSubtitleBottomPx.load(std::memory_order_relaxed);
  imageVisible = m_lastSubtitleImageVisible.load(std::memory_order_relaxed);
  return m_lastSubtitleVisible.load(std::memory_order_relaxed);
}

void CRenderer::SetVideoRect(CRect &source, CRect &dest, CRect &view)
{
  if (m_rv != view) // Screen resolution is changed
  {
    m_rv = view;
    OnViewChange();
  }
  m_rs = source;
  m_rd = dest;
}

void CRenderer::OnViewChange()
{
  m_isSettingsChanged = true;
  MarkOverlayRenderChanged();
}

void CRenderer::MarkOverlayRenderChanged()
{
  m_overlayRenderChangeGen.fetch_add(1, std::memory_order_relaxed);
}

void CRenderer::SetStereoMode(const std::string &stereomode)
{
  if (m_stereomode == stereomode)
    return;

  m_stereomode = stereomode;
  MarkOverlayRenderChanged();
}

void CRenderer::SetActiveAreaPx(int topPx, int bottomPx)
{
  const int prevTopPx = m_activeAreaTopOffsetPx.exchange(topPx, std::memory_order_relaxed);
  const int prevBottomPx = m_activeAreaBottomOffsetPx.exchange(bottomPx, std::memory_order_relaxed);
  if (prevTopPx != topPx || prevBottomPx != bottomPx)
    MarkOverlayRenderChanged();
}

void CRenderer::SetSubtitleVerticalPosition(const int value, bool save)
{
  std::unique_lock lock(m_section);
  const bool positionChanged = m_subtitlePosition != value;
  m_subtitlePosition = value;

  if (save && m_subtitleAlign == SUBTITLES::Align::MANUAL)
  {
    m_subtitlePosResInfo = POSRESINFO_SAVE_CHANGES;
    // We save the value to XML file settings when playback is stopped
    // to avoid saving to disk too many times
    m_saveSubtitlePosition = true;
  }

  if (positionChanged)
    MarkOverlayRenderChanged();
}

void CRenderer::ResetSubtitlePosition()
{
  // In the 'pos' var the vertical margin has been substracted because
  // we need to know the actual text baseline position on screen
  int pos{0};
  m_saveSubtitlePosition = false;
  RESOLUTION_INFO resInfo = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();

  if (m_subtitleAlign == SUBTITLES::Align::MANUAL)
  {
    // The position must be fixed to match the subtitle calibration bar
    m_subtitleVerticalMargin = static_cast<int>(
        static_cast<float>(resInfo.iHeight) / 100 *
        CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()->GetVerticalMarginPerc());

    m_subtitlePosResInfo = resInfo.iSubtitles;
    pos = resInfo.iSubtitles - m_subtitleVerticalMargin;
  }
  else
  {
    // The position must be relative to the screen frame
    m_subtitleVerticalMargin = static_cast<int>(
        static_cast<float>(m_rv.Height()) / 100 *
        CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()->GetVerticalMarginPerc());

    m_subtitlePosResInfo = static_cast<int>(m_rv.Height());
    pos = static_cast<int>(m_rv.Height()) - m_subtitleVerticalMargin + resInfo.Overscan.top;
  }

  // Update player value (and callback to CRenderer::SetSubtitleVerticalPosition)
  auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  appPlayer->SetSubtitleVerticalPosition(pos, false);
}

void CRenderer::CreateSubtitlesStyle()
{
  m_overlayStyle = std::make_shared<SUBTITLES::STYLE::style>();
  const auto settings{CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()};

  m_overlayStyle->fontName = settings->GetFontName();
  m_overlayStyle->fontSize = static_cast<double>(settings->GetFontSize());

  SUBTITLES::FontStyle fontStyle = settings->GetFontStyle();
  if (fontStyle == SUBTITLES::FontStyle::BOLD_ITALIC)
    m_overlayStyle->fontStyle = SUBTITLES::STYLE::FontStyle::BOLD_ITALIC;
  else if (fontStyle == SUBTITLES::FontStyle::BOLD)
    m_overlayStyle->fontStyle = SUBTITLES::STYLE::FontStyle::BOLD;
  else if (fontStyle == SUBTITLES::FontStyle::ITALIC)
    m_overlayStyle->fontStyle = SUBTITLES::STYLE::FontStyle::ITALIC;

  m_overlayStyle->fontColor = settings->GetFontColor();
  m_overlayStyle->fontBorderSize = settings->GetBorderSize();
  m_overlayStyle->fontBorderColor = settings->GetBorderColor();
  m_overlayStyle->fontOpacity = settings->GetFontOpacity();

  SUBTITLES::BackgroundType backgroundType = settings->GetBackgroundType();
  if (backgroundType == SUBTITLES::BackgroundType::NONE)
    m_overlayStyle->borderStyle = SUBTITLES::STYLE::BorderType::OUTLINE_NO_SHADOW;
  else if (backgroundType == SUBTITLES::BackgroundType::SHADOW)
    m_overlayStyle->borderStyle = SUBTITLES::STYLE::BorderType::OUTLINE;
  else if (backgroundType == SUBTITLES::BackgroundType::BOX)
    m_overlayStyle->borderStyle = SUBTITLES::STYLE::BorderType::BOX;
  else if (backgroundType == SUBTITLES::BackgroundType::SQUAREBOX)
    m_overlayStyle->borderStyle = SUBTITLES::STYLE::BorderType::SQUARE_BOX;

  m_overlayStyle->backgroundColor = settings->GetBackgroundColor();
  m_overlayStyle->backgroundOpacity = settings->GetBackgroundOpacity();

  m_overlayStyle->shadowColor = settings->GetShadowColor();
  m_overlayStyle->shadowOpacity = settings->GetShadowOpacity();
  m_overlayStyle->shadowSize = settings->GetShadowSize();

  SUBTITLES::Align subAlign = settings->GetAlignment();
  if (subAlign == SUBTITLES::Align::TOP_INSIDE || subAlign == SUBTITLES::Align::TOP_OUTSIDE)
    m_overlayStyle->alignment = SUBTITLES::STYLE::FontAlign::TOP_CENTER;
  else
    m_overlayStyle->alignment = SUBTITLES::STYLE::FontAlign::SUB_CENTER;

  if (settings->IsOverrideAss())
  {
    m_overlayStyle->assOverrideFont = settings->IsOverrideFonts();

    SUBTITLES::OverrideStyles overrideStyles = settings->GetOverrideStyles();
    if (overrideStyles == SUBTITLES::OverrideStyles::POSITIONS)
      m_overlayStyle->assOverrideStyles = SUBTITLES::STYLE::OverrideStyles::POSITIONS;
    else if (overrideStyles == SUBTITLES::OverrideStyles::STYLES)
      m_overlayStyle->assOverrideStyles = SUBTITLES::STYLE::OverrideStyles::STYLES;
    else if (overrideStyles == SUBTITLES::OverrideStyles::STYLES_POSITIONS)
      m_overlayStyle->assOverrideStyles = SUBTITLES::STYLE::OverrideStyles::STYLES_POSITIONS;
    else
      m_overlayStyle->assOverrideStyles = SUBTITLES::STYLE::OverrideStyles::DISABLED;
  }

  // Changing vertical margin while in playback causes side effects when you
  // rewind the video, displaying the previous text position (test Libass 15.2)
  // for now vertical margin setting will be disabled during playback
  m_overlayStyle->marginVertical =
      static_cast<int>(SUBTITLES::STYLE::VIEWPORT_HEIGHT / 100 *
                       static_cast<double>(settings->GetVerticalMarginPerc()));

  m_overlayStyle->blur = settings->GetBlurSize();
}

std::shared_ptr<COverlay> CRenderer::ConvertLibass(
    CDVDOverlayLibass& o,
    double pts,
    bool updateStyle,
    const std::shared_ptr<struct SUBTITLES::STYLE::style>& overlayStyle)
{
  SUBTITLES::STYLE::renderOpts rOpts;

  // libass render in a target area which named as frame. the frame size may bigger than video size,
  // and including margins between video to frame edge. libass allow to render subtitles into the margins.
  // this has been used to show subtitles in the top or bottom "black bar" between video to frame border.
  rOpts.sourceWidth = m_rs.Width();
  rOpts.sourceHeight = m_rs.Height();
  rOpts.videoWidth = m_rd.Width();
  rOpts.videoHeight = m_rd.Height();
  rOpts.frameWidth = m_rv.Width();
  rOpts.frameHeight = m_rv.Height();

  // Set position of subtitles based on video calibration settings
  RESOLUTION_INFO resInfo = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
  // Keep track of subtitle position value change,
  // can be changed by GUI Calibration or by window mode/resolution change or
  // by user manual change (e.g. keyboard shortcut)
  if (m_subtitlePosResInfo != resInfo.iSubtitles)
  {
    if (m_subtitlePosResInfo == POSRESINFO_SAVE_CHANGES)
    {
      // m_subtitlePosition has been changed
      // and has been requested to save the value to resInfo
      resInfo.iSubtitles = m_subtitlePosition + m_subtitleVerticalMargin;
      CServiceBroker::GetWinSystem()->GetGfxContext().SetResInfo(
          CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution(), resInfo);
      m_subtitlePosResInfo = m_subtitlePosition + m_subtitleVerticalMargin;
    }
    else
      ResetSubtitlePosition();
  }

  rOpts.m_par = resInfo.fPixelRatio;

  // rOpts.position and margins (set to style) can invalidate the text
  // positions to subtitles type that make use of margins to position text on
  // the screen (e.g. ASS/WebVTT) then we allow to set them when position
  // override setting is enabled only
  const int mode = m_pgsVerticalMode.load(std::memory_order_relaxed);

  if (mode != 0)
  {
    bool allowActiveArea = true;
    if (o.GetLibassHandler()->GetSubtitleType() == NATIVE)
    {
      allowActiveArea = m_overlayStyle &&
          (m_overlayStyle->assOverrideStyles == SUBTITLES::STYLE::OverrideStyles::POSITIONS ||
           m_overlayStyle->assOverrideStyles == SUBTITLES::STYLE::OverrideStyles::STYLES_POSITIONS);
    }

    if (mode == 1 || mode == 2)
    {
      rOpts.marginsMode = SUBTITLES::STYLE::MarginsMode::INSIDE_ACTIVE_AREA;
      rOpts.activeAreaTopOffsetPx = m_activeAreaTopOffsetPx;
      rOpts.activeAreaBottomOffsetPx = m_activeAreaBottomOffsetPx;
    }
    else if (mode == 3)
    {
      rOpts.marginsMode = SUBTITLES::STYLE::MarginsMode::DISABLED;
    }
    else
    {
      rOpts.marginsMode = SUBTITLES::STYLE::MarginsMode::INSIDE_VIDEO;
    }

    if (mode == 2 || mode == 4)
      rOpts.position = 100;
    else if (mode == 5)
    {
      rOpts.marginsMode = SUBTITLES::STYLE::MarginsMode::DISABLED;
      const int steps = m_pgsVerticalOffsetSteps.load(std::memory_order_relaxed);
      const double frameHeight = static_cast<double>(rOpts.frameHeight);
      const double refHeight = frameHeight * 75.0 / 1080.0;
      const double perStepPerc = frameHeight > 0 ? refHeight * 0.125 / frameHeight * 100.0 : 0.0;
      rOpts.position = std::clamp(static_cast<double>(steps) * perStepPerc, 0.0, 100.0);
    }

    {
      const auto curType = static_cast<int>(o.GetLibassHandler()->GetSubtitleType());
      static int s_lastMode = -1;
      static int s_lastType = -1;
      static bool s_lastAllowActive = false;
      if (mode != s_lastMode || curType != s_lastType || allowActiveArea != s_lastAllowActive)
      {
        logComponentM(LOGDEBUG, LOGVIDEO, "text sub mode={} type={} allowActive={} margins={}/{} pos={:.1f}",
                      mode, curType, allowActiveArea, rOpts.activeAreaTopOffsetPx,
                      rOpts.activeAreaBottomOffsetPx, rOpts.position);
        s_lastMode = mode;
        s_lastType = curType;
        s_lastAllowActive = allowActiveArea;
      }
    }
  }
  else if (m_subtitleAlign == SUBTITLES::Align::ORIGINAL &&
           CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()->GetRestrictToActiveArea())
  {
    rOpts.marginsMode = SUBTITLES::STYLE::MarginsMode::INSIDE_ACTIVE_AREA;
    rOpts.activeAreaTopOffsetPx = m_activeAreaTopOffsetPx;
    rOpts.activeAreaBottomOffsetPx = m_activeAreaBottomOffsetPx;
  }
  else if (o.IsForcedMargins())
  {
    rOpts.marginsMode = SUBTITLES::STYLE::MarginsMode::DISABLED;
  }
  else if (m_subtitleAlign == SUBTITLES::Align::MANUAL)
  {
    double posPx = static_cast<double>(m_subtitlePosition - resInfo.Overscan.top);

    double frameHeight = static_cast<double>(rOpts.frameHeight);

    if (m_stereomode == "top_bottom" || m_stereomode == "bottom_top")
    {
      if (rOpts.sourceWidth / rOpts.sourceHeight > 1.2f)
        frameHeight *= 2.0;
    }

    int assPlayResY = o.GetLibassHandler()->GetPlayResY();
    double assVertMargin = static_cast<double>(overlayStyle->marginVertical) *
                           (static_cast<double>(assPlayResY) / 720);

    double vertMarginScaled = assVertMargin / assPlayResY * frameHeight;
    double pos = posPx / (frameHeight - vertMarginScaled);

    rOpts.position = 100 - pos * 100;
  }
  else if (m_subtitleAlign == SUBTITLES::Align::BOTTOM_OUTSIDE)
  {
    double posPx =
        static_cast<double>(m_subtitlePosition + m_subtitleVerticalMargin - resInfo.Overscan.top);
    rOpts.position = 100 - posPx / static_cast<double>(rOpts.frameHeight) * 100;
  }
  else if (m_subtitleAlign == SUBTITLES::Align::BOTTOM_INSIDE ||
           m_subtitleAlign == SUBTITLES::Align::TOP_INSIDE)
  {
    rOpts.marginsMode = SUBTITLES::STYLE::MarginsMode::INSIDE_VIDEO;
  }

  // Set the horizontal text alignment (currently used to improve readability on CC subtitles only)
  // This setting influence style->alignment property
  if (o.IsTextAlignEnabled())
  {
    if (m_subtitleHorizontalAlign == SUBTITLES::HorizontalAlign::LEFT)
      rOpts.horizontalAlignment = SUBTITLES::STYLE::HorizontalAlign::LEFT;
    else if (m_subtitleHorizontalAlign == SUBTITLES::HorizontalAlign::RIGHT)
      rOpts.horizontalAlignment = SUBTITLES::STYLE::HorizontalAlign::RIGHT;
    else
      rOpts.horizontalAlignment = SUBTITLES::STYLE::HorizontalAlign::CENTER;
  }

  {
    static int s_cvType = -1, s_cvForced = -1, s_cvMode = -1, s_cvAlign = -1, s_cvOvr = -1,
               s_cvMM = -1;
    const int dbgType = static_cast<int>(o.GetLibassHandler()->GetSubtitleType());
    const int dbgForced = o.IsForcedMargins() ? 1 : 0;
    const int dbgAlign = static_cast<int>(m_subtitleAlign);
    const int dbgOvr = m_overlayStyle ? static_cast<int>(m_overlayStyle->assOverrideStyles) : -1;
    const int dbgMM = static_cast<int>(rOpts.marginsMode);
    if (dbgType != s_cvType || dbgForced != s_cvForced || mode != s_cvMode ||
        dbgAlign != s_cvAlign || dbgOvr != s_cvOvr || dbgMM != s_cvMM)
    {
      s_cvType = dbgType;
      s_cvForced = dbgForced;
      s_cvMode = mode;
      s_cvAlign = dbgAlign;
      s_cvOvr = dbgOvr;
      s_cvMM = dbgMM;
      logComponentM(LOGDEBUG, LOGVIDEO,
                    "LibassDecide type={} forced={} mode={} align={} ovrStyles={} -> marginsMode={} "
                    "rOptsAA={}/{} m_aa={}/{} pos={:.1f}",
                    dbgType, dbgForced, mode, dbgAlign, dbgOvr, dbgMM,
                    rOpts.activeAreaTopOffsetPx, rOpts.activeAreaBottomOffsetPx,
                    m_activeAreaTopOffsetPx.load(std::memory_order_relaxed),
                    m_activeAreaBottomOffsetPx.load(std::memory_order_relaxed), rOpts.position);
    }
  }

  // changes: Detect changes from previously rendered images, if > 0 they are changed
  int changes = 0;
  ASS_Image* images =
      o.GetLibassHandler()->RenderImage(pts, rOpts, updateStyle, overlayStyle, &changes);

  if (changes != 0)
    m_overlayRenderChangeGen.fetch_add(1, std::memory_order_relaxed);

  // If no images not execute the renderer
  if (!images)
    return nullptr;

  if (o.m_textureid)
  {
    if (changes == 0)
    {
      auto it =
          m_textureCache.find(o.m_textureid);
      if (it != m_textureCache.end())
        return it->second;
    }
  }

  std::shared_ptr<COverlay> overlay = COverlay::Create(images, rOpts.frameWidth, rOpts.frameHeight);

  if (overlay && rOpts.frameHeight > 0.0f)
  {
    float gMinY = 1.0f;
    float gMaxY = 0.0f;
    for (ASS_Image* im = images; im; im = im->next)
    {
      if (im->w <= 0 || im->h <= 0 || (im->color & 0xff) == 0xff)
        continue;
      const float t = static_cast<float>(im->dst_y) / rOpts.frameHeight;
      const float b = static_cast<float>(im->dst_y + im->h) / rOpts.frameHeight;
      if (t < gMinY)
        gMinY = t;
      if (b > gMaxY)
        gMaxY = b;
    }
    if (gMaxY > gMinY)
    {
      overlay->m_glyphMinY = gMinY;
      overlay->m_glyphMaxY = gMaxY;
    }
  }

  m_textureCache[m_textureid] = overlay;
  o.m_textureid = m_textureid;
  m_textureid++;
  return overlay;
}

std::shared_ptr<COverlay> CRenderer::Convert(CDVDOverlay& o, double pts)
{
  std::shared_ptr<COverlay> r = nullptr;

  if (o.IsOverlayType(DVDOVERLAY_TYPE_TEXT) || o.IsOverlayType(DVDOVERLAY_TYPE_SSA))
  {
    auto &ovAss = static_cast<CDVDOverlayLibass&>(o);
    if (!ovAss.GetLibassHandler())
      return nullptr;
    bool updateStyle = !m_overlayStyle || m_isSettingsChanged;
    if (updateStyle)
    {
      m_isSettingsChanged = false;
      LoadSettings();
      CreateSubtitlesStyle();
    }

    r = ConvertLibass(ovAss, pts, updateStyle, m_overlayStyle);

    if (!r)
      return nullptr;
  }
  else if (o.m_textureid)
  {
    auto it =
        m_textureCache.find(o.m_textureid);
    if (it != m_textureCache.end())
      r = it->second;
  }

  if (r)
  {
    if (o.IsOverlayType(DVDOVERLAY_TYPE_IMAGE))
    {
      r->m_pgsSubtitle = true;
      r->m_3dSubtitleDepth = o.m_3dSubtitleDepth;
      r->m_isDiscMenuOverlay = o.IsDiscMenuOverlay();
    }
    return r;
  }

  if (o.IsOverlayType(DVDOVERLAY_TYPE_IMAGE))
  {
    r = COverlay::Create(static_cast<CDVDOverlayImage&>(o), m_rs);
    if (r)
    {
      r->m_pgsSubtitle = true;
      r->m_3dSubtitleDepth = o.m_3dSubtitleDepth;
      r->m_isDiscMenuOverlay = o.IsDiscMenuOverlay();
    }
  }
  else if (o.IsOverlayType(DVDOVERLAY_TYPE_SPU))
    r = COverlay::Create(static_cast<CDVDOverlaySpu&>(o));

  m_textureCache[m_textureid] = r;
  o.m_textureid = m_textureid;
  m_textureid++;

  return r;
}

void CRenderer::Notify(const Observable& obs, const ObservableMessage msg)
{
  switch (msg)
  {
    case ObservableMessageSettingsChanged:
    {
      m_isSettingsChanged = true;
      const auto subSettings = CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings();
      m_pgsVerticalMode.store(subSettings->GetPgsVerticalMode(), std::memory_order_release);
      m_pgsVerticalOffsetSteps.store(subSettings->GetPgsVerticalOffsetSteps(), std::memory_order_release);
      m_pgsBitmapZoom.store(subSettings->GetPgsBitmapZoom(), std::memory_order_release);
      m_alignOriginal.store(subSettings->GetAlignment() == SUBTITLES::Align::ORIGINAL,
                            std::memory_order_release);
      m_restrictToActiveArea.store(subSettings->GetRestrictToActiveArea(),
                                   std::memory_order_release);
      MarkOverlayRenderChanged();
      break;
    }
    case ObservableMessagePositionChanged:
    {
      std::unique_lock lock(m_section);
      m_subtitlePosResInfo = POSRESINFO_UNSET;
      break;
    }
    default:
      break;
  }
}

void CRenderer::LoadSettings()
{
  const auto settings{CServiceBroker::GetSettingsComponent()->GetSubtitlesSettings()};
  m_subtitleHorizontalAlign = settings->GetHorizontalAlignment();
  m_subtitleAlign = settings->GetAlignment();
  m_pgsVerticalMode.store(settings->GetPgsVerticalMode(), std::memory_order_release);
  m_pgsVerticalOffsetSteps.store(settings->GetPgsVerticalOffsetSteps(), std::memory_order_release);
  m_pgsBitmapZoom.store(settings->GetPgsBitmapZoom(), std::memory_order_release);
  m_alignOriginal.store(m_subtitleAlign == SUBTITLES::Align::ORIGINAL, std::memory_order_release);
  m_restrictToActiveArea.store(settings->GetRestrictToActiveArea(), std::memory_order_release);
  ResetSubtitlePosition();
}
