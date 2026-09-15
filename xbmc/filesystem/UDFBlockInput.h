/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/CriticalSection.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <vector>

#include <udfread/blockinput.h>

namespace XFILE
{
class CFile;
}

/*!
 \brief A part of the image, held so that UDF's reads do not each cost a request to the source.
 */
struct CUDFBlockInputPage
{
  std::vector<uint8_t> data;

  //! Bytes the image holds here, short for the last page of an image that does not end on one
  size_t valid{0};

  //! Position in the least-recently-used list
  std::list<uint64_t>::iterator recent;
};

class CUDFBlockInput
{
public:
  CUDFBlockInput() = default;
  ~CUDFBlockInput() = default;

  udfread_block_input* GetBlockInput(const std::string& file);

  /*!
   \brief Tell the image's file cache how fast its content is being consumed.
   Mounting and reading a volume goes through this one file, so this is where the cache that
   reads the image ahead sits - not on any stream above it.
   \param rate bytes per second
   */
  void SetReadRate(uint32_t rate);

private:
  struct UDF_BI;

  static int Close(udfread_block_input* bi);
  static uint32_t Size(udfread_block_input* bi);
  static int Read(udfread_block_input* bi, uint32_t lba, void* buf, uint32_t nblocks, int flags);

  /*!
   \brief Get the page holding a block, reading it from the image if it is not held already.
   \return the page, or nullptr if the image could not be read
   */
  static CUDFBlockInputPage* GetPage(UDF_BI& bi, uint64_t page);

  struct UDF_BI
  {
    struct udfread_block_input bi;
    std::shared_ptr<XFILE::CFile> fp{nullptr};
    CCriticalSection lock;

    //! Pages held, most recently used first
    std::list<uint64_t> recent;

    std::map<uint64_t, CUDFBlockInputPage> pages;
  };

  std::unique_ptr<UDF_BI> m_bi{nullptr};
};
