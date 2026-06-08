/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/DVDStreamInfo.h"

#include "HDR10Plus.h"

enum class PeakBrightnessSource {
  Histogram = 0,
  Histogram99,
  MaxScl,
  MaxSclLuminance,
  HistogramPlus
};

struct VdrDmData {

  VdrDmData() {} // Default constructor

  uint16_t min_pq;
  uint16_t max_pq;
  uint16_t avg_pq;

  uint16_t source_min_pq;
  uint16_t source_max_pq;

  uint16_t max_display_mastering_luminance;
  uint16_t min_display_mastering_luminance;
  uint16_t max_content_light_level;
  uint16_t max_frame_average_light_level;
};

inline bool operator==(const VdrDmData& a, const VdrDmData& b)
{
  return a.min_pq == b.min_pq && a.max_pq == b.max_pq && a.avg_pq == b.avg_pq &&
         a.source_min_pq == b.source_min_pq && a.source_max_pq == b.source_max_pq &&
         a.max_display_mastering_luminance == b.max_display_mastering_luminance &&
         a.min_display_mastering_luminance == b.min_display_mastering_luminance &&
         a.max_content_light_level == b.max_content_light_level &&
         a.max_frame_average_light_level == b.max_frame_average_light_level;
}

inline bool operator!=(const VdrDmData& a, const VdrDmData& b)
{
  return !(a == b);
}

// ST2084 / PQ helpers — shared by HDR10+ and HDR Vivid conversion paths.
double nits_to_pq(double nits);
uint16_t cast_pq(double nits);
uint16_t clamp16(uint16_t d, uint16_t min, uint16_t max);

std::vector<uint8_t> create_dovi_rpu_nalu_from_hdr10plus(
  const Hdr10PlusMetadata& meta,
  const PeakBrightnessSource& peak_source,
  const HDRStaticMetadataInfo& hdrStaticMetadataInfo);

int max_pq_to_nits(int pq);
