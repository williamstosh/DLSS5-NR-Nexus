/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <mutex>
#include <shared_mutex>
#include <utility>

#include <include/reshade.hpp>

namespace renodx::addons::dlss5::configuration {

namespace internal {
static std::shared_mutex mutex;
}  // namespace internal

// The SDK returns the shared INI object after releasing its cache lock. Even reads
// can reload that object, so all of this add-on's SDK config access uses one lock.
template <typename... Args>
bool Get(Args&&... args) {
  const std::unique_lock lock(internal::mutex);
  return reshade::get_config_value(std::forward<Args>(args)...);
}

template <typename... Args>
void Set(Args&&... args) {
  const std::unique_lock lock(internal::mutex);
  reshade::set_config_value(std::forward<Args>(args)...);
}

}  // namespace renodx::addons::dlss5::configuration
