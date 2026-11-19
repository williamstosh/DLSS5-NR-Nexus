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

  input.delivery = capability::Delivery::ORDINARY;
  input.independent_size_bindings = false;
  const auto mismatched_shared = capability::EvaluateGroup(input, 0);
  check(mismatched_shared.decision.reason == capability::Reason::GROUP_SIZE_MISMATCH
            && !mismatched_shared.decision.independent_sizes_allowed && mismatched_shared.working_sizes == ordinary.working_sizes,
        "an ordinary consumer with one shared workset cannot bind two different grids");
  auto present_shared = input;
  present_shared.plan = stage::Build({.pass_count = 2, .hook_point = 2});
  present_shared.geometry[static_cast<unsigned>(stage::Stage::PRESENT)] = geometry;
  check(capability::EvaluateGroup(present_shared, 0).decision.reason == capability::Reason::GROUP_SIZE_MISMATCH,
        "ordinary Present with FG Off also requires matching grids when it has one workset");
  input.resolutions[0].mode = 0;
  const auto matching_shared = capability::EvaluateGroup(input, 0);
  check(matching_shared.decision.CanSubmit() && !matching_shared.decision.independent_sizes_allowed,
        "an ordinary consumer with one shared workset can bind matching grids");
  input.applied_sizes = {{{861, 483}, {860, 483}}};
  const auto mismatched_applied = capability::EvaluateGroup(input, 0);
  check(mismatched_applied.decision.reason == capability::Reason::GROUP_SIZE_MISMATCH
            && mismatched_applied.working_sizes == input.applied_sizes,
        "identical requests with two different applied grids cannot use one shared workset");
  input.applied_sizes[1] = input.applied_sizes[0];
  const auto matching_applied = capability::EvaluateGroup(input, 0);
  check(matching_applied.decision.CanSubmit() && matching_applied.working_sizes == input.applied_sizes,
        "matching applied grids are used exactly, independent of saved scale or Guide dimensions");
  input.applied_sizes[1].height = 0;
  check(capability::EvaluateGroup(input, 0).decision.reason == capability::Reason::INVALID_RESOLUTION,
        "a partially populated applied grid is invalid");
  input.applied_sizes = {};
  input.independent_size_bindings = true;
  input.delivery = capability::Delivery::FG_INPUT;

  input.required_roles = 2;
  const auto declared_off = capability::EvaluateGroup(input, 0);
  input.fg = capability::Fg::ON;
  const auto declared_on = capability::EvaluateGroup(input, 0);
  check(declared_off.decision.CanSubmit() && declared_on.decision.CanSubmit()
            && declared_off.working_sizes == declared_on.working_sizes
            && declared_off.output_count == 2 && declared_on.output_count == 2,
        "equal-size FG input keeps the same eligibility and capacity when only the declaration changes");

  capability::Input vulkan{
      .api = capability::Api::VULKAN,
      .backend = capability::Backend::NATIVE,
      .runtime = capability::Runtime::AVAILABLE,
      .plan = stage::Build({.pass_count = 2}),
      .main_resolution = {.follow_input = false, .scale = 1.f}};
  auto& vk_geometry = vulkan.geometry[static_cast<unsigned>(stage::Stage::UPSCALED)];
  vk_geometry = {.reference_width = 1919, .reference_height = 1079, .guide_width = 1280, .guide_height = 720};
  vulkan.resolutions[1] = {.mode = 2, .scale = .5f};
  const auto inherited = capability::Query(vulkan);
  check(inherited.CanSubmit() && inherited.working_sizes[0] == capability::Size{1919, 1079}
            && inherited.working_sizes[1] == inherited.working_sizes[0],
        "Vulkan preview uses full inherited output and ignores an inactive independent pass");
  auto vulkan_after = vulkan;
  vulkan_after.plan = stage::Build({.pass_count = 2, .hook_point = 1},
                                 {.mixed_dx12 = false, .ray_reconstruction = true, .present_available = false});
  const auto vulkan_rr = capability::Query(vulkan_after);
  check(vulkan_after.plan.requested[0] == stage::Stage::RENDER && vulkan_after.plan.effective[0] == stage::Stage::UPSCALED
            && vulkan_rr.CanSubmit() && vulkan_rr.plan_reason == stage::Reason::RR_AFTER
            && vulkan_rr.working_sizes == inherited.working_sizes,
        "Vulkan RR can submit its effective Upscaled group for a Render request");
  vulkan_after.plan = stage::Build({.pass_count = 2, .hook_point = 2}, {.mixed_dx12 = false, .present_available = false});
  const auto vulkan_present_after = capability::Query(vulkan_after);
  check(vulkan_after.plan.requested[0] == stage::Stage::PRESENT && vulkan_after.plan.effective[0] == stage::Stage::UPSCALED
            && vulkan_present_after.CanSubmit() && vulkan_present_after.plan_reason == stage::Reason::PRESENT_AFTER
            && vulkan_present_after.working_sizes == inherited.working_sizes,
        "Vulkan can submit its effective Upscaled fallback for a Present request");
  vulkan_after.plan = stage::Build({.pass_count = 2, .hook_point = 2});
  check(capability::Query(vulkan_after).configuration.reason == capability::Reason::BACKEND_UNSUPPORTED,
        "Vulkan cannot submit an effective Present group");
  for (const auto settings : {capability::EffectiveResolution{.follow_input = false, .scale = .67f},
                              capability::EffectiveResolution{.follow_input = true, .scale = 1.f}}) {
    vulkan.main_resolution = settings;
    vulkan.mode = capability::GeometryMode::PREVIEW;
    check(capability::Query(vulkan).configuration.reason == capability::Reason::INDEPENDENT_MODE_UNSUPPORTED,
          "Vulkan rejects a new scaled or Follow Input request");
    vulkan.mode = capability::GeometryMode::EXECUTION;
    vulkan.applied_sizes = {{{861, 483}, {861, 483}}};
    const auto legacy = capability::EvaluateGroup(vulkan, 0);
    check(legacy.decision.CanSubmit() && legacy.working_sizes == inherited.working_sizes,
          "Vulkan preserves full-size execution for a saved scaled or Follow Input request");
  }
  vulkan.main_resolution = {.follow_input = false, .scale = 1.f};
  vulkan.resolutions[0] = {.mode = 2, .scale = 1.f};
  vulkan.mode = capability::GeometryMode::PREVIEW;
  const auto blocked_preview = capability::Query(vulkan).configuration;
  vulkan.mode = capability::GeometryMode::EXECUTION;
  const auto blocked_execution = capability::EvaluateGroup(vulkan, 0).decision;
  check(blocked_preview.reason == capability::Reason::INDEPENDENT_MODE_UNSUPPORTED
            && blocked_preview.reason == blocked_execution.reason
            && blocked_preview.pass == 1 && blocked_execution.pass == 1,
        "Vulkan rejects an active independent mode even when it requests the same grid");

  capability::Input streamline{
      .api = capability::Api::DX12,
      .backend = capability::Backend::STREAMLINE_FALLBACK,
      .runtime = capability::Runtime::AVAILABLE,
      .plan = stage::Build({.pass_count = 2}),
      .main_resolution = {.follow_input = false, .scale = 1.f}};
  check(capability::Query(streamline).CanSubmit(), "independent Streamline fallback can submit an Upscaled group");
  streamline.resolutions[0] = {.mode = 2, .scale = .67f};
  streamline.geometry[static_cast<unsigned>(stage::Stage::UPSCALED)] = {.reference_width = 1920, .reference_height = 1080};
  auto native_upscaled = streamline;
  native_upscaled.backend = capability::Backend::NATIVE;
  const auto native_independent = capability::Query(native_upscaled);
  const auto streamline_independent = capability::Query(streamline);
  check(native_independent.CanSubmit() && streamline_independent.CanSubmit()
            && streamline_independent.groups[0].independent_sizes_allowed
            && streamline_independent.working_sizes == native_independent.working_sizes
            && streamline_independent.working_sizes[0] != streamline_independent.working_sizes[1],
        "Streamline and native DX12 use the same two independent Upscaled grids");
  streamline.independent_size_bindings = false;
  check(capability::Query(streamline).configuration.reason == capability::Reason::GROUP_SIZE_MISMATCH,
        "Streamline with one shared workset rejects different working grids");
  streamline.independent_size_bindings = true;
  streamline.resolutions[0] = {};
  streamline.plan = stage::Build({.pass_count = 2, .hook_point = 2});
  streamline.geometry[static_cast<unsigned>(stage::Stage::PRESENT)] = {.reference_width = 1920, .reference_height = 1080};
  const auto streamline_present = capability::Query(streamline);
  check(streamline.plan.effective[0] == stage::Stage::PRESENT && streamline_present.CanSubmit()
            && streamline_present.group_count == 1 && streamline_present.plan_reason == stage::Reason::NONE
            && streamline_present.working_sizes[0] == capability::Size{1920, 1080}
            && streamline_present.working_sizes[1] == streamline_present.working_sizes[0]
            && !streamline_present.execution_proven,
        "Streamline allows an ordinary Present group without proof of its execution");
  streamline.plan = stage::Build({.pass_count = 2, .hook_point = 1});
  check(streamline.plan.effective[0] == stage::Stage::RENDER
            && capability::Query(streamline).configuration.reason == capability::Reason::BACKEND_UNSUPPORTED,
        "independent Streamline fallback cannot submit a DLSS Render group");
  streamline.plan = stage::Build({.pass_count = 2, .hook_point = 1}, {.ray_reconstruction = true});
  const auto streamline_rr = capability::Query(streamline);
  check(streamline.plan.requested[0] == stage::Stage::RENDER && streamline.plan.effective[0] == stage::Stage::UPSCALED
            && streamline_rr.CanSubmit() && streamline_rr.plan_reason == stage::Reason::RR_AFTER,
        "independent Streamline RR fallback can submit its effective Upscaled group for a Render request");
  streamline.plan = stage::Build({.pass_count = 2, .hook_point = 1, .stages = {2}}, {.ray_reconstruction = true});
  check(streamline.plan.mixed && streamline.plan.group_count == 1
            && capability::Query(streamline).configuration.reason == capability::Reason::BACKEND_UNSUPPORTED,
        "independent Streamline fallback still rejects a mixed chain collapsed by RR");
  streamline.plan = stage::Build({.pass_count = 2, .hook_point = 1, .stages = {2}});
  check(capability::Query(streamline).configuration.reason == capability::Reason::BACKEND_UNSUPPORTED,
        "independent Streamline fallback cannot submit a mixed chain");
  streamline.backend = capability::Backend::NATIVE;
  check(capability::Query(streamline).CanSubmit(), "native DX12 retains Render and Upscaled groups");

  if (!passed) return 1;
  std::cout << "PASS capability policy evidence=CPU cases=" << cases << '\n';
  return 0;
}
