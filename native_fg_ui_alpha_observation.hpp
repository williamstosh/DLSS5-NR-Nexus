/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <include/reshade.hpp>

#include "nr_edit_fg_observation.hpp"

namespace renodx::addons::dlss5::native_ui_alpha_observation {

using Json = nlohmann::json;

inline constexpr uint32_t MAX_EVENTS = 256;
inline constexpr uint32_t MAX_CONSUMERS = 12;

struct Capture {
  std::shared_mutex mutex;
  std::atomic_uint64_t active{0};
  uint64_t generation = 0;
  uint32_t events = 0;
  uint32_t consumer_calls = 0;
  uintptr_t handle = 0;
  uintptr_t output = 0;
  std::optional<uint32_t> viewport;
  std::array<uintptr_t, MAX_CONSUMERS + MAX_EVENTS> watched_lists{};
  uint32_t watched_count = 0;
  const char* reason = "idle";
};
inline Capture capture;

inline uint64_t Active() {
  return capture.active.load(std::memory_order_acquire);
}

// The request is local to this add-on and may start inside an existing SDK
// generation. Its opening boundary cannot exclude already pending tokens.
inline Json Control(std::string_view action, bool nr_disabled) {
  const std::lock_guard lock(capture.mutex);
  if (action == "arm" && nr_disabled && Active() == 0) {
    capture.events = 0;
    capture.consumer_calls = 0;
    capture.handle = 0;
    capture.output = 0;
    capture.viewport.reset();
    capture.watched_count = 0;
    capture.reason = "observing";
    capture.active.store(++capture.generation, std::memory_order_release);
  } else if (action == "stop" || (!nr_disabled && Active() != 0)) {
    capture.reason = (nr_disabled ? "stopped" : "nr_enabled");
    capture.active.store(0, std::memory_order_release);
  }
  return {{"generation", capture.generation}, {"active", Active() != 0},
          {"events", capture.events}, {"consumer_calls", capture.consumer_calls},
          {"reason", capture.reason}};
}

inline uint64_t Emit(uint64_t generation, Json event, bool consumer = false, bool returned = false) {
  const std::lock_guard lock(capture.mutex);
  if (generation == 0 || generation != Active()) return 0;
  if (capture.events == MAX_EVENTS || (consumer && capture.consumer_calls == MAX_CONSUMERS)) {
    capture.reason = "event_limit";
    capture.active.store(0, std::memory_order_release);
    return 0;
  }
  const auto handle = event.value("fg_handle", uintptr_t{0});
  const auto output = event.value("sdk_scope_output", uintptr_t{0});
  const auto viewport = event.find("viewport");
  if ((handle != 0 && capture.handle != 0 && capture.handle != handle)
      || (output != 0 && capture.output != 0 && capture.output != output)
      || (viewport != event.end() && capture.viewport && *capture.viewport != viewport->get<uint32_t>())) {
    capture.reason = "scope_changed";
    capture.active.store(0, std::memory_order_release);
    return 0;
  }
  if (handle != 0) capture.handle = handle;
  if (output != 0) capture.output = output;
  if (viewport != event.end()) capture.viewport = viewport->get<uint32_t>();
  event["generation"] = generation;
  event["sequence"] = ++capture.events;
  event["thread"] = GetCurrentThreadId();
  event["observed_ns"] = edit_fg_observation::NowNs();
  if (consumer) ++capture.consumer_calls;
  reshade::log::message(reshade::log::level::info, ("NR_UIALPHA " + event.dump()).c_str());
  if (returned && capture.consumer_calls == MAX_CONSUMERS) {
    capture.reason = "consumer_limit";
    capture.active.store(0, std::memory_order_release);
  }
  return capture.events;
}

inline void EndScope(const char* reason) {
  if (Active() == 0) return;
  const std::lock_guard lock(capture.mutex);
  if (Active() != 0) {
    capture.reason = reason;
    capture.active.store(0, std::memory_order_release);
  }
}

inline Json Recording(uint64_t generation, ID3D12GraphicsCommandList* list) {
  const auto recording = edit_fg_observation::ObserveRecording(list);
  if (list != nullptr && recording.list_id != 0) {
    ID3D12CommandList* native = list;
    renodx::utils::directx::NativeFromReShadeProxy(&native);
    const auto raw = reinterpret_cast<uintptr_t>(native);
    const std::lock_guard lock(capture.mutex);
    auto end = capture.watched_lists.begin() + capture.watched_count;
    if (generation == Active() && end != capture.watched_lists.end()
        && std::find(capture.watched_lists.begin(), end, raw) == end) {
      *end = raw;
      ++capture.watched_count;
    }
  }
  return {{"raw", recording.raw}, {"id", recording.list_id},
          {"generation", recording.generation}};
}

inline void ObserveExecute(uint64_t generation, ID3D12CommandQueue* queue, UINT count,
                           ID3D12CommandList* const* lists, const char* phase) {
  if (lists == nullptr) return;
  Json recordings;
  for (UINT i = 0; i < count; ++i) {
    const auto raw = reinterpret_cast<uintptr_t>(lists[i]);
    {
      std::shared_lock lock(capture.mutex);
      if (generation != Active() || capture.watched_count == 0) return;
      const auto end = capture.watched_lists.begin() + capture.watched_count;
      if (std::find(capture.watched_lists.begin(), end, raw) == end) continue;
    }
    const auto observed = edit_fg_observation::ObserveRecording(lists[i]);
    recordings.push_back({{"id", observed.list_id}, {"generation", observed.generation}, {"batch_index", i}});
  }
  if (!recordings.empty()) {
    Emit(generation, {{"kind", "execute"}, {"phase", phase}, {"queue", reinterpret_cast<uintptr_t>(queue)},
        {"recordings", std::move(recordings)}, {"batch_count", count}, {"evidence", "CPU_Execute_only"}});
  }
}

inline Json Resource(ID3D12Resource* resource) {
  if (resource == nullptr) return nullptr;
  const auto identity = edit_fg_observation::ObserveResource(resource);
  const auto desc = resource->GetDesc();
  return {{"identity", identity.canonical}, {"raw", identity.raw}, {"width", desc.Width},
          {"height", desc.Height}, {"format", static_cast<uint32_t>(desc.Format)},
          {"mips", desc.MipLevels}, {"array", desc.DepthOrArraySize}, {"samples", desc.SampleDesc.Count},
          {"dimension", static_cast<uint32_t>(desc.Dimension)},
          {"flags", static_cast<uint32_t>(desc.Flags)}};
}

// A full single-subresource R8 copy is the only copy that can account for this
// role. This remains a recording observation until execution and lifetime
// evidence is available for both producer and consumer.
struct TagCopy {
  ID3D12GraphicsCommandList* list = nullptr;
  const void* source = nullptr;
  uintptr_t destination = 0;
  std::pair<uint64_t, uint64_t> recording;
  uint32_t matches = 0;
  bool eligible = false;
  inline static thread_local TagCopy* active = nullptr;

  void Record(ID3D12GraphicsCommandList* command_list, ID3D12Resource* input,
              ID3D12Resource* output, bool full) {
    if (source == nullptr || command_list != list || input == nullptr || output == nullptr
        || renodx::utils::directx::NativeIdentity(input) != source) return;
    ++matches;
    eligible = false;
    if (!full || matches != 1 || recording.first == 0
        || submission::RecordingIdentity(command_list) != recording) return;
    const auto input_desc = input->GetDesc();
    const auto output_desc = output->GetDesc();
    const auto single_r8 = [](const D3D12_RESOURCE_DESC& desc) {
      return desc.Format == DXGI_FORMAT_R8_UNORM && desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
          && desc.MipLevels == 1 && desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1;
    };
    if (!single_r8(input_desc) || !single_r8(output_desc) || input_desc.Width != output_desc.Width
        || input_desc.Height != output_desc.Height) return;
    const auto identity = renodx::utils::directx::NativeIdentity(output);
    if (identity == nullptr || identity == source) return;
    destination = reinterpret_cast<uintptr_t>(identity);
    eligible = true;
  }
};

struct CopyScope {
  TagCopy* previous;
  explicit CopyScope(TagCopy* copy) : previous(std::exchange(TagCopy::active, copy)) {}
  ~CopyScope() { TagCopy::active = previous; }
  CopyScope(const CopyScope&) = delete;
  CopyScope& operator=(const CopyScope&) = delete;
};

struct ConsumerCall {
  uint64_t generation = 0;
  uint64_t entry = 0;

  void Finish(uint32_t result) const {
    if (entry == 0) return;
    Emit(generation, {{"kind", "consumer_return"}, {"entry", entry}, {"result", result}}, false, true);
  }
};

}  // namespace renodx::addons::dlss5::native_ui_alpha_observation
#endif
