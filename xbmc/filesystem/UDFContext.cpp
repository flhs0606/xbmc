/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "UDFContext.h"

#include "URL.h"
#include "threads/CriticalSection.h"
#include "utils/log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <udfread/udfread.h>

using namespace XFILE;
using namespace std::chrono_literals;

namespace
{
/*!
 \brief How long a volume stays mounted after the last reader lets go of it.

 Examining a disc opens, reads and closes the volume several times in a row - the simple menu
 probes for index.bdmv, the directory lists the playlists, the player opens the disc - and each
 of those would otherwise re-mount. On a disc image reached over the network a mount costs a
 round trip to each of the volume's descriptors, and the cache underneath it is thrown away and
 has to be refilled, so the repeat is worth avoiding.

 The volume is held on to no longer than this after the last reader, because holding it keeps an
 open handle on the image and the whole of the file cache it reads through allocated to it.
 */
constexpr auto MOUNT_GRACE{20s};

//! How often the cleanup thread looks for volumes that have been let go
constexpr auto CLEANUP_INTERVAL{1s};

struct MountEntry
{
  std::shared_ptr<CUDFContext> context;

  //! Refreshed for as long as the volume has a reader, so a mount in use is never let go
  std::chrono::steady_clock::time_point lastUsed;
};

/*!
 \brief The volumes currently mounted, so that readers of one image share a single mount.

 Held on the heap and never freed: the cleanup thread refers to it for the life of the process,
 and it must outlive every static that could otherwise be destroyed before it.
 */
struct MountRegistry
{
  CCriticalSection lock;
  std::map<std::string, MountEntry, std::less<>> mounts;
  std::condition_variable_any cv;

  std::thread cleanup;
  std::atomic<bool> running{false};
};

MountRegistry& GetMountRegistry()
{
  static MountRegistry* registry{new MountRegistry()};
  return *registry;
}

void CleanupThreadFunc()
{
  MountRegistry& registry{GetMountRegistry()};

  std::unique_lock lock(registry.lock);
  while (registry.running)
  {
    registry.cv.wait_for(lock, CLEANUP_INTERVAL);
    if (!registry.running)
      break;

    const auto now{std::chrono::steady_clock::now()};

    // Volumes to unmount, destroyed outside the lock: letting a volume go closes the image
    std::vector<std::shared_ptr<CUDFContext>> expired;

    for (auto it = registry.mounts.begin(); it != registry.mounts.end();)
    {
      if (it->second.context.use_count() > 1)
      {
        // Something is reading from this volume, so its idle time starts again
        it->second.lastUsed = now;
        ++it;
      }
      else if (now - it->second.lastUsed >= MOUNT_GRACE)
      {
        CLog::LogF(LOGDEBUG, "Unmounting UDF volume of {}", CURL::GetRedacted(it->first));
        expired.emplace_back(std::move(it->second.context));
        it = registry.mounts.erase(it);
      }
      else
      {
        ++it;
      }
    }

    lock.unlock();
    expired.clear();
    lock.lock();
  }
}

void EnsureCleanupThread()
{
  MountRegistry& registry{GetMountRegistry()};
  if (!registry.running.exchange(true))
    registry.cleanup = std::thread(CleanupThreadFunc);
}

} // namespace

CUDFContext::~CUDFContext()
{
  if (m_udf)
    udfread_close(m_udf); // Closes the block input too
}

bool CUDFContext::Mount(const std::string& image)
{
  m_udf = udfread_init();
  if (!m_udf)
    return false;

  udfread_block_input* blockInput{m_blockInput.GetBlockInput(image)};
  if (!blockInput)
  {
    udfread_close(m_udf);
    m_udf = nullptr;
    return false;
  }

  if (udfread_open_input(m_udf, blockInput) < 0)
  {
    blockInput->close(blockInput);
    udfread_close(m_udf);
    m_udf = nullptr;
    return false;
  }

  return true;
}

std::shared_ptr<CUDFContext> CUDFContext::Get(const std::string& image)
{
  MountRegistry& registry{GetMountRegistry()};

  EnsureCleanupThread();

  std::unique_lock lock(registry.lock);

  if (const auto it{registry.mounts.find(image)}; it != registry.mounts.end())
  {
    it->second.lastUsed = std::chrono::steady_clock::now();
    return it->second.context;
  }

  // Cannot use make_shared as the constructor is private
  std::shared_ptr<CUDFContext> context{new CUDFContext()};
  if (!context->Mount(image))
    return nullptr;

  // One line per mount, so a run of reads that is sharing one mount is distinguishable from one
  // that is re-mounting for every file
  CLog::LogF(LOGDEBUG, "Mounted UDF volume of {}", CURL::GetRedacted(image));

  registry.mounts[image] = MountEntry{context, std::chrono::steady_clock::now()};

  return context;
}


CUDFMount::CUDFMount(const std::string& path)
{
  if (const CURL url{path}; url.IsProtocol("udf"))
    m_context = CUDFContext::Get(url.GetHostName());
}

