/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "ServiceBroker.h"
#include "utils/log.h"

#include <chrono>

#define LOG_THROTTLE_PERIODIC(level, component, intervalMs, ...) \
  do \
  { \
    if (CServiceBroker::GetLogging().IsLogLevelLogged(level) && \
        CServiceBroker::GetLogging().CanLogComponent(component)) \
    { \
      static std::chrono::steady_clock::time_point throttlePrev{}; \
      const auto throttleNow = std::chrono::steady_clock::now(); \
      if (throttlePrev.time_since_epoch().count() == 0 || \
          throttleNow - throttlePrev >= std::chrono::milliseconds(intervalMs)) \
      { \
        throttlePrev = throttleNow; \
        CLog::Log((level), (component), __VA_ARGS__); \
      } \
    } \
  } while (0)

#define LOG_THROTTLE_PERIODIC_GENERAL(level, intervalMs, ...) \
  do \
  { \
    if (CServiceBroker::GetLogging().IsLogLevelLogged(level)) \
    { \
      static std::chrono::steady_clock::time_point throttlePrev{}; \
      const auto throttleNow = std::chrono::steady_clock::now(); \
      if (throttlePrev.time_since_epoch().count() == 0 || \
          throttleNow - throttlePrev >= std::chrono::milliseconds(intervalMs)) \
      { \
        throttlePrev = throttleNow; \
        CLog::Log((level), __VA_ARGS__); \
      } \
    } \
  } while (0)

#define LOG_THROTTLE_ONCHANGE(level, component, key, intervalMs, ...) \
  do \
  { \
    if (CServiceBroker::GetLogging().IsLogLevelLogged(level) && \
        CServiceBroker::GetLogging().CanLogComponent(component)) \
    { \
      const auto throttleKey = (key); \
      static auto throttleLastKey = throttleKey; \
      static bool throttleSeen = false; \
      static std::chrono::steady_clock::time_point throttlePrev{}; \
      const auto throttleNow = std::chrono::steady_clock::now(); \
      if (!throttleSeen || throttleKey != throttleLastKey || \
          throttleNow - throttlePrev >= std::chrono::milliseconds(intervalMs)) \
      { \
        throttleSeen = true; \
        throttleLastKey = throttleKey; \
        throttlePrev = throttleNow; \
        CLog::Log((level), (component), __VA_ARGS__); \
      } \
    } \
  } while (0)
