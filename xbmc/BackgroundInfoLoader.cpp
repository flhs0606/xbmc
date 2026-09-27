/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BackgroundInfoLoader.h"

#include "FileItem.h"
#include "URL.h"
#include "threads/Thread.h"
#include "utils/log.h"

#include <algorithm>
#include <mutex>

CBackgroundInfoLoader::CBackgroundInfoLoader() = default;

CBackgroundInfoLoader::~CBackgroundInfoLoader()
{
  StopThread();
}

void CBackgroundInfoLoader::Reset()
{
  m_pVecItems = nullptr;
  m_vecItems.clear();
  m_bIsLoading = false;
  m_hasPriority.store(false, std::memory_order_relaxed);
  m_priorityStart.store(-1, std::memory_order_relaxed);
  m_priorityCount.store(0, std::memory_order_relaxed);
}

void CBackgroundInfoLoader::SetPriorityRange(int start, int count)
{
  m_priorityStart.store(std::max(0, start), std::memory_order_relaxed);
  m_priorityCount.store(std::max(0, count), std::memory_order_relaxed);
  m_hasPriority.store(true, std::memory_order_release);
}

void CBackgroundInfoLoader::Run()
{
  try
  {
    if (!m_vecItems.empty())
    {
      OnLoaderStart();

      // Stage 1: All "fast" stuff we have already cached
      auto processItemCached = [&](size_t idx) {
        if (idx >= m_vecItems.size())
          return;
        const CFileItemPtr& pItem = m_vecItems[idx];
        if (!pItem)
          return;
        try
        {
          if (LoadItemCached(pItem.get()) && m_pObserver)
            m_pObserver->OnItemLoaded(pItem.get());
        }
        catch (...)
        {
          CLog::Log(LOGERROR,
                    "CBackgroundInfoLoader::LoadItemCached - Unhandled exception for item {}",
                    CURL::GetRedacted(pItem->GetPath()));
        }
      };

      size_t iterIdx = 0;
      while (iterIdx < m_vecItems.size())
      {
        if ((m_pProgressCallback && m_pProgressCallback->Abort()) || m_bStop)
          break;

        // Prioritize viewport items requested by the UI thread
        if (m_hasPriority.exchange(false, std::memory_order_acq_rel))
        {
          const int pStart = m_priorityStart.load(std::memory_order_relaxed);
          const int pCount = m_priorityCount.load(std::memory_order_relaxed);
          const int pEnd = std::min(static_cast<int>(m_vecItems.size()), pStart + pCount);
          for (int p = pStart; p < pEnd; ++p)
          {
            if ((m_pProgressCallback && m_pProgressCallback->Abort()) || m_bStop)
              break;
            processItemCached(static_cast<size_t>(p));
          }
        }

        processItemCached(iterIdx++);
      }

      // Stage 2: All "slow" stuff that we need to lookup
      for (std::vector<CFileItemPtr>::const_iterator iter = m_vecItems.begin(); iter != m_vecItems.end(); ++iter)
      {
        const CFileItemPtr& pItem = *iter;

        // Ask the callback if we should abort
        if ((m_pProgressCallback && m_pProgressCallback->Abort()) || m_bStop)
          break;

        try
        {
          if (LoadItemLookup(pItem.get()) && m_pObserver)
            m_pObserver->OnItemLoaded(pItem.get());
        }
        catch (...)
        {
          CLog::Log(LOGERROR,
                    "CBackgroundInfoLoader::LoadItemLookup - Unhandled exception for item {}",
                    CURL::GetRedacted(pItem->GetPath()));
        }
      }
    }

    OnLoaderFinish();
  }
  catch (...)
  {
    CLog::Log(LOGERROR, "{} - Unhandled exception", __FUNCTION__);
  }

  Reset();
}

void CBackgroundInfoLoader::Load(CFileItemList& items)
{
  StopThread();

  if (items.IsEmpty())
    return;

  std::lock_guard lock(m_lock);

  for (int nItem=0; nItem < items.Size(); nItem++)
    m_vecItems.push_back(items[nItem]);

  m_pVecItems = &items;
  m_bStop = false;
  m_bIsLoading = true;

  m_thread = new CThread(this, "BackgroundLoader");
  m_thread->Create();
  m_thread->SetPriority(ThreadPriority::BELOW_NORMAL);
}

void CBackgroundInfoLoader::StopAsync()
{
  m_bStop = true;
}


void CBackgroundInfoLoader::StopThread()
{
  StopAsync();

  if (m_thread)
  {
    m_thread->StopThread();
    delete m_thread;
    m_thread = nullptr;
  }
  Reset();
}

bool CBackgroundInfoLoader::IsLoading()
{
  return m_bIsLoading;
}

void CBackgroundInfoLoader::SetObserver(IBackgroundLoaderObserver* pObserver)
{
  m_pObserver = pObserver;
}

void CBackgroundInfoLoader::SetProgressCallback(IProgressCallback* pCallback)
{
  m_pProgressCallback = pCallback;
}

