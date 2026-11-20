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
#include <utility>
#include <vector>

namespace renodx::addons::dlss5::adaptive_resolution {

constexpr int64_t SAMPLE_INTERVAL_NS = 250'000'000;
constexpr int64_t SAMPLE_MAX_AGE_NS = 2'000'000'000;
constexpr int64_t WARMUP_NS = 1'000'000'000;
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
enum ResourceStrategy : uint32_t {
  OFF,
  RETAIN,
  UPPER_CAPACITY,
  UPPER_CAPACITY_ON_DEMAND,
  UPPER_CAPACITY_ADJACENT,
};
enum class ResourceFallback { NONE, BINDING, LEVELS };

struct Settings {
  bool enabled = false;
  float target_fps = 60.f, minimum_scale = 0.67f;
  uint32_t prepared_levels = 0;
  uint32_t selected_levels = 0;
  // Alternate strategies use the existing retargetable preparation path.
  bool legacy_preparation = false;
  bool limited_residency = false;
  ResourceStrategy resource_strategy = ResourceStrategy::OFF;
  bool operator==(const Settings&) const = default;
};
struct Output {
  bool available = false;
  uint64_t source = 0, generation = 0, configuration = 0, serial = 0;
  int64_t begin_ns = 0, end_ns = 0;
  double fps = 0., mad = 0.;
  double observed_multiplier = 0.;
  bool multiplier_variable = false;
  bool split_available = false;
  std::array<double, 2> split_fps{};
  uint64_t owner = 0;
  double rate_multiplier = 0.;
  bool rate_comparable = true;
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
  uint64_t display_output_generation = 0, display_source_generation = 0;
  uint32_t display_stage = 0;
  ResourceStrategy requested_resource_strategy = ResourceStrategy::OFF;
  ResourceStrategy effective_resource_strategy = ResourceStrategy::OFF;
  ResourceFallback resource_fallback = ResourceFallback::NONE;
  bool resource_strategy_observed = false;
  bool resource_limited_residency = false;
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
  struct Conditions {
    uint64_t workload = 0, owner = 0, source = 0;
    float target_fps = 0.;
    Sizes upper{};
    bool multiplier_variable = false;
    std::array<bool, 2> edit_cadence{};
    std::array<std::array<uint64_t, MAX_PASSES>, 2> mask{};
    std::array<uint64_t, 2> normalization{};
    bool operator==(const Conditions&) const = default;
  };
  struct FailedRaise {
    Sizes lower{}, upper{};
    Conditions conditions;
    double multiplier = 0., fps = 0., mad = 0.;
  };
  struct WindowEvidence {
    int64_t end_ns = 0, duration_ns = 0;
    double fps = 0., mad = 0.;
  };
  Status status;
  Settings settings;
  Ladder ladder;
  uint64_t identity = 0, output_source = 0, output_generation = 0, output_configuration = 0;
  uint64_t workload_identity = 0, output_owner = 0;
  // Upper, then supported lower-tier reuse; independent of timing mode.
  std::array<bool, 2> edit_cadence{};
  bool delivered_edit_reuse = false;
  uint64_t delivered_edit_sequence = 0;
  uint64_t resource_identity = 0;
  uint64_t configuration_epoch = 0;
  uint64_t preparation_epoch = 0;
  uint64_t applied_generation = 0;
  bool prepared = false;
  bool target_ready = false;
  Reason preparation_reason = Reason::PREPARING;
  Sizes minimum{};
  uint64_t last_output_serial = 0, last_sequence = 0, gpu_context = 0;
  size_t level = 0, previous_level = 0, active_level = 0;
  size_t frozen_resume_level = MAX_LEVELS;
  int64_t applied_ns = 0, stable_after_ns = 0, last_switch_ns = 0;
  int64_t raise_after_ns = 0;
  int64_t next_measure_ns = 0;
  bool trial = false, raising = false, failed = false;
  bool raise_comparable = true;
  bool defer_preparation = false;
  bool minimum_pending = false;
  double baseline_fps = 0., baseline_fps_mad = 0., baseline_gpu_us = 0., baseline_gpu_mad = 0.;
  double baseline_multiplier = 0.;
  double output_multiplier = 0.;
  bool output_variable = false;
  Conditions trial_conditions;
  std::vector<FailedRaise> failed_raises;
  WindowEvidence deficit_window, target_window, surplus_window;
  std::array<double, 5> gpu_us{};
  std::array<int64_t, 5> gpu_recorded_ns{};
  size_t gpu_count = 0;
  GpuSample pending;
  uint32_t pending_roles = 0;
  bool paired_sampling = false;
  uint64_t paired_identity = 0;
  uint64_t sampling_generation = 0, measure_pair_first_frame = 0;
  uint64_t comparison_generation = 0;
  uint32_t measurement_roles = 3;
  GpuSample pending_pair;
  std::array<std::array<uint64_t, MAX_PASSES>, 2> mask_settings{};
  std::array<uint64_t, 2> normalization_settings{};

  void ResetGpuMeasurements() {
    gpu_count = 0;
    gpu_us = {};
    gpu_recorded_ns = {};
    pending = {};
    pending_roles = 0;
    measure_pair_first_frame = 0;
    pending_pair = {};
    status.gpu_ms = 0.;
  }

  void ResetMeasurements() {
    ResetGpuMeasurements();
    last_output_serial = 0;
    deficit_window = target_window = surplus_window = {};
  }

  void InvalidateSampling(int64_t now_ns = 0) {
    ++sampling_generation;
    ++comparison_generation;
    ResetMeasurements();
    if (now_ns > 0) stable_after_ns = std::max(stable_after_ns, now_ns);
  }

  void ObserveEffectiveSettings(uint64_t& previous, uint64_t effective_settings, int64_t now_ns) {
    const bool changed = previous != 0 && previous != effective_settings;
    previous = effective_settings;
    if (!changed) return;
    InvalidateSampling(now_ns);
    if (frozen_resume_level == MAX_LEVELS) {
      if (trial) {
        if (raising || applied_generation != status.generation) {
          Request(previous_level, now_ns);
        }
        trial = false;
      }
      status.reason = Reason::COLLECTING;
    }
  }

  void ObserveMaskSettings(uint32_t role, uint32_t pass, uint64_t effective_settings, int64_t now_ns) {
    if ((role != 1 && role != 2) || pass >= MAX_PASSES || effective_settings == 0) return;
    ObserveEffectiveSettings(mask_settings[role - 1][pass], effective_settings, now_ns);
  }

  void ObserveNormalizationSettings(uint32_t role, uint64_t effective_settings, int64_t now_ns) {
    if ((role != 1 && role != 2) || effective_settings == 0) return;
    ObserveEffectiveSettings(normalization_settings[role - 1], effective_settings, now_ns);
  }

  void SetPairedSampling(bool enabled, uint64_t sampling_identity = 0, int64_t = 0) {
    if (!enabled) sampling_identity = 0;
    if (paired_sampling == enabled && paired_identity == sampling_identity) return;
    paired_sampling = enabled;
    paired_identity = sampling_identity;
    ++sampling_generation;
    ResetGpuMeasurements();
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

  void Request(size_t candidate, int64_t) {
    if (candidate >= ladder.count) return;
    if (!IsLegal(ladder.sizes[candidate])) {
      candidate = 0;
    }
    level = candidate;
    ++status.generation;
    if (settings.limited_residency) {
      target_ready = false;
    }
    status.requested = ladder.sizes[level];
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
    if (failed || generation != status.generation || actual != status.requested || now_ns <= 0) return;
    if (settings.enabled && !settings.legacy_preparation && !TargetReady()) return;
    if (applied_generation == generation && applied_ns != 0) return;
    status.applied = actual;
    active_level = level;
    applied_generation = generation;
    applied_ns = now_ns;
    stable_after_ns = now_ns + WARMUP_NS;
    status.coefficient = ladder.coefficients[level];
    last_switch_ns = now_ns;
    if (settings.enabled) status.reason = Reason::WARMING;
  }

  void ConfirmEditCadence(uint64_t sequence, bool reuse, bool same_grid, int64_t now_ns) {
    if (sequence <= delivered_edit_sequence) return;
    const bool changed = same_grid && delivered_edit_sequence != 0 && delivered_edit_reuse != reuse;
    delivered_edit_sequence = sequence;
    delivered_edit_reuse = reuse;
    const size_t endpoint = (status.applied == status.upper ? 0u : 1u);
    if (reuse != edit_cadence[endpoint] && trial && raising) raise_comparable = false;
    if (!changed) return;
    InvalidateSampling(now_ns);
    raise_after_ns = now_ns;
    if (trial && raising) {
      Request(previous_level, now_ns);
      trial = false;
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
        InvalidateSampling(now_ns);
      }
      target_ready = false;
      preparation_reason = Reason::PREPARING;
      status.reason = Reason::PREPARING;
      return;
    }
    if (!target_ready) {
      target_ready = true;
      ResetMeasurements();
      preparation_reason = Reason::PREPARING;
      status.reason = Reason::PREPARING;
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
        InvalidateSampling(now_ns);
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
      preparation_reason = Reason::PREPARING;
      status.reason = Reason::PREPARING;
    }
  }

  void ObserveGpu(const GpuSample& sample, int64_t now_ns) {
    if (frozen_resume_level != MAX_LEVELS
        || sample.paired != paired_sampling || sample.sampling_generation != sampling_generation
        || (sample.paired && sample.pair_first_frame == 0)) return;
    if ((!TargetReady() && !settings.legacy_preparation) || sample.generation != status.generation
        || (sample.sequence <= last_sequence && (pending_roles == 0 || sample.frame != pending.frame))) return;
    last_sequence = std::max(last_sequence, sample.sequence);
    if (!sample.valid || !sample.stable || sample.recorded_ns < stable_after_ns || applied_ns == 0
        || applied_generation != status.generation
        || sample.recorded_ns > sample.completed_ns || sample.completed_ns > now_ns
        || now_ns - sample.recorded_ns > SAMPLE_MAX_AGE_NS
        || now_ns - sample.completed_ns > SAMPLE_MAX_AGE_NS
        || !std::isfinite(sample.total_us) || sample.total_us <= 0.) {
      pending_roles = 0;
      pending_pair = {};
      if (sample.paired && measure_pair_first_frame == sample.pair_first_frame) {
        measure_pair_first_frame = 0;
      }
      return;
    }
    if (gpu_context != 0 && gpu_context != sample.context) {
      ++sampling_generation;
      ResetGpuMeasurements();
      gpu_context = sample.context;
      return;
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

  static bool ComparableMultiplier(double first, double second) {
    return (first <= 0. && second <= 0.)
        || (first > 0. && second > 0. && std::abs(second / first - 1.) <= 0.10);
  }

  static bool Smaller(const Sizes& first, const Sizes& second) {
    for (size_t pass = 0; pass < MAX_PASSES; ++pass) {
      if (first[pass].width != second[pass].width) return first[pass].width < second[pass].width;
      if (first[pass].height != second[pass].height) return first[pass].height < second[pass].height;
    }
    return false;
  }

  size_t Adjacent(bool higher) const {
    size_t nearest = level;
    for (size_t slot = 0; slot < ladder.count; ++slot) {
      if (!IsLegal(ladder.sizes[slot])) continue;
      if (higher ? !Smaller(ladder.sizes[level], ladder.sizes[slot])
                 : !Smaller(ladder.sizes[slot], ladder.sizes[level])) continue;
      if (nearest == level || (higher ? Smaller(ladder.sizes[slot], ladder.sizes[nearest])
                                     : Smaller(ladder.sizes[nearest], ladder.sizes[slot]))) {
        nearest = slot;
      }
    }
    return nearest;
  }

  static bool AccumulateWindow(WindowEvidence* evidence, const Output& output) {
    if (evidence->end_ns != 0 && output.begin_ns < evidence->end_ns) return false;
    const int64_t duration_ns = output.end_ns - output.begin_ns;
    if (evidence->end_ns != 0 && output.begin_ns == evidence->end_ns) {
      evidence->fps = (evidence->fps * static_cast<double>(evidence->duration_ns)
          + output.fps * static_cast<double>(duration_ns)) / static_cast<double>(evidence->duration_ns + duration_ns);
      evidence->duration_ns += duration_ns;
      evidence->end_ns = output.end_ns;
      evidence->mad = std::max(evidence->mad, output.mad);
      return true;
    }
    *evidence = {.end_ns = output.end_ns, .duration_ns = duration_ns, .fps = output.fps, .mad = output.mad};
    return false;
  }

  Conditions CurrentConditions(const Output& output) const {
    Conditions result{.workload = workload_identity, .owner = output.owner, .source = output.source,
        .target_fps = settings.target_fps, .upper = status.upper, .multiplier_variable = output.multiplier_variable,
        .edit_cadence = edit_cadence,
        .mask = mask_settings, .normalization = normalization_settings};
    if (delivered_edit_sequence != 0) {
      result.edit_cadence[status.applied == status.upper ? 0u : 1u] = delivered_edit_reuse;
    }
    for (size_t role = 0; role < result.mask.size(); ++role) {
      if ((measurement_roles & (1u << role)) == 0) {
        result.mask[role] = {};
        result.normalization[role] = 0;
      } else {
        for (size_t pass = 0; pass < MAX_PASSES; ++pass) {
          if (status.upper[pass].width == 0) {
            result.mask[role][pass] = 0;
          }
        }
      }
    }
    return result;
  }

  const FailedRaise* FindFailedRaise(const Sizes& lower, const Sizes& upper, const Conditions& conditions,
                                    double multiplier) const {
    for (const auto& edge : failed_raises) {
      if (edge.lower == lower && edge.upper == upper && edge.conditions == conditions
          && ComparableMultiplier(edge.multiplier, multiplier)) return &edge;
    }
    return nullptr;
  }

  void RememberFailedRaise() {
    const FailedRaise failed_raise{.lower = ladder.sizes[previous_level], .upper = ladder.sizes[level],
        .conditions = trial_conditions, .multiplier = baseline_multiplier, .fps = baseline_fps, .mad = baseline_fps_mad};
    for (auto& edge : failed_raises) {
      if (edge.lower == failed_raise.lower && edge.upper == failed_raise.upper && edge.conditions == failed_raise.conditions
          && ComparableMultiplier(edge.multiplier, failed_raise.multiplier)) {
        edge = failed_raise;
        return;
      }
    }
    failed_raises.push_back(failed_raise);
  }

  void Prepare(const Settings& requested_settings, const Sizes& upper, const Sizes& floor,
               uint64_t source_identity, const Output& output, int64_t now_ns, bool supported, bool frozen,
               uint64_t allocation_identity = 0, uint64_t work_identity = 0, uint32_t model_roles = 3,
               std::array<bool, 2> cadence = {}, bool confirm_edit_cadence = false) {
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
    if (work_identity == 0) {
      work_identity = source_identity;
    }
    const uint32_t selected_levels = (next.selected_levels != 0 ? next.selected_levels
        : (next.legacy_preparation ? next.prepared_levels : std::max(2u, next.prepared_levels)));
    auto selected = BuildLadder(upper, floor, next.minimum_scale, selected_levels);
    const auto next_minimum = selected.sizes[selected.count - 1];
    const bool resources_changed = resource_identity != allocation_identity;
    const bool retain_legacy = next.legacy_preparation && settings.legacy_preparation && !resources_changed
        && status.upper == upper && settings.prepared_levels == next.prepared_levels
        && settings.selected_levels == next.selected_levels;
    if (retain_legacy) {
      selected = ladder;
    }
    const bool selection_changed = ladder.count != selected.count || ladder.sizes != selected.sizes;
    const bool selection_edited = settings.selected_levels != next.selected_levels || settings.minimum_scale != next.minimum_scale;
    const bool toggled = settings.enabled != next.enabled;
    const bool preparation_mode_changed = settings.legacy_preparation != next.legacy_preparation
        || settings.limited_residency != next.limited_residency || settings.resource_strategy != next.resource_strategy;
    const bool work_hash_changed = workload_identity != work_identity;
    const size_t active_cadence_index = (status.applied == status.upper ? 0u : 1u);
    const bool active_cadence_changed = !confirm_edit_cadence
        && edit_cadence[active_cadence_index] != cadence[active_cadence_index];
    const bool workload_changed = work_hash_changed || active_cadence_changed
        || edit_cadence[1u - active_cadence_index] != cadence[1u - active_cadence_index];
    const bool changed = (!frozen && (identity != source_identity
        || measurement_roles != model_roles))
        || settings != next || status.upper != upper || selection_changed;
    if (resources_changed) {
      const uint64_t generation = status.generation + 1;
      const uint64_t epoch = preparation_epoch + 1;
      auto retained_failures = std::move(failed_raises);
      *this = Session{};
      failed_raises = std::move(retained_failures);
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
      const auto retained = (trial && raising ? ladder.sizes[previous_level] : status.applied);
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
      frozen_resume_level = MAX_LEVELS;
      raising = false;
      minimum_pending = false;
      defer_preparation = defer_legacy;
      previous_level = candidate;
      if (!selection_changed && !toggled && !preparation_mode_changed && !retry_budget
          && applied_generation == status.generation && level == candidate
          && status.requested == ladder.sizes[candidate]) {
        InvalidateSampling(now_ns);
      } else {
        Request(candidate, now_ns);
      }
    }
    if (!frozen && workload_changed) {
      // A change at the other endpoint only rebuilds raise evidence. The
      // delivered tier needs a clean FPS window only if its own cadence changes.
      surplus_window = {};
      raise_after_ns = now_ns;
      if (active_cadence_changed) {
        InvalidateSampling(now_ns);
      }
      if (trial && raising) {
        if (active_cadence_changed || work_hash_changed) {
          Request(previous_level, now_ns);
          trial = false;
        } else if (applied_generation != status.generation && edit_cadence[0] != cadence[0]) {
          // An older recording may still deliver the pending upper tier.
          // Its cadence is unproven, so it cannot establish a failed edge.
          raise_comparable = false;
        }
      }
    }
    if (!frozen) {
      workload_identity = work_identity;
      edit_cadence = cadence;
    }
    measurement_roles = model_roles;
    if (!settings.enabled || !supported) {
      if (level != 0) Request(0, now_ns);
      trial = false;
      frozen_resume_level = MAX_LEVELS;
      minimum_pending = false;
      status.reason = (settings.enabled ? Reason::UNSUPPORTED : Reason::DISABLED);
      return;
    }
    if (failed) {
      status.reason = Reason::FAILED;
      return;
    }
    if (frozen) {
      if (frozen_resume_level == MAX_LEVELS) {
        frozen_resume_level = level;
        if (applied_ns != 0 && status.requested != status.applied) {
          const auto end = ladder.sizes.begin() + ladder.count;
          const auto confirmed = std::find(ladder.sizes.begin(), end, status.applied);
          if (confirmed != end && IsLegal(*confirmed)) {
            // Replace the pending generation before a ready tier can hot-switch.
            Request(static_cast<size_t>(confirmed - ladder.sizes.begin()), now_ns);
            frozen_resume_level = level;
            trial = false;
          }
        }
        InvalidateSampling(now_ns);
      }
      status.reason = Reason::FROZEN;
      return;
    }
    if (frozen_resume_level != MAX_LEVELS) {
      if (level != frozen_resume_level) {
        Request(frozen_resume_level, now_ns);
      }
      frozen_resume_level = MAX_LEVELS;
      InvalidateSampling(now_ns);
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
    if (ladder.count == 1 && ladder.sizes[0] == minimum) {
      status.reason = Reason::NO_RANGE;
      return;
    }
    const bool output_changed = output_source != output.source || output_generation != output.generation
        || output_configuration != output.configuration || output_owner != output.owner;
    const double multiplier = (output.rate_multiplier > 0. ? output.rate_multiplier : output.observed_multiplier);
    const bool rate_changed = output_variable != output.multiplier_variable
        || (output_multiplier > 0. && multiplier > 0. && !ComparableMultiplier(output_multiplier, multiplier));
    const bool reliable_output = output.available && std::isfinite(output.fps) && output.fps >= 0.
        && std::isfinite(output.mad) && output.mad >= 0. && output.begin_ns > 0 && output.end_ns > output.begin_ns
        && output.end_ns <= now_ns && now_ns - output.end_ns <= SAMPLE_MAX_AGE_NS
        && output.rate_comparable
        && (!output.multiplier_variable || (std::isfinite(output.observed_multiplier) && output.observed_multiplier > 0.));
    if (output_changed || rate_changed || !reliable_output) {
      // Missing comparison evidence cancels a pending request or an unjudged
      // quality increase. A successfully delivered lower tier remains in use.
      if (trial && (raising || applied_generation != status.generation)) {
        Request(previous_level, now_ns);
      }
      trial = false;
      InvalidateSampling(now_ns);
      output_source = output.source;
      output_generation = output.generation;
      output_configuration = output.configuration;
      output_owner = output.owner;
      output_variable = output.multiplier_variable;
      output_multiplier = multiplier;
    } else if (output_multiplier == 0.) {
      output_multiplier = multiplier;
    }
    status.output_fps = (output.available ? output.fps : 0.);
    double gpu = 0., gpu_mad = 0.;
    const bool gpu_available = GpuStatistics(now_ns, &gpu, &gpu_mad);
    status.gpu_ms = (gpu_available ? gpu / 1000. : 0.);
    if (!reliable_output) {
      status.reason = (output.available ? Reason::COLLECTING : Reason::OUTPUT_UNAVAILABLE);
      return;
    }
    if (settings.limited_residency && !target_ready) {
      status.reason = Reason::PREPARING;
      return;
    }
    if (applied_generation != status.generation || applied_ns == 0 || now_ns < stable_after_ns) {
      status.reason = Reason::WARMING;
      return;
    }
    if (output.begin_ns < stable_after_ns) {
      status.reason = Reason::COLLECTING;
      return;
    }
    if (last_output_serial == output.serial) return;
    last_output_serial = output.serial;
    const bool split = output.split_available && std::isfinite(output.split_fps[0])
        && std::isfinite(output.split_fps[1]) && output.split_fps[0] >= 0. && output.split_fps[1] >= 0.;
    bool deficit = false, on_target = false;
    if (output.fps < settings.target_fps) {
      target_window = surplus_window = {};
      deficit = (split && output.split_fps[0] < settings.target_fps && output.split_fps[1] < settings.target_fps)
          || AccumulateWindow(&deficit_window, output);
    } else {
      deficit_window = {};
      on_target = (split && output.split_fps[0] >= settings.target_fps && output.split_fps[1] >= settings.target_fps)
          || AccumulateWindow(&target_window, output);
    }
    if (trial && raising) {
      if (deficit) {
        if (raise_comparable) {
          RememberFailedRaise();
        }
        Request(previous_level, now_ns);
        trial = false;
        status.reason = Reason::NO_BENEFIT;
      } else if (on_target) {
        trial = false;
        status.reason = Reason::OBSERVING;
      } else {
        status.reason = Reason::COLLECTING;
      }
      return;
    }
    if (deficit || on_target) {
      trial = false;
    }
    if (deficit) {
      const size_t candidate = (settings.legacy_preparation && minimum_pending ? level : Adjacent(false));
      if (candidate == level && ladder.sizes[level] == minimum) {
        status.reason = Reason::AT_MINIMUM;
        return;
      }
      previous_level = level;
      baseline_fps = output.fps;
      baseline_fps_mad = output.mad;
      baseline_gpu_us = gpu;
      baseline_gpu_mad = gpu_mad;
      baseline_multiplier = multiplier;
      raising = false;
      trial = true;
      if (candidate == level) {
        if (!settings.legacy_preparation || !RequestMinimum(now_ns)) {
          trial = false;
        }
      } else {
        Request(candidate, now_ns);
      }
      status.reason = (trial ? Reason::LOWERING : Reason::PREPARING);
      return;
    }
    const size_t candidate = Adjacent(true);
    const auto conditions = CurrentConditions(output);
    const auto* failed_raise = FindFailedRaise(ladder.sizes[level], ladder.sizes[candidate], conditions, multiplier);
    const double margin = std::max({1., static_cast<double>(settings.target_fps) * 0.02, 2. * output.mad});
    bool headroom = output.begin_ns >= raise_after_ns && output.fps >= settings.target_fps + margin;
    if (failed_raise != nullptr) {
      const double recovery_margin = std::max({1., static_cast<double>(settings.target_fps) * 0.02,
          2. * std::max(failed_raise->mad, output.mad)});
      headroom &= output.fps >= failed_raise->fps + recovery_margin;
    }
    if (!headroom) {
      surplus_window = {};
    }
    if (candidate != level && headroom && AccumulateWindow(&surplus_window, output)) {
      previous_level = level;
      baseline_fps = surplus_window.fps;
      baseline_fps_mad = surplus_window.mad;
      baseline_gpu_us = gpu;
      baseline_gpu_mad = gpu_mad;
      baseline_multiplier = multiplier;
      trial_conditions = conditions;
      raise_comparable = true;
      trial = raising = true;
      Request(candidate, now_ns);
      status.reason = Reason::RAISING;
      return;
    }
    status.reason = (ladder.sizes[level] == minimum && output.fps < settings.target_fps
        ? Reason::AT_MINIMUM : Reason::OBSERVING);
  }
};

enum class DeliveryReceipt : uint8_t { PENDING, DELIVERED, REJECTED };

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
  uint64_t comparison_generation = 0;
  // Instrumented color roles share the consumer's decision.
  mutable std::shared_ptr<DeliveryReceipt> delivery;
};

}  // namespace renodx::addons::dlss5::adaptive_resolution
