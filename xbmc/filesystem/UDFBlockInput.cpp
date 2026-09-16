/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "UDFBlockInput.h"

#include "IFileTypes.h"
#include "filesystem/File.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include <udfread/udfread.h>

namespace
{
//! What one read from the image brings back: a request covers this many of UDF's 2048-byte blocks
constexpr size_t PAGE_SIZE{256 * 1024};
constexpr size_t PAGE_BLOCKS{PAGE_SIZE / UDF_BLOCK_SIZE};
static_assert(PAGE_BLOCKS > 0);

/*!
 \brief How many pages to keep.

 UDF keeps a file's entry in a metadata partition and its content in the data partition, so
 reading a file is a run of reads from one followed by a run from the other, over and over.
 A cache with a single window evicts one of them every time the other is read, which makes every
 read its own request to the source - on a disc image over a network, that is the whole cost of
 opening the disc. Holding several pages lets both runs stay resident, so a page is read once
 however often the two alternate.
 */
constexpr size_t MAX_PAGES{64}; // 16MB
} // namespace

int CUDFBlockInput::Close(udfread_block_input* bi)
{
  auto m_bi = reinterpret_cast<UDF_BI*>(bi);

  m_bi->pages.clear();
  m_bi->recent.clear();
  m_bi->fp->Close();

  return 0;
}

uint32_t CUDFBlockInput::Size(udfread_block_input* bi)
{
  auto m_bi = reinterpret_cast<UDF_BI*>(bi);

  return static_cast<uint32_t>(m_bi->fp->GetLength() / UDF_BLOCK_SIZE);
}

CUDFBlockInputPage* CUDFBlockInput::GetPage(UDF_BI& bi, uint64_t page)
{
  if (const auto it = bi.pages.find(page); it != bi.pages.end())
  {
    bi.recent.erase(it->second.recent);
    bi.recent.push_front(page);
    it->second.recent = bi.recent.begin();
    return &it->second;
  }

  // Drop the least recently used pages, which are the ones least likely to be asked for again
  while (bi.pages.size() >= MAX_PAGES)
  {
    bi.pages.erase(bi.recent.back());
    bi.recent.pop_back();
  }

  const int64_t offset = static_cast<int64_t>(page) * PAGE_SIZE;
  if (bi.fp->Seek(offset, SEEK_SET) != offset)
    return nullptr;

  CUDFBlockInputPage entry;
  entry.data.resize(PAGE_SIZE);

  const ssize_t read = bi.fp->Read(entry.data.data(), PAGE_SIZE);
  if (read <= 0)
    return nullptr;

  entry.valid = static_cast<size_t>(read);

  bi.recent.push_front(page);
  entry.recent = bi.recent.begin();

  return &bi.pages.emplace(page, std::move(entry)).first->second;
}

int CUDFBlockInput::Read(
    udfread_block_input* bi, uint32_t lba, void* buf, uint32_t blocks, int flags)
{
  auto m_bi = reinterpret_cast<UDF_BI*>(bi);

  if (!m_bi || !m_bi->fp || blocks == 0)
    return -1;

  std::lock_guard lock(m_bi->lock);

  auto* out = static_cast<uint8_t*>(buf);
  uint64_t block{lba};
  uint32_t left{blocks};

  while (left > 0)
  {
    const uint64_t page{block / PAGE_BLOCKS};
    const auto inPage{static_cast<uint32_t>(block % PAGE_BLOCKS)};
    const uint32_t take{std::min(left, static_cast<uint32_t>(PAGE_BLOCKS - inPage))};

    const CUDFBlockInputPage* entry{GetPage(*m_bi, page)};
    if (!entry)
      return -1;

    const size_t from{static_cast<size_t>(inPage) * UDF_BLOCK_SIZE};
    const size_t want{static_cast<size_t>(take) * UDF_BLOCK_SIZE};

    // Only the image ending part way through a page can leave less here than was asked for
    if (from >= entry->valid)
      break;

    const size_t have{std::min(want, entry->valid - from)};
    std::memcpy(out, entry->data.data() + from, have);

    out += have;
    block += take;
    left -= take;

    if (have < want)
      break;
  }

  const auto read{static_cast<size_t>(block - lba) * UDF_BLOCK_SIZE};
  return static_cast<int>(read / UDF_BLOCK_SIZE);
}


udfread_block_input* CUDFBlockInput::GetBlockInput(const std::string& file)
{
  auto fp = std::make_shared<XFILE::CFile>();

  if (fp->Open(file))
  {
    m_bi = std::make_unique<UDF_BI>();
    if (m_bi)
    {
      m_bi->fp = fp;
      m_bi->bi.close = CUDFBlockInput::Close;
      m_bi->bi.read = CUDFBlockInput::Read;
      m_bi->bi.size = CUDFBlockInput::Size;

      return &m_bi->bi;
    }

    fp->Close();
  }

  return nullptr;
}
