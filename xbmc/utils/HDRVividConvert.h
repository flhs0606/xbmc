/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <vector>

#include "HDRVivid.h"
#include "HDR10PlusConvert.h"  // for VdrDmData

/**
 * Convert HDR Vivid dynamic metadata to a Dolby Vision RPU NAL unit.
 *
 * Extracts min/max/avg luminance from Vivid window params and fills
 * a VdrDmData structure, then reuses DoViRpuWriter to generate the
 * RPU NAL bytes. The RPU is profile 8.1 compatible.
 *
 * @param meta        Parsed HDR Vivid metadata from SEI
 * @param hdrStatic   Static HDR metadata (MDCV + CLL from SEI or container)
 * @return            Complete DoVi RPU NAL unit bytes (with start code emulation prevention)
 */
std::vector<uint8_t> create_dovi_rpu_nalu_from_vivid(
    const HdrVividMetadata& meta,
    const HDRStaticMetadataInfo& hdrStatic);
