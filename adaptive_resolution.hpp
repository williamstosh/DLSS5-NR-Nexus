/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace renodx::addons::dlss5::adaptive_resolution {

constexpr int64_t SAMPLE_INTERVAL_NS = 250'000'000;
constexpr int64_t SAMPLE_MAX_AGE_NS = 2'000'000'000;
constexpr int64_t WARMUP_NS = 1'000'000'000;
constexpr int64_t RETRY_COOLDOWN_NS = 30'000'000'000;
constexpr size_t MAX_PASSES = 4, MAX_LEVELS = 16;
constexpr uint32_t MINIMUM_SCALE_PERCENT = 33, SCALE_STEP_PERCENT = 5;
constexpr float MINIMUM_SCALE = static_cast<float>(MINIMUM_SCALE_PERCENT) / 100.f;
constexpr float SCALE_STEP = static_cast<float>(SCALE_STEP_PERCENT) / 100.f;
constexpr uint32_t MAX_PREPARED_LEVELS =
    1u + (100u - MINIMUM_SCALE_PERCENT + SCALE_STEP_PERCENT - 1u) / SCALE_STEP_PERCENT;
static_assert(MAX_PREPARED_LEVELS <= MAX_LEVELS);

enum class Reason : uint8_t {
  DISABLED, UNSUPPORTED, OUTPUT_UNAVAILABLE, COLLECTING, WARMING, OBSERVING,
  LOWERING, RAISING, NO_BENEFIT, AT_MINIMUM, FROZEN, FAILED, PREPARING, BUDGET,
  NO_RANGE,
};

inline const char* ReasonText(Reason reason) {
  switch (reason) {
    case Reason::DISABLED: return "Off";
    case Reason::UNSUPPORTED: return "Automatic sizing is unavailable on this path";
    case Reason::OUTPUT_UNAVAILABLE: return "Waiting for a reliable output FPS estimate";
    case Reason::COLLECTING: return "Collecting stable samples";
    case Reason::WARMING: return "Warming up after a size change";
    case Reason::OBSERVING: return "Maintaining quality near the target";
    case Reason::LOWERING: return "Trying a lower working resolution";
    case Reason::RAISING: return "Trying higher quality";
    case Reason::NO_BENEFIT: return "Kept the previous working resolution";
    case Reason::AT_MINIMUM: return "Minimum resolution reached below the FPS target";
    case Reason::FROZEN: return "Paused while editing or comparing";
    case Reason::FAILED: return "Paused after an NR execution failure";
    case Reason::PREPARING: return "Preparing working resolutions";
    case Reason::BUDGET: return "Paused: insufficient workset capacity";
    case Reason::NO_RANGE: return "No working-resolution range to adjust";
  }
  return "Collecting stable samples";
}

struct Size {
  uint32_t width = 0, height = 0;
  bool operator==(const Size&) const = default;
};
using Sizes = std::array<Size, MAX_PASSES>;
struct Settings {
  bool enabled = false;
  float target_fps = 60.f, minimum_scale = 0.67f;
  uint32_t prepared_levels = 0;
  uint32_t selected_levels = 0;
  // The default-off preparation experiments retain their original on-demand ladder.
  bool legacy_preparation = false;
  bool limited_residency = false;
  bool operator==(const Settings&) const = default;
};
struct Output {
  bool available = false;
  uint64_t source = 0, generation = 0, configuration = 0, serial = 0;
  int64_t begin_ns = 0, end_ns = 0;
  double fps = 0., mad = 0.;
  double observed_multiplier = 0.;
  bool multiplier_variable = false;
};
struct Ladder {
  std::array<Sizes, MAX_LEVELS> sizes{};
  std::array<float, MAX_LEVELS> coefficients{};
  size_t count = 0;
};

inline Ladder BuildLadder(const Sizes& upper, const Sizes& stage_floor, float minimum_scale,
                          uint32_t selected_levels = 0) {
  Ladder result;
  minimum_scale = (std::isfinite(minimum_scale) ? std::clamp(minimum_scale, MINIMUM_SCALE, 1.f) : 0.67f);
  for (size_t step = 0; step < MAX_LEVELS; ++step) {
    const float coefficient = std::max(minimum_scale, 1.f - static_cast<float>(step) * SCALE_STEP);
    Sizes sizes;
    for (size_t pass = 0; pass < MAX_PASSES; ++pass) {
      if (upper[pass].width == 0 || upper[pass].height == 0) continue;
      sizes[pass] = {
          .width = std::min(upper[pass].width, std::max(stage_floor[pass].width,
              std::max(1u, static_cast<uint32_t>(std::llround(upper[pass].width * double{coefficient}))))),
          .height = std::min(upper[pass].height, std::max(stage_floor[pass].height,
              std::max(1u, static_cast<uint32_t>(std::llround(upper[pass].height * double{coefficient})))))};
    }
    if (result.count == 0 || result.sizes[result.count - 1] != sizes) {
      result.sizes[result.count] = sizes;
      result.coefficients[result.count++] = coefficient;
    }
    if (coefficient <= minimum_scale) break;
  }
  if (selected_levels >= 2 && result.count > selected_levels) {
    Ladder selected;
    selected.count = selected_levels;
    for (size_t tier = 0; tier < selected.count; ++tier) {
      const size_t index = tier * (result.count - 1) / (selected.count - 1);
      selected.sizes[tier] = result.sizes[index];
      selected.coefficients[tier] = result.coefficients[index];
    }
    return selected;
  }
  return result;
}

struct Status {
  Reason reason = Reason::DISABLED;
  float coefficient = 1.f;
  uint64_t generation = 0;
  Sizes upper{}, requested{}, applied{};
  double output_fps = 0., gpu_ms = 0.;
  uint32_t ready_levels = 0, prepared_levels = 0;
  double preparation_cpu_ms = 0.;
  uint64_t preparation_vram_delta_bytes = 0, peak_process_vram_bytes = 0;
};
struct GpuSample {
  uint64_t generation = 0, frame = 0, sequence = 0, context = 0;
  int64_t recorded_ns = 0, completed_ns = 0;
  double total_us = 0.;
  uint32_t role = 1, required_roles = 1;
  bool valid = false, stable = false;
  bool paired = false;
  uint64_t pair_first_frame = 0, sampling_generation = 0;
};

struct Session {
  Status status;
  Settings settings;
  Ladder ladder;
  uint64_t identity = 0, output_source = 0, output_generation = 0, output_configuration = 0;
  uint64_t resource_identity = 0;
  uint64_t configuration_epoch = 0;
  uint64_t preparation_epoch = 0;
  bool prepared = false;
  bool target_ready = false;
  Reason preparation_reason = Reason::PREPARING;
  Sizes minimum{};
  uint64_t last_output_serial = 0, last_sequence = 0, gpu_context = 0;
  size_t level = 0, previous_level = 0, active_level = 0;
  int64_t applied_ns = 0, stable_after_ns = 0, last_switch_ns = 0, at_target_since_ns = 0;
  int64_t next_measure_ns = 0;
  int64_t failed_raise_until_ns = 0;
  int64_t blocked_until_ns = 0;
  size_t failed_raise_level = MAX_LEVELS;
  bool trial = false, raising = false, extra_window = false, blocked = false, failed = false;
  bool defer_preparation = false;
  bool minimum_pending = false;
  double baseline_fps = 0., baseline_fps_mad = 0., baseline_gpu_us = 0., baseline_gpu_mad = 0.;
  double baseline_multiplier = 0.;
  double blocked_fps = 0., blocked_gpu_us = 0.;
  std::array<double, 5> gpu_us{};
  std::array<int64_t, 5> gpu_recorded_ns{};
  size_t gpu_count = 0;
  GpuSample pending;
  uint32_t pending_roles = 0;
  bool paired_sampling = false;
  uint64_t paired_identity = 0;
  uint64_t sampling_generation = 0, measure_pair_first_frame = 0;
  GpuSample pending_pair;

  void ResetMeasurements() {
    gpu_count = 0;
    gpu_us = {};
    gpu_recorded_ns = {};
    pending = {};
    pending_roles = 0;
    measure_pair_first_frame = 0;
    pending_pair = {};
    last_output_serial = 0;
    at_target_since_ns = 0;
  }

  void InvalidateSampling() {
    ++sampling_generation;
    ResetMeasurements();
  }

  void SetPairedSampling(bool enabled, uint64_t sampling_identity = 0) {
    if (!enabled) sampling_identity = 0;
    if (paired_sampling == enabled && paired_identity == sampling_identity) return;
    paired_sampling = enabled;
    paired_identity = sampling_identity;
    InvalidateSampling();
    trial = false;
  }

  uint64_t SelectMeasurementPair(uint64_t source_frame, bool first_due) {
    if (!paired_sampling || source_frame == 0) {
      measure_pair_first_frame = 0;
      return 0;
    }
    if (measure_pair_first_frame != 0) {
      const uint64_t first = measure_pair_first_frame;
      if (source_frame == first) return 0;
      measure_pair_first_frame = 0;
      if (source_frame == first + 1) return first;
    }
    if (!first_due) return 0;
    measure_pair_first_frame = source_frame;
    return source_frame;
  }

  bool IsLegal(const Sizes& sizes) const {
    for (size_t pass = 0; pass < MAX_PASSES; ++pass) {
      if (sizes[pass].width < minimum[pass].width || sizes[pass].height < minimum[pass].height) return false;
    }
    return true;
  }

  bool TargetReady() const {
    return (settings.limited_residency ? target_ready : prepared);
  }

  void Request(size_t candidate, int64_t now_ns) {
    if (candidate >= ladder.count) return;
    if (!IsLegal(ladder.sizes[candidate])) {
      candidate = 0;
    }
    level = candidate;
    ++status.generation;
    if (settings.limited_residency) {
      target_ready = false;
    } else {
      status.coefficient = ladder.coefficients[level];
    }
    status.requested = ladder.sizes[level];
    applied_ns = 0;
    stable_after_ns = now_ns + WARMUP_NS;
    last_switch_ns = now_ns;
    ResetMeasurements();
  }

  bool RequestMinimum(int64_t now_ns) {
    for (size_t slot = 0; slot < ladder.count; ++slot) {
      if (ladder.sizes[slot] == minimum) {
        minimum_pending = false;
        Request(slot, now_ns);
        return true;
      }
    }
    size_t replacement = ladder.count;
    for (size_t slot = 1; slot < ladder.count; ++slot) {
      if (slot == level || slot == active_level || ladder.sizes[slot] == status.applied
          || (trial && slot == previous_level)) continue;
      if (replacement == ladder.count || !IsLegal(ladder.sizes[slot])
          || ladder.coefficients[slot] > ladder.coefficients[replacement]) {
        replacement = slot;
        if (!IsLegal(ladder.sizes[slot])) break;
      }
    }
    const size_t capacity = (settings.prepared_levels == 0
        ? MAX_LEVELS : std::min<size_t>(settings.prepared_levels, MAX_PREPARED_LEVELS));
    if (replacement == ladder.count && ladder.count < capacity) {
      replacement = ladder.count++;
    }
    if (replacement == ladder.count) {
      // Two resident tiers may need to return to the upper tier before the
      // old lower allocation can retire. No GPU wait creates extra capacity.
      minimum_pending = true;
      Request(0, now_ns);
      return false;
    }
    ladder.sizes[replacement] = minimum;
    ladder.coefficients[replacement] = settings.minimum_scale;
    ++preparation_epoch;
    prepared = false;
    preparation_reason = Reason::PREPARING;
    status.ready_levels = 0;
    status.prepared_levels = static_cast<uint32_t>(ladder.count);
    defer_preparation = true;
    minimum_pending = false;
    Request(replacement, now_ns);
    return true;
  }

  void Applied(uint64_t generation, const Sizes& actual, int64_t now_ns) {
    if (generation != status.generation || actual != status.requested) return;
    if (settings.enabled && settings.limited_residency && !target_ready) return;
    status.applied = actual;
    active_level = level;
    if (applied_ns != 0) return;
    applied_ns = now_ns;
    stable_after_ns = now_ns + WARMUP_NS;
    if (settings.limited_residency) {
      status.coefficient = ladder.coefficients[level];
      last_switch_ns = now_ns;
    }
  }

  void Fail(uint64_t generation) {
    if (generation != status.generation) return;
    failed = true;
    prepared = false;
    target_ready = false;
    preparation_reason = Reason::FAILED;
    trial = false;
    status.reason = Reason::FAILED;
    ResetMeasurements();
  }

  void UpdateTargetPreparation(uint64_t epoch, uint64_t generation, uint32_t ready, uint32_t required,
                               bool target_complete, Reason reason, int64_t now_ns) {
    if (!settings.limited_residency || epoch != preparation_epoch || generation != status.generation
        || !settings.enabled) return;
    status.ready_levels = std::min(ready, required);
    status.prepared_levels = required;
    prepared = required == ladder.count && required != 0 && ready == required;
    if (failed || preparation_reason == Reason::FAILED || reason == Reason::FAILED) {
      Fail(generation);
      return;
    }
    if (reason == Reason::BUDGET || preparation_reason == Reason::BUDGET) {
      target_ready = false;
      prepared = false;
      trial = false;
      preparation_reason = Reason::BUDGET;
      status.reason = Reason::BUDGET;
      ResetMeasurements();
      return;
    }
    const bool complete = required == ladder.count && required != 0 && ready != 0 && target_complete;
    if (!complete) {
      if (target_ready) {
        ResetMeasurements();
        applied_ns = 0;
      }
      target_ready = false;
      preparation_reason = Reason::PREPARING;
      status.reason = Reason::PREPARING;
      return;
    }
    if (!target_ready) {
      target_ready = true;
      ResetMeasurements();
      stable_after_ns = now_ns + WARMUP_NS;
      preparation_reason = Reason::PREPARING;
      status.reason = Reason::WARMING;
    }
  }

  void UpdatePreparation(uint64_t epoch, uint32_t ready, uint32_t required, Reason reason, int64_t now_ns) {
    if (settings.limited_residency) {
      UpdateTargetPreparation(epoch, status.generation, ready, required, false, reason, now_ns);
      return;
    }
    if (epoch != preparation_epoch || !settings.enabled) return;
    status.ready_levels = std::min(ready, required);
    status.prepared_levels = required;
    if (failed || preparation_reason == Reason::FAILED) {
      status.reason = Reason::FAILED;
      return;
    }
    if (reason == Reason::FAILED) {
      if (level != 0) Request(0, now_ns);
      Fail(status.generation);
      return;
    }
    if (reason == Reason::BUDGET || preparation_reason == Reason::BUDGET) {
      if (level != 0) Request(0, now_ns);
      prepared = false;
      trial = false;
      preparation_reason = Reason::BUDGET;
      status.reason = Reason::BUDGET;
      ResetMeasurements();
      return;
    }
    const bool complete = required == ladder.count && required != 0 && ready == required;
    if (!complete) {
      if (prepared) {
        ResetMeasurements();
      }
      prepared = false;
      trial = false;
      preparation_reason = Reason::PREPARING;
      status.reason = Reason::PREPARING;
      return;
    }
    if (!prepared) {
      prepared = true;
      ResetMeasurements();
      stable_after_ns = now_ns + WARMUP_NS;
      preparation_reason = Reason::PREPARING;
      status.reason = Reason::WARMING;
    }
  }

  void ObserveGpu(const GpuSample& sample, int64_t now_ns) {
    if (sample.paired != paired_sampling || sample.sampling_generation != sampling_generation
        || (sample.paired && sample.pair_first_frame == 0)) return;
    if ((!TargetReady() && !settings.legacy_preparation) || sample.generation != status.generation
        || (sample.sequence <= last_sequence && (pending_roles == 0 || sample.frame != pending.frame))) return;
    last_sequence = std::max(last_sequence, sample.sequence);
    if (!sample.valid || !sample.stable || sample.recorded_ns < stable_after_ns || applied_ns == 0
        || sample.recorded_ns > sample.completed_ns || sample.completed_ns > now_ns
        || now_ns - sample.recorded_ns > SAMPLE_MAX_AGE_NS
        || now_ns - sample.completed_ns > SAMPLE_MAX_AGE_NS
        || (sample.paired && (!std::isfinite(sample.total_us) || sample.total_us <= 0.))) {
      pending_roles = 0;
      pending_pair = {};
      if (sample.paired && measure_pair_first_frame == sample.pair_first_frame) {
        measure_pair_first_frame = 0;
      }
      return;
    }
    if (gpu_context != 0 && gpu_context != sample.context) {
      ResetMeasurements();
      trial = false;
    }
    gpu_context = sample.context;
    auto complete = sample;
    if (sample.paired) {
      if (sample.role != 1 || sample.required_roles != 1) {
        pending_pair = {};
        return;
      }
      if (sample.frame == sample.pair_first_frame) {
        pending_pair = sample;
        return;
      }
      if (pending_pair.pair_first_frame != sample.pair_first_frame
          || pending_pair.frame + 1 != sample.frame || pending_pair.generation != sample.generation
          || pending_pair.context != sample.context || pending_pair.sampling_generation != sample.sampling_generation
          || sample.recorded_ns <= pending_pair.recorded_ns
          || now_ns - pending_pair.recorded_ns > SAMPLE_MAX_AGE_NS
          || now_ns - pending_pair.completed_ns > SAMPLE_MAX_AGE_NS) {
        pending_pair = {};
        return;
      }
      complete.total_us = (pending_pair.total_us + sample.total_us) / 2.;
      complete.recorded_ns = pending_pair.recorded_ns;
      complete.completed_ns = std::max(pending_pair.completed_ns, sample.completed_ns);
      complete.frame = pending_pair.frame;
      pending_pair = {};
    }
    if (pending_roles == 0 || pending.frame != complete.frame || pending.generation != complete.generation
        || pending.required_roles != complete.required_roles || pending.context != complete.context) {
      pending = complete;
      pending.total_us = 0.;
      pending_roles = 0;
    }
    if ((pending_roles & complete.role) != 0) return;
    pending_roles |= complete.role;
    pending.total_us += complete.total_us;
    pending.recorded_ns = std::min(pending.recorded_ns, complete.recorded_ns);
    if (pending_roles != complete.required_roles) return;
    pending_roles = 0;
    if (gpu_count != 0 && pending.recorded_ns - gpu_recorded_ns[gpu_count - 1] < SAMPLE_INTERVAL_NS) return;
    if (gpu_count == gpu_us.size()) {
      std::move(gpu_us.begin() + 1, gpu_us.end(), gpu_us.begin());
      std::move(gpu_recorded_ns.begin() + 1, gpu_recorded_ns.end(), gpu_recorded_ns.begin());
      --gpu_count;
    }
    gpu_us[gpu_count] = pending.total_us;
    gpu_recorded_ns[gpu_count++] = pending.recorded_ns;
  }

  bool GpuStatistics(int64_t now_ns, double* median, double* mad) const {
    if (gpu_count != gpu_us.size() || gpu_recorded_ns.back() - gpu_recorded_ns.front() < WARMUP_NS
        || now_ns < gpu_recorded_ns.back() || now_ns - gpu_recorded_ns.front() > SAMPLE_MAX_AGE_NS) return false;
    auto sorted = gpu_us;
    std::sort(sorted.begin(), sorted.end());
    *median = sorted[2];
    for (size_t i = 0; i < sorted.size(); ++i) sorted[i] = std::abs(gpu_us[i] - *median);
    std::sort(sorted.begin(), sorted.end());
    *mad = sorted[2];
    return true;
  }

  void Prepare(const Settings& requested_settings, const Sizes& upper, const Sizes& floor,
               uint64_t source_identity, const Output& output, int64_t now_ns, bool supported, bool frozen,
               uint64_t allocation_identity = 0) {
    Settings next = requested_settings;
    next.target_fps = (std::isfinite(next.target_fps) ? std::clamp(next.target_fps, 30.f, 360.f) : 60.f);
    next.minimum_scale = (std::isfinite(next.minimum_scale) ? std::clamp(next.minimum_scale, MINIMUM_SCALE, 1.f) : 0.67f);
    next.prepared_levels = std::min(next.prepared_levels, MAX_PREPARED_LEVELS);
    if (next.selected_levels != 0) {
      next.selected_levels = std::clamp(next.selected_levels, 2u, MAX_PREPARED_LEVELS);
    }
    if (allocation_identity == 0) {
      allocation_identity = source_identity;
    }
    const uint32_t selected_levels = (next.selected_levels != 0 ? next.selected_levels
        : (next.legacy_preparation ? next.prepared_levels : std::max(2u, next.prepared_levels)));
    auto selected = BuildLadder(upper, floor, next.minimum_scale, selected_levels);
    const auto next_minimum = selected.sizes[selected.count - 1];
    const bool resources_changed = resource_identity != allocation_identity;
    const bool retain_legacy = next.legacy_preparation && settings.legacy_preparation && !resources_changed
        && status.upper == upper && settings.prepared_levels == next.prepared_levels;
    if (retain_legacy) {
      selected = ladder;
    }
    const bool selection_changed = ladder.count != selected.count || ladder.sizes != selected.sizes;
    const bool selection_edited = settings.selected_levels != next.selected_levels || settings.minimum_scale != next.minimum_scale;
    const bool toggled = settings.enabled != next.enabled;
    const bool preparation_mode_changed = settings.legacy_preparation != next.legacy_preparation
        || settings.limited_residency != next.limited_residency;
    const bool changed = identity != source_identity || settings != next || status.upper != upper || selection_changed;
    if (resources_changed) {
      const uint64_t generation = status.generation + 1;
      const uint64_t epoch = preparation_epoch + 1;
      *this = Session{};
      status.generation = generation;
      preparation_epoch = epoch;
      settings = next;
      identity = source_identity;
      resource_identity = allocation_identity;
      status.upper = upper;
      ladder = selected;
      minimum = next_minimum;
      status.prepared_levels = static_cast<uint32_t>(ladder.count);
      Request(0, now_ns);
    } else if (changed) {
      const auto retained = (trial ? ladder.sizes[previous_level] : status.applied);
      const bool defer_legacy = retain_legacy && (defer_preparation || settings.minimum_scale != next.minimum_scale);
      const bool retry_budget = preparation_reason == Reason::BUDGET
          && (selection_edited || settings.limited_residency != next.limited_residency);
      if (selection_changed || toggled || preparation_mode_changed || retry_budget) {
        ++preparation_epoch;
        prepared = false;
        target_ready = false;
        status.ready_levels = 0;
        status.prepared_levels = static_cast<uint32_t>(selected.count);
      }
      if ((!settings.enabled && next.enabled) || retry_budget) {
        preparation_reason = Reason::PREPARING;
      }
      if (!settings.enabled && next.enabled) {
        failed = false;
      }
      settings = next;
      identity = source_identity;
      status.upper = upper;
      ladder = selected;
      minimum = next_minimum;
      size_t candidate = 0;
      active_level = 0;
      for (size_t slot = 0; slot < ladder.count; ++slot) {
        if (ladder.sizes[slot] == status.applied) {
          active_level = slot;
        }
        if (next.enabled && ladder.sizes[slot] == retained && IsLegal(ladder.sizes[slot])) {
          candidate = slot;
        }
      }
      trial = false;
      raising = false;
      extra_window = false;
      blocked = false;
      blocked_until_ns = 0;
      minimum_pending = false;
      defer_preparation = defer_legacy;
      failed_raise_level = MAX_LEVELS;
      failed_raise_until_ns = 0;
      previous_level = candidate;
      Request(candidate, now_ns);
    }
    if (!settings.enabled || !supported) {
      if (level != 0) Request(0, now_ns);
      trial = false;
      minimum_pending = false;
      status.reason = (settings.enabled ? Reason::UNSUPPORTED : Reason::DISABLED);
      return;
    }
    if (failed) {
      status.reason = Reason::FAILED;
      return;
    }
    if (frozen) {
      trial = false;
      ResetMeasurements();
      status.reason = Reason::FROZEN;
      return;
    }
    if (!settings.legacy_preparation && (preparation_reason == Reason::BUDGET
        || (!settings.limited_residency && ladder.count > settings.prepared_levels))) {
      UpdatePreparation(preparation_epoch, status.ready_levels, static_cast<uint32_t>(ladder.count), Reason::BUDGET, now_ns);
      return;
    }
    if (!settings.legacy_preparation && !settings.limited_residency && !prepared) {
      status.reason = Reason::PREPARING;
      return;
    }
    if (!settings.legacy_preparation && ladder.count == 1) {
      status.reason = Reason::NO_RANGE;
      return;
    }
    const bool output_changed = output_source != output.source || output_generation != output.generation
        || output_configuration != output.configuration;
    if (output_changed || !output.available) {
      if (trial) Request(previous_level, now_ns);
      trial = false;
      blocked = false;
      blocked_until_ns = 0;
      minimum_pending = false;
      ResetMeasurements();
      output_source = output.source;
      output_generation = output.generation;
      output_configuration = output.configuration;
    }
    status.output_fps = (output.available ? output.fps : 0.);
    if (!output.available) {
      status.reason = Reason::OUTPUT_UNAVAILABLE;
      return;
    }
    if (output.multiplier_variable && (!std::isfinite(output.observed_multiplier) || output.observed_multiplier <= 0.)) {
      if (trial) Request(previous_level, now_ns);
      trial = false;
      ResetMeasurements();
      status.reason = Reason::COLLECTING;
      return;
    }
    if (settings.limited_residency && !target_ready) {
      status.reason = Reason::PREPARING;
      return;
    }
    if (blocked && now_ns >= blocked_until_ns) {
      blocked = false;
      blocked_until_ns = 0;
      ResetMeasurements();
      stable_after_ns = now_ns;
      status.reason = Reason::COLLECTING;
      return;
    }
    if (applied_ns == 0 || now_ns < stable_after_ns) {
      status.reason = Reason::WARMING;
      return;
    }
    double gpu = 0., gpu_mad = 0.;
    if (output.begin_ns < stable_after_ns || output.end_ns > now_ns || now_ns - output.end_ns > SAMPLE_MAX_AGE_NS
        || !GpuStatistics(now_ns, &gpu, &gpu_mad)) {
      status.reason = Reason::COLLECTING;
      return;
    }
    status.gpu_ms = gpu / 1000.;
    if (last_output_serial == output.serial) return;
    last_output_serial = output.serial;
    const auto adjacent = [&](bool higher, size_t steps) {
      size_t result = level;
      for (size_t step = 0; step < steps; ++step) {
        size_t nearest = result;
        for (size_t slot = 0; slot < ladder.count; ++slot) {
          if (!IsLegal(ladder.sizes[slot])) continue;
          if (higher ? ladder.coefficients[slot] <= ladder.coefficients[result]
                     : ladder.coefficients[slot] >= ladder.coefficients[result]) continue;
          if (nearest == result || (higher ? ladder.coefficients[slot] < ladder.coefficients[nearest]
                                          : ladder.coefficients[slot] > ladder.coefficients[nearest])) {
            nearest = slot;
          }
        }
        if (nearest == result) break;
        result = nearest;
      }
      return result;
    };
    if (trial) {
      const bool multiplier_changed = baseline_multiplier > 0. && output.observed_multiplier > 0.
          && std::abs(output.observed_multiplier / baseline_multiplier - 1.) > 0.10;
      if (multiplier_changed && !extra_window) {
        extra_window = true;
        stable_after_ns = output.end_ns;
        return;
      }
      const bool on_target = output.fps >= settings.target_fps * 0.95;
      const double fps_margin = std::max({1., baseline_fps * 0.02, 2. * output.mad, 2. * baseline_fps_mad});
      const bool cheaper = baseline_gpu_us - gpu > std::max({1., 2. * baseline_gpu_mad, 2. * gpu_mad});
      const bool improved = output.fps - baseline_fps > fps_margin;
      if (!multiplier_changed && ((raising && on_target) || (!raising && cheaper && (on_target || improved)))) {
        trial = false;
        status.reason = Reason::OBSERVING;
      } else {
        const bool uncertain = !multiplier_changed && !raising && cheaper && output.fps >= baseline_fps
            && output.fps - baseline_fps <= fps_margin && output.mad > 0.;
        if (uncertain && !extra_window) {
          extra_window = true;
          stable_after_ns = output.end_ns;
          return;
        }
        if (!settings.legacy_preparation && TargetReady() && !raising && !multiplier_changed && cheaper
            && output.fps >= baseline_fps - fps_margin) {
          const size_t candidate = adjacent(false, 1);
          if (candidate != level) {
            // Compare cumulative savings with the accepted anchor. A small
            // adjacent step cannot establish that every lower ready tier is useless.
            if (now_ns - last_switch_ns >= 3'000'000'000) {
              extra_window = false;
              Request(candidate, now_ns);
            }
            status.reason = Reason::LOWERING;
            return;
          }
        }
        if (raising) {
          failed_raise_level = level;
          failed_raise_until_ns = now_ns + RETRY_COOLDOWN_NS;
        } else {
          blocked = true;
          blocked_until_ns = now_ns + RETRY_COOLDOWN_NS;
          blocked_fps = (multiplier_changed ? output.fps : baseline_fps);
          blocked_gpu_us = baseline_gpu_us;
        }
        Request(previous_level, now_ns);
        trial = false;
        status.reason = Reason::NO_BENEFIT;
        return;
      }
    }
    if (blocked && (std::abs(output.fps - blocked_fps) > std::max(3., blocked_fps * 0.15)
        || std::abs(gpu - blocked_gpu_us) > std::max(100., blocked_gpu_us * 0.2))) {
      blocked = false;
      blocked_until_ns = 0;
    }
    if (output.fps >= settings.target_fps * 0.99) {
      if (at_target_since_ns == 0) at_target_since_ns = output.begin_ns;
    } else {
      at_target_since_ns = 0;
    }
    size_t candidate = level;
    if (output.fps < settings.target_fps * 0.95 && !blocked && now_ns - last_switch_ns >= 3'000'000'000) {
      candidate = (settings.legacy_preparation && minimum_pending ? MAX_LEVELS
          : adjacent(false, settings.legacy_preparation && settings.prepared_levels == 0 ? 2 : 1));
      if (settings.legacy_preparation && candidate == level && ladder.sizes[level] != minimum) {
        candidate = MAX_LEVELS;
      }
      raising = false;
    } else if (level != 0 && at_target_since_ns != 0 && now_ns - at_target_since_ns >= 6'000'000'000
        && now_ns - last_switch_ns >= 10'000'000'000
        && (adjacent(true, 1) != failed_raise_level || now_ns >= failed_raise_until_ns)) {
      candidate = adjacent(true, 1);
      raising = true;
    }
    if (candidate == level) {
      status.reason = (blocked ? Reason::NO_BENEFIT
          : (ladder.sizes[level] == minimum && output.fps < settings.target_fps * 0.95 ? Reason::AT_MINIMUM : Reason::OBSERVING));
      return;
    }
    previous_level = level;
    baseline_fps = output.fps;
    baseline_fps_mad = output.mad;
    baseline_gpu_us = gpu;
    baseline_gpu_mad = gpu_mad;
    baseline_multiplier = output.observed_multiplier;
    extra_window = false;
    trial = true;
    if (candidate == MAX_LEVELS) {
      if (!RequestMinimum(now_ns)) {
        trial = false;
      }
    } else {
      Request(candidate, now_ns);
    }
    status.reason = (raising ? Reason::RAISING : (trial ? Reason::LOWERING : Reason::PREPARING));
  }
};

struct Frame {
  std::shared_ptr<Session> session;
  Sizes sizes{}, upper{};
  uint64_t generation = 0, application_frame = 0;
  uint64_t layout_identity = 0;
  uint64_t preparation_epoch = 0;
  uint32_t required_roles = 1, role = 1;
  uint32_t slot = 0;
  uint32_t output_count = 1;
  uintptr_t output_identity = 0;
  bool enabled = false, measure = false, preparing = false, immediate = true;
  bool upper_capacity = false;
  bool paired = false;
  uint64_t pair_first_frame = 0, sampling_generation = 0;
};

}  // namespace renodx::addons::dlss5::adaptive_resolution
