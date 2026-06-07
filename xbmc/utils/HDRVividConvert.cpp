/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  HDR Vivid → DoVi RPU conversion.
 *
 *  Converts per-frame maximum_maxrgb / average_maxrgb → DoVi L1 max_pq / avg_pq.
 *  min_pq and source_min/max_pq come from static MDCV look-up (same as HDR10+).
 *  peakNits uses MDCV max_lum, with 10000-nit fallback when absent.
 *
 *  TM parameters m_b (black level) and m_a (highlight transition width) are
 *  read from Vivid base_params to modulate min_pq and max_pq on a per-frame
 *  basis.  No temporal smoothing — RPU caching handles intra-scene stability.
 */

#include "HDRVividConvert.h"

#include "DoViRpuWriter.h"
#include "HDR10PlusConvert.h"  // nits_to_pq, cast_pq, clamp16, VdrDmData, operator==
#include "utils/log.h"

#include <cmath>
#include <cstdint>
#include <vector>

// DoVi L1 clamp values
static constexpr uint16_t kL1MaxPqMinValue = 2081;
static constexpr uint16_t kL1MaxPqMaxValue = 4095;
static constexpr uint16_t kL1AvgPqMinValue = 819;

// ---------------------------------------------------------------------------
// Vivid → VdrDmData
// ---------------------------------------------------------------------------

static uint16_t source_max_pq_from_mdcv(uint32_t maxLum)
{
  switch (maxLum)
  {
    case 0:     return 3079;  // default 1000 nits
    case 1000:  return 3079;
    case 2000:  return 3388;
    case 4000:  return 3696;
    case 10000: return 4095;
    default:    return cast_pq(static_cast<double>(maxLum));
  }
}

static uint16_t source_min_pq_from_mdcv(uint32_t minLum)
{
  if (minLum == 0)   return 0;
  if (minLum < 2)    return 7;   // 0.0001 nits → PQ
  if (minLum < 5)    return 10;
  if (minLum < 10)   return 17;
  if (minLum < 20)   return 26;
  if (minLum < 50)   return 38;
  return 62;                     // 0.005 nits
}

// Cache the last RPU to avoid redundant generation.  thread_local because
// the parser is called from the decoder thread and the cache must not race
// with itself.
static thread_local std::vector<uint8_t> s_lastVividRpu;
static thread_local VdrDmData s_lastVividVdrDmData = {};

// ---------------------------------------------------------------------------
// Layer 1 helpers: MDCV mastering display peak → calibrated peak luminance
// ---------------------------------------------------------------------------

/**
 * Derive the peak luminance reference (nits) for maxrgb→PQ conversion.
 *
 * Precedence:
 *   1. MDCV max_lum from container (mastering display peak – correct reference)
 *   2. 10000 nits (ST.2084 max — conservative upper-bound when MDCV is missing)
 */
static double vivid_peak_nits(const HDRStaticMetadataInfo& hdrStatic)
{
  // Primary: MDCV mastering display peak
  if (hdrStatic.max_lum > 0)
    return static_cast<double>(hdrStatic.max_lum);

  // MDCV missing: use ST.2084 maximum as conservative upper-bound.
  // This prevents the darkened-PQ problem caused by using a low display
  // luminance as the mastering peak reference.
  return 10000.0;
}

// ---------------------------------------------------------------------------
// Main conversion entry point
// ---------------------------------------------------------------------------

std::vector<uint8_t> create_dovi_rpu_nalu_from_vivid(
    const HdrVividMetadata& meta,
    const HDRStaticMetadataInfo& hdrStatic)
{
  if (meta.num_windows == 0)
    return s_lastVividRpu; // no data, return previous if any

  const HdrVividWindowParams& win = meta.params[0];

  // ── Layer 1: calibrated peak luminance (MDCV-first, TM fallback) ──
  double peakNits = vivid_peak_nits(hdrStatic);

  // ── source_min_pq / source_max_pq (static per stream) ──
  uint16_t srcMinPq = source_min_pq_from_mdcv(hdrStatic.min_lum);
  uint16_t srcMaxPq = source_max_pq_from_mdcv(hdrStatic.max_lum);

  // ── Raw maxrgb → nits → PQ ──
  uint16_t maxrgb = win.maximum_maxrgb;

  // Black / transition frame guard
  if (maxrgb == 0)
    return s_lastVividRpu;

  double rawMaxNits = (static_cast<double>(maxrgb) / kVividMaxrgbDen) * peakNits;

  uint16_t avgrgb = win.average_maxrgb;
  if (avgrgb > maxrgb)
    avgrgb = maxrgb;
  double rawAvgNits = (static_cast<double>(avgrgb) / kVividMaxrgbDen) * peakNits;

  // min_pq: static MDCV-based lookup (same as HDR10+).  Do NOT derive
  // min_pq from per-frame minimum_maxrgb — it fluctuates from 0 to
  // >1000 PQ between frames depending on scene content, causing massive
  // L1 MIN wobble and visible flicker.  min_pq represents the mastering
  // display black floor, not the per-frame content minimum.
  uint16_t minPq = source_min_pq_from_mdcv(hdrStatic.min_lum);

  uint16_t maxPq = cast_pq(std::max(rawMaxNits, 1.0));
  uint16_t avgPq = cast_pq(std::max(rawAvgNits, 0.0));

  // ── Scene-level TM intent → L1 bias ──
  //
  // Vivid base_params m_b (black level) and m_a (highlight transition
  // width) encode per-scene creative intent set by the encoder.
  //
  // No EMA / smoothing — m_b/m_a are constant within each encoder scene
  // (typically 5+ seconds).  The RPU cache (uint16_t exact match across
  // 9 fields) absorbs any sub-scene ±1-2 LSB encoder jitter.
  {
    bool hasBase = win.tone_mapping_mode_flag &&
                   win.tone_mapping_param_num > 0 &&
                   win.tm_params[0].base_enable_flag;

    if (hasBase)
    {
      double m_b = static_cast<double>(
          win.tm_params[0].base_params.base_param_m_b) / kVividBaseParamMBDen;
      double m_a = static_cast<double>(
          win.tm_params[0].base_params.base_param_m_a) / kVividBaseParamMADen;

      // m_b → min_pq: darker scene intent → raise min_pq
      // m_b=0.0 → +0; m_b=0.65 → +300
      // Clamp ceiling = 2081 (max_pq floor), so min_pq never exceeds max_pq.
      int32_t mbBias = static_cast<int32_t>(m_b * 460.0);
      minPq = clamp16(
          static_cast<uint16_t>(static_cast<int32_t>(minPq) + mbBias),
          0, kL1MaxPqMinValue);

      // m_a → highlight scale: m_a < 0.5 means compressed highlights
      // m_a=1.0 → scale=1.0; m_a=0.17 → scale≈0.59
      double maScale = 0.5 + 0.5 * m_a;
      maxPq = static_cast<uint16_t>(static_cast<double>(maxPq) * maScale + 0.5);
      avgPq = static_cast<uint16_t>(static_cast<double>(avgPq) * maScale + 0.5);
    }
  }

  // ── Final L1 clamping ──
  maxPq = clamp16(maxPq, kL1MaxPqMinValue, kL1MaxPqMaxValue);
  avgPq = clamp16(avgPq, kL1AvgPqMinValue,
                  (maxPq > kL1AvgPqMinValue) ? maxPq - 1 : kL1AvgPqMinValue);

  // ── Assemble VdrDmData ──
  VdrDmData vdr = {};
  vdr.source_min_pq = srcMinPq;
  vdr.source_max_pq = srcMaxPq;
  vdr.min_pq = minPq;
  vdr.max_pq = maxPq;
  vdr.avg_pq = avgPq;
  vdr.max_display_mastering_luminance = hdrStatic.max_lum;
  vdr.min_display_mastering_luminance = hdrStatic.min_lum;
  vdr.max_content_light_level = hdrStatic.max_cll;
  vdr.max_frame_average_light_level = hdrStatic.max_fall;

  // ── Cached RPU generation ──
  if (s_lastVividVdrDmData != vdr)
  {
    s_lastVividRpu = create_dovi_rpu_nalu(vdr);
    s_lastVividVdrDmData = vdr;

    logM(LOGDEBUG, "HDRVividConvert",
         "src_min_pq [{}] src_max_pq [{}] min_pq [{}] max_pq [{}] avg_pq [{}] "
         "mdml_max [{}] mdml_min [{}] cll [{}] fall [{}] peakNits [{:.0f}]",
         vdr.source_min_pq, vdr.source_max_pq,
         vdr.min_pq, vdr.max_pq, vdr.avg_pq,
         vdr.max_display_mastering_luminance,
         vdr.min_display_mastering_luminance,
         vdr.max_content_light_level,
         vdr.max_frame_average_light_level,
         peakNits);
  }

  return s_lastVividRpu;
}
