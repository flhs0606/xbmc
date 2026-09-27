/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "IProgressCallback.h"
#include "threads/CriticalSection.h"
#include "threads/IRunnable.h"

#include <atomic>
#include <memory>
#include <vector>

class CFileItem; typedef std::shared_ptr<CFileItem> CFileItemPtr;
class CFileItemList;
class CThread;

class IBackgroundLoaderObserver
{
public:
  virtual ~IBackgroundLoaderObserver() = default;
  virtual void OnItemLoaded(CFileItem* pItem) = 0;
};

class CBackgroundInfoLoader : public IRunnable
{
public:
  CBackgroundInfoLoader();
  ~CBackgroundInfoLoader() override;

  void Load(CFileItemList& items);
  bool IsLoading();
  void Run() override;
  void SetObserver(IBackgroundLoaderObserver* pObserver);
  void SetProgressCallback(IProgressCallback* pCallback);
  virtual bool LoadItem(CFileItem* pItem) { return false; }
  virtual bool LoadItemCached(CFileItem* pItem) { return false; }
  virtual bool LoadItemLookup(CFileItem* pItem) { return false; }

  void StopThread(); // will actually stop the loader thread.
  void StopAsync();  // will ask loader to stop as soon as possible, but not block

  /*! \brief Set high-priority range for visible viewport items to be loaded first
   \param start The start index in the item vector
   \param count The number of items to prioritize
   */
  void SetPriorityRange(int start, int count);

protected:
  virtual void OnLoaderStart() {}
  virtual void OnLoaderFinish() {}

  CFileItemList* m_pVecItems{nullptr};
  std::vector<CFileItemPtr> m_vecItems; // FileItemList would delete the items and we only want to keep a reference.
  CCriticalSection m_lock;

  std::atomic<int> m_priorityStart{-1};
  std::atomic<int> m_priorityCount{0};
  std::atomic<bool> m_hasPriority{false};

  volatile bool m_bIsLoading{false};
  volatile bool m_bStop{true};
  CThread* m_thread{nullptr};

  IBackgroundLoaderObserver* m_pObserver{nullptr};
  IProgressCallback* m_pProgressCallback{nullptr};

private:
  void Reset();
};

