/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "../addon.cpp"

namespace output_framerate_registration_cases {
namespace addon = renodx::addons::dlss5;
namespace rate = renodx::addons::dlss5::output_framerate;
inline std::map<reshade::addon_event, std::vector<void*>> callbacks;

template <reshade::addon_event Event, typename... Args>
void Dispatch(Args... args) {
  using Callback = typename reshade::addon_event_traits<Event>::decl;
  for (auto* entry : callbacks[Event]) {
    reinterpret_cast<Callback>(entry)(args...);
  }
}
}  // namespace output_framerate_registration_cases

extern "C" __declspec(dllexport) void ReShadeRegisterEvent(reshade::addon_event event, void* callback) {
  output_framerate_registration_cases::callbacks[event].push_back(callback);
}

extern "C" __declspec(dllexport) void ReShadeUnregisterEvent(reshade::addon_event event, void* callback) {
  std::erase(output_framerate_registration_cases::callbacks[event], callback);
}

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(void*, uint32_t version) {
  return version == RESHADE_API_VERSION;
}

extern "C" __declspec(dllexport) bool ReShadeUnregisterAddon(void*) { return true; }

#if defined(IMGUI_VERSION_NUM)
extern "C" __declspec(dllexport) const imgui_function_table* ReShadeGetImGuiFunctionTable(uint32_t version) {
  static const imgui_function_table table{};
  return (version == IMGUI_VERSION_NUM ? &table : nullptr);
}
#endif

int RunOutputFramerateRegistrationCases() {
  using namespace output_framerate_registration_cases;
  using nr_edit_reuse_present_cases::Swapchain;
  try {
    const auto module = GetModuleHandleW(nullptr);
    reshade::internal::get_reshade_module_handle(module);
    Check(DllMain(module, DLL_PROCESS_ATTACH, nullptr), "the production add-on accepts the offline ReShade host");
    const auto detach = [](HINSTANCE attached) { DllMain(attached, DLL_PROCESS_DETACH, nullptr); };
    std::unique_ptr<std::remove_pointer_t<HINSTANCE>, decltype(detach)> attached(module, detach);
    Check(renodx::utils::directx::Initialize(), "Output registration initializes DirectX");
    Gpu gpu;
    model_gpu = &gpu;
    native::enabled = true;
    native::monitor_requested = false;
    native::hooks_enabled = true;
    native::hook_point = native::kHookPresent;
    native::present_pre_fg = 0;
    native::present_frames = 0;
    native::present_guides = native::kPresentGuidesNever;
    native::stack_passes = 1;
    native::edit_reuse_enabled = false;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::nr_adaptive_enabled = true;
    native::nr_adaptive_selected_levels = 2;
    native::nr_adaptive_target_fps = 100.f;
    native::nr_adaptive_min_scale = .67f;
    native::codec_mode = 0;
    native::feed_mode = 1;
    native::norm_governor = 0;
    native::look_mode = false;
    native::control_mask_enabled = false;
    native::use_auto_mask = false;
    native::telemetry_seconds = 0;
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::intensity = 1.f;
    native::preset = 2;
    native::direct_api.module = module;
    native::direct_api.create = CreateModel;
    native::direct_api.evaluate = EvaluateModel;
    native::direct_api.release = ReleaseModel;
    native::direct_load_state = native::DirectLoadState::Ready;
    native::direct_device = gpu.device.Get();
    native::direct_device->AddRef();
    native::direct_device_native = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::direct_runtime_requirements.file_version = "310.9.16.0";
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    native::ngx_loader_staging.nr_ready = true;
    native::ngx_loader_staging.core_ready = true;
    native::ngx_loader_staging.nr_requirements = native::direct_runtime_requirements;
    native::PublishNgxLoaderSymbols();
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "Output registration installs native queue tracking");

    const auto make_window = [&] {
      const auto window = CreateWindowExW(0, L"STATIC", L"NR output callback fixture", WS_OVERLAPPED,
          0, 0, 16, 16, nullptr, nullptr, module, nullptr);
      Check(window != nullptr, "Output registration has a hidden DXGI window");
      return std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)>(window, &DestroyWindow);
    };
    const auto main_window = make_window();
    const auto side_window = make_window();
    Swapchain main(gpu, main_window.get(), 16, 16);
    Swapchain side(gpu, side_window.get(), 16, 16);
    Dispatch<reshade::addon_event::init_swapchain>(&main, false);
    Dispatch<reshade::addon_event::init_swapchain>(&side, false);
    const auto main_identity = rate::MatchPresentOutput(reinterpret_cast<uintptr_t>(&main),
        main.get_current_back_buffer().handle);
    const auto side_identity = rate::MatchPresentOutput(reinterpret_cast<uintptr_t>(&side),
        side.get_current_back_buffer().handle);
    Check(main_identity && side_identity && main_identity != side_identity,
          "production sampling registration admits distinct live DXGI outputs");
    const auto session = [&]() -> std::shared_ptr<const addon::adaptive_resolution::Session> {
      const auto stream = native::present_streams.find(&main);
      if (stream == native::present_streams.end()) return {};
      const auto source = native::features.find(stream->second.Handle(true));
      return (source != native::features.end() ? source->second.adaptive_session : nullptr);
    };
    const auto drive = [&](std::chrono::nanoseconds interval, int frames, bool side_events) {
      const auto begin = std::chrono::steady_clock::now();
      bool requested_lower = false;
      for (int index = 0; index < frames; ++index) {
        std::this_thread::sleep_until(begin + interval * index);
        Dispatch<reshade::addon_event::present>(&main.queue, &main, nullptr, nullptr, 0, nullptr);
        if (const auto current = session(); current != nullptr) {
          const auto requested = current->status.requested[0].width;
          requested_lower |= requested != 0 && requested < 16;
        }
        if (side_events && index % 55 == 0) {
          // Dispatch sampler and executor in the actual registration order.
          Dispatch<reshade::addon_event::present>(&side.queue, &side, nullptr, nullptr, 0, nullptr);
        }
      }
      return requested_lower;
    };
    const bool tried_high = drive(std::chrono::nanoseconds(8'333'333), 390, true);
    const auto upper = rate::Read(main_identity, rate::NowNs());
    const auto upper_session = session();
    Check(upper.available && upper.source == rate::Source::PRESENT && upper.fps >= 105 && upper.fps <= 135
              && upper_session != nullptr && upper_session->status.output_fps >= 105
              && upper_session->status.ready_levels == 2 && upper_session->status.applied[0].width == 16
              && upper_session->level == 0 && !tried_high,
          "registered ordinary callbacks deliver the main 120 FPS window to production NR at its upper tier");
    {
      addon::RuntimeLock lock(native::runtime_mutex);
      const auto stage = addon::stage_plan::Stage::PRESENT;
      const native::AdaptiveCompletionKey key{main_identity.generation, upper_session->status.display_source_generation};
      const auto& scopes = native::adaptive_completion_scopes[static_cast<size_t>(stage)];
      const auto scope = scopes.find(key);
      Check(!native::monitor_requested.load() && scope != scopes.end()
                && native::CurrentCompletedGroup(scope->second.retained, native::RequestedStagePlan().At(stage),
                    rate::NowNs(), nullptr, &key)
                && scope->second.retained.passes[0].width == upper_session->status.applied[0].width,
            "the registered GPU completion callback retains the main output and source's upper size with the monitor off");
    }
    const auto upper_applied = upper_session->status.applied;
    const auto upper_generation = upper_session->applied_generation;
    const auto upper_sampling_generation = upper_session->sampling_generation;
    const auto teardown_matches = native::destroy_swapchain_matches.load();
    Check(!native::teardown_pending.load(), "the ordinary main output has no pending device teardown");
    Dispatch<reshade::addon_event::destroy_swapchain>(&side, false);
    const auto after_destroy = rate::Read(main_identity, rate::NowNs());
    Check(after_destroy.available && after_destroy.source_generation == upper.source_generation
              && after_destroy.window_serial == upper.window_serial && !rate::IsCurrentOutput(side_identity),
          "registered side-chain destruction preserves the production main output window");
    const auto after_destroy_session = session();
    Check(after_destroy_session == upper_session && after_destroy_session->status.applied == upper_applied
              && after_destroy_session->applied_generation == upper_generation
              && after_destroy_session->sampling_generation == upper_sampling_generation
              && native::destroy_swapchain_matches.load() == teardown_matches && !native::teardown_pending.load()
              && !native::present_streams.contains(&side),
          "destroying the same-device side chain retires it without tearing down the main NR session");
    const bool tried_low = drive(std::chrono::nanoseconds(16'666'667), 440, false);
    const auto slow = rate::Read(main_identity, rate::NowNs());
    const auto slow_session = session();
    Check(slow.available && slow.fps >= 50 && slow.fps <= 70 && slow_session != nullptr
              && slow_session->status.output_fps >= 50 && slow_session->status.output_fps <= 70 && tried_low,
          "only changing the registered application's cadence reaches the ordinary adaptive controller");
    Dispatch<reshade::addon_event::destroy_swapchain>(&main, true);
    Dispatch<reshade::addon_event::init_swapchain>(&main, true);
    Check(!rate::IsCurrentOutput(main_identity), "registered resize removes the old output generation");
    {
      namespace sdk = addon::fg::native_options;
      const auto forwarded_identity = rate::MatchPresentOutput(reinterpret_cast<uintptr_t>(&main),
          main.get_current_back_buffer().handle);
      Check(forwarded_identity && !rate::Read(forwarded_identity, rate::NowNs()).available,
            "the Off forwarding control starts with a fresh output window");
      funnel_accounting_tests::Restore sdk_restore;
      static Swapchain* forwarded_output = nullptr;
      sdk_restore.Value(&forwarded_output);
      sdk_restore.Value(&sdk::internal::state.present);
      sdk_restore.Value(&sdk::internal::state.real);
      forwarded_output = &main;
      sdk::internal::state.present = +[](IDXGISwapChain* swapchain, UINT, UINT, bool& skip) -> HRESULT {
        if (swapchain != forwarded_output->swapchain.Get() || !sdk::InSdkPresent()) return E_FAIL;
        skip = false;
        Dispatch<reshade::addon_event::present>(&forwarded_output->queue, forwarded_output, nullptr, nullptr, 0, nullptr);
        return S_OK;
      };
      sdk::internal::state.real = +[](const sl::ViewportHandle&, const sl::DLSSGOptions&) { return sl::Result::eOk; };
      const sl::ViewportHandle sdk_viewport(83);
      sl::Resource sdk_back_buffer(sl::ResourceType::eTex2d, main.buffers[main.get_current_back_buffer_index()].Get());
      const sl::ResourceTag sdk_tag(&sdk_back_buffer, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent);
      sdk::ObserveBackBufferTags(static_cast<uint32_t>(sdk_viewport), &sdk_tag, 1);
      sl::DLSSGOptions sdk_options;
      sdk_options.mode = sl::DLSSGMode::eOff;
      Check(sdk::internal::HookedSetOptions(sdk_viewport, sdk_options) == sl::Result::eOk
                && rate::NativeMode(forwarded_identity) == static_cast<uint32_t>(sl::DLSSGMode::eOff),
            "the fake Native SDK confirms Off for the registered replacement output");
      const auto begin = std::chrono::steady_clock::now();
      for (int index = 0; index < 156; ++index) {
        std::this_thread::sleep_until(begin + std::chrono::nanoseconds(16'666'667) * index);
        bool skip = true;
        Check(sdk::internal::HookedPresent(main.swapchain.Get(), 0, 0, skip) == S_OK && !skip,
              "the Off SDK forwards to the real registered ordinary Present callbacks");
      }
      const auto forwarded = rate::Read(forwarded_identity, rate::NowNs());
      Check(forwarded.available && forwarded.source == rate::Source::PRESENT && forwarded.fps >= 50 && forwarded.fps <= 70,
            "the Off SDK's registered forwarding creates a new ordinary Present window");
    }
    Dispatch<reshade::addon_event::destroy_swapchain>(&main, false);
    attached.reset();
    Check(callbacks[reshade::addon_event::present].empty(), "ordinary sampling unregisters every test callback");
    model_gpu = nullptr;
    std::cout << "PASS production registered sampling to ordinary adaptive NR evidence=WARP+MOCK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL output callback registration: " << error.what() << '\n';
    return 1;
  }
}
