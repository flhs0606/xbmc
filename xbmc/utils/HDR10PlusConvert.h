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

  bool operator==(const VdrDmData& other) const
  {
    return min_pq == other.min_pq &&
           max_pq == other.max_pq &&
           avg_pq == other.avg_pq &&
           source_min_pq == other.source_min_pq &&
           source_max_pq == other.source_max_pq &&
           max_display_mastering_luminance == other.max_display_mastering_luminance &&
           min_display_mastering_luminance == other.min_display_mastering_luminance &&
           max_content_light_level == other.max_content_light_level &&
           max_frame_average_light_level == other.max_frame_average_light_level;
  }
  bool operator!=(const VdrDmData& other) const { return !(*this == other); }
};

uint16_t cast_pq(double nits);
uint16_t clamp16(uint16_t d, uint16_t min, uint16_t max);

std::vector<uint8_t> create_dovi_rpu_nalu_from_hdr10plus(
  const Hdr10PlusMetadata& meta,
  const PeakBrightnessSource& peak_source,
  const HDRStaticMetadataInfo& hdrStaticMetadataInfo);

int max_pq_to_nits(int pq);
int nits_to_max_pq(int nits);
