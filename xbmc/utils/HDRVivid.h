/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>

#include "BitstreamReader.h"

// CUVA 005.1:2022 HDR Vivid metadata structures
// Reference: FFmpeg libavutil/hdr_dynamic_vivid_metadata.h

// Denominator constants for CUVA 005.1 bitstream values
static constexpr int32_t kVividMaxrgbDen = 4095;
static constexpr int32_t kVividBaseParamMPDen = 16383;
static constexpr int32_t kVividBaseParamMMDen = 10;
static constexpr int32_t kVividBaseParamMADen = 1023;
static constexpr int32_t kVividBaseParamMBDen = 1023;
static constexpr int32_t kVividBaseParamMNDen = 10;
static constexpr int32_t kVividBaseParamDeltaDen = 127;
static constexpr int32_t kVividMaximumLuminanceDen = 4095;
static constexpr int32_t kVividColorSaturationGainDen = 128;

/**
 * Three-Spline parameters for HDR Vivid tone mapping.
 */
struct HdrVivid3SplineParams
{
  uint8_t th_mode{0};           // 2 bits, range 0~3
  uint8_t th_enable_mb{0};      // 8 bits, den=255
  uint16_t th_enable{0};        // 12 bits, den=4095
  uint16_t th_delta1{0};        // 10 bits, den=1023
  uint16_t th_delta2{0};        // 10 bits, den=1023
  uint8_t enable_strength{0};   // 8 bits, den=255
};

/**
 * Base parameters for HDR Vivid tone mapping.
 */
struct HdrVividBaseParams
{
  uint16_t base_param_m_p{0};               // 14 bits, den=16383
  uint8_t  base_param_m_m{0};               //  6 bits, den=10
  uint16_t base_param_m_a{0};               // 10 bits, den=1023
  uint16_t base_param_m_b{0};               // 10 bits, den=1023
  uint8_t  base_param_m_n{0};               //  6 bits, den=10
  uint8_t  base_param_k1{0};                //  2 bits
  uint8_t  base_param_k2{0};                //  2 bits
  uint8_t  base_param_k3{0};                //  4 bits
  uint8_t  base_param_Delta_enable_mode{0}; //  3 bits
  uint8_t  base_param_Delta{0};             //  7 bits, den=127
};

/**
 * Color tone mapping parameters at a processing window.
 */
struct HdrVividToneMappingParams
{
  uint16_t targeted_system_display_maximum_luminance{0}; // 12 bits, den=4095
  bool     base_enable_flag{false};
  HdrVividBaseParams base_params;
  bool     three_Spline_enable_flag{false};
  uint8_t  three_Spline_num{0};             // 1~2
  HdrVivid3SplineParams three_spline[2];
};

/**
 * Color transform parameters at a processing window.
 */
struct HdrVividWindowParams
{
  uint16_t minimum_maxrgb{0};   // 12 bits, den=4095
  uint16_t average_maxrgb{0};   // 12 bits, den=4095
  uint16_t variance_maxrgb{0};  // 12 bits, den=4095
  uint16_t maximum_maxrgb{0};   // 12 bits, den=4095

  bool     tone_mapping_mode_flag{false};
  uint8_t  tone_mapping_param_num{0};   // 1~2
  HdrVividToneMappingParams tm_params[2];

  bool     color_saturation_mapping_flag{false};
  uint8_t  color_saturation_num{0};     // 0~7
  uint8_t  color_saturation_gain[8]{};  // den=128, range 0.0~2.0
};

/**
 * Top-level HDR Vivid dynamic metadata (CUVA 005.1:2022).
 */
struct HdrVividMetadata
{
  uint8_t system_start_code{0};   // 8 bits, 0x01~0x07
  uint8_t num_windows{0};         // default 1
  HdrVividWindowParams params[3];
};

/**
 * Parse HDR Vivid SEI payload into structured metadata.
 * The reader MUST be positioned at the start of the T.35 payload
 * (itu_t_t35_country_code). This matches hdr10plus_sei_to_metadata
 * convention — the full T.35 header is consumed internally.
 *
 * @param br BitstreamReader positioned at T.35 country_code
 * @return Parsed HDR Vivid metadata
 */
HdrVividMetadata hdr_vivid_sei_to_metadata(CBitstreamReader& br);
