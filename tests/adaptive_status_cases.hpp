/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>

// Include after the FG test host's Check declaration.
namespace adaptive_status_cases {
namespace runtime = renodx::addons::dlss5::internal;
namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
namespace rate = renodx::addons::dlss5::output_framerate;
using renodx::addons::dlss5::stage_plan::Stage;

inline int Run() {
  try {
    constexpr uintptr_t device = 0x9000;
    constexpr uintptr_t main_swapchain = 0xA000;
    constexpr uintptr_t secondary_swapchain = 0xB000;
    constexpr std::array<uintptr_t, 2> main_buffers{0x1000, 0x2000};
    constexpr std::array<uintptr_t, 2> secondary_buffers{0x3000, 0x4000};
    rate::RegisterPresentOutput(main_swapchain, device, 0, main_buffers);
    rate::RegisterPresentOutput(secondary_swapchain, device, 0, secondary_buffers);
    const auto main_output = rate::FindPresentOutput(device, main_buffers[0]);
    const auto secondary_output = rate::FindPresentOutput(device, secondary_buffers[0]);
    Check(main_output && secondary_output && main_output != secondary_output
            && rate::FindPresentOutput(device, main_buffers[1]) == main_output,
        "outputs on the same device are distinct and rotating buffers resolve to one owner");

    constexpr uint64_t main_source = 10;
    constexpr uint64_t secondary_source = 20;
    const auto publish = [](const rate::OutputIdentity& output, Stage stage, uint64_t source, adaptive::Reason reason) {
      adaptive::Status status;
      status.reason = reason;
      status.display_output_generation = output.generation;
      status.display_source_generation = source;
      status.display_stage = static_cast<uint32_t>(stage);
      runtime::PublishAdaptiveStatus(status);
    };
    publish(main_output, Stage::PRESENT, main_source, adaptive::Reason::OBSERVING);
    publish(secondary_output, Stage::PRESENT, secondary_source, adaptive::Reason::FAILED);
    publish(main_output, Stage::UPSCALED, main_source, adaptive::Reason::PREPARING);
    auto now = rate::NowNs();
    Check(runtime::CopyAdaptiveStatus(main_output, Stage::PRESENT, now, main_source).reason == adaptive::Reason::OBSERVING
            && runtime::CopyAdaptiveStatus(secondary_output, Stage::PRESENT, now, secondary_source).reason == adaptive::Reason::FAILED,
        "a later secondary publish cannot replace the selected output's reason");
    Check(runtime::CopyAdaptiveStatus(main_output, Stage::UPSCALED, now, main_source).reason == adaptive::Reason::PREPARING
            && runtime::CopyAdaptiveStatus(main_output, Stage::RENDER, now, main_source).reason == adaptive::Reason::DISABLED
            && runtime::CopyAdaptiveStatus(main_output, Stage::PRESENT, now, secondary_source).reason == adaptive::Reason::DISABLED,
        "only the requested stage and source can return a reason");
    Check(runtime::CopyAdaptiveStatus(main_output, Stage::PRESENT, now + adaptive::SAMPLE_MAX_AGE_NS + 1).reason
            == adaptive::Reason::DISABLED,
        "expired reasons become pending");

    namespace options = renodx::addons::dlss5::fg::native_options;
    constexpr uintptr_t main_feature = 0xE001;
    constexpr uintptr_t secondary_feature = 0xE002;
    rate::BindNativeOutput(main_output, 7);
    rate::BindNativeOutput(secondary_output, 8);
    rate::BindNativeFeatureOutput(main_output, main_feature);
    rate::BindNativeFeatureOutput(secondary_output, secondary_feature);
    options::internal::state.real = +[](const sl::ViewportHandle&, const sl::DLSSGOptions&) { return sl::Result::eOk; };
    options::Configure(runtime::OnNativeFgOptions);
    runtime::native_fg_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(main_feature);
    runtime::native_fg_owner_identity = main_feature;
    runtime::native_fg_owns = true;
    runtime::native_fg_public_off = false;
    runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOn;
    sl::DLSSGOptions off;
    off.mode = sl::DLSSGMode::eOff;
    const auto history = runtime::history_reset_epoch.load();
    rate::ObserveNativeOptions(7, 1, 1, 2);
    Check(options::internal::HookedSetOptions(sl::ViewportHandle(9), off) == sl::Result::eOk
            && runtime::native_fg_owns.load() && runtime::history_reset_epoch.load() == history
            && !rate::NativeMode(main_output).has_value(),
        "an unassociated declaration withdraws fixed-rate evidence without resetting the actual owner's image history");
    Check(options::internal::HookedSetOptions(sl::ViewportHandle(8), off) == sl::Result::eOk
            && runtime::native_fg_owns.load() && !runtime::native_fg_public_off.load()
            && runtime::history_reset_epoch.load() == history && rate::NativeMode(secondary_output) == 0,
        "a successful other-viewport Off callback cannot withdraw the active Native owner or reset its history");
    Check(options::internal::HookedSetOptions(sl::ViewportHandle(7), off) == sl::Result::eOk
            && !runtime::native_fg_owns.load() && runtime::native_fg_public_off.load()
            && runtime::history_reset_epoch.load() > history && rate::NativeMode(main_output) == 0,
        "the matching viewport's successful Off still withdraws the actual Native owner");

    const auto saved_adaptive = runtime::nr_adaptive_enabled.exchange(true);
    const auto saved_follow = runtime::nr_follow_input_res.exchange(false);
    const auto saved_scale = runtime::nr_resolution_scale.exchange(1.f);
    const auto saved_mode = runtime::pass_resolution_mode[0].exchange(2);
    const auto saved_pass_scale = runtime::pass_resolution_scale[0].exchange(.5f);
    const auto end = rate::NowNs();
    for (int index = 0; index <= 4; ++index) {
      rate::ObservePresent(main_output, end - (4 - index) * rate::Stream::BIN_NS);
    }
    const auto plan = renodx::addons::dlss5::stage_plan::Build({.pass_count = 2, .hook_point = 2});
    std::shared_ptr<adaptive::Session> independent;
    const auto freeze = [&] {
      return runtime::FreezeAdaptiveFrame(&independent, plan, plan.At(Stage::PRESENT), 16, 16, 16, 16,
          0x501, true, 1, 1, false, 1, 0, false, nullptr, main_output, main_source);
    };
    const auto explicit_off = freeze();
    Check(explicit_off.enabled && explicit_off.upper[0].width == 16 && explicit_off.upper[1].width == 8,
        "ordinary independent Pass sizes retain the matching successful Off qualification");
    const auto saved_hook = runtime::hook_point.exchange(runtime::kHookPresent);
    runtime::PresentTarget target;
    target.independent_resolution_pair = true;
    target.domain = renodx::addons::dlss5::present_restore_scope::Domain::FINAL_PRESENT;
    target.adaptive_output = main_output;
    Check(runtime::PresentResolutionPairAllowed(target), "the scoped Off qualification reaches the real recording gate");
    constexpr std::array<uintptr_t, 1> unassociated_buffers{0x5000};
    rate::RegisterPresentOutput(0xC000, device, 0, unassociated_buffers);
    const auto unassociated_output = rate::FindPresentOutput(device, unassociated_buffers[0]);
    std::shared_ptr<adaptive::Session> waiting;
    const auto waiting_frame = runtime::FreezeAdaptiveFrame(&waiting, plan, plan.At(Stage::PRESENT), 16, 16, 16, 16,
        0x502, true, 1, 1, false, 1, 0, false, nullptr, unassociated_output, main_source);
    auto manual = target;
    manual.adaptive_output = unassociated_output;
    Check(!waiting_frame.enabled && runtime::PresentResolutionPairAllowed(manual),
        "unattributed Off pauses automatic sizing while preserving the existing legal manual upper group");
    rate::DestroyPresentOutput(0xC000);
    const auto saved_attack = runtime::kSetNormAttack.Get();
    runtime::FinalResources absolute;
    absolute.units.absolute = true;
    {
      const runtime::AdaptiveFrameScope active(&explicit_off);
      Check(runtime::PrepareFrameScale(nullptr, nullptr, absolute, nullptr, {}) == 0,
          "absolute HDR performs the actual no-governor branch without issuing GPU work");
    }
    const auto dormant_sampling = independent->sampling_generation;
    const auto dormant_history = runtime::history_reset_epoch.load();
    independent->gpu_count = 5;
    runtime::kSetNormAttack.Commit(saved_attack + 1.f);
    const auto dormant_frame = freeze();
    {
      const runtime::AdaptiveFrameScope active(&dormant_frame);
      Check(runtime::PrepareFrameScale(nullptr, nullptr, absolute, nullptr, {}) == 0
              && independent->sampling_generation == dormant_sampling && independent->gpu_count == 5
              && runtime::history_reset_epoch.load() == dormant_history,
          "an unused absolute-HDR governor rate leaves the existing comparison and history untouched");
    }
    runtime::kSetNormAttack.Commit(saved_attack);
    independent->gpu_count = 0;
    const auto saved_governor = runtime::norm_governor.exchange(2);
    runtime::NormStream first_stream, second_stream;
    uint64_t snaps = 0;
    const auto first_plan = runtime::PlanFrameScale(&first_stream, &snaps, {});
    runtime::kSetNormAttack.Commit(saved_attack + 1.f);
    const auto second_plan = runtime::PlanFrameScale(&second_stream, &snaps, {});
    Check(first_plan.measurement_identity != second_plan.measurement_identity
            && first_plan.governor[0] != second_plan.governor[0],
        "an active governor rate changes both the actual dispatch constant and its measurement identity");
    independent->ObserveNormalizationSettings(1, first_plan.measurement_identity, rate::NowNs());
    independent->gpu_count = 5;
    const auto active_sampling = independent->sampling_generation;
    independent->ObserveNormalizationSettings(1, second_plan.measurement_identity, rate::NowNs());
    Check(independent->sampling_generation > active_sampling && independent->gpu_count == 0,
        "the dispatched governor change withdraws old comparable costs");
    runtime::kSetNormAttack.Commit(saved_attack);
    runtime::norm_governor = saved_governor;
    const auto sampling = independent->sampling_generation;
    const auto generation = independent->status.generation;
    sl::DLSSGOptions on;
    on.mode = sl::DLSSGMode::eOn;
    Check(options::internal::HookedSetOptions(sl::ViewportHandle(8), on) == sl::Result::eOk && freeze().enabled
            && independent->sampling_generation == sampling && independent->status.generation == generation
            && runtime::PresentResolutionPairAllowed(target),
        "another viewport's On declaration cannot revoke this output's independent-size qualification or comparison");
    Check(options::internal::HookedSetOptions(sl::ViewportHandle(7), on) == sl::Result::eOk && !freeze().enabled
            && independent->status.reason == adaptive::Reason::UNSUPPORTED && !runtime::PresentResolutionPairAllowed(target),
        "withdrawing the actual required Off receipt rechecks the existing independent-size support gate");
    options::internal::HookedSetOptions(sl::ViewportHandle(7), off);
    runtime::nr_adaptive_enabled = saved_adaptive;
    runtime::nr_follow_input_res = saved_follow;
    runtime::nr_resolution_scale = saved_scale;
    runtime::pass_resolution_mode[0] = saved_mode;
    runtime::pass_resolution_scale[0] = saved_pass_scale;
    runtime::hook_point = saved_hook;

    rate::BindNativeFeatureOutput(main_output, main_feature);
    rate::BindNativeFeatureOutput(secondary_output, secondary_feature);
    for (int index = 0; index <= 4; ++index) {
      rate::ObservePresent(main_output, 1'000'000'000LL + index * rate::Stream::BIN_NS);
      rate::ObservePresent(secondary_output, 1'000'000'000LL + index * rate::Stream::BIN_NS);
    }
    const auto main_window = rate::Read(main_output, 3'000'000'000LL);
    Check(main_window.available, "the live output has a complete window before another FG source is released");
    runtime::native_fg_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(secondary_feature);
    runtime::native_fg_owner_identity = secondary_feature;
    runtime::native_fg_owns = true;
    runtime::OnNativeFgFeatureReleased(reinterpret_cast<const NVSDK_NGX_Handle*>(secondary_feature));
    const auto after_release = rate::Read(main_output, 3'000'000'000LL);
    Check(after_release.available && after_release.source_generation == main_window.source_generation
            && after_release.window_serial == main_window.window_serial,
        "the real FG feature-release adapter invalidates only the released feature's output");
    rate::ObserveNativeOptions(7, 1, 1, 2);
    const auto old_query = rate::BeginNativeState(7);
    rate::BindNativeFeatureOutput(main_output, main_feature + 10);
    rate::ObserveNativeState(old_query, true, 120, 4'000'000'000LL);
    Check(!rate::Read(main_output, 4'000'000'000LL).available
            && rate::NativeFeatureOutput(main_feature + 10) == main_output
            && !rate::NativeFeatureOutput(main_feature),
        "a changed actual FG source cuts the old window and rejects its in-flight state query");
    const auto binding = rate::NativeBindingGeneration(main_output);
    runtime::OnNativeFgFeatureReleased(reinterpret_cast<const NVSDK_NGX_Handle*>(main_feature));
    Check(rate::NativeBindingGeneration(main_output) == binding,
        "late release of the previous FG handle cannot revoke the replacement source");
    runtime::native_fg_owner = nullptr;
    runtime::native_fg_owner_identity = 0;
    runtime::native_fg_owns = false;
    options::Configure(nullptr);

    publish(main_output, Stage::PRESENT, main_source, adaptive::Reason::OBSERVING);
    rate::DestroyPresentOutput(secondary_swapchain);
    now = rate::NowNs();
    Check(runtime::CopyAdaptiveStatus(secondary_output, Stage::PRESENT, now).reason == adaptive::Reason::DISABLED
            && runtime::CopyAdaptiveStatus(main_output, Stage::PRESENT, now).reason == adaptive::Reason::OBSERVING,
        "destroying the secondary output invalidates only its publication");

    publish(main_output, Stage::PRESENT, 0, adaptive::Reason::FAILED);
    Check(runtime::CopyAdaptiveStatus(main_output, Stage::PRESENT, rate::NowNs(), main_source).reason
            == adaptive::Reason::OBSERVING,
        "a publication without a source cannot replace the selected output's reason");
    publish(main_output, Stage::PRESENT, secondary_source, adaptive::Reason::WARMING);
    now = rate::NowNs();
    Check(runtime::CopyAdaptiveStatus(main_output, Stage::PRESENT, now, main_source).reason == adaptive::Reason::DISABLED
            && runtime::CopyAdaptiveStatus(main_output, Stage::PRESENT, now, secondary_source).reason == adaptive::Reason::WARMING,
        "a new source on the same output cannot confirm the old source's reason");

    rate::DestroyPresentOutput(main_swapchain);
    rate::RegisterPresentOutput(main_swapchain, device, 0, main_buffers);
    const auto resized = rate::FindPresentOutput(device, main_buffers[0]);
    publish(main_output, Stage::PRESENT, main_source, adaptive::Reason::FAILED);
    now = rate::NowNs();
    Check(resized && resized != main_output
            && runtime::CopyAdaptiveStatus(main_output, Stage::PRESENT, now).reason == adaptive::Reason::DISABLED
            && runtime::CopyAdaptiveStatus(resized, Stage::PRESENT, now).reason == adaptive::Reason::DISABLED,
        "a recreated output with reused addresses cannot inherit a previous or late publication");
    rate::DestroyPresentOutput(main_swapchain);
    std::cout << "PASS case=adaptive-status evidence=MOCK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL adaptive status: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace adaptive_status_cases
