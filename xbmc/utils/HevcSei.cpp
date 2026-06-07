/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "HevcSei.h"
#include "HDR10Plus.h"
#include "HDRVivid.h"

void HevcAddStartCodeEmulationPrevention3Byte(std::vector<uint8_t>& buf)
{
  size_t i = 0;

  while (i < buf.size())
  {
    if (i > 2 && buf[i - 2] == 0 && buf[i - 1] == 0 && buf[i] <= 3)
      buf.insert(buf.begin() + i, 3);

    i += 1;
  }
}

void HevcClearStartCodeEmulationPrevention3Byte(const uint8_t* buf,
                                                const size_t len,
                                                std::vector<uint8_t>& out)
{
  size_t i = 0;

  if (len > 2)
  {
    out.reserve(len);

    out.emplace_back(buf[0]);
    out.emplace_back(buf[1]);

    for (i = 2; i < len; i++)
    {
      if (!(buf[i - 2] == 0 && buf[i - 1] == 0 && buf[i] == 3))
        out.emplace_back(buf[i]);
    }
  }
  else
  {
    out.assign(buf, buf + len);
  }
}

int CHevcSei::ParseSeiMessage(CBitstreamReader& br, std::vector<CHevcSei>& messages)
{
  CHevcSei sei;
  uint8_t lastPayloadTypeByte{0};
  uint8_t lastPayloadSizeByte{0};

  sei.m_msgOffset = br.Position() / 8;

  lastPayloadTypeByte = br.ReadBits(8);
  while (lastPayloadTypeByte == 0xFF)
  {
    lastPayloadTypeByte = br.ReadBits(8);
    sei.m_payloadType += 255;
  }

  sei.m_payloadType += lastPayloadTypeByte;

  lastPayloadSizeByte = br.ReadBits(8);
  while (lastPayloadSizeByte == 0xFF)
  {
    lastPayloadSizeByte = br.ReadBits(8);
    sei.m_payloadSize += 255;
  }

  sei.m_payloadSize += lastPayloadSizeByte;
  sei.m_payloadOffset = br.Position() / 8;

  // Invalid size — guard against corrupt NALs where payload size is zero
  // (which would cause an infinite loop in ParseSeiRbspInternal since
  // SkipBits(0) does not advance the bitstream position)
  if (sei.m_payloadSize == 0 || sei.m_payloadSize > br.AvailableBits())
    return 1;

  br.SkipBits(sei.m_payloadSize * 8);
  messages.emplace_back(sei);

  return 0;
}

std::vector<CHevcSei> CHevcSei::ParseSeiRbspInternal(const uint8_t* buf, const size_t len)
{
  std::vector<CHevcSei> messages;

  if (len > 4)
  {
    CBitstreamReader br(buf, len);

    // forbidden_zero_bit, nal_type, nuh_layer_id, temporal_id
    // nal_type == SEI_PREFIX should already be verified by caller
    br.SkipBits(16);

    while (true)
    {
      if (ParseSeiMessage(br, messages))
        break;

      if (br.AvailableBits() <= 8)
        break;
    }
  }

  return messages;
}

std::vector<CHevcSei> CHevcSei::ParseSeiRbsp(const uint8_t* buf, const size_t len)
{
  return ParseSeiRbspInternal(buf, len);
}

std::vector<CHevcSei> CHevcSei::ParseSeiRbspUnclearedEmulation(const uint8_t* inData,
                                                               const size_t inDataLen,
                                                               std::vector<uint8_t>& buf)
{
  HevcClearStartCodeEmulationPrevention3Byte(inData, inDataLen, buf);
  return ParseSeiRbsp(buf.data(), buf.size());
}

std::optional<const CHevcSei*> CHevcSei::FindHdr10PlusSeiMessage(
    const std::vector<uint8_t>& buf, const std::vector<CHevcSei>& messages)
{
  for (const CHevcSei& sei : messages)
  {
    // User Data Registered ITU-T T.35
    if (sei.m_payloadType == 4 && sei.m_payloadSize >= 7)
    {
      CBitstreamReader br(buf.data() + sei.m_payloadOffset, sei.m_payloadSize);
      const auto itu_t_t35_country_code = br.ReadBits(8);
      const auto itu_t_t35_terminal_provider_code = br.ReadBits(16);
      const auto itu_t_t35_terminal_provider_oriented_code = br.ReadBits(16);

      // United States, Samsung Electronics America, ST 2094-40
      if (itu_t_t35_country_code == 0xB5 && itu_t_t35_terminal_provider_code == 0x003C &&
          itu_t_t35_terminal_provider_oriented_code == 0x0001)
      {
        const auto application_identifier = br.ReadBits(8);
        const auto application_version = br.ReadBits(8);

        if (application_identifier == 4 && application_version <= 1)
          return &sei;
      }
    }
  }

  return {};
}

const std::optional<const Hdr10PlusMetadata> CHevcSei::ExtractHdr10Plus(
  const std::vector<CHevcSei>& messages,
  const std::vector<uint8_t>& buf)
{
  for (const CHevcSei& sei : messages)
  {
    // User Data Registered ITU-T T.35
    if (sei.m_payloadType == 4 && sei.m_payloadSize >= 7)
    {
      const unsigned char* data = buf.data() + sei.m_payloadOffset;
      size_t size = sei.m_payloadSize;

      CBitstreamReader br(data, size);
      const auto itu_t_t35_country_code = br.ReadBits(8);
      const auto itu_t_t35_terminal_provider_code = br.ReadBits(16);
      const auto itu_t_t35_terminal_provider_oriented_code = br.ReadBits(16);

      // United States, Samsung Electronics America, ST 2094-40
      if (itu_t_t35_country_code == 0xB5 && itu_t_t35_terminal_provider_code == 0x003C &&
          itu_t_t35_terminal_provider_oriented_code == 0x0001)
      {
        const auto application_identifier = br.ReadBits(8);
        const auto application_version = br.ReadBits(8);

        if (application_identifier == 4 && application_version <= 1) {
          CBitstreamReader br2(data, size);
          return hdr10plus_sei_to_metadata(br2);
        }
      }
    }
  }

  return std::nullopt;
}

// Validates a User-Data-Registered ITU-T T.35 SEI payload as HDR Vivid
// (CUVA 005.1:2022).  The reader is positioned at the start of the payload
// on entry; on success the reader is left positioned right after the
// system_start_code byte, ready to be rewound for full re-parse by
// hdr_vivid_sei_to_metadata().  On failure the reader is in an
// unspecified state.
static bool IsHdrVividSeiPayload(CBitstreamReader& br, size_t payloadSize)
{
  // 8 (country) + 16 (provider) + 16 (oriented) + 8 (system_start_code)
  if (payloadSize < 6)
    return false;

  const auto countryCode = br.ReadBits(8);
  const auto providerCode = br.ReadBits(16);
  const auto orientedCode = br.ReadBits(16);

  if (countryCode != 0x26 || providerCode != 0x0004 || orientedCode != 0x0005)
    return false;

  const auto systemStartCode = br.ReadBits(8);
  return systemStartCode >= 0x01 && systemStartCode <= 0x07;
}

const std::optional<const HdrVividMetadata> CHevcSei::ExtractHdrVivid(
  const std::vector<CHevcSei>& messages,
  const std::vector<uint8_t>& buf)
{
  for (const CHevcSei& sei : messages)
  {
    if (sei.m_payloadType == 4)
    {
      CBitstreamReader br(buf.data() + sei.m_payloadOffset, sei.m_payloadSize);
      if (IsHdrVividSeiPayload(br, sei.m_payloadSize))
      {
        // Re-parse the validated payload from the start.  This is one
        // extra pass over the 6-byte T.35 + system_start_code header,
        // but keeps the public hdr_vivid_sei_to_metadata contract
        // (reader positioned at country_code) unchanged.
        br.Rewind();
        return hdr_vivid_sei_to_metadata(br);
      }
    }
  }

  return std::nullopt;
}

const std::vector<uint8_t> CHevcSei::RemoveHdrVividFromSeiNalu(const uint8_t* inData, const size_t inDataLen)
{
  std::vector<uint8_t> buf;
  std::vector<CHevcSei> messages = CHevcSei::ParseSeiRbspUnclearedEmulation(inData, inDataLen, buf);

  // Find the HDR Vivid SEI message
  const CHevcSei* vividSei = nullptr;
  for (const CHevcSei& sei : messages)
  {
    if (sei.m_payloadType == 4)
    {
      CBitstreamReader br(buf.data() + sei.m_payloadOffset, sei.m_payloadSize);
      if (IsHdrVividSeiPayload(br, sei.m_payloadSize))
      {
        vividSei = &sei;
        break;
      }
    }
  }

  if (!vividSei)
  {
    buf.clear();
    return buf;
  }

  if (messages.size() > 1)
  {
    // Guard against corrupt NALs with invalid offsets
    if (vividSei->m_msgOffset <= buf.size() &&
        vividSei->m_payloadOffset + vividSei->m_payloadSize <= buf.size() &&
        vividSei->m_msgOffset <= vividSei->m_payloadOffset + vividSei->m_payloadSize)
    {
      // Multiple SEI messages: erase only the Vivid one from cleared buffer
      buf.erase(std::next(buf.begin(), vividSei->m_msgOffset),
                std::next(buf.begin(), vividSei->m_payloadOffset + vividSei->m_payloadSize));
      HevcAddStartCodeEmulationPrevention3Byte(buf);
    }
    else
    {
      // Offsets out of range — discard everything to be safe
      buf.clear();
    }
  }
  else
  {
    // Single SEI message: the whole payload was Vivid, discard
    buf.clear();
  }

  return buf;
}

const std::optional<MasteringDisplayColourVolume> CHevcSei::ExtractMasteringDisplayColourVolume(
  const std::vector<CHevcSei>& messages,
  const std::vector<uint8_t>& buf) {

  for (const auto& sei : messages) {

    // Check for Mastering Display Metadata SEI (payload type 137)
    if (sei.m_payloadType == 137 && sei.m_payloadSize >= 24) {

      CBitstreamReader br(buf.data() + sei.m_payloadOffset, sei.m_payloadSize);

      MasteringDisplayColourVolume metadata;

      // Read display primaries
      for (int i = 0; i < 3; ++i) {
          metadata.displayPrimaries[i].x = br.ReadBits(16);
          metadata.displayPrimaries[i].y = br.ReadBits(16);
      }

      // Read white point
      metadata.whitePoint.x = br.ReadBits(16);
      metadata.whitePoint.y = br.ReadBits(16);

      // Read max and min luminance
      uint32_t maxLuminanceRaw = br.ReadBits(32);
      uint32_t minLuminanceRaw = br.ReadBits(32);

      // Convert to nits (cd/m²) for max only.
      metadata.maxLuminance = static_cast<uint32_t>(maxLuminanceRaw) / 10000.0f;
      metadata.minLuminance = static_cast<uint32_t>(minLuminanceRaw);

      return metadata;
    }
  }
  return std::nullopt;
}

const std::optional<ContentLightLevel> CHevcSei::ExtractContentLightLevel(
  const std::vector<CHevcSei>& messages,
  const std::vector<uint8_t>& buf) {

  for (const auto& sei : messages) {

    // Check for Content Light Level Information SEI (payload type 144)
    if (sei.m_payloadType == 144 && sei.m_payloadSize >= 4) {

        CBitstreamReader br(buf.data() + sei.m_payloadOffset, sei.m_payloadSize);

        uint16_t maxCLL = br.ReadBits(16);
        uint16_t maxFALL = br.ReadBits(16);

        return ContentLightLevel{maxCLL, maxFALL};
    }
  }
  return std::nullopt;
}

const std::vector<uint8_t> CHevcSei::RemoveHdr10PlusFromSeiNalu(const uint8_t* inData, const size_t inDataLen)
{

  std::vector<uint8_t> buf;
  std::vector<CHevcSei> messages = CHevcSei::ParseSeiRbspUnclearedEmulation(inData, inDataLen, buf);

  if (auto res = CHevcSei::FindHdr10PlusSeiMessage(buf, messages))
  {
    auto msg = *res;
    if (messages.size() > 1)
    {
      // Guard against corrupt NALs with invalid offsets
      if (msg->m_msgOffset <= buf.size() &&
          msg->m_payloadOffset + msg->m_payloadSize <= buf.size() &&
          msg->m_msgOffset <= msg->m_payloadOffset + msg->m_payloadSize)
      {
        // Multiple SEI messages in NALU, remove only the HDR10+ one
        buf.erase(std::next(buf.begin(), msg->m_msgOffset),
                  std::next(buf.begin(), msg->m_payloadOffset + msg->m_payloadSize));
        HevcAddStartCodeEmulationPrevention3Byte(buf);
      }
      else
      {
        // Offsets out of range — discard everything to be safe
        buf.clear();
      }
    }
    else
    {
      // Single SEI message in NALU
      buf.clear();
    }
  }
  else
  {
    // No HDR10+
    buf.clear();
  }

  return buf;
}
