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
#include <iostream>

#include "../adaptive_resolution.hpp"

namespace adaptive_resolution_cases {
namespace adaptive = renodx::addons::dlss5::adaptive_resolution;

struct FpsTrace {
  static constexpr int64_t WINDOW_NS = 2'000'000'000;
  static constexpr int64_t BUCKET_NS = WINDOW_NS / 4;
  adaptive::Sizes upper{{{1920, 1080}, {960, 540}}};
  adaptive::Sizes floor{{{960, 540}, {480, 270}}};
  adaptive::Settings settings{.enabled = true, .target_fps = 60.f, .minimum_scale = .5f,
      .prepared_levels = 2, .selected_levels = 2};
  adaptive::Output output{.source = 1, .generation = 1, .configuration = 1, .split_available = true, .owner = 7};
  adaptive::Session controller;
  int64_t now = 1'000'000'000;
  uint64_t workload = 1;
  uint32_t model_roles = 3;
  uint64_t frame = 0;

  void Prepare(bool supported = true, bool frozen = false) {
    controller.Prepare(settings, upper, floor, 1, output, now, supported, frozen, 1, workload, model_roles);
  }

  void Ready() {
    const auto required = static_cast<uint32_t>(controller.ladder.count);
    if (settings.limited_residency) {
      controller.UpdateTargetPreparation(controller.preparation_epoch, controller.status.generation, 2, required,
          true, adaptive::Reason::PREPARING, now);
    } else {
      controller.UpdatePreparation(controller.preparation_epoch, required, required, adaptive::Reason::PREPARING, now);
    }
  }

  void Deliver() {
    controller.Applied(controller.status.generation, controller.status.requested, now);
  }

  void Start() {
    Prepare();
    Ready();
    Prepare();
    Deliver();
  }

  void WarmUp() {
    now += adaptive::WARMUP_NS;
  }

  void CompleteWindow(int64_t begin_ns, int64_t end_ns, double first_fps, double last_fps, double mad = 0.) {
    now = end_ns;
    output.available = true;
    ++output.serial;
    output.begin_ns = begin_ns;
    output.end_ns = end_ns;
    output.fps = (first_fps + last_fps) * .5;
    output.mad = mad;
    output.split_fps = {first_fps, last_fps};
    Prepare();
  }

  void Window(double fps, int64_t duration_ns = WINDOW_NS, double mad = 0.) {
    CompleteWindow(now, now + duration_ns, fps, fps, mad);
  }

  void GpuCost(double cost_us, uint64_t context = 1) {
    // Five completed observations at the production recording cadence.
    for (uint32_t index = 0; index < 5; ++index) {
      const auto recorded_ns = now - adaptive::WARMUP_NS + index * adaptive::SAMPLE_INTERVAL_NS;
      controller.ObserveGpu({.generation = controller.applied_generation, .frame = ++frame, .sequence = frame,
          .context = context, .recorded_ns = recorded_ns, .completed_ns = now, .total_us = cost_us,
          .valid = true, .stable = true, .sampling_generation = controller.sampling_generation}, now);
    }
  }
};

template <typename Check>
void RunFpsCases(const Check& check) {
  constexpr auto window_ns = FpsTrace::WINDOW_NS;
  constexpr auto bucket_ns = FpsTrace::BUCKET_NS;
  constexpr auto warmup_ns = adaptive::WARMUP_NS;
  constexpr auto at_minimum = adaptive::Reason::AT_MINIMUM;

  for (const uint32_t count : {3u, 4u, 9u, adaptive::MAX_PREPARED_LEVELS}) {
    FpsTrace trace;
    trace.settings.selected_levels = trace.settings.prepared_levels = count;
    trace.settings.limited_residency = count == 9;
    if (trace.settings.limited_residency) {
      trace.settings.prepared_levels = 2;
    }
    if (count == adaptive::MAX_PREPARED_LEVELS) {
      trace.settings.minimum_scale = .33f;
      trace.floor[0] = {.width = 634, .height = 356};
      trace.floor[1] = {.width = 317, .height = 178};
    }
    trace.Start();
    check(trace.controller.ladder.count == count && trace.controller.ladder.sizes[0] == trace.upper
              && trace.controller.ladder.sizes[count - 1] == trace.floor,
          "the selected ladder preserves both complete physical endpoints");
    for (uint32_t tier = 1; tier < count; ++tier) {
      const auto old_sizes = trace.controller.ladder.sizes[tier - 1];
      const auto next_sizes = trace.controller.ladder.sizes[tier];
      check(next_sizes[0].width < old_sizes[0].width && next_sizes[1].width < old_sizes[1].width,
            "the shared group coefficient orders both Passes at each physical tier");
      const auto delivered_at = trace.controller.applied_ns;
      trace.WarmUp();
      if (count != adaptive::MAX_PREPARED_LEVELS) {
        const double cost_us = (count == 3 ? 3000. : 3000. + 100. * (tier - 1));
        trace.now += warmup_ns;
        trace.GpuCost(cost_us);
        double median = 0., mad = 0.;
        check(trace.controller.GpuStatistics(trace.now, &median, &mad) && median == cost_us,
              "each delivered tier retains its own complete GPU diagnostics");
      }
      trace.Window(tier == 1 ? 57. : 58.);
      check(trace.controller.status.requested == next_sizes && trace.controller.status.applied == old_sizes
                && trace.controller.applied_ns == delivered_at && trace.controller.status.reason != at_minimum,
            "a sustained deficit requests only the adjacent tier even with tiny or zero FPS gains");
      trace.Ready();
      check(trace.controller.status.applied == old_sizes && trace.controller.applied_ns == delivered_at,
            "ready resources do not confirm either a cold or fully prepared target");
      trace.Deliver();
      check(trace.controller.status.applied == next_sizes && trace.controller.applied_ns == trace.now
                && trace.controller.stable_after_ns == trace.now + warmup_ns,
            "the complete group delivery starts the new tier's warmup");
    }
    trace.WarmUp();
    trace.Window(58.);
    check(trace.controller.status.applied == trace.floor && trace.controller.status.requested == trace.floor
              && trace.controller.status.reason == at_minimum && trace.controller.status.output_fps == 58.,
          "constant or increasing costs and entirely missing costs all allow reaching the effective minimum");
  }

  {
    FpsTrace trace;
    trace.settings.selected_levels = trace.settings.prepared_levels = 3;
    trace.Start();
    trace.WarmUp();
    trace.Window(59.);
    trace.Ready();
    trace.Deliver();
    const auto middle = trace.controller.ladder.sizes[1];
    const auto generation = trace.controller.status.generation;
    trace.WarmUp();
    for (int index = 0; index < 20; ++index) {
      trace.Window(60.);
    }
    check(trace.controller.status.generation == generation && trace.controller.status.applied == middle
              && trace.controller.status.requested == middle && trace.controller.status.reason != at_minimum,
          "a 59 FPS deficit descends but exact 60 FPS at the middle tier holds indefinitely");
    trace.now += 1;
    trace.Prepare(true, true);
    check(trace.controller.status.reason == adaptive::Reason::FROZEN && trace.controller.status.requested == middle
              && trace.controller.status.applied == middle && trace.controller.status.generation == generation,
          "comparison freezes the confirmed tier and generation");
    const auto thawed = ++trace.now;
    trace.Prepare();
    trace.CompleteWindow(thawed - bucket_ns, thawed - bucket_ns + window_ns, 59., 59.);
    check(trace.controller.status.requested == middle, "a window crossing thaw cannot initiate a request");
    trace.Window(59.);
    check(trace.controller.status.requested == trace.floor, "a fully post-thaw window resumes control");
    auto frozen_pending = trace;
    const auto withdrawn_generation = frozen_pending.controller.status.generation;
    ++frozen_pending.now;
    frozen_pending.Prepare(true, true);
    frozen_pending.controller.Applied(withdrawn_generation, trace.floor, frozen_pending.now);
    check(frozen_pending.controller.status.reason == adaptive::Reason::FROZEN
              && frozen_pending.controller.status.requested == middle && frozen_pending.controller.status.applied == middle
              && frozen_pending.controller.status.generation != withdrawn_generation,
          "comparison withdraws a pending tier and rejects its late completion");
    auto unsupported = trace;
    unsupported.Prepare(false, true);
    check(unsupported.controller.status.reason == adaptive::Reason::UNSUPPORTED
              && unsupported.controller.status.requested == trace.upper,
          "the unsupported path keeps its manual upper-bound fallback during comparison");
    trace.settings.enabled = false;
    trace.Prepare();
    check(trace.controller.status.reason == adaptive::Reason::DISABLED
              && trace.controller.status.requested == trace.upper,
          "disabling control restores the manual upper bound");
  }

  for (const bool straddling : {false, true}) {
    FpsTrace trace;
    trace.output.split_available = straddling;
    trace.Start();
    trace.WarmUp();
    const auto begin_ns = trace.now;
    constexpr std::array<double, 8> buckets{52., 52., 52., 68., 52., 52., 52., 68.};
    for (size_t index = 0; index < 4; ++index) {
      const auto offset = static_cast<int64_t>(index) * bucket_ns;
      trace.CompleteWindow(begin_ns + offset, begin_ns + offset + window_ns,
          (buckets[index] + buckets[index + 1]) * .5, (buckets[index + 2] + buckets[index + 3]) * .5);
    }
    check(trace.controller.status.requested == trace.upper,
          "one below-target whole window and overlapping publications cannot replace disjoint coverage");
    // Continue the evaluated cadence with a separate full window.
    trace.CompleteWindow(begin_ns + window_ns, begin_ns + 2 * window_ns, 52., 60.);
    check(trace.controller.status.requested == trace.floor,
          "two contiguous below-target window averages support the whole-window fallback");
  }
  {
    FpsTrace trace;
    trace.output.split_available = false;
    trace.Start();
    trace.WarmUp();
    trace.Window(59.);
    trace.now += bucket_ns;
    trace.Window(59.);
    check(trace.controller.status.requested == trace.upper, "an uncovered gap breaks below-target persistence");
    trace.Window(59.);
    check(trace.controller.status.requested == trace.floor, "two new windows after a gap can resume descent");
  }

  for (const bool lost_fps : {false, true}) {
    FpsTrace trace;
    trace.settings.selected_levels = trace.settings.prepared_levels = 3;
    trace.settings.limited_residency = true;
    trace.output.source = 3;
    trace.output.multiplier_variable = !lost_fps;
    trace.output.observed_multiplier = 2.;
    trace.Start();
    trace.WarmUp();
    trace.Window(56.);
    trace.Ready();
    trace.Deliver();
    const auto middle = trace.controller.status.applied;
    const auto confirmed_at = trace.now;
    const auto generation = trace.controller.status.generation;
    trace.WarmUp();
    trace.output.observed_multiplier = 0.;
    if (lost_fps) {
      trace.now += window_ns;
      trace.output.available = false;
      trace.Prepare();
    } else {
      trace.Window(48.);
    }
    check(trace.controller.status.generation == generation && trace.controller.status.requested == middle
              && trace.controller.status.applied == middle && trace.controller.applied_ns == confirmed_at,
          "loss of the FPS window or its dynamic denominator keeps the confirmed lower tier");
    const auto recovered_at = trace.now;
    trace.output.observed_multiplier = 2.;
    trace.CompleteWindow(recovered_at - bucket_ns, recovered_at - bucket_ns + window_ns, 56., 56.);
    check(trace.controller.status.requested == middle, "a window that crosses lost coverage cannot advance a tier");
    trace.Window(56.);
    check(trace.controller.status.requested == trace.floor,
          "resumed complete coverage can request the next legal cold tier");
  }

  {
    FpsTrace fixed;
    fixed.settings.target_fps = 100.f;
    fixed.output.source = 3;
    fixed.output.rate_multiplier = 2.;
    fixed.Start();
    fixed.WarmUp();
    fixed.Window(80., 3 * window_ns);
    check(fixed.controller.status.requested == fixed.floor && fixed.controller.status.applied == fixed.upper,
          "fixed SDK counts can request the lower tier with 80 FPS and no application denominator");
    fixed.Ready();
    fixed.Deliver();
    fixed.WarmUp();
    fixed.Window(100., 3 * window_ns);
    check(fixed.controller.status.applied == fixed.floor && fixed.controller.status.requested == fixed.floor
              && fixed.controller.status.output_fps == 100. && !fixed.controller.trial,
          "the lower tier holds the exact 100 FPS target using a complete slow SDK window");

    FpsTrace dynamic;
    dynamic.output.source = 3;
    dynamic.output.multiplier_variable = true;
    dynamic.Start();
    dynamic.WarmUp();
    dynamic.Window(56.);
    check(dynamic.controller.status.requested == dynamic.upper && dynamic.controller.status.output_fps == 56.
              && dynamic.controller.status.reason == adaptive::Reason::COLLECTING,
          "dynamic SDK counts can report FPS while missing denominator coverage suspends control");
    dynamic.output.observed_multiplier = 2.;
    dynamic.Window(56.);
    check(dynamic.controller.status.requested == dynamic.floor, "restoring usable dynamic coverage permits descent");
    dynamic.Ready();
    dynamic.Deliver();
    dynamic.WarmUp();
    dynamic.Window(70.);
    dynamic.output.observed_multiplier = 4.;
    dynamic.Window(140.);
    check(dynamic.controller.status.requested == dynamic.floor && dynamic.controller.status.applied == dynamic.floor,
          "a 2x to 4x rate change cannot complete the earlier margin comparison");
    dynamic.Window(140.);
    check(dynamic.controller.status.requested == dynamic.floor, "the new rate still needs a second independent window");
    dynamic.Window(140.);
    check(dynamic.controller.status.requested == dynamic.upper, "two new windows at a comparable 4x rate can raise");
    const auto pending_generation = dynamic.controller.status.generation;
    ++dynamic.output.owner;
    dynamic.Window(140.);
    dynamic.controller.Applied(pending_generation, dynamic.upper, dynamic.now);
    check(dynamic.controller.status.requested == dynamic.floor && dynamic.controller.status.applied == dynamic.floor
              && dynamic.controller.status.generation != pending_generation,
          "a replacement output withdraws the previous owner's request and rejects its late delivery");
  }

  {
    FpsTrace trace;
    trace.Start();
    trace.WarmUp();
    trace.Window(56.);
    trace.Ready();
    trace.Deliver();
    trace.WarmUp();
    for (int index = 0; index < 4; ++index) {
      trace.Window(60.);
    }
    const auto floor_generation = trace.controller.status.generation;
    const auto first_begin_ns = trace.now;
    // One 84 FPS bucket among seven 60 FPS buckets yields four above-target
    // publications. All four share precisely the same spike.
    constexpr std::array<double, 7> buckets{60., 60., 60., 84., 60., 60., 60.};
    for (size_t index = 0; index < 4; ++index) {
      const auto begin_ns = first_begin_ns + static_cast<int64_t>(index) * bucket_ns;
      trace.CompleteWindow(begin_ns, begin_ns + window_ns,
          (buckets[index] + buckets[index + 1]) * .5, (buckets[index + 2] + buckets[index + 3]) * .5);
      trace.Prepare();
    }
    check(trace.controller.status.generation == floor_generation && trace.controller.status.requested == trace.floor,
          "exact target and one reused spike produce no raise across the rolling publications");
    trace.Window(60.);
    trace.Window(61.);
    trace.Window(61.);
    trace.Window(64., window_ns, 3.);
    trace.Window(64., window_ns, 3.);
    check(trace.controller.status.requested == trace.floor, "minimum target-relative headroom and MAD both gate a raise");
    trace.Window(64.);
    const auto first_end_ns = trace.now;
    check(trace.controller.status.requested == trace.floor, "one genuine margin window only starts the observation");
    trace.CompleteWindow(first_end_ns - bucket_ns, first_end_ns - bucket_ns + window_ns, 64., 64.);
    check(trace.controller.status.requested == trace.floor, "a second published margin window may still overlap the first");
    trace.CompleteWindow(first_end_ns, first_end_ns + window_ns, 64., 64.);
    check(trace.controller.status.requested == trace.upper && trace.controller.status.applied == trace.floor,
          "two disjoint contiguous headroom windows request one higher tier");
    trace.Ready();
    trace.Deliver();
    const auto raised_generation = trace.controller.status.generation;
    auto at_target = trace;
    at_target.WarmUp();
    at_target.Window(60.);
    check(at_target.controller.status.requested == trace.upper && at_target.controller.status.applied == trace.upper
              && !at_target.controller.trial && at_target.controller.status.generation == raised_generation,
          "the delivered higher tier is retained when its new window sustains the exact target");
    trace.WarmUp();
    trace.Window(59.);
    check(trace.controller.status.applied == trace.upper && trace.controller.status.requested == trace.floor,
          "a delivered raise slightly below target requests rollback to the prior physical tier");
    trace.Ready();
    trace.Deliver();
    check(trace.controller.status.applied == trace.floor && trace.controller.applied_ns == trace.now,
          "a failed raise rolls back only on actual lower-tier delivery");
  }

  {
    FpsTrace trace;
    trace.output.source = 3;
    trace.output.multiplier_variable = true;
    trace.output.observed_multiplier = 2.;
    trace.output.rate_multiplier = 2.;
    trace.Start();
    trace.WarmUp();
    trace.Window(56.);
    trace.Ready();
    trace.Deliver();
    trace.WarmUp();
    trace.Window(64.);
    trace.Window(72., 3 * window_ns);
    check(trace.controller.status.requested == trace.upper && trace.controller.status.applied == trace.floor,
          "independent low-frequency output windows use their full covered durations");
    trace.Ready();
    trace.Deliver();
    auto interrupted = trace;
    interrupted.now += bucket_ns;
    interrupted.output.available = false;
    interrupted.Prepare();
    check(interrupted.controller.status.applied == trace.upper && interrupted.controller.status.requested == trace.floor,
          "lost output during a delivered raise requests the previous lower tier");
    interrupted.Ready();
    interrupted.Deliver();
    interrupted.WarmUp();
    interrupted.Window(64.);
    interrupted.Window(72., 3 * window_ns);
    check(interrupted.controller.status.requested == trace.upper,
          "an unmeasurable raise records no failure and the same valid headroom can retry");
    trace.WarmUp();
    trace.Window(59.);
    check(trace.controller.status.requested == trace.floor && trace.controller.status.applied == trace.upper,
          "valid below-target coverage, rather than lost input, makes the weighted-baseline raise fail");
    trace.Ready();
    trace.Deliver();
    const auto failed_generation = trace.controller.status.generation;
    trace.WarmUp();
    for (int index = 0; index < 20; ++index) {
      trace.Window(70.);
    }
    check(trace.controller.status.generation == failed_generation && trace.controller.status.requested == trace.floor,
          "the time-weighted 70 FPS baseline stays blocked beyond the former cooldown");
    trace.now += window_ns + 1;
    trace.output.available = false;
    ++trace.output.generation;
    trace.Prepare();
    trace.Window(70.);
    trace.Window(70.);
    check(trace.controller.status.generation == failed_generation && trace.controller.status.requested == trace.floor,
          "a temporary interruption and new producer generation preserve the comparable edge");
    trace.CompleteWindow(trace.now, trace.now + window_ns, 74., 70.);
    trace.Window(70.);
    check(trace.controller.status.requested == trace.floor, "a brief spike cannot improve the failed edge's stable baseline");

    auto mixed_rate = trace;
    mixed_rate.Window(72.);
    mixed_rate.output.rate_comparable = false;
    mixed_rate.Window(72.);
    check(mixed_rate.controller.status.reason == adaptive::Reason::COLLECTING
              && mixed_rate.controller.status.output_fps == 72.
              && mixed_rate.controller.status.applied == trace.floor
              && mixed_rate.controller.status.generation == failed_generation,
          "a mixed-rate SDK window displays measured FPS and cannot complete a raise");
    mixed_rate.output.rate_comparable = true;
    mixed_rate.Window(70.);
    mixed_rate.Window(70.);
    check(mixed_rate.controller.status.requested == trace.floor
              && mixed_rate.controller.status.generation == failed_generation,
          "restored comparable output at the same FPS retains the failed edge");

    auto timing_only = trace;
    timing_only.controller.SetPairedSampling(true, 2, ++timing_only.now);
    timing_only.Window(70.);
    timing_only.Window(70.);
    check(timing_only.controller.status.requested == trace.floor
              && timing_only.controller.status.generation == failed_generation,
          "diagnostic paired timing alone cannot reopen a failed edge at unchanged FPS");

    auto other_workload = timing_only;
    // Model the workload identity supplied by FreezeAdaptiveFrame once x2 is effective.
    other_workload.workload = 2;
    ++other_workload.now;
    other_workload.Prepare();
    other_workload.Window(70.);
    check(other_workload.controller.status.requested == trace.floor, "effective x2 requires fresh independent windows");
    auto changed_target = other_workload;
    changed_target.workload = trace.workload;
    changed_target.settings.target_fps = 65.f;
    ++changed_target.now;
    changed_target.Prepare();
    changed_target.Window(70.);
    changed_target.Window(70.);
    check(changed_target.controller.status.requested == trace.upper, "a changed target can re-evaluate the same sizes");
    auto returned_workload = other_workload;
    returned_workload.workload = trace.workload;
    ++returned_workload.now;
    returned_workload.Prepare();
    returned_workload.Window(70.);
    returned_workload.Window(70.);
    check(returned_workload.controller.status.requested == trace.floor,
          "a temporary workload B followed by A retains A's failed edge");
    other_workload.Window(70.);
    check(other_workload.controller.status.requested == trace.upper && other_workload.controller.status.applied == trace.floor,
          "effective x2 can retry the same physical edge at unchanged FPS after new coverage");

    auto reindexed = trace;
    reindexed.settings.selected_levels = reindexed.settings.prepared_levels = 3;
    ++reindexed.now;
    reindexed.Prepare();
    reindexed.Ready();
    reindexed.Deliver();
    reindexed.settings.selected_levels = reindexed.settings.prepared_levels = 2;
    ++reindexed.now;
    reindexed.Prepare();
    reindexed.Ready();
    reindexed.Deliver();
    reindexed.WarmUp();
    reindexed.Window(70.);
    reindexed.Window(70.);
    check(reindexed.controller.status.applied == trace.floor && reindexed.controller.status.requested == trace.floor,
          "reindexing and restoring the same size pair cannot erase its failed comparison");

    trace.Window(72.);
    trace.Window(72.);
    check(trace.controller.status.requested == trace.upper, "sustained improvement over the failed low-tier baseline can retry");
    trace.Ready();
    trace.Deliver();
    trace.WarmUp();
    trace.Window(59.);
    trace.Ready();
    trace.Deliver();
    trace.WarmUp();
    trace.Window(72.);
    trace.Window(72.);
    check(trace.controller.status.requested == trace.floor, "a second failure records the improved low-tier reference");
  }
  {
    FpsTrace trace;
    trace.model_roles = 2;
    trace.Start();
    trace.controller.ObserveMaskSettings(2, 0, 20, trace.now);
    trace.controller.ObserveNormalizationSettings(2, 21, trace.now);
    trace.WarmUp();
    trace.Window(56.);
    trace.Ready();
    trace.Deliver();
    trace.WarmUp();
    trace.Window(70.);
    trace.Window(70.);
    check(trace.controller.status.requested == trace.upper && trace.controller.status.applied == trace.floor,
          "role 2 alone can start a raise with two complete surplus windows");
    trace.Ready();
    trace.Deliver();
    trace.WarmUp();
    trace.Window(59.);
    check(trace.controller.status.requested == trace.floor && trace.controller.status.applied == trace.upper,
          "below-target output records a failed raise with only role 2 active");
    trace.Ready();
    trace.Deliver();
    trace.WarmUp();
    trace.model_roles = 3;
    ++trace.now;
    trace.Prepare();
    trace.controller.ObserveMaskSettings(1, 0, 10, trace.now);
    trace.controller.ObserveNormalizationSettings(1, 11, trace.now);
    trace.Window(70.);
    trace.model_roles = 2;
    ++trace.now;
    trace.Prepare();
    const auto lower_generation = trace.controller.status.generation;
    trace.Window(70.);
    trace.Window(70.);
    check(trace.controller.status.requested == trace.floor && trace.controller.status.applied == trace.floor
              && trace.controller.status.generation == lower_generation,
          "role 1 history from a temporary dual-role excursion leaves the original role 2 failed edge intact");

    for (const bool change_mask : {true, false}) {
      auto changed = trace;
      ++changed.now;
      if (change_mask) {
        changed.controller.ObserveMaskSettings(2, 0, 30, changed.now);
      } else {
        changed.controller.ObserveNormalizationSettings(2, 31, changed.now);
      }
      changed.Window(70.);
      check(changed.controller.status.requested == trace.floor,
            "after changing role 2 parameters, one fresh surplus window is still insufficient");
      changed.Window(70.);
      check(changed.controller.status.requested == trace.upper && changed.controller.status.applied == trace.floor,
            "changing role 2 mask or normalization permits a retrial at the unchanged low-tier FPS");
    }
  }
  for (const bool upper_costs_available : {false, true}) {
    FpsTrace trace;
    trace.Start();
    trace.controller.SetPairedSampling(true, 1, trace.now);
    trace.WarmUp();
    const auto window_begin_ns = trace.now;
    const auto window_end_ns = window_begin_ns + window_ns;
    double median = 0., mad = 0.;
    if (upper_costs_available) {
      for (uint32_t index = 0; index < 5; ++index) {
        const auto first_ns = window_begin_ns + index * adaptive::SAMPLE_INTERVAL_NS;
        const auto first = ++trace.frame;
        trace.controller.ObserveGpu({.generation = trace.controller.applied_generation, .frame = first,
            .sequence = first, .context = 1, .recorded_ns = first_ns, .completed_ns = first_ns,
            .total_us = 3000., .valid = true, .stable = true, .paired = true, .pair_first_frame = first,
            .sampling_generation = trace.controller.sampling_generation}, first_ns);
        const auto second_ns = first_ns + 1;
        const auto second = ++trace.frame;
        trace.controller.ObserveGpu({.generation = trace.controller.applied_generation, .frame = second,
            .sequence = second, .context = 1, .recorded_ns = second_ns, .completed_ns = second_ns,
            .total_us = 1000., .valid = true, .stable = true, .paired = true, .pair_first_frame = first,
            .sampling_generation = trace.controller.sampling_generation}, second_ns);
      }
    }
    check(trace.controller.GpuStatistics(window_end_ns, &median, &mad) == upper_costs_available
              && (!upper_costs_available || median == 2000.),
          "complete x2 pairs provide the per-application-frame mean and absent pairs leave diagnostics unavailable");
    trace.CompleteWindow(window_begin_ns, window_end_ns, 56., 56.);
    const auto lower_generation = trace.controller.status.generation;
    const auto upper_generation = trace.controller.applied_generation;
    const auto upper_switch_ns = trace.controller.last_switch_ns;
    const auto paired_generation = trace.controller.sampling_generation;
    trace.controller.SetPairedSampling(false, 0, trace.now);
    check(trace.controller.status.requested == trace.floor && trace.controller.status.applied == trace.upper
              && trace.controller.status.generation == lower_generation
              && trace.controller.last_switch_ns == upper_switch_ns,
          "switching to per-frame timing preserves the same FPS request with or without measured x2 costs");
    trace.Ready();
    trace.Deliver();
    trace.WarmUp();
    trace.now += warmup_ns;
    trace.GpuCost(1500.);
    trace.controller.ObserveGpu({.generation = upper_generation, .frame = ++trace.frame, .sequence = trace.frame,
        .context = 1, .recorded_ns = trace.now, .completed_ns = trace.now, .total_us = 1.,
        .valid = true, .stable = true, .paired = true, .sampling_generation = paired_generation}, trace.now);
    check(trace.controller.GpuStatistics(trace.now, &median, &mad) && median == 1500.,
          "a late paired upper-tier completion cannot enter the lower tier's per-frame costs");
    trace.Window(64.);
    const auto low_switch_ns = trace.controller.last_switch_ns;
    trace.now += adaptive::SAMPLE_INTERVAL_NS;
    trace.controller.ObserveGpu({.generation = lower_generation, .frame = ++trace.frame, .sequence = trace.frame,
        .context = 2, .recorded_ns = trace.now, .completed_ns = trace.now, .total_us = 1500.,
        .valid = true, .stable = true, .sampling_generation = trace.controller.sampling_generation}, trace.now);
    check(trace.controller.gpu_context == 2 && !trace.controller.GpuStatistics(trace.now, &median, &mad),
          "a new queue starts a fresh cost distribution");
    const auto first_end_ns = trace.output.end_ns;
    trace.CompleteWindow(first_end_ns, first_end_ns + window_ns, 64., 64.);
    check(trace.controller.status.requested == trace.upper && trace.controller.status.applied == trace.floor
              && trace.controller.last_switch_ns == low_switch_ns,
          "a GPU queue change leaves both completed FPS headroom windows comparable");
    trace.controller.SetPairedSampling(true, 2, trace.now);
    trace.controller.SetPairedSampling(true, 3, trace.now);
    check(trace.controller.status.requested == trace.upper && trace.controller.status.applied == trace.floor,
          "paired-mode and identity changes cannot withdraw a valid pending raise");
  }
}

inline bool Run() {
  bool passed = true;
  const auto check = [&](bool condition, const char* name) {
    if (!condition) {
      std::cerr << "adaptive-resolution: " << name << '\n';
      passed = false;
    }
  };
  RunFpsCases(check);
  adaptive::Sizes upper{}, floor{};
  upper[0] = {.width = 2560, .height = 1440};
  floor[0] = {.width = 1267, .height = 713};
  auto ladder = adaptive::BuildLadder(upper, floor, 0.67f);
  check(ladder.count == 8 && std::abs(ladder.coefficients[7] - 0.67f) < 0.001f,
        "ladder includes the exact relative lower bound");
  check(ladder.sizes[7][0] == adaptive::Size{.width = 1715, .height = 965}, "both axes share one coefficient");
  upper[1] = {.width = 10, .height = 10};
  floor[1] = {.width = 20, .height = 20};
  ladder = adaptive::BuildLadder(upper, floor, 0.33f);
  check(ladder.sizes[ladder.count - 1][0] == floor[0], "stage floor constrains relative minimum");
  check(ladder.sizes.back()[1].width == 0 || ladder.sizes[ladder.count - 1][1] == upper[1],
        "a Follow upper bound below the floor never grows");
  upper[1] = floor[1] = {};

  adaptive::Session session;
  session.Prepare({.enabled = true, .target_fps = 20.f}, upper, floor, 1, {}, 1, true, false);
  check(session.settings.target_fps == 30.f, "controller target lower bound is 30 FPS");
  constexpr int64_t start = 1'000'000'000;
  for (const int frequency : {60, 120, 240}) {
    adaptive::Session profiling;
    profiling.prepared = true;
    profiling.status.generation = 1;
    profiling.applied_generation = 1;
    profiling.applied_ns = start;
    profiling.stable_after_ns = start;
    for (int i = 0; i < frequency * 2; ++i) {
      const int64_t now = start + static_cast<int64_t>(i) * 1'000'000'000 / frequency;
      profiling.ObserveGpu({.generation = 1, .frame = static_cast<uint64_t>(i + 1),
          .sequence = static_cast<uint64_t>(i + 1), .context = 1,
          .recorded_ns = now, .completed_ns = now, .total_us = 3000., .valid = true, .stable = true}, now);
    }
    check(profiling.gpu_count == 5
        && profiling.gpu_recorded_ns.back() - profiling.gpu_recorded_ns.front() >= 1'000'000'000,
        "profiling is downsampled by recording time at 60/120/240 Hz");
  }
  // Two-phase NR costs must be averaged before the 250 ms / five-point filter.
  // At 120 Hz that cadence can otherwise stay on either phase with zero MAD.
  constexpr double paired_refresh_us = 3390.9725, paired_reuse_us = 533.0355;
  constexpr double paired_mean_us = (paired_refresh_us + paired_reuse_us) / 2.;
  const auto paired_sample = [](adaptive::Session& target, uint64_t frame, uint64_t first,
                                int64_t now, double cost, bool valid = true, uint64_t context = 1) {
    adaptive::GpuSample sample{.generation = target.status.generation, .frame = frame, .sequence = frame,
        .context = context, .recorded_ns = now, .completed_ns = now, .total_us = cost, .valid = valid, .stable = true};
    sample.paired = true;
    sample.pair_first_frame = first;
    sample.sampling_generation = target.sampling_generation;
    return sample;
  };
  for (uint32_t phase = 0; phase < 2; ++phase) {
    adaptive::Session profiling;
    profiling.prepared = true;
    profiling.status.generation = 1;
    profiling.applied_generation = 1;
    profiling.applied_ns = profiling.stable_after_ns = start;
    profiling.SetPairedSampling(true);
    uint32_t measured_sources = 0;
    int64_t now = start;
    for (uint64_t frame = 1; frame <= 240; ++frame) {
      now = start + static_cast<int64_t>(frame - 1) * 1'000'000'000 / 120;
      const auto first = profiling.SelectMeasurementPair(frame, (frame - 1) % 30 == 0);
      measured_sources += first != 0;
      const double cost = ((frame - 1 + phase) % 2 == 0 ? paired_refresh_us : paired_reuse_us);
      // Include profiling's extra completions; they must not enter as single samples.
      profiling.ObserveGpu(paired_sample(profiling, frame, first, now, cost), now);
    }
    double median = 0., mad = 0.;
    check(measured_sources == 16 && profiling.GpuStatistics(now, &median, &mad)
              && std::abs(median - paired_mean_us) < .0001 && mad < .0001,
          "either 120 Hz phase selects only adjacent pairs and reports their complete per-source mean");
  }
  {
    adaptive::Session pairing;
    pairing.prepared = true;
    pairing.status.generation = 1;
    pairing.applied_generation = 1;
    pairing.applied_ns = pairing.stable_after_ns = start;
    pairing.SetPairedSampling(true, 0xA1);
    const auto feed = [&](uint64_t frame, uint64_t first, bool valid = true, uint64_t context = 1) {
      const int64_t now = start + static_cast<int64_t>(frame) * 10'000'000;
      pairing.ObserveGpu(paired_sample(pairing, frame, first, now, 2000., valid, context), now);
    };
    check(pairing.SelectMeasurementPair(1, true) == 1 && pairing.SelectMeasurementPair(3, false) == 0,
          "a missing adjacent source does not extend a measurement pair to a later source");
    feed(1, 1);
    check(pairing.gpu_count == 0, "one completed source cannot publish a paired cost");
    feed(2, 1, false);
    feed(3, 3);
    feed(5, 3);
    check(pairing.gpu_count == 0, "invalid or nonadjacent completions never form a GPU cost pair");
    feed(6, 6);
    auto old_completion = paired_sample(pairing, 7, 6, start + 70'000'000, 2000.);
    pairing.InvalidateSampling();
    pairing.ObserveGpu(old_completion, old_completion.completed_ns);
    check(pairing.gpu_count == 0, "Reset invalidation rejects a late second source in the same adaptive generation");
    feed(8, 8);
    feed(9, 8, true, 2);
    check(pairing.gpu_count == 0, "different queue contexts cannot supply opposite halves of one pair");
    feed(10, 10, true, 2);
    feed(11, 10, true, 2);
    check(pairing.gpu_count == 1 && pairing.gpu_us[0] == 2000., "a fresh complete pair resumes after rejected halves");
    feed(12, 12, true, 2);
    check(pairing.SelectMeasurementPair(12, true) == 12, "the identity probe has a scheduled adjacent source");
    pairing.trial = true;
    const auto identity_epoch = pairing.sampling_generation;
    const auto identity_sequence = pairing.last_sequence;
    pairing.SetPairedSampling(true, 0xA1);
    check(pairing.paired_identity == 0xA1 && pairing.sampling_generation == identity_epoch
              && pairing.gpu_count == 1 && pairing.gpu_us[0] == 2000. && pairing.trial
              && pairing.pending_pair.frame == 12 && pairing.measure_pair_first_frame == 12,
          "the same paired identity preserves costs, in-flight halves, scheduling and the active trial");
    old_completion = paired_sample(pairing, 13, 12, start + 130'000'000, 2000., true, 2);
    pairing.SetPairedSampling(true, 0xB2);
    check(pairing.paired_sampling && pairing.paired_identity == 0xB2
              && pairing.sampling_generation == identity_epoch + 1 && pairing.gpu_count == 0 && pairing.trial
              && pairing.pending_pair.frame == 0 && pairing.pending_roles == 0 && pairing.measure_pair_first_frame == 0,
          "a changed paired identity clears costs and in-flight work while the FPS trial continues");
    pairing.ObserveGpu(old_completion, old_completion.completed_ns);
    check(pairing.gpu_count == 0 && pairing.pending_pair.frame == 0 && pairing.last_sequence == identity_sequence,
          "an old-identity completion cannot advance the current sampling sequence or seed a half pair");
    feed(14, 14, true, 2);
    feed(15, 14, true, 2);
    check(pairing.gpu_count == 1 && pairing.gpu_us[0] == 2000., "new-identity adjacent sources resume complete cost pairs");
    feed(16, 16, true, 2);
    old_completion = paired_sample(pairing, 17, 16, start + 170'000'000, 2000., true, 2);
    pairing.SetPairedSampling(false, 0xB2);
    pairing.ObserveGpu(old_completion, old_completion.completed_ns);
    check(pairing.gpu_count == 0 && !pairing.paired_sampling && pairing.paired_identity == 0,
          "leaving paired sampling zeroes its identity and rejects old-mode completions");
    auto ordinary = paired_sample(pairing, 18, 0, start + 180'000'000, 3000., true, 2);
    ordinary.paired = false;
    pairing.ObserveGpu(ordinary, ordinary.completed_ns);
    check(pairing.gpu_count == 1 && pairing.gpu_us[0] == 3000., "ordinary single-source sampling resumes after paired mode");
  }
  adaptive::Session dual;
  dual.prepared = true;
  dual.status.generation = 1;
  dual.applied_generation = 1;
  dual.applied_ns = dual.stable_after_ns = start;
  dual.ObserveGpu({.generation = 1, .frame = 4, .sequence = 2, .context = 1,
      .recorded_ns = start, .completed_ns = start, .total_us = 2000.,
      .role = 1, .required_roles = 3, .valid = true, .stable = true}, start);
  check(dual.gpu_count == 0, "a partial dual-color chain never becomes a GPU sample");
  dual.ObserveGpu({.generation = 1, .frame = 4, .sequence = 1, .context = 1,
      .recorded_ns = start, .completed_ns = start, .total_us = 1000.,
      .role = 2, .required_roles = 3, .valid = true, .stable = true}, start);
  check(dual.gpu_count == 1 && dual.gpu_us[0] == 3000., "out-of-order color completion produces one complete sum");

  const auto logical = adaptive::BuildLadder(upper, floor, .67f);
  for (uint32_t count = 2; count <= adaptive::MAX_PREPARED_LEVELS; ++count) {
    const auto selected = adaptive::BuildLadder(upper, floor, .67f, count);
    check(selected.count == std::min<size_t>(count, logical.count)
        && selected.sizes[0] == upper && selected.sizes[selected.count - 1] == logical.sizes[logical.count - 1],
        "selected counts retain both physical endpoints on the existing logical grid");
    for (size_t index = 1; index < selected.count; ++index) {
      check(selected.sizes[index] != selected.sizes[index - 1]
          && selected.coefficients[index] < selected.coefficients[index - 1],
          "selected tiers have distinct complete pass tuples in quality order");
    }
  }
  const auto three = adaptive::BuildLadder(upper, floor, .67f, 3);
  check(three.sizes[1] == logical.sizes[3], "three tiers use the existing uniform grid sampling rule");
  adaptive::Sizes tiny{}, tiny_floor{};
  tiny[0] = {.width = 2, .height = 2};
  tiny_floor[0] = {.width = 1, .height = 1};
  const auto rounded = adaptive::BuildLadder(tiny, tiny_floor, .33f, 4);
  check(rounded.count == 2 && rounded.sizes[0] == tiny && rounded.sizes[1] == tiny_floor,
      "rounding duplicates reduce the physical tier count without duplicate allocations");
  tiny[1] = {.width = 100, .height = 60};
  const auto multi_pass = adaptive::BuildLadder(tiny, tiny_floor, .67f, 4);
  check(multi_pass.count == 4 && multi_pass.sizes[0][0] == multi_pass.sizes[1][0]
      && multi_pass.sizes[0] != multi_pass.sizes[1],
      "deduplication compares the complete pass tuple rather than only the first pass");

  adaptive::Sizes full_upper{}, full_floor{};
  full_upper[0] = {.width = 3840, .height = 2160};
  full_floor[0] = {.width = 1267, .height = 713};
  const auto default_range = adaptive::BuildLadder(full_upper, full_floor, .67f);
  const auto full_range = adaptive::BuildLadder(full_upper, full_floor, .33f);
  check(default_range.count == 8 && full_range.count == 15 && full_range.count == adaptive::MAX_PREPARED_LEVELS
      && full_range.sizes[0] == full_upper && full_range.sizes[full_range.count - 1] == full_floor,
      "the unchanged five-percent grid provides eight default-range sizes and fifteen at the legal minimum");
  for (const uint32_t count : {9u, adaptive::MAX_PREPARED_LEVELS}) {
    adaptive::Settings extended_settings{
        .enabled = true, .minimum_scale = .33f, .prepared_levels = adaptive::MAX_PREPARED_LEVELS, .selected_levels = count};
    adaptive::Session extended;
    extended.Prepare(extended_settings, full_upper, full_floor, 1, {}, start, true, false, 70);
    check(extended.settings.selected_levels == count && extended.ladder.count == count
        && extended.ladder.sizes[0] == full_upper && extended.ladder.sizes[count - 1] == full_floor,
        "a nine- or fifteen-tier selection preserves its requested count and endpoints beyond the former eight-tier cap");
    for (size_t tier = 1; tier < extended.ladder.count; ++tier) {
      check(extended.ladder.sizes[tier] != extended.ladder.sizes[tier - 1]
          && extended.ladder.coefficients[tier] < extended.ladder.coefficients[tier - 1],
          "extended selections contain distinct complete tuples in quality order");
    }
    extended.UpdatePreparation(extended.preparation_epoch, count - 1, count, adaptive::Reason::PREPARING, start + 1);
    check(!extended.prepared && extended.status.ready_levels == count - 1
        && extended.status.prepared_levels == count,
        "every selected tier must complete before an extended collection opens the ready gate");
    extended.UpdatePreparation(extended.preparation_epoch, count, count, adaptive::Reason::PREPARING, start + 2);
    check(extended.prepared && extended.status.ready_levels == count,
        "complete extended readiness is counted without an eight-tier mask or truncation");

    adaptive::Session limited;
    extended_settings.prepared_levels = count - 1;
    limited.Prepare(extended_settings, full_upper, full_floor, 1, {}, start, true, false, 71);
    const auto limited_epoch = limited.preparation_epoch;
    limited.UpdatePreparation(limited_epoch, count, count, adaptive::Reason::PREPARING, start + 1);
    extended_settings.target_fps = 144.f;
    limited.Prepare(extended_settings, full_upper, full_floor, 2, {}, start + 2, true, false, 71);
    check(limited.settings.selected_levels == count && limited.ladder.count == count && !limited.prepared
        && limited.status.reason == adaptive::Reason::BUDGET && limited.preparation_epoch == limited_epoch,
        "insufficient runtime capacity preserves an extended request and FPS edits do not retry its budget");
  }

  adaptive::Settings settings{.enabled = true, .prepared_levels = 4, .selected_levels = 4};
  adaptive::Session gated;
  adaptive::Output gated_output{.source = 1, .generation = 1, .configuration = 1, .fps = 50.,
      .split_available = true, .split_fps = {50., 50.}};
  gated.Prepare(settings, upper, floor, 1, gated_output, start, true, false, 10);
  gated.Applied(gated.status.generation, upper, start);
  const auto gated_generation = gated.status.generation;
  const auto gated_epoch = gated.preparation_epoch;
  for (uint64_t index = 1; index <= 20; ++index) {
    const int64_t now = start + static_cast<int64_t>(index) * adaptive::SAMPLE_INTERVAL_NS;
    gated.ObserveGpu({.generation = gated.status.generation, .frame = index, .sequence = index,
        .context = 1, .recorded_ns = now, .completed_ns = now, .total_us = 3000.,
        .valid = true, .stable = true}, now);
    if ((now - start) % FpsTrace::WINDOW_NS == 0) {
      ++gated_output.serial;
      gated_output.available = true;
      gated_output.begin_ns = now - FpsTrace::WINDOW_NS;
      gated_output.end_ns = now;
    }
    gated.Prepare(settings, upper, floor, 1, gated_output, now, true, false, 10);
    gated.UpdatePreparation(gated_epoch, 3, 4, adaptive::Reason::PREPARING, now);
  }
  check(!gated.prepared && gated.level == 0 && gated.status.generation == gated_generation
      && gated.gpu_count == 0 && gated.status.reason == adaptive::Reason::PREPARING,
      "incomplete owner readiness cannot collect eligible costs or start an automatic adjustment");
  gated.UpdatePreparation(gated_epoch - 1, 4, 4, adaptive::Reason::PREPARING, start + 6'000'000'000);
  gated.UpdatePreparation(gated_epoch, 2, 2, adaptive::Reason::PREPARING, start + 6'000'000'000);
  check(!gated.prepared, "old epochs and a partial group's smaller required count cannot open the selected-set gate");
  const int64_t ready_ns = start + 6'000'000'000;
  gated.UpdatePreparation(gated_epoch, 4, 4, adaptive::Reason::PREPARING, ready_ns);
  check(gated.prepared && gated.applied_ns == 0 && gated.stable_after_ns == 0
      && gated.status.reason == adaptive::Reason::PREPARING,
      "complete owner readiness cannot confirm a size or consume its delivery warmup");
  gated.Applied(gated.status.generation, gated.status.requested, ready_ns);
  check(gated.applied_generation == gated.status.generation && gated.applied_ns == ready_ns
      && gated.stable_after_ns == ready_ns + adaptive::WARMUP_NS,
      "the first complete delivery starts the warmup after resource readiness");
  gated.Prepare(settings, upper, floor, 1, gated_output, ready_ns, true, false, 10);
  check(!gated.trial && gated.level == 0, "the completion observation itself does not start an automatic size request");
  gated.UpdatePreparation(gated_epoch, 3, 4, adaptive::Reason::PREPARING, ready_ns + 1);
  check(!gated.prepared && gated.gpu_count == 0 && gated.resource_identity == 10,
      "temporarily missing complete-tier evidence pauses measurement without changing allocation identity");

  adaptive::Session reused;
  settings.selected_levels = 0;
  reused.Prepare(settings, upper, floor, 1, {}, start, true, false, 20);
  const auto original = reused.ladder;
  const auto original_epoch = reused.preparation_epoch;
  check(reused.ladder.count == 4 && reused.settings.selected_levels == 0,
      "the default choice keeps the owner's existing resource-budget tier count");
  reused.UpdatePreparation(original_epoch, 4, 4, adaptive::Reason::PREPARING, start);
  reused.Request(3, start + 1);
  reused.Applied(reused.status.generation, reused.status.requested, start + 1);
  const auto lower = reused.status.applied;
  const auto generation = reused.status.generation;
  reused.gpu_count = 5;
  settings.target_fps = 116.f;
  reused.Prepare(settings, upper, floor, 2, {}, start + 2, true, false, 20);
  check(reused.ladder.sizes == original.sizes && reused.level == 3 && reused.status.requested == lower
      && reused.status.generation == generation && reused.gpu_count == 0
      && reused.applied_generation == generation && reused.applied_ns == start + 1
      && reused.last_switch_ns == start + 1 && reused.stable_after_ns == start + adaptive::WARMUP_NS + 1
      && reused.prepared && reused.preparation_epoch == original_epoch,
      "target FPS and measurement edits retain the active allocation set and only restart observation");
  for (int index = 0; index < 32; ++index) {
    reused.Prepare(settings, upper, floor, 2, {}, start + 3 + index, true, false, 20);
  }
  check(reused.preparation_epoch == original_epoch && reused.ladder.sizes == original.sizes,
      "unchanged per-frame capacity does not restart preparation");

  settings.selected_levels = 2;
  reused.Prepare(settings, upper, floor, 3, {}, start + 40, true, false, 20);
  check(reused.ladder.count == 2 && reused.ladder.sizes[0] == original.sizes[0]
      && reused.ladder.sizes[1] == original.sizes[3] && reused.level == 1 && reused.status.applied == lower
      && reused.status.requested == lower && !reused.prepared && reused.preparation_epoch > original_epoch
      && reused.resource_identity == 20,
      "a count edit remaps a retained complete size and asks the owner to revalidate the selected subset");
  const auto two_epoch = reused.preparation_epoch;
  reused.UpdatePreparation(original_epoch, 4, 4, adaptive::Reason::PREPARING, start + 41);
  check(!reused.prepared, "late readiness from the larger old collection cannot qualify the new subset");
  reused.UpdatePreparation(two_epoch, 2, 2, adaptive::Reason::PREPARING, start + 42);
  const auto previous_generation = reused.status.generation;
  settings.minimum_scale = .5f;
  reused.Prepare(settings, upper, floor, 4, {}, start + 43, true, false, 20);
  const auto lower_logical = adaptive::BuildLadder(upper, floor, .5f);
  check(reused.ladder.count == 2 && reused.ladder.sizes[0] == upper
      && reused.ladder.sizes[1] == lower_logical.sizes[lower_logical.count - 1]
      && reused.status.requested == upper && reused.status.applied == lower
      && !reused.prepared && reused.resource_identity == 20,
      "a minimum edit selects the exact new endpoint before automatic control without falsifying the applied size");
  reused.Applied(previous_generation, reused.minimum, start + 44);
  reused.UpdatePreparation(two_epoch, 2, 2, adaptive::Reason::PREPARING, start + 44);
  check(reused.status.applied == lower && !reused.prepared,
      "old size completions and old preparation readiness remain ineligible after a minimum edit");

  reused.UpdatePreparation(reused.preparation_epoch, 1, 2, adaptive::Reason::FAILED, start + 45);
  settings.target_fps = 120.f;
  reused.Prepare(settings, upper, floor, 5, {}, start + 46, true, false, 20);
  settings.selected_levels = 3;
  reused.Prepare(settings, upper, floor, 6, {}, start + 47, true, false, 20);
  reused.UpdatePreparation(reused.preparation_epoch, 3, 3, adaptive::Reason::PREPARING, start + 48);
  check(reused.failed && !reused.prepared && reused.status.reason == adaptive::Reason::FAILED,
      "execution failure remains latched across target FPS, measurement, count edits and late successful readiness");
  const auto failed_epoch = reused.preparation_epoch;
  settings.enabled = false;
  reused.Prepare(settings, upper, floor, 7, {}, start + 49, true, false, 20);
  const auto cancelled_epoch = reused.preparation_epoch;
  reused.UpdatePreparation(failed_epoch, 3, 3, adaptive::Reason::PREPARING, start + 50);
  check(reused.status.requested == upper && !reused.prepared
      && reused.status.reason == adaptive::Reason::DISABLED && cancelled_epoch > failed_epoch,
      "disabling cancels pending preparation and keeps the compatible manual upper bound");
  settings.enabled = true;
  reused.Prepare(settings, upper, floor, 8, {}, start + 51, true, false, 20);
  check(!reused.failed && !reused.prepared && reused.preparation_epoch > cancelled_epoch
      && reused.status.reason == adaptive::Reason::PREPARING,
      "explicit re-enabling begins one new preparation attempt");
  reused.Fail(reused.status.generation);
  reused.Prepare(settings, upper, floor, 9, {}, start + 52, true, false, 21);
  check(!reused.failed && reused.resource_identity == 21 && reused.level == 0 && !reused.prepared,
      "a genuinely new allocation contract clears the previous execution failure");

  adaptive::Session budget;
  settings = {.enabled = true, .prepared_levels = 2, .selected_levels = 4};
  budget.Prepare(settings, upper, floor, 1, {}, start, true, false, 30);
  const auto budget_epoch = budget.preparation_epoch;
  budget.UpdatePreparation(budget_epoch, 4, 4, adaptive::Reason::PREPARING, start + 1);
  check(budget.settings.selected_levels == 4 && budget.ladder.count == 4
      && !budget.prepared && budget.status.reason == adaptive::Reason::BUDGET,
      "insufficient capacity preserves the selected four-tier preference and pauses instead of silently reducing it");
  settings.target_fps = 144.f;
  budget.Prepare(settings, upper, floor, 2, {}, start + 2, true, false, 30);
  check(budget.preparation_epoch == budget_epoch && budget.preparation_reason == adaptive::Reason::BUDGET,
      "target FPS and measurement edits do not retry a rejected budget");
  settings.selected_levels = 2;
  budget.Prepare(settings, upper, floor, 3, {}, start + 3, true, false, 30);
  check(budget.preparation_epoch > budget_epoch && budget.ladder.count == 2 && !budget.prepared
      && budget.status.reason == adaptive::Reason::PREPARING,
      "explicitly reducing selected count allows a fresh bounded budget check");
  budget.UpdatePreparation(budget.preparation_epoch, 2, 2, adaptive::Reason::PREPARING, start + 4);
  check(budget.prepared, "the reduced collection resumes after complete owner readiness");

  adaptive::Session single;
  settings = {.enabled = true, .minimum_scale = 1.f, .prepared_levels = 1, .selected_levels = 4};
  single.Prepare(settings, upper, floor, 1, {}, start, true, false, 40);
  single.UpdatePreparation(single.preparation_epoch, 1, 1, adaptive::Reason::PREPARING, start);
  single.Applied(single.status.generation, upper, start);
  single.Prepare(settings, upper, floor, 1, {}, start + 1, true, false, 40);
  check(single.ladder.count == 1 && single.prepared && single.settings.selected_levels == 4
      && single.status.reason == adaptive::Reason::NO_RANGE,
      "one unique physical size needs no forced second allocation or automatic adjustment");

  adaptive::Session legacy;
  settings = {.enabled = true, .minimum_scale = .8f, .prepared_levels = 2, .legacy_preparation = true};
  legacy.Prepare(settings, upper, floor, 1, {}, start, true, false, 50);
  legacy.Request(1, start + 1);
  legacy.Applied(legacy.status.generation, legacy.status.requested, start + 1);
  const auto legacy_lower = legacy.ladder.sizes[1];
  settings.minimum_scale = .5f;
  legacy.Prepare(settings, upper, floor, 2, {}, start + 2, true, false, 50);
  check(legacy.ladder.sizes[1] == legacy_lower && legacy.defer_preparation
      && !legacy.RequestMinimum(start + 3) && legacy.level == 0 && legacy.active_level == 1,
      "the default-off experiment retains the legacy active-slot protection before an on-demand endpoint replacement");
  legacy.Applied(legacy.status.generation, upper, start + 4);
  check(legacy.RequestMinimum(start + 5) && legacy.level == 1 && legacy.ladder.sizes[1] == legacy.minimum,
      "the default-off experiment can still request its endpoint after the real upper tier is applied");

  adaptive::Session legacy_automatic;
  settings.minimum_scale = 1.f;
  settings.prepared_levels = 3;
  legacy_automatic.Prepare(settings, upper, floor, 1, {}, start, true, false, 60);
  legacy_automatic.Applied(legacy_automatic.status.generation, upper, start);
  settings.minimum_scale = .5f;
  adaptive::Output legacy_output{.source = 1, .generation = 1, .configuration = 1, .fps = 50.,
      .split_available = true, .split_fps = {50., 50.}};
  legacy_automatic.Prepare(settings, upper, floor, 2, legacy_output, start + 1, true, false, 60);
  legacy_automatic.Applied(legacy_automatic.status.generation, upper, start + 1);
  legacy_output.available = true;
  legacy_output.serial = 1;
  legacy_output.begin_ns = legacy_automatic.stable_after_ns;
  legacy_output.end_ns = legacy_output.begin_ns + FpsTrace::WINDOW_NS;
  legacy_automatic.Prepare(settings, upper, floor, 2, legacy_output, legacy_output.end_ns, true, false, 60);
  check(legacy_automatic.level == 1 && legacy_automatic.ladder.count == 2
      && legacy_automatic.status.requested == legacy_automatic.minimum && !legacy_automatic.prepared,
      "the default-off experiment still starts its automatic on-demand endpoint request without a full-set gate");

  {
    FpsTrace retargeted;
    retargeted.settings.minimum_scale = .8f;
    retargeted.settings.legacy_preparation = true;
    const adaptive::Sizes old_minimum{{{1536, 864}, {768, 432}}};
    retargeted.Start();
    retargeted.WarmUp();
    retargeted.Window(56.);
    retargeted.Ready();
    retargeted.Deliver();
    const auto old_delivery_ns = retargeted.now;
    check(retargeted.controller.status.applied == old_minimum && retargeted.controller.ladder.count == 2,
          "the two-slot strategy first reaches the delivered 80 percent endpoint");

    retargeted.settings.minimum_scale = .5f;
    retargeted.WarmUp();
    retargeted.Prepare();
    retargeted.Window(56.);
    const auto return_generation = retargeted.controller.status.generation;
    check(retargeted.controller.status.requested == retargeted.upper && retargeted.controller.status.applied == old_minimum
              && retargeted.controller.ladder.sizes[1] == old_minimum && retargeted.controller.applied_ns == old_delivery_ns,
          "a sustained deficit with the new minimum requests the upper tier while the old lower slot is still live");
    retargeted.Window(56.);
    check(retargeted.controller.status.generation == return_generation
              && retargeted.controller.ladder.sizes[1] == old_minimum,
          "continued output from the old low tier cannot retire its slot before the upper tier is delivered");
    retargeted.Ready();
    retargeted.Deliver();
    const auto upper_delivery_ns = retargeted.now;
    check(retargeted.controller.applied_ns == upper_delivery_ns
              && retargeted.controller.status.applied == retargeted.upper,
          "the supported retargeting path first delivers the upper tier");
    retargeted.now += FpsTrace::BUCKET_NS;
    retargeted.output.available = false;
    retargeted.Prepare();
    retargeted.Window(56.);
    check(retargeted.controller.status.generation == return_generation,
          "the first window crossing the upper tier's warmup cannot advance the pending minimum");
    retargeted.Window(56.);
    check(retargeted.controller.status.requested == retargeted.floor
              && retargeted.controller.status.applied == retargeted.upper
              && retargeted.controller.ladder.count == 2 && retargeted.controller.ladder.sizes[1] == retargeted.floor,
          "the first fresh deficit at the upper tier replaces the stale slot with 50 percent instead of retrying 80 percent");
    retargeted.Ready();
    retargeted.Deliver();
    retargeted.WarmUp();
    retargeted.Window(56.);
    check(retargeted.controller.status.applied == retargeted.floor
              && retargeted.controller.status.reason == adaptive::Reason::AT_MINIMUM,
          "only the delivered new minimum reports that the two-slot strategy is at its lower bound");
  }

  adaptive::Settings limited_settings{.enabled = true, .target_fps = 360.f, .minimum_scale = .33f,
      .prepared_levels = 2, .selected_levels = 15, .limited_residency = true};
  adaptive::Session limited;
  adaptive::Output limited_output{.source = 1, .generation = 1, .configuration = 1,
      .fps = 100., .observed_multiplier = 4., .split_available = true, .split_fps = {100., 100.}};
  limited.Prepare(limited_settings, full_upper, full_floor, 1, limited_output, start, true, false, 70);
  limited.UpdateTargetPreparation(limited.preparation_epoch, limited.status.generation, 1, 15,
      true, adaptive::Reason::PREPARING, start);
  limited.Applied(limited.status.generation, full_upper, start);
  check(limited.ladder.count == 15 && !limited.prepared && limited.TargetReady()
      && limited.status.ready_levels == 1 && limited.status.applied == full_upper,
      "limited residency retains the full logical ladder while reporting only the ready target");
  limited_output.available = true;
  limited_output.serial = 1;
  limited_output.begin_ns = limited.stable_after_ns;
  limited_output.end_ns = limited_output.begin_ns + FpsTrace::WINDOW_NS;
  limited.Prepare(limited_settings, full_upper, full_floor, 1, limited_output, limited_output.end_ns, true, false, 70);
  const auto cold_generation = limited.status.generation;
  const auto cold_target = limited.status.requested;
  const auto request_ns = limited_output.end_ns;
  for (uint64_t index = 1; index <= 12; ++index) {
    const int64_t now = request_ns + static_cast<int64_t>(index) * adaptive::SAMPLE_INTERVAL_NS;
    limited.ObserveGpu({.generation = cold_generation, .frame = index, .sequence = index,
        .context = 1, .recorded_ns = now, .completed_ns = now, .total_us = 100.,
        .valid = true, .stable = true}, now);
    if ((now - request_ns) % FpsTrace::WINDOW_NS == 0) {
      ++limited_output.serial;
      limited_output.begin_ns = now - FpsTrace::WINDOW_NS;
      limited_output.end_ns = now;
    }
    limited.Prepare(limited_settings, full_upper, full_floor, 1, limited_output, now, true, false, 70);
    limited.UpdateTargetPreparation(limited.preparation_epoch, cold_generation, 1, 15,
        false, adaptive::Reason::PREPARING, now);
    limited.Applied(cold_generation, cold_target, now);
  }
  check(limited.level == 1 && limited.status.requested == cold_target
      && limited.status.applied == full_upper && limited.active_level == 0 && limited.status.coefficient == 1.f
      && limited.applied_ns == start && limited.applied_generation != cold_generation
      && limited.gpu_count == 0 && !limited.TargetReady(),
      "a cold target keeps actual output and rejects cost samples from the not-yet-delivered generation");
  const int64_t target_ready_ns = start + 7'000'000'000;
  limited.UpdateTargetPreparation(limited.preparation_epoch, cold_generation - 1, 2, 15,
      true, adaptive::Reason::PREPARING, target_ready_ns);
  limited.UpdateTargetPreparation(limited.preparation_epoch, cold_generation, 1, 1,
      true, adaptive::Reason::PREPARING, target_ready_ns);
  check(!limited.TargetReady(), "stale targets and partial logical contracts cannot publish target readiness");
  limited.UpdateTargetPreparation(limited.preparation_epoch, cold_generation, 2, 15,
      true, adaptive::Reason::PREPARING, target_ready_ns);
  check(limited.TargetReady() && !limited.prepared && limited.applied_ns == start
      && limited.applied_generation != cold_generation,
      "a complete target receipt is distinct from actual application and full-ladder readiness");
  const int64_t applied_now = target_ready_ns + adaptive::WARMUP_NS;
  limited.Applied(cold_generation, cold_target, applied_now);
  check(limited.active_level == 1 && limited.status.applied == cold_target
      && limited.stable_after_ns == applied_now + adaptive::WARMUP_NS && limited.last_switch_ns == applied_now,
      "cold waiting does not consume the actual application's warmup or minimum switch spacing");
  limited.Request(2, applied_now + 1);
  const auto superseded = limited.status.generation;
  limited.Request(0, applied_now + 2);
  limited.UpdateTargetPreparation(limited.preparation_epoch, superseded, 3, 15,
      true, adaptive::Reason::PREPARING, applied_now + 3);
  limited.Applied(superseded, limited.ladder.sizes[2], applied_now + 3);
  check(!limited.TargetReady() && limited.status.applied == cold_target && limited.active_level == 1,
      "a superseded cold completion cannot apply after a reverse request");
  limited.UpdateTargetPreparation(limited.preparation_epoch, limited.status.generation, 2, 15,
      true, adaptive::Reason::PREPARING, applied_now + 4);
  limited.Applied(limited.status.generation, full_upper, applied_now + 5);
  check(limited.status.applied == full_upper && limited.active_level == 0 && !limited.prepared,
      "a reverse request can reuse its ready rollback tier without claiming the full ladder is resident");
  limited.Request(3, applied_now + 6);
  limited.UpdateTargetPreparation(limited.preparation_epoch, limited.status.generation, 2, 15,
      false, adaptive::Reason::BUDGET, applied_now + 7);
  check(limited.status.reason == adaptive::Reason::BUDGET && limited.level == 3
      && limited.status.applied == full_upper && !limited.TargetReady(),
      "capacity failure leaves actual fallback ownership to the runtime without inventing an applied upper");
  auto disabled_limited = limited;
  disabled_limited.status.applied = cold_target;
  disabled_limited.active_level = 1;
  auto disabled_settings = limited_settings;
  disabled_settings.enabled = false;
  disabled_limited.Prepare(disabled_settings, full_upper, full_floor, 2, limited_output,
      applied_now + 8, true, false, 70);
  disabled_limited.Applied(disabled_limited.status.generation, full_upper, applied_now + 9);
  check(disabled_limited.status.reason == adaptive::Reason::DISABLED
      && disabled_limited.status.applied == full_upper && disabled_limited.active_level == 0,
      "disabling automatic sizing records the successful return to its retained manual upper");

  adaptive::Session mode_switch;
  auto mode_settings = limited_settings;
  mode_settings.selected_levels = mode_settings.prepared_levels = 15;
  mode_settings.limited_residency = false;
  mode_switch.Prepare(mode_settings, full_upper, full_floor, 1, limited_output, start, true, false, 72);
  mode_switch.UpdatePreparation(mode_switch.preparation_epoch, 15, 15, adaptive::Reason::PREPARING, start);
  mode_switch.Applied(mode_switch.status.generation, full_upper, start);
  mode_switch.UpdatePreparation(mode_switch.preparation_epoch, 1, 15, adaptive::Reason::BUDGET, start + 1);
  const auto denied_epoch = mode_switch.preparation_epoch;
  mode_settings.limited_residency = true;
  mode_switch.Prepare(mode_settings, full_upper, full_floor, 1, limited_output, start + 2, true, false, 72);
  check(mode_switch.preparation_epoch == denied_epoch + 1 && mode_switch.ladder.count == 15
      && mode_switch.preparation_reason == adaptive::Reason::PREPARING && !mode_switch.failed
      && !mode_switch.TargetReady() && mode_switch.status.applied == full_upper,
      "changing residency mode explicitly retries a denied budget while preserving all logical levels and actual output");
  mode_switch.UpdateTargetPreparation(mode_switch.preparation_epoch, mode_switch.status.generation, 1, 15,
      true, adaptive::Reason::PREPARING, start + 3);
  mode_switch.Applied(mode_switch.status.generation, full_upper, start + 3);
  mode_settings.limited_residency = false;
  mode_switch.Prepare(mode_settings, full_upper, full_floor, 1, limited_output, start + 4, true, false, 72);
  mode_switch.UpdatePreparation(mode_switch.preparation_epoch, 1, 15, adaptive::Reason::PREPARING, start + 5);
  check(!mode_switch.prepared && !mode_switch.TargetReady() && mode_switch.ladder.count == 15,
      "returning to full preparation closes automatic decisions until the complete logical collection is ready");
  mode_switch.UpdatePreparation(mode_switch.preparation_epoch, 15, 15, adaptive::Reason::PREPARING, start + 6);
  check(mode_switch.prepared && mode_switch.TargetReady(), "full preparation reopens only after the complete collection receipt");
  mode_switch.Fail(mode_switch.status.generation);
  mode_settings.limited_residency = true;
  mode_switch.Prepare(mode_settings, full_upper, full_floor, 1, limited_output, start + 7, true, false, 72);
  check(mode_switch.failed && mode_switch.status.reason == adaptive::Reason::FAILED && !mode_switch.TargetReady(),
      "a mode edit does not retry an actual execution failure through the budget recovery path");

  for (const bool limited_residency : {false, true}) {
    adaptive::Session delivered;
    const adaptive::Settings delivery_settings{.enabled = true, .target_fps = 100.f, .minimum_scale = .5f,
        .prepared_levels = 2, .selected_levels = 2, .limited_residency = limited_residency};
    adaptive::Output observed{.source = 1, .generation = 1, .configuration = 1, .fps = 80.,
        .split_available = true, .split_fps = {80., 80.}};
    delivered.Prepare(delivery_settings, full_upper, full_floor, 1, observed, start, true, false, 80);
    const auto ready = [&](int64_t now) {
      if (limited_residency) {
        delivered.UpdateTargetPreparation(delivered.preparation_epoch, delivered.status.generation, 2, 2,
            true, adaptive::Reason::PREPARING, now);
      } else {
        delivered.UpdatePreparation(delivered.preparation_epoch, 2, 2, adaptive::Reason::PREPARING, now);
      }
    };
    ready(start);
    check(delivered.TargetReady() && delivered.applied_ns == 0 && delivered.last_switch_ns == 0,
        "resource readiness cannot start the first delivery clock in either residency mode");
    delivered.Prepare(delivery_settings, full_upper, full_floor, 1, observed, start, true, false, 80);
    delivered.Applied(delivered.status.generation, full_upper, start);
    const auto accepted_generation = delivered.applied_generation;
    const auto accepted_stability = delivered.stable_after_ns;
    observed.available = true;
    observed.serial = 1;
    observed.begin_ns = accepted_stability;
    observed.end_ns = observed.begin_ns + FpsTrace::WINDOW_NS;
    delivered.Prepare(delivery_settings, full_upper, full_floor, 1, observed, observed.end_ns, true, false, 80);
    const auto rejected_generation = delivered.status.generation;
    const auto rejected_sizes = delivered.status.requested;
    check(rejected_generation != accepted_generation && rejected_sizes == delivered.ladder.sizes[1],
        "a complete new deficit requests the candidate before its target frame is delivered");
    ready(start + 5'000'000'000);
    check(delivered.TargetReady() && delivered.status.applied == full_upper && delivered.status.coefficient == 1.f
        && delivered.applied_generation == accepted_generation && delivered.applied_ns == start
        && delivered.last_switch_ns == start && delivered.stable_after_ns == accepted_stability,
        "request and ready preserve the last delivered size, coefficient, switch time and warmup");
    delivered.ObserveGpu({.generation = rejected_generation, .frame = 1, .sequence = 1, .context = 1,
        .recorded_ns = start + 5'000'000'000, .completed_ns = start + 5'000'000'000,
        .total_us = 10., .valid = true, .stable = true,
        .sampling_generation = delivered.sampling_generation}, start + 5'000'000'000);
    check(delivered.gpu_count == 0, "a completed candidate cannot contribute costs before its successful delivery");
    observed.available = false;
    delivered.Prepare(delivery_settings, full_upper, full_floor, 1, observed,
        start + 6'000'000'000, true, false, 80);
    check(!delivered.trial && delivered.level == 0 && delivered.status.requested == full_upper
        && delivered.status.applied == full_upper && delivered.applied_ns == start
        && delivered.status.reason == adaptive::Reason::OUTPUT_UNAVAILABLE,
        "losing the output withdraws the pending request and preserves the confirmed tier");
    delivered.Applied(rejected_generation, rejected_sizes, start + 7'000'000'000);
    check(delivered.applied_generation == accepted_generation && delivered.applied_ns == start,
        "late delivery of the withdrawn trial cannot confirm its old request");
    const auto resumed_ns = start + 8'000'000'000;
    ready(resumed_ns);
    delivered.Applied(delivered.status.generation, full_upper, resumed_ns);
    delivered.Applied(delivered.status.generation, full_upper, resumed_ns + 1'000'000'000);
    check(delivered.applied_generation == delivered.status.generation && delivered.applied_ns == resumed_ns
        && delivered.last_switch_ns == resumed_ns && delivered.stable_after_ns == resumed_ns + adaptive::WARMUP_NS,
        "the recovered request confirms once and repeated consumption cannot restart its warmup");
  }
  return passed;
}
}  // namespace adaptive_resolution_cases
