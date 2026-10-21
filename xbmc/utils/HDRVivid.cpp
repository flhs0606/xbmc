/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  HDR Vivid (CUVA 005.1:2022) bitstream parser.
 *  Reference: FFmpeg libavcodec/dynamic_hdr_vivid.c
 *             ff_parse_itu_t_t35_to_dynamic_hdr_vivid()
 */

#include "HDRVivid.h"
#include "BitstreamReader.h"

#include <cstdint>

HdrVividMetadata hdr_vivid_sei_to_metadata(CBitstreamReader& br)
{
  HdrVividMetadata metadata = {};

  // Skip T.35 header: country_code(8) + provider_code(16) + oriented_code(16)
  // The caller (HevcSei::ExtractHdrVivid) already verified these match
  // China / CUVA HDR Vivid, so we skip ahead to system_start_code.
  br.ReadBits(8);   // itu_t_t35_country_code
  br.ReadBits(16);  // itu_t_t35_terminal_provider_code
  br.ReadBits(16);  // itu_t_t35_terminal_provider_oriented_code

  // system_start_code: 8 bits (0x01~0x07 for CUVA 005.1)
  metadata.system_start_code = br.ReadBits(8);

  // T/UWA 005.1-2022, table 11: system_start_code 0x01~0x07 → num_windows = 1
  if (metadata.system_start_code >= 0x01 && metadata.system_start_code <= 0x07)
  {
    metadata.num_windows = 1;

    for (uint8_t w = 0; w < metadata.num_windows; w++)
    {
      HdrVividWindowParams& params = metadata.params[w];

      // 4 × 12-bit maxrgb values
      params.minimum_maxrgb  = br.ReadBits(12);
      params.average_maxrgb  = br.ReadBits(12);
      params.variance_maxrgb = br.ReadBits(12);
      params.maximum_maxrgb  = br.ReadBits(12);
    }

    // Tone mapping parameters per window
    for (uint8_t w = 0; w < metadata.num_windows; w++)
    {
      HdrVividWindowParams& params = metadata.params[w];

      params.tone_mapping_mode_flag = br.ReadBits(1);

      if (params.tone_mapping_mode_flag)
      {
        // tone_mapping_param_num: 1 bit → actual = value + 1, range 1~2
        params.tone_mapping_param_num = br.ReadBits(1) + 1;

        for (uint8_t i = 0; i < params.tone_mapping_param_num; i++)
        {
          HdrVividToneMappingParams& tm = params.tm_params[i];

          // targeted_system_display_maximum_luminance: 12 bits, den=4095
          tm.targeted_system_display_maximum_luminance = br.ReadBits(12);

          tm.base_enable_flag = br.ReadBits(1);

          if (tm.base_enable_flag)
          {
            // Base parameters: 14+6+10+10+6+2+2+4+3+7 = 64 bits
            tm.base_params.base_param_m_p   = br.ReadBits(14);
            tm.base_params.base_param_m_m   = br.ReadBits(6);
            tm.base_params.base_param_m_a   = br.ReadBits(10);
            tm.base_params.base_param_m_b   = br.ReadBits(10);
            tm.base_params.base_param_m_n   = br.ReadBits(6);
            tm.base_params.base_param_k1    = br.ReadBits(2);
            tm.base_params.base_param_k2    = br.ReadBits(2);
            tm.base_params.base_param_k3    = br.ReadBits(4);
            tm.base_params.base_param_Delta_enable_mode = br.ReadBits(3);
            tm.base_params.base_param_Delta  = br.ReadBits(7);
          }

          // Three-Spline parameters
          tm.three_Spline_enable_flag = br.ReadBits(1);

          if (tm.three_Spline_enable_flag)
          {
            // three_Spline_num: 1 bit → actual = value + 1, range 1~2
            tm.three_Spline_num = br.ReadBits(1) + 1;

            for (uint8_t j = 0; j < tm.three_Spline_num; j++)
            {
              HdrVivid3SplineParams& spline = tm.three_spline[j];

              spline.th_mode        = br.ReadBits(2);

              if (spline.th_mode == 0 || spline.th_mode == 2)
                spline.th_enable_mb = br.ReadBits(8);

              spline.th_enable      = br.ReadBits(12);
              spline.th_delta1      = br.ReadBits(10);
              spline.th_delta2      = br.ReadBits(10);
              spline.enable_strength = br.ReadBits(8);
            }
          }
        }
      }

      // Color saturation mapping
      params.color_saturation_mapping_flag = br.ReadBits(1);

      if (params.color_saturation_mapping_flag)
      {
        params.color_saturation_num = br.ReadBits(3);

        for (uint8_t i = 0; i < params.color_saturation_num; i++)
        {
          params.color_saturation_gain[i] = br.ReadBits(8);
        }
      }
    }
  }

  return metadata;
}
