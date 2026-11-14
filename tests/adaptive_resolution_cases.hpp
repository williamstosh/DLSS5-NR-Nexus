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
        .total_us = gpu_us, .valid = true, .stable = true}, now);
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
  for (int i = 14; i <= 27; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 2400.);
  check(session.level == 0 && session.blocked && !session.trial,
        "lower NR cost without output FPS benefit restores quality and stops probing");
  for (int i = 28; i <= 60; ++i) tick(start + i * adaptive::SAMPLE_INTERVAL_NS, 50., 3000.);
  check(session.level == 0 && session.blocked, "a steady cap does not trigger repeated downscaling");
  const size_t samples = session.gpu_count;
  session.ObserveGpu({.generation = lower_generation, .frame = ++sequence, .sequence = sequence,
      .recorded_ns = start, .completed_ns = start, .total_us = 1., .valid = true, .stable = true},
      start + 20'000'000'000);
  check(session.gpu_count == samples, "old generation completions cannot drive the new size");

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
  adaptive::Session dual;
  dual.prepared = true;
  dual.status.generation = 1;
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
  check(gated.prepared && gated.stable_after_ns == ready_ns + adaptive::WARMUP_NS
      && gated.status.reason == adaptive::Reason::WARMING,
      "complete owner readiness starts a fresh observation window after preparation");
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
      && reused.status.generation > generation && reused.gpu_count == 0 && !reused.blocked
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
        .valid = true, .stable = true}, now);
    ++legacy_output.serial;
    legacy_output.begin_ns = now - 2'000'000'000;
    legacy_output.end_ns = now;
    legacy_automatic.Prepare(settings, upper, floor, 2, legacy_output, now, true, false, 60);
    legacy_automatic.Applied(legacy_automatic.status.generation, legacy_automatic.status.requested, now);
  }
  check(legacy_automatic.trial && legacy_automatic.level == 1 && legacy_automatic.ladder.count == 2
      && legacy_automatic.status.requested == legacy_automatic.minimum && !legacy_automatic.prepared,
      "the default-off experiment still starts its automatic on-demand endpoint request without a full-set gate");
  return passed;
}
}  // namespace adaptive_resolution_cases
