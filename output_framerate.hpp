/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace renodx::addons::dlss5::output_framerate {

enum class Source { NONE, PRESENT, XEFG_STATUS, NATIVE_STATE, NATIVE_FIXED };
enum class Reason { kWARMUP, kREADY, kSTALE, kFAILED, kTEST_PRESENT, kDUPLICATE, kUNKNOWN_FG, kAMBIGUOUS };

inline const char* SourceText(Source source) {
  switch (source) {
    case Source::PRESENT: return "Application Present submission estimate";
    case Source::XEFG_STATUS: return "XeFG SDK output estimate";
    case Source::NATIVE_STATE: return "DLSS FG SDK output estimate";
    case Source::NATIVE_FIXED: return "Fixed FG multiplier estimate";
    default: return "Output FPS unavailable";
  }
}

inline const char* ReasonText(Reason reason) {
  switch (reason) {
    case Reason::kREADY: return "Ready";
    case Reason::kSTALE: return "Waiting for fresh output samples";
    case Reason::kFAILED: return "Output observation failed";
    case Reason::kTEST_PRESENT: return "Waiting after a test Present";
    case Reason::kDUPLICATE: return "Waiting after a repeated application frame";
    case Reason::kUNKNOWN_FG: return "FG output rate is unknown";
    case Reason::kAMBIGUOUS: return "Output identity is ambiguous";
    default: return "Collecting output frame rate";
  }
}

struct OutputIdentity {
  uintptr_t swapchain = 0;
  uintptr_t device = 0;
  uintptr_t view = 0;
  uint64_t generation = 0;

  explicit operator bool() const { return generation != 0; }
  bool operator==(const OutputIdentity&) const = default;
};

struct SourceIdentity {
  uintptr_t handle = 0;
  uint64_t generation = 0;
  uintptr_t device = 0;
  uintptr_t resource = 0;
  uint64_t resource_generation = 0;

  bool operator==(const SourceIdentity&) const = default;
};

struct NativeApplicationFrame {
  uint64_t generation = 0;
  uint32_t index = 0;

  bool operator==(const NativeApplicationFrame&) const = default;
};

struct NativeApplicationTag {
  uintptr_t back_buffer = 0;
  std::optional<uint32_t> index;
  int64_t observed_ns = 0;
  bool conflicting = false;
};

struct NativeLegacySourceTag {
  OutputIdentity output;
  uintptr_t resource = 0;
  uint64_t resource_generation = 0;
  uint64_t revision = 0;
};

struct Snapshot {
  bool available = false;
  Reason reason = Reason::kWARMUP;
  Source source = Source::NONE;
  uint64_t source_generation = 0;
  uint64_t configuration_generation = 0;
  uint64_t window_serial = 0;
  int64_t window_begin_ns = 0;
  int64_t window_end_ns = 0;
  double fps = 0;
  double fps_mad = 0;
  uint32_t requested_multiplier = 0;
  double observed_multiplier = 0;
  bool dynamic = false;
  uint32_t maximum_multiplier = 0;
  uint32_t declared_multiplier = 0;
  bool fg_off = false;
  bool multiplier_variable = false;
  uint64_t identity = 0;
  OutputIdentity output;
  bool split_available = false;
  std::array<double, 2> split_fps{};
  double rate_multiplier = 0.;
  bool rate_comparable = true;
};

#if defined(NR_NEXUS_TEST_NATIVE_SDK_CLOCK)
static thread_local int64_t test_now_ns = 0;
#endif

inline int64_t NowNs() {
#if defined(NR_NEXUS_TEST_NATIVE_SDK_CLOCK)
  if (test_now_ns != 0) return test_now_ns;
#endif
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Four complete cadence bins retain no per-frame history. SDK counts include
// zero: a successful interval with no queued frames is still a measurement.
struct Stream {
  static constexpr int64_t BIN_NS = 500'000'000;
  static constexpr int64_t STALE_NS = 1'000'000'000;
  static constexpr int64_t NATIVE_STATE_STALE_NS = 2'000'000'000;
  static constexpr int64_t MaxAgeNs(Source source) {
    return (source == Source::NATIVE_STATE ? NATIVE_STATE_STALE_NS : STALE_NS);
  }
  struct Bin {
    int64_t begin_ns = 0, end_ns = 0;
    double frames = 0;
    uint64_t applications = 0;
    bool applications_known = true;
    double minimum_multiplier = std::numeric_limits<double>::infinity();
    double maximum_multiplier = 0.;
  };
  Snapshot snapshot;
  std::array<Bin, 4> bins{};
  Bin pending;
  size_t count = 0;
  size_t next = 0;
  int64_t previous_ns = 0;
  uint64_t identity = 0;

  void Reset(Reason reason = Reason::kWARMUP) {
    snapshot.available = false;
    snapshot.reason = reason;
    snapshot.window_begin_ns = snapshot.window_end_ns = 0;
    snapshot.fps = snapshot.fps_mad = 0;
    snapshot.split_available = false;
    snapshot.split_fps = {};
    snapshot.rate_comparable = true;
    snapshot.observed_multiplier = 0;
    snapshot.requested_multiplier = snapshot.maximum_multiplier = snapshot.declared_multiplier = 0;
    snapshot.dynamic = snapshot.fg_off = false;
    snapshot.multiplier_variable = false;
    ++snapshot.source_generation;
    bins = {};
    pending = {};
    count = next = 0;
    previous_ns = 0;
  }

  void Observe(Source source, uint64_t next_identity, uint64_t configuration,
               int64_t now_ns, double frames, uint32_t requested = 0,
               uint32_t = 0, bool dynamic = false, uint32_t maximum = 0, bool fg_off = false,
               std::optional<uint64_t> applications = std::nullopt) {
    if (snapshot.source != source || identity != next_identity
        || snapshot.configuration_generation != configuration) {
      Reset();
    }
    snapshot.source = source;
    snapshot.configuration_generation = configuration;
    snapshot.requested_multiplier = requested;
    snapshot.dynamic = dynamic;
    snapshot.maximum_multiplier = maximum;
    snapshot.fg_off = fg_off;
    identity = next_identity;
    if (now_ns <= 0 || !std::isfinite(frames) || frames < 0) {
      Reset(Reason::kFAILED);
      return;
    }
    if (previous_ns != 0 && (now_ns <= previous_ns || now_ns - previous_ns > MaxAgeNs(source))) {
      Reset(Reason::kSTALE);
    }
    if (previous_ns == 0) {
      previous_ns = pending.begin_ns = now_ns;
      return;
    }
    previous_ns = pending.end_ns = now_ns;
    pending.frames += frames;
    if (applications.has_value()) {
      pending.applications += *applications;
    } else if (source == Source::XEFG_STATUS) {
      ++pending.applications;
    } else {
      pending.applications_known = false;
    }
    if (source == Source::NATIVE_STATE) {
      // Several SDK reads can contribute to one bin. Keep the interval range
      // so an unchanged aggregate ratio cannot hide a change within the bin.
      const uint64_t interval_applications = applications.value_or(0);
      const double multiplier = (interval_applications != 0 ? frames / static_cast<double>(interval_applications) : 0.);
      pending.minimum_multiplier = std::min(pending.minimum_multiplier, multiplier);
      pending.maximum_multiplier = std::max(pending.maximum_multiplier, multiplier);
    }
    if (pending.end_ns - pending.begin_ns < BIN_NS) return;
    bins[next] = pending;
    next = (next + 1) % bins.size();
    count = std::min(count + 1, bins.size());
    pending = {.begin_ns = now_ns};
    if (count != bins.size()) return;
    double total_frames = 0;
    uint64_t total_applications = 0;
    bool applications_known = true;
    int64_t total_ns = 0;
    std::array<double, 4> rates{};
    double minimum_multiplier = std::numeric_limits<double>::infinity();
    double maximum_multiplier = 0.;
    for (size_t index = 0; index < bins.size(); ++index) {
      const auto& bin = bins[index];
      total_frames += bin.frames;
      total_applications += bin.applications;
      applications_known &= bin.applications_known;
      total_ns += bin.end_ns - bin.begin_ns;
      rates[index] = bin.frames * 1e9 / static_cast<double>(bin.end_ns - bin.begin_ns);
      minimum_multiplier = std::min(minimum_multiplier, bin.minimum_multiplier);
      maximum_multiplier = std::max(maximum_multiplier, bin.maximum_multiplier);
    }
    std::sort(rates.begin(), rates.end());
    const double median = (rates[1] + rates[2]) * 0.5;
    for (auto& rate : rates) rate = std::abs(rate - median);
    std::sort(rates.begin(), rates.end());
    snapshot.available = true;
    snapshot.reason = Reason::kREADY;
    snapshot.window_begin_ns = bins[next].begin_ns;
    snapshot.window_end_ns = now_ns;
    snapshot.fps = total_frames * 1e9 / static_cast<double>(total_ns);
    snapshot.fps_mad = (rates[1] + rates[2]) * 0.5;
    // Preserve complete SDK intervals and use their actual covered duration.
    // Ring storage order is independent of chronological half-window order.
    for (size_t half = 0; half < snapshot.split_fps.size(); ++half) {
      double frames = 0.;
      int64_t covered_ns = 0;
      for (size_t index = 0; index < bins.size() / 2; ++index) {
        const auto& bin = bins[(next + half * (bins.size() / 2) + index) % bins.size()];
        frames += bin.frames;
        covered_ns += bin.end_ns - bin.begin_ns;
      }
      snapshot.split_fps[half] = frames * 1e9 / static_cast<double>(covered_ns);
    }
    snapshot.split_available = true;
    snapshot.observed_multiplier = ((source == Source::XEFG_STATUS || source == Source::NATIVE_STATE)
        && applications_known && total_applications != 0
        ? total_frames / static_cast<double>(total_applications) : 0);
    snapshot.rate_comparable = source != Source::NATIVE_STATE
        || (minimum_multiplier > 0. && maximum_multiplier <= minimum_multiplier * 1.10);
    ++snapshot.window_serial;
  }

  Snapshot Read(int64_t now_ns) const {
    auto result = snapshot;
    result.identity = identity;
    if (previous_ns != 0 && (now_ns < previous_ns || now_ns - previous_ns > MaxAgeNs(snapshot.source))) {
      result.available = false;
      result.reason = Reason::kSTALE;
      result.requested_multiplier = 0;
      result.maximum_multiplier = result.declared_multiplier = 0;
      result.dynamic = result.fg_off = false;
    }
    if (!result.available) result.observed_multiplier = 0;
    return result;
  }
};

namespace internal {
struct NativeBinding {
  std::optional<uint32_t> viewport;
  uint64_t generation = 0;
  uintptr_t feature = 0;
  bool routed = false;
  int64_t evaluation_ns = 0;
  int64_t fixed_since_ns = 0;
  uint32_t declared_multiplier = 0;
  int64_t application_ns = 0;
  std::optional<NativeApplicationFrame> application_frame;
  uint64_t applications = 0;
  uint64_t coverage = 0;
  int64_t state_ns = 0;
  uint64_t state_applications = 0;
  std::optional<uint64_t> state_coverage;
  uint64_t state_query = 0;
  uintptr_t tagged_source = 0;
  uint64_t tagged_source_generation = 0;
  std::optional<uint32_t> tagged_source_frame;
  std::unordered_map<uintptr_t, NativeApplicationTag> application_tags;
  std::optional<SourceIdentity> legacy_source_binding;
};

struct Output {
  OutputIdentity identity;
  uintptr_t native_swapchain = 0;
  std::vector<uintptr_t> back_buffers;
  std::vector<SourceIdentity> sources;
  Stream stream;
  NativeBinding native;
  uint32_t xefg_requested = 0;
  uint32_t xefg_maximum = 0;
  uint64_t xefg_serial = 0;
  OutputIdentity linked_present;
};

struct NativeOptions {
  OutputIdentity output;
  uint32_t mode = 0;
  uint32_t multiplier = 0;
  bool known = false;
  NativeLegacySourceTag legacy_source;
};

struct State {
  std::shared_mutex mutex;
  std::unordered_map<uintptr_t, Output> present;
  std::unordered_map<uintptr_t, Output> xefg;
  std::unordered_map<uint32_t, NativeOptions> native_options;
  uint64_t generation = 0;
  uint64_t state_query = 0;
  int64_t xefg_ns = 0;
};
static State state;

inline Output* FindOutput(const OutputIdentity& identity) {
  auto& outputs = (identity.view != 0 ? state.xefg : state.present);
  const auto it = outputs.find(identity.view != 0 ? identity.view : identity.swapchain);
  if (it == outputs.end() || it->second.identity != identity) return nullptr;
  return &it->second;
}

inline bool OwnsBackBuffer(const Output& output, uintptr_t back_buffer) {
  return back_buffer != 0 && std::ranges::find(output.back_buffers, back_buffer) != output.back_buffers.end();
}

inline OutputIdentity ResolvePresentOutput(const Output& output) {
  if (output.native_swapchain == 0) return output.identity;
  OutputIdentity proxy;
  for (const auto& [context, candidate] : state.xefg) {
    if (candidate.linked_present != output.identity && candidate.identity.swapchain != output.native_swapchain
        && candidate.native_swapchain != output.native_swapchain) continue;
    if (proxy) return {};
    proxy = candidate.identity;
  }
  return (proxy ? proxy : output.identity);
}

inline bool Fresh(int64_t event_ns, int64_t now_ns, int64_t max_age_ns = Stream::STALE_NS) {
  return event_ns > 0 && now_ns >= event_ns && now_ns - event_ns <= max_age_ns;
}

inline bool MatchesLegacySourceTag(const Output& output, const NativeLegacySourceTag& tag,
    uintptr_t resource, uint64_t resource_generation) {
  if (!output.native.viewport || tag.resource != resource || tag.resource_generation != resource_generation
      || tag.revision == 0) return false;
  const auto options = state.native_options.find(*output.native.viewport);
  return options != state.native_options.end() && options->second.output == tag.output
      && options->second.legacy_source.revision == tag.revision;
}

inline const NativeOptions* Options(const NativeBinding& native) {
  if (!native.viewport.has_value()) return nullptr;
  const auto it = state.native_options.find(*native.viewport);
  return (it != state.native_options.end() && it->second.known ? &it->second : nullptr);
}

inline uint32_t FixedMultiplier(const NativeBinding& native, const NativeOptions* options, int64_t now_ns) {
  if (options == nullptr || options->mode != 1 || options->multiplier < 2
      || native.fixed_since_ns == 0 || !Fresh(native.evaluation_ns, now_ns)
      || (native.declared_multiplier != 0 && native.declared_multiplier != options->multiplier)) return 0;
  return options->multiplier;
}

inline void ResetNativeBinding(Output* output) {
  if (output->native.viewport.has_value()) {
    auto& options = state.native_options.at(*output->native.viewport);
    options.output = {};
    options.legacy_source = {};
  }
  if (output->native.routed || output->native.viewport.has_value() || !output->sources.empty()) {
    output->stream.Reset();
  }
  output->sources.clear();
  output->native = {.generation = ++state.generation};
}

inline Snapshot ReadOutput(const Output& output, int64_t now_ns) {
  auto snapshot = output.stream.Read(now_ns);
  snapshot.output = output.identity;
  if (output.identity.view != 0) {
    snapshot.maximum_multiplier = output.xefg_maximum;
    snapshot.requested_multiplier = output.xefg_requested;
  }
  const auto& native = output.native;
  const auto* options = Options(native);
  const auto fixed_multiplier = FixedMultiplier(native, options, now_ns);
  const bool fixed_for_window = fixed_multiplier != 0 && native.fixed_since_ns <= snapshot.window_begin_ns;
  if (snapshot.source == Source::NATIVE_FIXED && !fixed_for_window) {
    snapshot.available = false;
    snapshot.reason = Reason::kUNKNOWN_FG;
  }
  if (snapshot.source == Source::NATIVE_STATE) {
    snapshot.multiplier_variable = !fixed_for_window;
    if (fixed_for_window) {
      snapshot.rate_comparable = true;
    } else if (!Fresh(native.application_ns, now_ns) || native.state_coverage != native.coverage) {
      snapshot.observed_multiplier = 0.;
      snapshot.rate_comparable = false;
    }
  }
  if ((snapshot.source == Source::NATIVE_STATE || snapshot.source == Source::NATIVE_FIXED) && fixed_for_window) {
    snapshot.rate_multiplier = fixed_multiplier;
  } else {
    snapshot.rate_multiplier = snapshot.observed_multiplier;
  }
  if (native.routed && options != nullptr) {
    snapshot.dynamic = options->mode == 3;
    snapshot.requested_multiplier = options->multiplier;
  }
  if (Fresh(native.evaluation_ns, now_ns)) {
    snapshot.declared_multiplier = native.declared_multiplier;
  }
  return snapshot;
}
}  // namespace internal

inline OutputIdentity MatchPresentOutput(uintptr_t swapchain, uintptr_t back_buffer) {
  std::shared_lock lock(internal::state.mutex);
  const auto it = internal::state.present.find(swapchain);
  if (it == internal::state.present.end() || !internal::OwnsBackBuffer(it->second, back_buffer)) return {};
  return internal::ResolvePresentOutput(it->second);
}

inline bool HasRegisteredPresentOutputs() {
  std::shared_lock lock(internal::state.mutex);
  return !internal::state.present.empty();
}

namespace internal {
inline const Output* FindPresentOwner(uintptr_t device, uintptr_t back_buffer) {
  const Output* match = nullptr;
  for (const auto& [swapchain, candidate] : state.present) {
    if ((device != 0 && candidate.identity.device != device)
        || !OwnsBackBuffer(candidate, back_buffer)) continue;
    if (match != nullptr) return nullptr;
    match = &candidate;
  }
  return match;
}

inline OutputIdentity FindPresentOutput(uintptr_t device, uintptr_t back_buffer) {
  const auto* match = FindPresentOwner(device, back_buffer);
  return (match != nullptr ? ResolvePresentOutput(*match) : OutputIdentity{});
}

inline bool ValidSource(const SourceIdentity& source) {
  return source.handle != 0 && source.generation != 0 && source.device != 0 && source.resource != 0
      && source.resource_generation != 0;
}

inline void BindSourceOutput(const SourceIdentity& source, Output* output) {
  if (output == nullptr || std::ranges::find(output->sources, source) != output->sources.end()) return;
  std::erase_if(output->sources, [&](const auto& previous) {
    return previous.handle == source.handle && (previous.generation != source.generation
        || (previous.resource == source.resource && previous.resource_generation != source.resource_generation));
  });
  output->sources.push_back(source);
}

inline OutputIdentity FindSourceOutput(const SourceIdentity& source) {
  OutputIdentity match;
  for (const auto* outputs : {&state.present, &state.xefg}) {
    for (const auto& [key, output] : *outputs) {
      if (std::ranges::find(output.sources, source) == output.sources.end()
          && output.native.legacy_source_binding != source) continue;
      const auto candidate = (output.identity.view != 0 ? output.identity : ResolvePresentOutput(output));
      if (!candidate || (match && match != candidate)) return {};
      match = candidate;
    }
  }
  return match;
}
}  // namespace internal

inline OutputIdentity FindPresentOutput(uintptr_t device, uintptr_t back_buffer) {
  std::shared_lock lock(internal::state.mutex);
  return internal::FindPresentOutput(device, back_buffer);
}

inline OutputIdentity FindPresentOwner(uintptr_t device, uintptr_t back_buffer) {
  std::shared_lock lock(internal::state.mutex);
  const auto* owner = internal::FindPresentOwner(device, back_buffer);
  return (owner != nullptr ? owner->identity : OutputIdentity{});
}

inline void BindSourceOutput(const SourceIdentity& source, const OutputIdentity& identity) {
  if (!internal::ValidSource(source)) return;
  std::unique_lock lock(internal::state.mutex);
  internal::BindSourceOutput(source, internal::FindOutput(identity));
}

inline void ForgetSourceOutput(uintptr_t handle, const SourceIdentity* exact_source = nullptr) {
  std::unique_lock lock(internal::state.mutex);
  for (auto* outputs : {&internal::state.present, &internal::state.xefg}) {
    for (auto& [key, output] : *outputs) {
      std::erase_if(output.sources, [&](const auto& source) {
        return source.handle == handle && (exact_source == nullptr || source == *exact_source);
      });
      if (output.native.legacy_source_binding && output.native.legacy_source_binding->handle == handle
          && (exact_source == nullptr || output.native.legacy_source_binding == *exact_source)) {
        output.native.legacy_source_binding.reset();
      }
    }
  }
}

inline OutputIdentity FindSourceOutput(const SourceIdentity& source) {
  std::shared_lock lock(internal::state.mutex);
  return internal::FindSourceOutput(source);
}

inline OutputIdentity MatchNativeSwapchain(uintptr_t native_swapchain) {
  if (native_swapchain == 0) return {};
  std::shared_lock lock(internal::state.mutex);
  OutputIdentity match;
  for (const auto& [swapchain, candidate] : internal::state.present) {
    if (candidate.native_swapchain != native_swapchain) continue;
    if (match) return {};
    match = candidate.identity;
  }
  return match;
}

inline void RegisterPresentOutput(uintptr_t swapchain, uintptr_t device, uintptr_t native_swapchain,
                                  std::span<const uintptr_t> back_buffers) {
  if (swapchain == 0 || device == 0 || back_buffers.empty()) return;
  std::unique_lock lock(internal::state.mutex);
  auto& state = internal::state;
  auto& output = state.present[swapchain];
  internal::ResetNativeBinding(&output);
  output = {
      .identity = {.swapchain = swapchain, .device = device, .generation = ++state.generation},
      .native_swapchain = native_swapchain,
      .back_buffers = {back_buffers.begin(), back_buffers.end()},
  };
}

inline void DestroyPresentOutput(uintptr_t swapchain) {
  std::unique_lock lock(internal::state.mutex);
  auto& outputs = internal::state.present;
  if (const auto it = outputs.find(swapchain); it != outputs.end()) {
    std::erase_if(internal::state.xefg, [&](const auto& entry) {
      const auto& proxy = entry.second;
      const auto native = it->second.native_swapchain;
      return proxy.linked_present == it->second.identity || (native != 0
          && (proxy.identity.swapchain == native || proxy.native_swapchain == native));
    });
    internal::ResetNativeBinding(&it->second);
    outputs.erase(it);
  }
}

inline void ObservePresent(const OutputIdentity& identity, int64_t now_ns) {
  std::unique_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(identity);
  if (output == nullptr || identity.view != 0) return;
  if (output->native.routed) return;
  if (internal::ResolvePresentOutput(*output) != identity) return;
  output->stream.Observe(Source::PRESENT, identity.swapchain, identity.generation, now_ns, 1);
}

inline void Invalidate(const OutputIdentity& identity, Reason reason = Reason::kFAILED) {
  std::unique_lock lock(internal::state.mutex);
  if (auto* output = internal::FindOutput(identity); output != nullptr) {
    output->stream.Reset(reason);
  }
}

inline Snapshot Read(const OutputIdentity& identity, int64_t now_ns) {
  if (!identity) return {.reason = Reason::kAMBIGUOUS};
  std::shared_lock lock(internal::state.mutex);
  const auto* output = internal::FindOutput(identity);
  if (output == nullptr) return {.reason = Reason::kSTALE};
  return internal::ReadOutput(*output, now_ns);
}

inline bool IsCurrentOutput(const OutputIdentity& identity) {
  if (!identity) return false;
  std::shared_lock lock(internal::state.mutex);
  const auto* output = internal::FindOutput(identity);
  return output != nullptr && (identity.view != 0 || internal::ResolvePresentOutput(*output) == identity);
}

// The unscoped read is for diagnostics without an output context.
inline Snapshot Read(int64_t now_ns) {
  std::shared_lock lock(internal::state.mutex);
  Snapshot sample;
  bool ambiguous = false;
  for (const auto* outputs : {&internal::state.present, &internal::state.xefg}) {
    for (const auto& [key, output] : *outputs) {
      const auto candidate = internal::ReadOutput(output, now_ns);
      if (candidate.source == Source::NONE) continue;
      if (sample.output && sample.available == candidate.available) {
        ambiguous = true;
      } else if (!sample.output || candidate.available) {
        sample = candidate;
        ambiguous = false;
      }
    }
  }
  return (ambiguous ? Snapshot{.reason = Reason::kAMBIGUOUS} : sample);
}

inline void BindNativeOutput(const OutputIdentity& identity, uint32_t viewport,
                             uintptr_t tagged_source = 0, std::optional<uint32_t> frame = std::nullopt,
                             const NativeApplicationTag& application_tag = {}, uint64_t resource_generation = 0) {
  std::unique_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(identity);
  if (output == nullptr || identity.view != 0) return;
  if (output->native.viewport != viewport) {
    auto& options = internal::state.native_options[viewport];
    if (auto* previous = internal::FindOutput(options.output); previous != nullptr) {
      internal::ResetNativeBinding(previous);
    }
    if (output->native.viewport.has_value()) {
      internal::ResetNativeBinding(output);
    }
    output->native.viewport = viewport;
    output->native.generation = ++internal::state.generation;
    options.output = identity;
  }
  output->native.tagged_source = tagged_source;
  output->native.tagged_source_generation = resource_generation;
  output->native.tagged_source_frame = frame;
  if (application_tag.observed_ns == 0) return;
  auto& tags = output->native.application_tags;
  if (application_tag.back_buffer == 0) {
    tags.clear();
    return;
  }
  if (!internal::OwnsBackBuffer(*output, application_tag.back_buffer)) return;
  const auto [found, inserted] = tags.try_emplace(application_tag.back_buffer, application_tag);
  if (!inserted) {
    if (found->second.index != application_tag.index) {
      // Framed resource tags can run ahead of Present. Two outstanding frame
      // tokens for one BB cannot identify the frame the SDK will consume.
      found->second.conflicting |= found->second.index.has_value() && application_tag.index.has_value();
      found->second.index.reset();
    }
    found->second.observed_ns = application_tag.observed_ns;
  }
}

inline bool BindNativeTaggedSourceOutput(uint32_t viewport, uint32_t frame, const SourceIdentity& source) {
  if (!internal::ValidSource(source)) return false;
  std::unique_lock lock(internal::state.mutex);
  const auto it = internal::state.native_options.find(viewport);
  if (it == internal::state.native_options.end()) return false;
  const auto* output = internal::FindOutput(it->second.output);
  if (output == nullptr || output->identity.device != source.device || output->native.tagged_source_frame != frame
      || output->native.tagged_source != source.resource
      || output->native.tagged_source_generation != source.resource_generation) {
    return false;
  }
  auto* resolved = internal::FindOutput(internal::ResolvePresentOutput(*output));
  if (resolved == nullptr) return false;
  internal::BindSourceOutput(source, resolved);
  return true;
}

namespace internal {
inline bool BindNativeViewportIfCompatible(Output& output, uint32_t viewport) {
  const auto& identity = output.identity;
  // Reusing a proved SR relation may establish an ownership edge. An explicit
  // tag may change it, but the SR relation must not evict either current owner.
  if (output.native.viewport && output.native.viewport != viewport) return false;
  auto& options = state.native_options[viewport];
  if (options.output != identity && FindOutput(options.output) != nullptr) return false;
  if (output.native.viewport == viewport) return true;
  output.native.viewport = viewport;
  output.native.generation = ++state.generation;
  options.output = identity;
  return true;
}
}  // namespace internal

inline bool BindNativeOutputFromSource(uint32_t viewport, const SourceIdentity& source) {
  if (!internal::ValidSource(source)) return false;
  std::unique_lock lock(internal::state.mutex);
  auto identity = internal::FindSourceOutput(source);
  if (!identity) identity = internal::FindPresentOutput(source.device, source.resource);
  auto* output = internal::FindOutput(identity);
  return output != nullptr && identity.view == 0 && identity.device == source.device
      && internal::BindNativeViewportIfCompatible(*output, viewport);
}

inline bool BindNativeLocalSourceOutput(uint32_t viewport, const SourceIdentity& source, const OutputIdentity& identity) {
  if (!internal::ValidSource(source) || identity.view != 0 || identity.device != source.device) return false;
  std::unique_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(identity);
  const auto previous = internal::FindSourceOutput(source);
  if (output == nullptr || (previous && previous != internal::ResolvePresentOutput(*output))
      || !internal::BindNativeViewportIfCompatible(*output, viewport)) return false;
  internal::BindSourceOutput(source, output);
  return true;
}

inline void ObserveNativeLegacySourceTag(uint32_t viewport, std::optional<OutputIdentity> back_buffer,
    bool source_tagged, uintptr_t resource, uint64_t resource_generation) {
  if (!back_buffer && !source_tagged) return;
  std::unique_lock lock(internal::state.mutex);
  auto& options = internal::state.native_options[viewport];
  auto& tag = options.legacy_source;
  auto* output = internal::FindOutput(options.output);
  if (back_buffer) {
    if (tag.output && tag.output != *back_buffer) tag = {};
    tag.output = (output != nullptr && output->identity == *back_buffer ? *back_buffer : OutputIdentity{});
  }
  if (source_tagged) {
    tag.resource = resource;
    tag.resource_generation = (resource != 0 ? resource_generation : 0);
  }
  tag.revision = ++internal::state.generation;
  if (output == nullptr) return;
  auto& binding = output->native.legacy_source_binding;
  if (binding && (!tag.output || binding->resource != tag.resource
          || binding->resource_generation != tag.resource_generation)) binding.reset();
}

inline std::optional<NativeLegacySourceTag> CaptureNativeLegacySourceTag(uint32_t viewport) {
  std::shared_lock lock(internal::state.mutex);
  const auto it = internal::state.native_options.find(viewport);
  if (it == internal::state.native_options.end()) return std::nullopt;
  const auto& tag = it->second.legacy_source;
  const auto* output = internal::FindOutput(tag.output);
  if (output == nullptr || tag.output != it->second.output || tag.resource == 0 || tag.resource_generation == 0) {
    return std::nullopt;
  }
  return tag;
}

inline OutputIdentity MatchNativeLegacySourceTag(const NativeLegacySourceTag& tag, uintptr_t resource,
    uint64_t resource_generation) {
  std::shared_lock lock(internal::state.mutex);
  const auto* output = internal::FindOutput(tag.output);
  if (output == nullptr || !internal::MatchesLegacySourceTag(*output, tag, resource, resource_generation)) return {};
  return internal::ResolvePresentOutput(*output);
}

inline bool BindNativeLegacySourceOutput(const SourceIdentity& source, const NativeLegacySourceTag& tag) {
  if (!internal::ValidSource(source)) return false;
  std::unique_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(tag.output);
  if (output == nullptr || output->identity.device != source.device || !internal::MatchesLegacySourceTag(
          *output, tag, source.resource, source.resource_generation)) return false;
  output->native.legacy_source_binding = source;
  return true;
}

inline uint64_t BeginNativeOutputPresent(const OutputIdentity& identity) {
  if (!identity) return 0;
  std::shared_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(identity);
  if (output == nullptr) return 0;
  return output->native.generation;
}

inline void WithdrawNativeApplicationTag(uint32_t viewport, uint32_t frame) {
  std::unique_lock lock(internal::state.mutex);
  const auto options = internal::state.native_options.find(viewport);
  if (options == internal::state.native_options.end()) return;
  auto* output = internal::FindOutput(options->second.output);
  if (output == nullptr) return;
  std::erase_if(output->native.application_tags, [&](const auto& tag) { return tag.second.index == frame; });
}

inline void ReleaseNativeViewport(uint32_t viewport, bool forget_options = true) {
  std::unique_lock lock(internal::state.mutex);
  auto& options = internal::state.native_options;
  const auto it = options.find(viewport);
  if (it == options.end()) return;
  if (auto* output = internal::FindOutput(it->second.output); output != nullptr) {
    internal::ResetNativeBinding(output);
  }
  it->second.legacy_source = {};
  if (forget_options) {
    options.erase(it);
  }
}

inline void ReleaseNative() {
  std::unique_lock lock(internal::state.mutex);
  for (auto& [swapchain, output] : internal::state.present) {
    internal::ResetNativeBinding(&output);
  }
  internal::state.native_options.clear();
}

inline OutputIdentity NativeViewportOutput(uint32_t viewport) {
  std::shared_lock lock(internal::state.mutex);
  const auto it = internal::state.native_options.find(viewport);
  return (it == internal::state.native_options.end() ? OutputIdentity{} : it->second.output);
}

inline uint64_t NativeBindingGeneration(const OutputIdentity& identity) {
  std::shared_lock lock(internal::state.mutex);
  const auto* output = internal::FindOutput(identity);
  return (output != nullptr ? output->native.generation : 0);
}

inline bool HasNativeApplicationTags(const OutputIdentity& identity) {
  std::shared_lock lock(internal::state.mutex);
  const auto* output = internal::FindOutput(identity);
  return output != nullptr && !output->native.application_tags.empty();
}

inline std::optional<NativeApplicationFrame> TakeNativeApplicationFrame(const OutputIdentity& identity,
    uint64_t binding_generation, uintptr_t back_buffer, int64_t now_ns, bool* conflicting = nullptr) {
  if (conflicting != nullptr) {
    *conflicting = false;
  }
  std::unique_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(identity);
  if (output == nullptr || output->native.generation != binding_generation) return std::nullopt;
  auto& tags = output->native.application_tags;
  const auto found = tags.find(back_buffer);
  if (found == tags.end()) return std::nullopt;
  const auto tag = found->second;
  tags.erase(found);
  if (!internal::Fresh(tag.observed_ns, now_ns)) return std::nullopt;
  if (conflicting != nullptr) {
    *conflicting = tag.conflicting;
  }
  if (!tag.index) return std::nullopt;
  return NativeApplicationFrame{.generation = binding_generation, .index = *tag.index};
}

inline std::optional<uint32_t> NativeMode(const OutputIdentity& identity) {
  std::shared_lock lock(internal::state.mutex);
  const auto* output = internal::FindOutput(identity);
  const auto* options = (output != nullptr ? internal::Options(output->native) : nullptr);
  return (options != nullptr ? std::optional(options->mode) : std::nullopt);
}

inline void WithdrawNativeFixedness(const OutputIdentity& identity) {
  std::unique_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(identity);
  if (output == nullptr) return;
  auto& native = output->native;
  native.fixed_since_ns = 0;
  if (native.viewport.has_value()) internal::state.native_options.at(*native.viewport).known = false;
  if (output->stream.snapshot.source == Source::NATIVE_FIXED) {
    output->stream.Reset(Reason::kUNKNOWN_FG);
  }
}

inline OutputIdentity NativeFeatureOutput(uintptr_t feature) {
  if (feature == 0) return {};
  std::shared_lock lock(internal::state.mutex);
  OutputIdentity match;
  for (const auto& [key, output] : internal::state.present) {
    if (output.native.feature != feature) continue;
    if (match) return {};
    match = output.identity;
  }
  return match;
}

inline void BindNativeFeatureOutput(const OutputIdentity& identity, uintptr_t feature) {
  if (feature == 0) return;
  std::unique_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(identity);
  if (output == nullptr || identity.view != 0 || output->native.feature == feature) return;
  auto& native = output->native;
  output->stream.Reset();
  native = {.viewport = native.viewport, .generation = ++internal::state.generation, .feature = feature,
      .tagged_source = native.tagged_source, .tagged_source_frame = native.tagged_source_frame};
}

inline void ReleaseNativeFeature(uintptr_t feature) {
  if (feature == 0) return;
  std::unique_lock lock(internal::state.mutex);
  for (auto& [key, output] : internal::state.present) {
    if (output.native.feature != feature) continue;
    output.stream.Reset();
    output.native = {.viewport = output.native.viewport, .generation = ++internal::state.generation};
  }
}

inline void ObserveNativeOptions(uint32_t viewport, uint64_t version, uint32_t mode, uint32_t multiplier,
                                 std::optional<OutputIdentity> entered_output = std::nullopt) {
  if (version == 0 || mode > 3) return;
  std::unique_lock lock(internal::state.mutex);
  auto& options = internal::state.native_options[viewport];
  if (entered_output.has_value() && options.output != *entered_output) return;
  const bool changed = !options.known || options.mode != mode || options.multiplier != multiplier;
  options.known = true;
  options.mode = mode;
  options.multiplier = multiplier;
  if (!changed) return;
  auto* output = internal::FindOutput(options.output);
  if (output == nullptr) return;
  auto& native = output->native;
  native.fixed_since_ns = 0;
  if (mode == 0 && native.routed) {
    output->stream.Reset();
    native = {.viewport = viewport, .generation = ++internal::state.generation};
  } else if (output->stream.snapshot.source == Source::NATIVE_FIXED) {
    output->stream.Reset(Reason::kUNKNOWN_FG);
  }
}

inline void ObserveNativeEvaluation(const OutputIdentity& identity, uint32_t multiplier, bool success, int64_t now_ns,
                                    uintptr_t feature = 0) {
  if (!identity) return;
  BindNativeFeatureOutput(identity, feature);
  std::unique_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(identity);
  if (output == nullptr || identity.view != 0) return;
  auto& native = output->native;
  const auto* options = internal::Options(native);
  if (!success || now_ns < native.evaluation_ns) {
    native.evaluation_ns = native.fixed_since_ns = native.state_ns = 0;
    native.declared_multiplier = 0;
    output->stream.Reset(Reason::kFAILED);
    return;
  }
  if (!native.routed) {
    native.routed = true;
    output->stream.Reset(Reason::kUNKNOWN_FG);
  }
  if (options == nullptr || options->mode != 1 || options->multiplier < 2
      || (multiplier != 0 && multiplier != options->multiplier)) {
    native.fixed_since_ns = 0;
  } else if (native.fixed_since_ns == 0 || !internal::Fresh(native.evaluation_ns, now_ns)) {
    native.fixed_since_ns = now_ns;
  }
  native.evaluation_ns = now_ns;
  native.declared_multiplier = multiplier;
  if (output->stream.snapshot.source == Source::NATIVE_FIXED
      && (native.fixed_since_ns == 0 || native.fixed_since_ns > output->stream.previous_ns)) {
    output->stream.Reset(Reason::kUNKNOWN_FG);
  }
}

struct NativeStateQuery {
  OutputIdentity output;
  uint64_t binding = 0;
  uint64_t serial = 0;
};

inline NativeStateQuery BeginNativeState(uint32_t viewport) {
  std::unique_lock lock(internal::state.mutex);
  auto& state = internal::state;
  const auto it = state.native_options.find(viewport);
  if (it == state.native_options.end() || (it->second.known && it->second.mode == 0)) return {};
  auto* output = internal::FindOutput(it->second.output);
  if (output == nullptr) return {};
  if (output->native.state_query != 0) {
    output->native.state_ns = 0;
    if (output->stream.snapshot.source == Source::NATIVE_STATE) {
      output->stream.Reset(Reason::kAMBIGUOUS);
    }
  }
  const auto serial = ++state.state_query;
  output->native.state_query = serial;
  return {.output = output->identity, .binding = output->native.generation, .serial = serial};
}

inline void ObserveNativeState(const NativeStateQuery& query, bool success, uint32_t frames, int64_t now_ns) {
  if (!query.output) return;
  std::unique_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(query.output);
  if (output == nullptr || output->native.generation != query.binding) return;
  auto& native = output->native;
  if (native.state_query != query.serial) return;
  native.state_query = 0;
  if (!success || now_ns <= native.state_ns) {
    output->stream.Reset(Reason::kFAILED);
    native.state_ns = 0;
    native.state_coverage.reset();
    return;
  }
  native.routed = true;
  const bool application_coverage = internal::Fresh(native.application_ns, now_ns);
  std::optional<uint64_t> applications;
  if (native.state_ns != 0 && native.state_coverage == native.coverage && application_coverage) {
    applications = native.applications - native.state_applications;
  }
  native.state_ns = now_ns;
  native.state_applications = native.applications;
  native.state_coverage = (application_coverage ? std::optional(native.coverage) : std::nullopt);
  output->stream.Observe(Source::NATIVE_STATE, query.output.swapchain, query.binding, now_ns, frames,
      0, 0, false, 0, false, applications);
}

inline void ObserveNativePresent(const OutputIdentity& identity, bool success, int64_t now_ns, bool handled = false,
                                 std::optional<uint64_t> entered_binding = std::nullopt,
                                 std::optional<NativeApplicationFrame> application_frame = std::nullopt) {
  if (!identity) return;
  std::unique_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(identity);
  if (output == nullptr || identity.view != 0) return;
  auto& native = output->native;
  if (entered_binding.has_value() && native.generation != *entered_binding) return;
  if (!success || now_ns <= 0 || (native.application_ns != 0 && now_ns < native.application_ns)) {
    ++native.coverage;
    native.application_ns = native.state_ns = 0;
    native.state_coverage.reset();
    output->stream.Reset(Reason::kFAILED);
    return;
  }
  const auto* options = internal::Options(native);
  if (handled && options != nullptr && options->mode != 0 && !native.routed) {
    native.routed = true;
    output->stream.Reset(Reason::kUNKNOWN_FG);
  }
  if (!native.routed && !native.viewport.has_value()) return;
  const auto invalidate_application_coverage = [&] {
    ++native.coverage;
    native.application_ns = 0;
    if (output->stream.snapshot.source == Source::NATIVE_FIXED) {
      output->stream.Reset(Reason::kUNKNOWN_FG);
    }
  };
  if (!application_frame.has_value()) {
    invalidate_application_coverage();
    return;
  }
  const auto previous = native.application_frame;
  const bool repeated = previous == application_frame;
  const bool same_generation = previous.has_value() && previous->generation == application_frame->generation;
  const bool next = same_generation && previous->index + 1u == application_frame->index;
  if (same_generation && application_frame->index < previous->index && !next) {
    invalidate_application_coverage();
    return;
  }
  if (!internal::Fresh(native.application_ns, now_ns) || (!repeated && !next)) {
    invalidate_application_coverage();
  }
  native.application_ns = now_ns;
  if (repeated) return;
  native.application_frame = application_frame;
  ++native.applications;
  if (!native.routed || internal::Fresh(native.state_ns, now_ns, Stream::NATIVE_STATE_STALE_NS)) return;
  const auto multiplier = internal::FixedMultiplier(native, options, now_ns);
  if (multiplier != 0) {
    output->stream.Observe(Source::NATIVE_FIXED, identity.swapchain, native.generation, now_ns, multiplier);
  } else if (output->stream.snapshot.source != Source::NATIVE_STATE) {
    output->stream.Reset(Reason::kUNKNOWN_FG);
  }
}

inline OutputIdentity RegisterXeFGOutput(uintptr_t proxy, uintptr_t device, uintptr_t context,
                                         uintptr_t application_swapchain) {
  if (proxy == 0 || device == 0 || context == 0) return {};
  std::unique_lock lock(internal::state.mutex);
  auto& state = internal::state;
  for (auto& [swapchain, output] : state.present) {
    if (output.native_swapchain == proxy || (application_swapchain != 0
        && output.native_swapchain == application_swapchain)) {
      output.stream.Reset(Reason::kUNKNOWN_FG);
    }
  }
  auto& output = state.xefg[context];
  output = {.identity = {.swapchain = proxy, .device = device, .view = context, .generation = ++state.generation},
      .native_swapchain = application_swapchain};
  return output.identity;
}

inline void ReleaseXeFGOutput(const OutputIdentity& identity) {
  if (!identity) return;
  std::unique_lock lock(internal::state.mutex);
  if (identity.view != 0 && internal::FindOutput(identity) != nullptr) {
    internal::state.xefg.erase(identity.view);
  }
}

inline void ObserveXeFGOutputLink(const OutputIdentity& identity, uintptr_t swapchain, uintptr_t native_swapchain) {
  if (!identity || identity.view == 0) return;
  std::unique_lock lock(internal::state.mutex);
  auto* proxy = internal::FindOutput(identity);
  if (proxy == nullptr) return;
  auto& outputs = internal::state.present;
  const auto it = outputs.find(swapchain);
  if (it != outputs.end() && it->second.native_swapchain == native_swapchain && native_swapchain != 0) {
    if (proxy->linked_present != it->second.identity) {
      it->second.stream.Reset(Reason::kUNKNOWN_FG);
      proxy->linked_present = it->second.identity;
    }
  } else {
    proxy->linked_present = {};
  }
}

inline void ObserveXeFG(const OutputIdentity& identity, uint64_t call_serial, int64_t now_ns,
                        bool present_ok, bool status_ok, uint32_t frames, bool new_application, bool fg_off) {
  std::unique_lock lock(internal::state.mutex);
  auto* output = internal::FindOutput(identity);
  if (output == nullptr || identity.view == 0) return;
  if (call_serial <= output->xefg_serial) return;
  output->xefg_serial = call_serial;
  internal::state.xefg_ns = now_ns;
  auto& stream = output->stream;
  if (!present_ok || !status_ok) {
    stream.Reset(Reason::kFAILED);
    return;
  }
  if (stream.previous_ns != 0 && stream.snapshot.fg_off != fg_off) {
    stream.Reset();
  }
  stream.Observe(Source::XEFG_STATUS, identity.view, identity.generation, now_ns, frames,
      0, 0, false, 0, fg_off, new_application ? 1u : 0u);
}

inline void ObserveXeFGMetadata(const OutputIdentity& identity, uint32_t maximum, uint32_t requested) {
  std::unique_lock lock(internal::state.mutex);
  if (auto* output = internal::FindOutput(identity); output != nullptr && identity.view != 0) {
    output->xefg_maximum = maximum;
    output->xefg_requested = requested;
  }
}

}  // namespace renodx::addons::dlss5::output_framerate
