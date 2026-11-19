/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <iostream>

#include <capability.hpp>

int main() {
  namespace capability = renodx::addons::dlss5::capability;
  namespace stage = renodx::addons::dlss5::stage_plan;
  unsigned cases = 0;
  bool passed = true;
  const auto check = [&](bool condition, const char* name) {
    ++cases;
    if (!condition) {
      std::cerr << "capability policy: " << name << '\n';
      passed = false;
    }
  };

  capability::Input input{
      .api = capability::Api::DX12,
      .backend = capability::Backend::NATIVE,
      .runtime = capability::Runtime::AVAILABLE,
      .fg = capability::Fg::OFF,
      .plan = stage::Build({.pass_count = 3}),
      .main_resolution = {.follow_input = true, .scale = .5f},
      .resolutions = {{{.mode = 2, .scale = .5f}}}};
  auto& geometry = input.geometry[static_cast<unsigned>(stage::Stage::UPSCALED)];
  geometry = {.reference_width = 1920, .reference_height = 1080};
  const auto preview = capability::EvaluateGroup(input, 0);
  const auto query = capability::Query(input);
  check(preview.decision.reason == capability::Reason::DIMENSIONS_UNKNOWN
            && preview.working_sizes[0] == capability::Size{}
            && preview.working_sizes[1] == capability::Size{960, 540}
            && preview.working_sizes[2] == capability::Size{}
            && query.configuration.reason == preview.decision.reason
            && query.working_sizes == preview.working_sizes,
        "preview reports missing Guide as unknown in both the decision and returned size");

  input.resolutions[0] = {.mode = 1, .scale = .75f};
  const auto follow_pair = capability::EvaluateGroup(input, 0);
  input.mode = capability::GeometryMode::EXECUTION;
  const auto different_fallbacks = capability::EvaluateGroup(input, 0);
  check(follow_pair.decision.CanSubmit() && follow_pair.working_sizes[0] == capability::Size{}
            && follow_pair.working_sizes[1] == capability::Size{}
            && different_fallbacks.decision.reason == capability::Reason::GROUP_SIZE_MISMATCH,
        "Follow Input preview ignores idle saved scales while execution compares the fallback sizes it uses");
  input.resolutions[0] = {.mode = 2, .scale = .5f};
  const auto fallback = capability::EvaluateGroup(input, 0);
  check(fallback.decision.CanSubmit() && fallback.working_sizes[0] == capability::Size{960, 540}
            && fallback.working_sizes[0] == fallback.working_sizes[1]
            && fallback.working_sizes[0] == fallback.working_sizes[2],
        "execution uses the saved-scale fallback for the active group");
  geometry.guide_width = 1280;
  geometry.guide_height = 720;
  const auto guide = capability::EvaluateGroup(input, 0);
  check(guide.decision.reason == capability::Reason::GROUP_SIZE_MISMATCH
            && guide.working_sizes[0] == capability::Size{1280, 720}
            && guide.working_sizes[1] == fallback.working_sizes[1],
        "current Guide dimensions take precedence and can make the shared workset incompatible");

  input.plan = stage::Build({.pass_count = 2});
  input.required_roles = 3;
  input.source_outputs = 2;
  const auto ordinary = capability::EvaluateGroup(input, 0);
  input.delivery = capability::Delivery::FG_INPUT;
  const auto fg_input = capability::EvaluateGroup(input, 0);
  check(ordinary.decision.CanSubmit() && ordinary.decision.independent_sizes_allowed
            && fg_input.decision.reason == capability::Reason::GROUP_SIZE_MISMATCH
            && !fg_input.decision.independent_sizes_allowed
            && ordinary.working_sizes == fg_input.working_sizes
            && ordinary.output_count == 4 && fg_input.output_count == 4,
        "FG input delivery requires shared sizes even with an Off declaration, with capacity based on actual roles and outputs");

  input.resolutions[0].mode = 0;
  input.required_roles = 2;
  const auto declared_off = capability::EvaluateGroup(input, 0);
  input.fg = capability::Fg::ON;
  const auto declared_on = capability::EvaluateGroup(input, 0);
  check(declared_off.decision.CanSubmit() && declared_on.decision.CanSubmit()
            && declared_off.working_sizes == declared_on.working_sizes
            && declared_off.output_count == 2 && declared_on.output_count == 2,
        "equal-size FG input keeps the same eligibility and capacity when only the declaration changes");

  if (!passed) return 1;
  std::cout << "PASS capability policy evidence=CPU cases=" << cases << '\n';
  return 0;
}
