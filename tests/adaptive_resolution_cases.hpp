/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "../adaptive_resolution.hpp"

namespace adaptive_resolution_cases {
namespace adaptive = renodx::addons::dlss5::adaptive_resolution;

inline bool Run() {
  bool passed = true;
  const auto check = [&](bool condition, const char* name) {
    if (!condition) {
      std::cerr << "adaptive-resolution: " << name << '\n';
      passed = false;
    }
  };
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
  session = {};
  adaptive::Output output{.available = true, .source = 1, .generation = 1, .configuration = 1};
  uint64_t sequence = 0;
  constexpr int64_t start = 1'000'000'000;
  const auto tick = [&](int64_t now, double fps, double gpu_us) {
    session.ObserveGpu({.generation = session.status.generation, .frame = ++sequence,
        .sequence = sequence, .context = 1, .recorded_ns = now, .completed_ns = now,
        .total_us = gpu_us, .valid = true, .stable = true,
        .sampling_generation = session.sampling_generation}, now);
    output.serial++;
    output.begin_ns = now - 2'000'000'000;
    output.end_ns = now;
    output.fps = fps;
    session.Prepare({.enabled = true, .prepared_levels = 8}, upper, floor, 1, output, now, true, false);
    session.UpdatePreparation(session.preparation_epoch, static_cast<uint32_t>(session.ladder.count),
        static_cast<uint32_t>(session.ladder.count), adaptive::Reason::PREPARING, now);
    session.Applied(session.status.generation, session.status.requested, now);
  };
  tick(start, 50., 3000.);
  for (int i = 1; i <= 13; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(session.level == 1 && session.trial, "sustained low FPS probes the next prepared size without a prior NR cost increase");
  const auto lower_generation = session.status.generation;
  {
    auto frozen_trial = session;
    const auto lower_sizes = frozen_trial.status.applied;
    const auto lower_applied_ns = frozen_trial.applied_ns;
    const auto freeze_ns = output.end_ns + adaptive::SAMPLE_INTERVAL_NS;
    const auto thaw_ns = freeze_ns + 4 * adaptive::SAMPLE_INTERVAL_NS;
    const auto prepare = [&](adaptive::Session* controller, int64_t now, bool supported, bool frozen,
                             uint64_t measurement_identity = 1) {
      controller->Prepare(controller->settings, upper, floor, measurement_identity, output, now, supported, frozen, 1);
    };
    frozen_trial.ObserveMaskSettings(1, 0, 1, freeze_ns - 1);
    prepare(&frozen_trial, freeze_ns, true, true);
    prepare(&frozen_trial, thaw_ns - adaptive::SAMPLE_INTERVAL_NS, true, true, 2);
    frozen_trial.ObserveMaskSettings(1, 0, 2, thaw_ns - 2);
    frozen_trial.SetPairedSampling(true, 1, thaw_ns - 1);
    check(lower_sizes != upper && frozen_trial.status.reason == adaptive::Reason::FROZEN
              && frozen_trial.status.requested == lower_sizes && frozen_trial.status.applied == lower_sizes
              && frozen_trial.status.generation == lower_generation && frozen_trial.applied_generation == lower_generation
              && frozen_trial.applied_ns == lower_applied_ns && frozen_trial.trial && frozen_trial.previous_level == 0,
          "comparison holds the delivered trial size and generation while retaining its accepted anchor");
    const auto frozen_sampling_generation = frozen_trial.sampling_generation;
    prepare(&frozen_trial, thaw_ns, true, false);
    check(!frozen_trial.trial && frozen_trial.status.requested == upper && frozen_trial.status.applied == lower_sizes
              && frozen_trial.applied_generation == lower_generation && frozen_trial.gpu_count == 0
              && frozen_trial.sampling_generation > frozen_sampling_generation && frozen_trial.stable_after_ns >= thaw_ns,
          "thaw requests the accepted anchor and starts a fresh measurement window");

    auto pending_trial = session;
    pending_trial.Request(2, freeze_ns - 1);
    const auto pending_generation = pending_trial.status.generation;
    const auto pending_sizes = pending_trial.status.requested;
    prepare(&pending_trial, freeze_ns, true, true);
    const auto held_generation = pending_trial.status.generation;
    pending_trial.Applied(pending_generation, pending_sizes, freeze_ns + 1);
    prepare(&pending_trial, freeze_ns + 2, true, true);
    check(pending_sizes != lower_sizes && pending_trial.status.reason == adaptive::Reason::FROZEN
              && pending_trial.status.requested == lower_sizes && pending_trial.status.applied == lower_sizes
              && pending_trial.applied_generation == lower_generation && held_generation != pending_generation
              && pending_trial.status.generation == held_generation,
          "comparison holds the last delivery during a cumulative trial and rejects the withdrawn pending tier");
    prepare(&pending_trial, thaw_ns, true, false);
    check(!pending_trial.trial && pending_trial.status.requested == upper
              && pending_trial.status.generation > held_generation && pending_trial.stable_after_ns >= thaw_ns,
          "thaw restores the accepted anchor after a cumulative trial was interrupted");

    auto unsupported = session;
    prepare(&unsupported, freeze_ns, false, true);
    check(unsupported.status.reason == adaptive::Reason::UNSUPPORTED && unsupported.status.requested == upper
              && !unsupported.trial,
          "comparison preserves the unsupported path's upper-bound fallback");
  }
  for (int i = 14; i <= 100; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 2400.);
  check(session.level == 0 && session.blocked && !session.trial,
        "a bounded descent without output FPS benefit restores quality and stops probing");
  for (int i = 101; i <= 140; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(session.level == 0 && session.blocked, "a steady cap does not trigger downscaling during the cooldown");
  const size_t samples = session.gpu_count;
  session.ObserveGpu({.generation = lower_generation, .frame = ++sequence, .sequence = sequence,
      .recorded_ns = start, .completed_ns = start, .total_us = 1., .valid = true, .stable = true},
      start + 20'000'000'000);
  check(session.gpu_count == samples, "old generation completions cannot drive the new size");

  const int64_t blocked_deadline = session.blocked_until_ns;
  int retry_step = 141;
  for (; start + retry_step * adaptive::SAMPLE_INTERVAL_NS < blocked_deadline; ++retry_step) {
    tick(start + retry_step * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  }
  check(session.blocked && !session.trial && session.level == 0,
        "an unchanged workload remains at the accepted anchor until the bounded cooldown ends");
  const int64_t retry_ns = start + retry_step * adaptive::SAMPLE_INTERVAL_NS;
  tick(retry_ns, 50., 3000.);
  check(!session.blocked && !session.trial && session.gpu_count == 0 && session.stable_after_ns == retry_ns,
        "cooldown expiry discards prior measurements instead of immediately retrying with old windows");
  for (int i = 1; i < 8; ++i) tick(retry_ns + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(!session.trial && session.level == 0, "pre-expiry output windows cannot launch the retry");
  tick(retry_ns + 8 * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(session.trial && session.level == 1,
        "a fresh output and GPU window reopens a bounded trial after the cooldown");

  session = {};
  sequence = 0;
  for (int i = 0; i <= 13; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  for (int i = 14; i <= 27; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 60., 2400.);
  check(session.level == 1 && !session.trial, "useful lower resolution is accepted");
  for (int i = 28; i <= 54; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 60., 2400.);
  check(session.level == 0 && session.trial && session.raising,
        "quality recovers at a locked target after the conservative hold");
  for (int i = 55; i <= 68; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(session.level == 1 && !session.trial && session.failed_raise_until_ns != 0,
        "unsuccessful quality recovery rolls back with a boundary cooldown");
  session.Prepare({.enabled = false}, upper, floor, 1, output, start + 20'000'000'000, true, false);
  check(session.level == 0 && session.status.requested == upper
      && session.status.reason == adaptive::Reason::DISABLED, "disabling restores the manual upper bound");

  session = {};
  sequence = 0;
  output.observed_multiplier = 2.;
  for (int i = 0; i <= 13; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  output.observed_multiplier = 4.;
  for (int i = 14; i <= 27; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 100., 2900.);
  check(session.level == 1 && session.trial && session.extra_window,
        "an FG ratio increase gets one extra window instead of accepting an apparent NR benefit");
  for (int i = 28; i <= 38; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 100., 2900.);
  check(session.level == 0 && session.blocked && !session.trial,
        "a sustained FG ratio change restores the original quality after the bounded comparison");
  output.observed_multiplier = 0.;

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
              && pairing.sampling_generation == identity_epoch + 1 && pairing.gpu_count == 0 && !pairing.trial
              && pairing.pending_pair.frame == 0 && pairing.pending_roles == 0 && pairing.measure_pair_first_frame == 0,
          "a changed paired identity clears costs and in-flight work and cancels the old trial");
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
  for (const bool fps_benefit : {true, false}) {
    adaptive::Sizes paired_upper{}, paired_floor{};
    paired_upper[0] = {.width = 3840, .height = 2160};
    paired_floor[0] = {.width = 2880, .height = 1620};
    const adaptive::Settings paired_settings{.enabled = true, .target_fps = 60.f, .minimum_scale = .75f,
        .prepared_levels = 2, .selected_levels = 2};
    adaptive::Session controller;
    adaptive::Output paired_output{.available = true, .source = 1, .generation = 1, .configuration = 1};
    controller.Prepare(paired_settings, paired_upper, paired_floor, 1, paired_output, start, true, false);
    controller.UpdatePreparation(controller.preparation_epoch, 2, 2, adaptive::Reason::PREPARING, start);
    controller.Applied(controller.status.generation, controller.status.requested, start);
    controller.SetPairedSampling(true);
    const uint32_t steps = (fps_benefit ? 40u : 72u);
    uint32_t changes = 0;
    for (uint32_t step = 1; step <= steps; ++step) {
      const auto now = start + step * adaptive::SAMPLE_INTERVAL_NS;
      const auto next = now + 8'333'333;
      const uint64_t first = static_cast<uint64_t>(step) * 30 + 1;
      const double refresh = (controller.level == 0 ? 5150. : paired_refresh_us);
      const double reuse = (controller.level == 0 ? 737. : paired_reuse_us);
      const auto selected = controller.SelectMeasurementPair(first, true);
      controller.ObserveGpu(paired_sample(controller, first, selected, now, step % 2 == 0 ? refresh : reuse), now);
      const auto adjacent = controller.SelectMeasurementPair(first + 1, false);
      controller.ObserveGpu(paired_sample(controller, first + 1, adjacent, next, step % 2 == 0 ? reuse : refresh), next);
      ++paired_output.serial;
      paired_output.begin_ns = next - 2'000'000'000;
      paired_output.end_ns = next;
      paired_output.fps = (controller.level == 1 && fps_benefit ? 60. : 50.);
      const auto previous = controller.level;
      controller.Prepare(paired_settings, paired_upper, paired_floor, 1, paired_output, next, true, false);
      controller.UpdatePreparation(controller.preparation_epoch, 2, 2, adaptive::Reason::PREPARING, next);
      controller.Applied(controller.status.generation, controller.status.requested, next);
      changes += previous != controller.level;
    }
    check(fps_benefit ? controller.level == 1 && !controller.trial && !controller.blocked && changes == 1
                      : controller.level == 0 && !controller.trial && controller.blocked && changes == 2,
          fps_benefit ? "phase-independent paired costs accept a useful lower tier under the unchanged FPS policy"
                      : "an unchanged FPS cap rolls back once and changing pair phase cannot reopen its cooldown");
  }

  for (const bool benefit : {true, false}) {
    adaptive::Sizes transition_upper{}, transition_floor{};
    transition_upper[0] = {.width = 3840, .height = 2160};
    transition_floor[0] = {.width = 2880, .height = 1620};
    const adaptive::Settings transition_settings{.enabled = true, .target_fps = 100.f, .minimum_scale = .75f,
        .prepared_levels = 2, .selected_levels = 2};
    adaptive::Output transition_output{.available = true, .source = 1, .generation = 1, .configuration = 1};
    adaptive::Session controller;
    controller.Prepare(transition_settings, transition_upper, transition_floor, 1, transition_output, start, true, false);
    controller.UpdatePreparation(controller.preparation_epoch, 2, 2, adaptive::Reason::PREPARING, start);
    controller.Applied(controller.status.generation, controller.status.requested, start);
    controller.SetPairedSampling(true, 0, start);
    uint64_t source = 0;
    int64_t now = start;
    const auto prepare = [&](double fps) {
      transition_output.serial++;
      transition_output.begin_ns = now - adaptive::WARMUP_NS;
      transition_output.end_ns = now;
      transition_output.fps = fps;
      controller.Prepare(transition_settings, transition_upper, transition_floor, 1, transition_output, now, true, false);
    };
    for (uint32_t tick = 0; tick < 24 && !controller.trial; ++tick) {
      now += adaptive::SAMPLE_INTERVAL_NS;
      const auto first = ++source;
      controller.ObserveGpu(paired_sample(controller, first, first, now, 3000.), now);
      now += 8'333'333;
      controller.ObserveGpu(paired_sample(controller, ++source, first, now, 1000.), now);
      prepare(80.);
    }
    check(controller.trial && controller.level == 1 && controller.previous_level == 0
              && controller.baseline_fps == 80. && controller.baseline_gpu_us == 2000.
              && controller.status.applied == transition_upper,
          "a complete delivered x2 baseline requests the per-frame lower tier without confirming its delivery");
    const auto generation = controller.status.generation;
    const auto old_sampling = controller.sampling_generation;
    const auto late_source = ++source;
    auto late_pair = paired_sample(controller, late_source, late_source - 1, now + 1, 1000.);
    controller.SetPairedSampling(false, 0, now);
    check(controller.trial && controller.level == 1 && controller.previous_level == 0
              && controller.status.generation == generation && controller.baseline_gpu_us == 2000.
              && controller.baseline_fps == 80. && controller.sampling_generation > old_sampling,
          "paired-to-per-frame timing preserves the same trial and its frozen per-base-frame benefit anchor");
    controller.ObserveGpu(late_pair, late_pair.completed_ns);
    check(controller.gpu_count == 0 && controller.pending_pair.frame == 0,
          "a pre-transition completion cannot enter the lower-tier timing window");
    const auto before_delivery = controller.applied_ns;
    now += 10'000'000;
    controller.Applied(generation, transition_floor, now);
    check(controller.applied_ns == now && controller.applied_ns > before_delivery
              && controller.stable_after_ns == now + adaptive::WARMUP_NS && controller.trial,
          "only the lower tier's first actual delivery starts its warmup after the timing transition");
    {
      auto missing_cost = controller;
      auto new_output = transition_output;
      const auto deadline = missing_cost.stable_after_ns + adaptive::SAMPLE_MAX_AGE_NS;
      new_output.serial++;
      new_output.begin_ns = missing_cost.stable_after_ns;
      new_output.end_ns = deadline;
      missing_cost.Prepare(transition_settings, transition_upper, transition_floor, 1, new_output,
          deadline - 1, true, false);
      check(missing_cost.trial && missing_cost.level == 1,
          "missing lower-tier completion waits through the bounded cost collection interval");
      missing_cost.Prepare(transition_settings, transition_upper, transition_floor, 1, new_output,
          deadline, true, false);
      const auto rollback_generation = missing_cost.status.generation;
      check(!missing_cost.trial && missing_cost.level == 0 && missing_cost.status.applied == transition_floor
                && missing_cost.status.requested == transition_upper,
          "missing cost at expiry requests the accepted anchor while retaining the last real delivery");
      missing_cost.Prepare(transition_settings, transition_upper, transition_floor, 1, new_output,
          deadline + 1, true, false);
      check(missing_cost.status.generation == rollback_generation && !missing_cost.trial,
          "a persistent completion gap cannot repeatedly request the same rollback");
    }
    for (uint32_t tick = 0; tick < 24 && controller.trial; ++tick) {
      now += adaptive::SAMPLE_INTERVAL_NS;
      controller.ObserveGpu({.generation = controller.status.generation, .frame = ++source, .sequence = source,
          .context = 1, .recorded_ns = now, .completed_ns = now, .total_us = 1500., .valid = true, .stable = true,
          .sampling_generation = controller.sampling_generation}, now);
      prepare(benefit ? 100. : 80.);
    }
    check(benefit ? controller.level == 1 && !controller.trial && !controller.blocked
                  : controller.level == 0 && !controller.trial && controller.blocked,
          benefit ? "fresh lower-tier per-frame cost and output accept the original cross-mode trial"
                  : "fresh lower-tier cost without output benefit returns the original trial to its anchor once");
  }
  if (passed) {
    std::cout << "NR_RELEASE_PARITY dx12-upscaled-adaptive-x2 controller paired_mean_us=2000 lower_us=1500 preserved_trial=1 accepted=1 rollback=1 bounded_missing_cost=1 evidence=CPU\n";
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
  for (const uint32_t count : {9u, 4u}) {
    const adaptive::Settings curve_settings{.enabled = true, .target_fps = 360.f, .minimum_scale = .33f,
        .prepared_levels = count, .selected_levels = count};
    adaptive::Session cumulative;
    adaptive::Output measured{.available = true, .source = 1, .generation = 1, .configuration = 1,
        .observed_multiplier = 4.};
    const auto advance = [&](adaptive::Session* controller, adaptive::Output* observation,
                             uint64_t step, double fps, double gpu_us) {
      const int64_t now = start + static_cast<int64_t>(step) * adaptive::SAMPLE_INTERVAL_NS;
      const uint64_t sequence = ++observation->serial;
      controller->ObserveGpu({.generation = controller->status.generation, .frame = sequence,
          .sequence = sequence, .context = 1, .recorded_ns = now, .completed_ns = now,
          .total_us = gpu_us, .valid = true, .stable = true,
          .sampling_generation = controller->sampling_generation}, now);
      observation->begin_ns = now - 2'000'000'000;
      observation->end_ns = now;
      observation->fps = fps;
      controller->Prepare(curve_settings, full_upper, full_floor, 1, *observation, now, true, false);
      controller->UpdatePreparation(controller->preparation_epoch, static_cast<uint32_t>(controller->ladder.count),
          static_cast<uint32_t>(controller->ladder.count), adaptive::Reason::PREPARING, now);
      controller->Applied(controller->status.generation, controller->status.requested, now);
    };
    bool retained_anchor = false, checked_boundaries = false;
    size_t deepest = 0;
    for (uint64_t step = 0; step <= 480; ++step) {
      const double coefficient = (cumulative.ladder.count == 0 ? 1. : cumulative.ladder.coefficients[cumulative.level]);
      advance(&cumulative, &measured, step, 100. + 30. * (1. - coefficient), 4000. * coefficient);
      deepest = std::max(deepest, cumulative.level);
      if (count == 9 && cumulative.trial && cumulative.level > 1 && cumulative.previous_level == 0) {
        retained_anchor |= cumulative.baseline_fps == 100. && cumulative.baseline_gpu_us == 4000.;
        if (!checked_boundaries) {
          checked_boundaries = true;
          auto worsening = cumulative;
          auto worse_output = measured;
          for (uint64_t offset = 1; offset <= 13; ++offset) {
            advance(&worsening, &worse_output, step + offset, 80., 1000.);
          }
          check(worsening.level == 0 && worsening.blocked && !worsening.trial,
                "a clearly worse cumulative trial returns to the accepted anchor without descending further");
          auto changed = cumulative;
          auto changed_output = measured;
          ++changed_output.configuration;
          changed.Prepare(curve_settings, full_upper, full_floor, 1, changed_output,
              measured.end_ns + 1, true, false);
          check(changed.level == 0 && !changed.trial && changed.gpu_count == 0,
                "an output contract change abandons the cumulative trial and its old measurements");
          auto unready = cumulative;
          unready.UpdatePreparation(unready.preparation_epoch, count - 1, count,
              adaptive::Reason::PREPARING, measured.end_ns + 1);
          const auto unready_generation = unready.status.generation;
          unready.Prepare(curve_settings, full_upper, full_floor, 1, measured,
              measured.end_ns + 2, true, false);
          check(!unready.prepared && !unready.trial && unready.status.generation == unready_generation
              && unready.status.reason == adaptive::Reason::PREPARING,
                "partial readiness cannot advance a cumulative trial into another tier");
        }
      }
    }
    check(deepest == cumulative.ladder.count - 1 && cumulative.status.coefficient <= .4001f,
          "the recorded fine/coarse synthetic curve explores the low end instead of sticking at 95 percent");
    if (count == 9) {
      check(retained_anchor && checked_boundaries,
            "two individually small steps compare their cumulative benefit with the same accepted anchor");
    } else {
      check(cumulative.level == cumulative.ladder.count - 1 && !cumulative.blocked,
            "the existing coarse-step curve still accepts its useful exact minimum");
    }
  }
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
  adaptive::Output gated_output{.available = true, .source = 1, .generation = 1, .configuration = 1, .fps = 50.};
  gated.Prepare(settings, upper, floor, 1, gated_output, start, true, false, 10);
  gated.Applied(gated.status.generation, upper, start);
  const auto gated_generation = gated.status.generation;
  const auto gated_epoch = gated.preparation_epoch;
  for (uint64_t index = 1; index <= 20; ++index) {
    const int64_t now = start + static_cast<int64_t>(index) * adaptive::SAMPLE_INTERVAL_NS;
    gated.ObserveGpu({.generation = gated.status.generation, .frame = index, .sequence = index,
        .context = 1, .recorded_ns = now, .completed_ns = now, .total_us = 3000.,
        .valid = true, .stable = true}, now);
    gated_output.serial++;
    gated_output.begin_ns = now - 2'000'000'000;
    gated_output.end_ns = now;
    gated.Prepare(settings, upper, floor, 1, gated_output, now, true, false, 10);
    gated.UpdatePreparation(gated_epoch, 3, 4, adaptive::Reason::PREPARING, now);
  }
  check(!gated.prepared && gated.level == 0 && gated.status.generation == gated_generation
      && gated.gpu_count == 0 && gated.status.reason == adaptive::Reason::PREPARING,
      "incomplete owner readiness cannot collect benefit samples or start an automatic trial");
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
  reused.blocked = true;
  settings.target_fps = 116.f;
  reused.Prepare(settings, upper, floor, 2, {}, start + 2, true, false, 20);
  check(reused.ladder.sizes == original.sizes && reused.level == 3 && reused.status.requested == lower
      && reused.status.generation == generation && reused.gpu_count == 0 && !reused.blocked
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
  adaptive::Output legacy_output{.available = true, .source = 1, .generation = 1, .configuration = 1, .fps = 50.};
  legacy_automatic.Prepare(settings, upper, floor, 2, legacy_output, start + 1, true, false, 60);
  legacy_automatic.Applied(legacy_automatic.status.generation, upper, start + 1);
  for (uint64_t index = 1; index <= 14; ++index) {
    const int64_t now = start + static_cast<int64_t>(index) * adaptive::SAMPLE_INTERVAL_NS;
    legacy_automatic.ObserveGpu({.generation = legacy_automatic.status.generation, .frame = index, .sequence = index,
        .context = 1, .recorded_ns = now, .completed_ns = now, .total_us = 3000.,
        .valid = true, .stable = true, .sampling_generation = legacy_automatic.sampling_generation}, now);
    ++legacy_output.serial;
    legacy_output.begin_ns = now - 2'000'000'000;
    legacy_output.end_ns = now;
    legacy_automatic.Prepare(settings, upper, floor, 2, legacy_output, now, true, false, 60);
    legacy_automatic.Applied(legacy_automatic.status.generation, legacy_automatic.status.requested, now);
  }
  check(legacy_automatic.trial && legacy_automatic.level == 1 && legacy_automatic.ladder.count == 2
      && legacy_automatic.status.requested == legacy_automatic.minimum && !legacy_automatic.prepared,
      "the default-off experiment still starts its automatic on-demand endpoint request without a full-set gate");

  adaptive::Settings limited_settings{.enabled = true, .target_fps = 360.f, .minimum_scale = .33f,
      .prepared_levels = 2, .selected_levels = 15, .limited_residency = true};
  adaptive::Session limited;
  adaptive::Output limited_output{.available = true, .source = 1, .generation = 1, .configuration = 1,
      .fps = 100., .observed_multiplier = 4.};
  limited.Prepare(limited_settings, full_upper, full_floor, 1, limited_output, start, true, false, 70);
  limited.UpdateTargetPreparation(limited.preparation_epoch, limited.status.generation, 1, 15,
      true, adaptive::Reason::PREPARING, start);
  limited.Applied(limited.status.generation, full_upper, start);
  check(limited.ladder.count == 15 && !limited.prepared && limited.TargetReady()
      && limited.status.ready_levels == 1 && limited.status.applied == full_upper,
      "limited residency retains the full logical ladder while reporting only the ready target");
  limited.trial = true;
  limited.previous_level = 0;
  limited.baseline_fps = 100.;
  limited.baseline_gpu_us = 4000.;
  limited.Request(1, start + 1);
  const auto cold_generation = limited.status.generation;
  const auto cold_target = limited.status.requested;
  for (uint64_t index = 1; index <= 24; ++index) {
    const int64_t now = start + static_cast<int64_t>(index) * adaptive::SAMPLE_INTERVAL_NS;
    limited.ObserveGpu({.generation = cold_generation, .frame = index, .sequence = index,
        .context = 1, .recorded_ns = now, .completed_ns = now, .total_us = 100.,
        .valid = true, .stable = true}, now);
    limited_output.serial++;
    limited_output.begin_ns = now - adaptive::WARMUP_NS;
    limited_output.end_ns = now;
    limited.Prepare(limited_settings, full_upper, full_floor, 1, limited_output, now, true, false, 70);
    limited.UpdateTargetPreparation(limited.preparation_epoch, cold_generation, 1, 15,
        false, adaptive::Reason::PREPARING, now);
    limited.Applied(cold_generation, cold_target, now);
  }
  check(limited.level == 1 && limited.trial && limited.previous_level == 0 && limited.baseline_fps == 100.
      && limited.status.applied == full_upper && limited.active_level == 0 && limited.status.coefficient == 1.f
      && limited.applied_ns == start && limited.applied_generation != cold_generation
      && limited.gpu_count == 0 && !limited.TargetReady(),
      "a cold target keeps the actual output and cumulative anchor without measuring fallback work as benefit");
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

  adaptive::Session limited_curve;
  limited_settings.selected_levels = 9;
  limited_output = {.available = true, .source = 1, .generation = 1, .configuration = 1,
      .fps = 100., .observed_multiplier = 4.};
  uint64_t pending_generation = 0;
  int pending_ticks = 0;
  size_t deepest_applied = 0;
  for (uint64_t index = 0; index <= 220; ++index) {
    const int64_t now = start + static_cast<int64_t>(index) * adaptive::SAMPLE_INTERVAL_NS;
    const double coefficient = limited_curve.status.coefficient;
    limited_curve.ObserveGpu({.generation = limited_curve.status.generation, .frame = index + 1,
        .sequence = index + 1, .context = 1, .recorded_ns = now, .completed_ns = now,
        .total_us = 4000. * coefficient, .valid = true, .stable = true,
        .sampling_generation = limited_curve.sampling_generation}, now);
    limited_output.serial++;
    limited_output.begin_ns = now - 2'000'000'000;
    limited_output.end_ns = now;
    limited_output.fps = 100. + 30. * (1. - coefficient);
    limited_curve.Prepare(limited_settings, full_upper, full_floor, 1, limited_output, now, true, false, 71);
    if (pending_generation != limited_curve.status.generation) {
      pending_generation = limited_curve.status.generation;
      pending_ticks = 0;
    }
    const bool complete = ++pending_ticks >= 8;
    limited_curve.UpdateTargetPreparation(limited_curve.preparation_epoch, pending_generation, 2, 9,
        complete, adaptive::Reason::PREPARING, now);
    if (complete) {
      limited_curve.Applied(pending_generation, limited_curve.status.requested, now);
      deepest_applied = std::max(deepest_applied, limited_curve.active_level);
    }
  }
  check(deepest_applied >= 3 && !limited_curve.prepared && !limited_curve.settings.legacy_preparation,
      "delayed cold targets still explore useful fine steps through the normal cumulative controller");

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

  // CPU design check only: FPS, GPU durations, completion delay and time are
  // synthetic here. The native single case obtains GPU samples and time live.
  adaptive::Session automatic_player;
  const adaptive::Settings automatic_settings{.enabled = true, .target_fps = 100.f, .minimum_scale = .5f,
      .prepared_levels = 3, .selected_levels = 3, .limited_residency = true};
  adaptive::Output automatic_output{.available = true, .source = 1, .generation = 1, .configuration = 1};
  uint32_t automatic_phase = 0, completion_ticks = 0;
  uint64_t completion_generation = 0;
  size_t actual_slot = 0;
  int64_t rejected_cooldown = 0;
  bool returned_anchor = false, anchor_ready = false, same_tick_rollback = false;
  for (uint64_t index = 0; index <= 80 && automatic_phase != 9; ++index) {
    const auto now = start + static_cast<int64_t>(index) * adaptive::SAMPLE_INTERVAL_NS;
    automatic_player.ObserveGpu({.generation = automatic_player.status.generation,
        .frame = index + 1, .sequence = index + 1, .context = 1, .recorded_ns = now, .completed_ns = now,
        .total_us = (actual_slot == 0 ? 4000. : (actual_slot == 1 ? 3000. : 2000.)),
        .valid = true, .stable = true, .sampling_generation = automatic_player.sampling_generation}, now);
    ++automatic_output.serial;
    automatic_output.begin_ns = now - 500'000'000;
    automatic_output.end_ns = now;
    automatic_output.fps = (actual_slot == 0 ? 80. : (actual_slot == 1 && !returned_anchor ? 84. : 60.));
    const auto before_requested = automatic_player.level;
    const auto before_generation = automatic_player.status.generation;
    const bool before_trial = automatic_player.trial;
    const auto before_previous = automatic_player.previous_level;
    const double before_baseline_fps = automatic_player.baseline_fps;
    automatic_player.Prepare(automatic_settings, full_upper, full_floor, 1, automatic_output, now, true, false, 73);
    if (completion_generation != automatic_player.status.generation) {
      completion_generation = automatic_player.status.generation;
      completion_ticks = 0;
    }
    const bool ready = actual_slot == automatic_player.level
        || (automatic_player.level == 1 && anchor_ready) || ++completion_ticks >= 3;
    automatic_player.UpdateTargetPreparation(automatic_player.preparation_epoch, completion_generation,
        ready ? 2u : 1u, 3, ready, adaptive::Reason::PREPARING, now);
    if (ready) {
      automatic_player.Applied(completion_generation, automatic_player.status.requested, now);
      actual_slot = automatic_player.active_level;
      anchor_ready |= actual_slot == 1;
    }
    if (automatic_phase == 0 && automatic_player.TargetReady() && actual_slot == 0) {
      check(automatic_player.ladder.count == 3 && automatic_player.ladder.coefficients[1] == .75f
          && automatic_player.ladder.coefficients[2] == .5f, "the bounded player curve uses exactly 100/75/50 logical sizes");
      ++automatic_phase;
    } else if (automatic_phase == 1 && automatic_player.level == 1 && automatic_player.trial) {
      ++automatic_phase;
    } else if (automatic_phase == 2 && automatic_player.TargetReady() && actual_slot == 1) {
      ++automatic_phase;
    } else if (automatic_phase == 3 && automatic_player.level == 2 && automatic_player.trial
        && automatic_player.previous_level == 1 && automatic_player.baseline_fps == 84.) {
      ++automatic_phase;
    } else if (automatic_phase == 4 && automatic_player.TargetReady() && actual_slot == 2) {
      ++automatic_phase;
    } else if (automatic_phase == 5 && before_trial && before_requested == 2 && before_previous == 1
        && before_baseline_fps == 84. && automatic_player.status.generation > before_generation
        && automatic_player.level == 1 && automatic_player.blocked && !automatic_player.trial) {
      rejected_cooldown = automatic_player.blocked_until_ns;
      returned_anchor = true;
      same_tick_rollback = actual_slot == 1 && automatic_player.applied_ns == now;
      check(same_tick_rollback, "a hot rollback is observed even when Applied completes in the same synthetic tick");
      ++automatic_phase;
    } else if (automatic_phase == 6 && automatic_player.TargetReady() && actual_slot == 1) {
      ++automatic_phase;
    } else if (automatic_phase == 7 && automatic_player.level == 2 && automatic_player.trial && !automatic_player.blocked) {
      check(now < rejected_cooldown && automatic_player.baseline_fps == 60.,
          "the synthetic workload change reopens the existing gate before its unchanged 30-second expiry");
      ++automatic_phase;
    } else if (automatic_phase == 8 && automatic_player.TargetReady() && actual_slot == 2) {
      ++automatic_phase;
    }
  }
  check(automatic_phase == 9 && actual_slot == 2 && automatic_player.previous_level == 1 && same_tick_rollback,
      "synthetic CPU input validates two automatic cold descents, no-benefit rollback and workload-triggered re-exploration");
  std::cout << "ADAPTIVE_AUTOMATIC_DESIGN evidence=CPU synthetic_FPS_GPU_completion_time=1 completed_phase=" << automatic_phase
      << " hot_rollback_same_tick=" << same_tick_rollback << '\n';
  for (const bool limited_residency : {false, true}) {
    adaptive::Session delivered;
    const adaptive::Settings delivery_settings{.enabled = true, .target_fps = 100.f, .minimum_scale = .5f,
        .prepared_levels = 2, .selected_levels = 2, .limited_residency = limited_residency};
    adaptive::Output observed{.available = true, .source = 1, .generation = 1, .configuration = 1,
        .serial = 1, .begin_ns = start, .end_ns = start, .fps = 80.};
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
    delivered.Applied(delivered.status.generation, full_upper, start);
    const auto accepted_generation = delivered.applied_generation;
    const auto accepted_stability = delivered.stable_after_ns;
    delivered.trial = true;
    delivered.previous_level = 0;
    delivered.Request(1, start + 4'000'000'000);
    const auto rejected_generation = delivered.status.generation;
    const auto rejected_sizes = delivered.status.requested;
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
        "a failed comparison returns the next request to the accepted anchor without inventing delivery");
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
