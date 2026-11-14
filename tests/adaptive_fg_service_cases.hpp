/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <utility>
#include <vector>

namespace adaptive_fg_service_cases {
namespace addon = renodx::addons::dlss5;

using CloseFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
CloseFn real_close = nullptr;
ResetFn real_reset = nullptr;
struct ListCall {
  ID3D12GraphicsCommandList* list;
  bool reset;
  HRESULT result;
};
std::vector<ListCall> list_calls;
using CreatePipelineFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
CreatePipelineFn real_create_pipeline = nullptr;
uint32_t pipeline_calls = 0;
bool fail_next_pipeline = false;

HRESULT STDMETHODCALLTYPE ObserveCreatePipeline(ID3D12Device* device,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** pipeline) {
  ++pipeline_calls;
  if (std::exchange(fail_next_pipeline, false)) {
    *pipeline = nullptr;
    return E_FAIL;
  }
  return real_create_pipeline(device, desc, iid, pipeline);
}

HRESULT STDMETHODCALLTYPE ObserveClose(ID3D12GraphicsCommandList* list) {
  const auto result = real_close(list);
  list_calls.push_back({.list = list, .reset = false, .result = result});
  return result;
}

HRESULT STDMETHODCALLTYPE ObserveReset(ID3D12GraphicsCommandList* list,
                                      ID3D12CommandAllocator* allocator, ID3D12PipelineState* state) {
  const auto result = real_reset(list, allocator, state);
  list_calls.push_back({.list = list, .reset = true, .result = result});
  return result;
}

int Run() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    const auto debug_result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (SUCCEEDED(debug_result)) {
      debug->EnableDebugLayer();
    }
    Gpu gpu(false, 16, 16);
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    std::cout << "ADAPTIVE_SERVICE debug_layer=" << (debug != nullptr)
        << " debug_hresult=0x" << std::hex << static_cast<uint32_t>(debug_result) << std::dec
        << " info_queue=" << (info != nullptr) << '\n';
    native::enabled.store(true);
    native::hooks_enabled.store(true);
    native::hook_point.store(native::kHookPresent);
    native::present_pre_fg.store(1);
    native::present_guides.store(native::kPresentGuidesRequired);
    native::OnNativeFgOptions(false);
    native::stack_passes.store(2);
    native::pass_stage[0].store(0);
    native::nr_follow_input_res.store(false);
    native::nr_resolution_scale.store(1.f);
    native::pass_resolution_mode[0].store(0);
    native::pass_follow[0].store(false);
    native::pass_preset[0].store(2);
    native::codec_mode.store(0);
    native::feed_mode.store(0);
    native::look_mode.store(false);
    native::detail_stability.store(0);
    native::gpu_timers_enabled.store(true);
    native::chained_temporal_history.store(true);
    native::transfer_strength.store(1.f);
    native::color_strength.store(1.f);
    native::intensity.store(.5f);
    native::pass_intensity[0].store(.75f);
    native::pass_transfer_strength[0].store(1.f);
    native::pass_color_strength[0].store(1.f);
    native::control_mask_enabled.store(true);
    native::nr_adaptive_selected_levels.store(0);
    native::nr_adaptive_min_scale.store(.33f);
    native::nr_adaptive_enabled.store(false);
    native::max_worksets.store(4);
    native::adaptive_configuration_settings = native::comparison_ui::CaptureAdaptiveSettings;
    addon::gpu_timers::comparison_settings = native::comparison_ui::CaptureSettings;
    native::direct_api.module = GetModuleHandleW(nullptr);
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
    uint32_t masked_calls = 0;
    uint32_t active_successes = 0;
    uint32_t active_failures = 0;
    uint32_t preparation_calls = 0;
    uint32_t preparation_failures = 0;
    uint64_t observing_context = 0;
    std::array<uint32_t, 2> active_pass_masks{}, failed_pass_masks{}, reset_pass_masks{};
    uint32_t partial_role_recordings = 0;
    model_parameter_observer = [&](const NVSDK_NGX_Handle* model, const NVSDK_NGX_Parameter* parameters) {
      Check(native::GetD3D12Resource(parameters, "DLSSNR.ControlMask") != nullptr,
          "every model call receives the production GPU ControlMask");
      ++masked_calls;
      const bool failing = model_calls + 1 == fail_call;
      if (native::preparing_adaptive_frame != nullptr) {
        ++preparation_calls;
        preparation_failures += failing;
      } else {
        const auto stream = native::fg_service::streams.find({false, observing_context});
        Check(stream != native::fg_service::streams.end(), "active model Evaluate belongs to the observed Service context");
        bool identified = false;
        for (size_t role = 0; role < active_pass_masks.size(); ++role) {
          const auto feature = native::features.find(stream->second.roles[role].Handle());
          if (feature == native::features.end()) continue;
          for (uint32_t pass = 0; pass < native::kMaxNrPasses; ++pass) {
            if (feature->second.slots[pass].handle != model) continue;
            Check(!identified && ((active_pass_masks[role] | failed_pass_masks[role]) & (1u << pass)) == 0,
                "the actual model handle identifies one active role and one new Pass invocation");
            identified = true;
            if (native::GetInt(parameters, "DLSSNR.Reset") != 0) {
              reset_pass_masks[role] |= 1u << pass;
            }
            if (failing) {
              failed_pass_masks[role] |= 1u << pass;
            } else {
              active_pass_masks[role] |= 1u << pass;
            }
          }
        }
        Check(identified, "every non-preparation model Evaluate has an independently observed active role owner");
        if (failing) {
          ++active_failures;
        } else {
          ++active_successes;
        }
      }
    };
    std::cout << "ADAPTIVE_SERVICE evidence=WARP+MOCK Runtime_contract=mock_310.9.16.0 native_NGX=not_run\n";
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "adaptive service real queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "adaptive service real list hooks");
    real_close = reinterpret_cast<CloseFn>((*reinterpret_cast<void***>(gpu.list.Get()))[9]);
    real_reset = reinterpret_cast<ResetFn>((*reinterpret_cast<void***>(gpu.list.Get()))[addon::kCmdSlotReset]);
    Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR,
        "adaptive service lifecycle observer transaction");
    Check(DetourAttach(reinterpret_cast<PVOID*>(&real_close), reinterpret_cast<PVOID>(ObserveClose)) == NO_ERROR
        && DetourAttach(reinterpret_cast<PVOID*>(&real_reset), reinterpret_cast<PVOID>(ObserveReset)) == NO_ERROR,
        "adaptive service lifecycle observer attach");
    Check(DetourTransactionCommit() == NO_ERROR, "adaptive service lifecycle observer commit");
    const auto texture = [&](DXGI_FORMAT format) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = 16, .Height = 16, .DepthOrArraySize = 1, .MipLevels = 1,
          .Format = format, .SampleDesc = {1, 0}};
      ComPtr<ID3D12Resource> resource;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
          &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))), "adaptive service texture");
      gpu.Write(resource.Get(), 0);
      gpu.Transition(resource.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      return resource;
    };
    auto color = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT);
    gpu.Submit();
    fg::FrameInput input;
    input.context = 0xADAF6;
    input.epoch = 1;
    input.queue = gpu.queue.Get();
    input.backbuffer = color.Get();
    input.backbuffer_state = NATIVE_INPUT_STATE;
    input.depth = depth.Get();
    input.depth_state = NATIVE_INPUT_STATE;
    input.motion = motion.Get();
    input.motion_state = NATIVE_INPUT_STATE;
    input.color_width = input.color_height = input.guide_width = input.guide_height = 16;
    input.motion_scale_x = input.motion_scale_y = 16.f;
    input.has_constants = true;
    input.fg_enabled = true;
    input.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    const auto service = [&] {
      ++input.application_frame;
      const auto before_source = native::SourceSuccessfulEvaluations();
      observing_context = input.context;
      active_pass_masks = {};
      failed_pass_masks = {};
      reset_pass_masks = {};
      const auto before_list_calls = list_calls.size();
      const auto result = native::fg_service::Service(input);
      for (size_t index = before_list_calls; index < list_calls.size(); ++index) {
        const auto& call = list_calls[index];
        bool closed_twice = false;
        if (!call.reset) {
          for (size_t previous = index; previous > 0; --previous) {
            const auto& prior = list_calls[previous - 1];
            if (prior.list != call.list) continue;
            closed_twice = !prior.reset && SUCCEEDED(prior.result);
            break;
          }
        }
        if (FAILED(call.result) || closed_twice) {
          for (size_t trace = before_list_calls; trace < list_calls.size(); ++trace) {
            const auto& event = list_calls[trace];
            std::cerr << "ADAPTIVE_LIST frame=" << input.application_frame
                << " method=" << (event.reset ? "Reset" : "Close") << " list=" << event.list
                << " hresult=0x" << std::hex << static_cast<uint32_t>(event.result) << std::dec << '\n';
          }
          Check(false, "Service closes each recording once, after its successful Reset");
        }
      }
      uint64_t successful_active_roles = 0;
      uint32_t partial_roles = 0;
      for (size_t role = 0; role < active_pass_masks.size(); ++role) {
        if (active_pass_masks[role] == 0 || failed_pass_masks[role] != 0) continue;
        ++successful_active_roles;
        if (active_pass_masks[role] != (1u << native::stack_passes.load()) - 1u) {
          partial_roles |= 1u << role;
          ++partial_role_recordings;
        }
      }
      // SourceSuccessfulEvaluations counts active role invocations, including
      // a partially matured stack. It is not a complete application-frame count.
      if (native::SourceSuccessfulEvaluations() != before_source + successful_active_roles) {
        std::cerr << "ADAPTIVE_SOURCE frame=" << input.application_frame << " result=" << static_cast<int>(result)
            << " actual_delta=" << native::SourceSuccessfulEvaluations() - before_source
            << " expected_active_roles=" << successful_active_roles
            << " active_masks=" << active_pass_masks[0] << ',' << active_pass_masks[1]
            << " failed_masks=" << failed_pass_masks[0] << ',' << failed_pass_masks[1] << '\n';
      }
      Check(native::SourceSuccessfulEvaluations() == before_source + successful_active_roles,
          "source successes count independently observed successful active roles and exclude all preparation calls");
      gpu.Submit();
      native::DrainRetiredResources(true);
      auto& stream = native::fg_service::streams.at({false, input.context});
      std::cout << "ADAPTIVE_SERVICE frame=" << input.application_frame << " result=" << static_cast<int>(result)
          << " calls=" << model_calls << " failed=" << (stream.adaptive_session && stream.adaptive_session->failed)
          << " source_successes=" << native::SourceSuccessfulEvaluations() << " preparation_calls=" << preparation_calls
          << " active_role_invocations=" << successful_active_roles
          << " active_pass_masks=" << active_pass_masks[0] << ',' << active_pass_masks[1]
          << " partial_roles=" << partial_roles
          << " reason=" << (stream.adaptive_session ? addon::adaptive_resolution::ReasonText(stream.adaptive_session->status.reason) : "Off")
          << '\n';
      return result;
    };
    {
      real_create_pipeline = reinterpret_cast<CreatePipelineFn>((*reinterpret_cast<void***>(gpu.device.Get()))[11]);
      Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
          && DetourAttach(reinterpret_cast<PVOID*>(&real_create_pipeline), reinterpret_cast<PVOID>(ObserveCreatePipeline)) == NO_ERROR
          && DetourTransactionCommit() == NO_ERROR, "cold Service PSO observer attach");
      auto cold_hudless = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
      gpu.Submit();
      input.context = 0xADAF5;
      input.hudless = cold_hudless.Get();
      input.hudless_state = NATIVE_INPUT_STATE;
      native::nr_adaptive_enabled = true;
      const auto models_before = next_handle;
      const auto evaluates_before = model_calls;
      const auto worksets_before = native::worksets_created;
      // A rejected runtime must be distinguished from CPU pipeline pending,
      // and must be checked before using the global codec/device cache.
      auto* runtime_device = std::exchange(native::direct_device, nullptr);
      native::direct_runtime_failed = true;
      Check(service() == fg::Result::NOT_SERVED && pipeline_calls == 0,
          "runtime unavailability returns its original failure before any PSO creation");
      native::direct_device = runtime_device;
      native::direct_runtime_failed = false;
      for (uint32_t source = 0; source < 10; ++source) {
        const auto before = pipeline_calls;
        Check(service() == fg::Result::PREPARED && pipeline_calls == before + 1,
            "one dual-color two-Pass application source creates one actual PSO and remains unserved");
        const auto& cold = native::fg_service::streams.at({false, input.context});
        Check(next_handle == models_before && model_calls == evaluates_before
            && native::worksets_created == worksets_before && !cold.adaptive_session->prepared
            && cold.adaptive_model_roles == 0 && cold.adaptive_required_roles == 0
            && cold.last_model_frame[0] == 0 && cold.last_model_frame[1] == 0,
            "CPU pending never publishes model preparation, role completion, scratch or selected-tier readiness");
        for (const auto& role : cold.roles) {
          Check(!native::features.contains(role.Handle()), "CPU pending has no feature or tier completion receipt");
        }
        std::cout << "ADAPTIVE_PIPELINE_SERVICE source=" << source + 1 << " actual_PSO=" << pipeline_calls - before
            << " roles=2 Passes=2 models=0 evaluates=0 selected_worksets=0 ready=0\n";
      }
      auto primary = gpu.CreateTextureTransfer(color.Get());
      auto auxiliary = gpu.CreateTextureTransfer(cold_hudless.Get());
      for (const auto& [surface, readback] : {std::pair{color.Get(), &primary}, std::pair{cold_hudless.Get(), &auxiliary}}) {
        gpu.Transition(surface, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(surface, *readback);
        gpu.Transition(surface, D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
      }
      gpu.Submit();
      for (const auto* readback : {&primary, &auxiliary}) {
        const auto pixels = gpu.ReadTexture(*readback);
        Check(std::all_of(pixels.begin(), pixels.end(), [](uint8_t value) { return value == 0; }),
            "CPU-only pending leaves both actual game color surfaces unchanged");
      }
      for (uint32_t source = 0; source < 32; ++source) service();
      Check(native::fg_service::streams.at({false, input.context}).adaptive_session->prepared
          && model_calls > evaluates_before && pipeline_calls == 10,
          "both roles and Passes reach full ready with cache hits after the final cold PSO source");
      {
        auto ui = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
        gpu.Submit();
        native::fg_service::Stream coupled{.device = gpu.device.Get(), .queue = gpu.queue.Get()};
        native::fg_service::Slot coupled_slot;
        auto coupled_input = input;
        coupled_input.ui = ui.Get();
        addon::bridge::OwnedParameters composition;
        composition.Set(NVSDK_NGX_DLSSG_Parameter_UI, ui.Get());
        native::PresentTarget target;
        bool output_ready = false;
        std::array<native::NrRecording, 2> recordings;
        fail_next_pipeline = true;
        Check(native::fg_service::Record(&coupled, &coupled_slot, gpu.list.Get(), coupled_input, target,
                  &output_ready, &recordings, &composition, false) == fg::Result::PREPARED
            && !output_ready && coupled.adaptive_session->failed && coupled.color_pipeline == nullptr
            && pipeline_calls == 11 && native::codec_pipeline.preparation_failed,
            "coupled-color PSO failure latches its real stream after all generic pipelines are ready");
        // The already healthy Service is a different real Session. It can
        // clear the shared API latch without granting the failed stream a retry.
        service();
        Check(!native::codec_pipeline.preparation_failed && pipeline_calls == 11,
            "another healthy Session can use the complete generic cache without creating a PSO");
        const auto calls = model_calls;
        const auto models = next_handle;
        const auto scratch = native::worksets_created;
        for (uint32_t returned_source = 0; returned_source < 2; ++returned_source) {
          ++coupled_input.application_frame;
          Check(native::fg_service::Record(&coupled, &coupled_slot, gpu.list.Get(), coupled_input, target,
                    &output_ready, &recordings, &composition, false) == fg::Result::PREPARED
              && !output_ready && coupled_slot.pipeline_pending && coupled.adaptive_session->failed
              && coupled.color_pipeline == nullptr && pipeline_calls == 11
              && model_calls == calls && next_handle == models && native::worksets_created == scratch,
              "returning failed coupled source cannot retry its missing PSO through the generic healthy-upper exception");
          for (const auto& recording : recordings) {
            Check(!recording.prepared && !recording.model_invoked && !recording.model_recorded
                && !recording.output_ready && !recording.group_complete && recording.model_evaluate_mask == 0
                && recording.model_pass_mask == 0, "failed coupled preparation keeps every model/output field empty");
          }
        }
      }
      Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
          && DetourDetach(reinterpret_cast<PVOID*>(&real_create_pipeline), reinterpret_cast<PVOID>(ObserveCreatePipeline)) == NO_ERROR
          && DetourTransactionCommit() == NO_ERROR, "cold Service PSO observer detach");
      native::nr_adaptive_enabled = false;
      native::fg_service::Retire();
      gpu.Submit();
      native::DrainRetiredResources(true);
      native::ReleaseCodecPipeline();
      gpu.Transition(color.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
      gpu.Write(color.Get(), 0);
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      gpu.Submit();
      input.context = 0xADAF6;
      input.application_frame = 0;
      input.hudless = nullptr;
    }
    for (uint32_t frame = 0; frame < 8; ++frame) service();
    Check(service() == fg::Result::SUBMITTED, "manual two-Pass single Backbuffer matures");
    const auto* occupied_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADAF7});
    Check(native::EnsureWorkset(gpu.device.Get(), occupied_owner, color.Get(), 16, 16, 16, 16,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1.f, 0, false) != nullptr,
        "an unrelated stream can temporarily occupy one real workset");
    const auto before_occupied_features = native::nr_features_created;
    const auto before_occupied_worksets = native::worksets_created;
    native::nr_adaptive_enabled.store(true);
    service();
    auto& stream = native::fg_service::streams.at({false, input.context});
    auto cache = native::features.at(stream.roles[0].Handle()).adaptive_cache;
    Check(cache != nullptr && cache->count == 4 && cache->budget_blocked
        && stream.adaptive_session->status.reason == addon::adaptive_resolution::Reason::BUDGET
        && native::nr_features_created == before_occupied_features
        && native::worksets_created == before_occupied_worksets,
        "capacity pressure pauses the selected session before allocating an incomplete collection");
    native::RetireWorkset({occupied_owner, color.Get()});
    native::DrainRetiredResources(true);
    service();
    Check(native::nr_features_created == before_occupied_features && cache->budget_blocked,
        "free capacity alone cannot retry a budget-denied session every source frame");
    native::nr_adaptive_target_fps.store(61.f);
    service();
    Check(native::nr_features_created == before_occupied_features && cache->budget_blocked,
        "editing target FPS preserves the budget pause");
    native::nr_adaptive_enabled.store(false);
    service();
    native::nr_adaptive_enabled.store(true);
    native::nr_adaptive_target_fps.store(60.f);

    for (uint32_t frame = 0; frame < 16; ++frame) service();
    Check(stream.adaptive_session != nullptr && !stream.adaptive_session->failed,
        "turning adaptive On after manual two-Pass does not pause a healthy service");
    Check(masked_calls == model_calls && masked_calls != 0,
        "both Passes and standby first-evaluates use the actual GPU mask path");
    Check(preparation_calls != 0 && model_calls > native::SourceSuccessfulEvaluations() * 2,
        "additional standby model calls really execute while source successes remain active-frame only");
    cache = native::features.at(stream.roles[0].Handle()).adaptive_cache;
    native::PollAdaptiveCache(cache.get());
    for (size_t tier = 0; tier < cache->count; ++tier) {
      Check(cache->tiers[tier].ready,
          "every tier matures through complete two-Pass recording and actual WARP submission");
    }
    Check(stream.adaptive_session->status.applied[0].width == 16
        && stream.adaptive_session->status.applied[1].width == 16,
        "the active two-Pass Service still reports both full working sizes after preparation");
    native::nr_adaptive_enabled.store(false);
    Check(service() == fg::Result::SUBMITTED && stream.adaptive_session->status.applied[0].width == 16,
        "adaptive Off returns the same Service stream to its saved manual upper size");
    native::nr_adaptive_enabled.store(true);
    for (uint32_t frame = 0; frame < 16; ++frame) service();
    cache = native::features.at(stream.roles[0].Handle()).adaptive_cache;
    native::PollAdaptiveCache(cache.get());
    Check(!cache->failed && !stream.adaptive_session->failed
        && std::all_of(cache->tiers.begin(), cache->tiers.begin() + cache->count,
            [](const auto& tier) { return tier.ready; }),
        "Off then On rebuilds and completes the same stream's standby tiers");
    auto& standby = cache->tiers[1];
    Check(standby.state.control_mask_stream != nullptr,
        "standby tier has its own ordered ControlMask producer");
    standby.output_completions.clear();
    standby.ready = false;
    Check(addon::control_mask_execution::BeginRecording(standby.state.control_mask_stream, gpu.list.Get()),
        "standby preparation can be deferred by an earlier unsubmitted producer");
    service();
    Check(!stream.adaptive_session->failed && !cache->failed,
        "standby admission rejection after a successful active frame never latches NR failure");
    for (uint32_t frame = 0; frame < 8; ++frame) service();
    native::PollAdaptiveCache(cache.get());
    Check(standby.ready, "standby preparation resumes after its earlier producer submits");
    auto mask_stream = native::features.at(stream.roles[0].Handle()).control_mask_stream;
    Check(mask_stream != nullptr, "the mocked 310.9.16 contract enables the real ControlMask producer");
    Check(addon::control_mask_execution::BeginRecording(mask_stream, gpu.list.Get()),
        "a previous application recording can retain the History ordering turn before submission");
    const auto before_deferred = model_calls;
    service();
    Check(model_calls == before_deferred && !stream.adaptive_session->failed,
        "a temporary unsubmitted ControlMask producer neither invokes NR nor latches an execution failure");
    for (uint32_t frame = 0; frame < 16; ++frame) service();
    Check(service() == fg::Result::SUBMITTED && !stream.adaptive_session->failed,
        "the service resumes after the pending producer is submitted");
    auto failure_cache = native::features.at(stream.roles[0].Handle()).adaptive_cache;
    Check(failure_cache != nullptr && failure_cache->count > 1,
        "active-stream recovery exposes the current standby cache for failure injection");
    auto& failure_standby = failure_cache->tiers[(failure_cache->active + 1) % failure_cache->count];
    failure_standby.output_completions.clear();
    failure_standby.ready = false;
    const auto before_failure = model_calls;
    fail_call = model_calls + 3;  // Both active Passes succeed, then standby P1 fails.
    service();
    Check(model_calls == before_failure + 3 && stream.adaptive_session->failed && failure_cache->failed,
        "an actual standby Evaluate failure after both active Passes pauses the adaptive session");
    Check(preparation_failures == 1 && active_failures == 0,
        "the failed standby API call remains classified as a real preparation failure");
    fail_call = 0;
    const auto before_recreate = native::configuration_generation.load();
    const auto before_rebuild = native::nr_features_created;
    native::RecreateFeatures();
    Check(native::configuration_generation.load() == before_recreate + 1
        && native::features.at(stream.roles[0].Handle()).adaptive_cache == nullptr,
        "the production clear-failure action retires the failed adaptive cache");
    for (uint32_t frame = 0; frame < 24; ++frame) service();
    auto recovered_cache = native::features.at(stream.roles[0].Handle()).adaptive_cache;
    native::PollAdaptiveCache(recovered_cache.get());
    Check(service() == fg::Result::SUBMITTED && recovered_cache != failure_cache
        && !stream.adaptive_session->failed && !recovered_cache->failed
        && native::nr_features_created > before_rebuild
        && stream.adaptive_session->status.applied[0].width == 16
        && std::all_of(recovered_cache->tiers.begin(), recovered_cache->tiers.begin() + recovered_cache->count,
            [](const auto& tier) { return tier.ready; }),
        "explicit clear-failure rebuilds real Features and successfully resumes the existing Service");
    native::fg_service::Retire();
    gpu.Submit();
    native::DrainRetiredResources(true);
    auto hudless = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
    gpu.Submit();
    input.context = 0xADAF8;
    input.application_frame = 0;
    input.hudless = hudless.Get();
    input.hudless_state = NATIVE_INPUT_STATE;
    const auto before_partial_roles = partial_role_recordings;
    for (uint32_t frame = 0; frame < 24; ++frame) service();
    auto& dual = native::fg_service::streams.at({false, input.context});
    auto primary = native::features.at(dual.roles[0].Handle()).adaptive_cache;
    auto auxiliary = native::features.at(dual.roles[1].Handle()).adaptive_cache;
    Check(primary != nullptr && auxiliary != nullptr && primary->count == 2 && auxiliary->count == 2
        && primary->tiers[1].ready && auxiliary->tiers[1].ready,
        "two real color roles retain two GPU-ready coarse tiers within the original four-workset budget");
    Check(partial_role_recordings > before_partial_roles,
        "dual-color cold maturation observes partial Pass masks without treating them as a complete color group");
    const auto role_models = native::nr_features_created;
    const auto role_worksets = native::worksets_created;
    auto paused_primary_readback = gpu.CreateTextureTransfer(color.Get());
    auto paused_auxiliary_readback = gpu.CreateTextureTransfer(hudless.Get());
    const auto seed_paused_colors = [&] {
      for (auto* surface : {color.Get(), hudless.Get()}) {
        gpu.Transition(surface, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Write(surface, 0x7f);
        gpu.Transition(surface, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      }
      gpu.Submit();
    };
    const auto check_paused_colors = [&] {
      for (const auto& [surface, readback] : {std::pair{color.Get(), &paused_primary_readback},
               std::pair{hudless.Get(), &paused_auxiliary_readback}}) {
        gpu.Transition(surface, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(surface, *readback);
        gpu.Transition(surface, D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
      }
      gpu.Submit();
      for (const auto* readback : {&paused_primary_readback, &paused_auxiliary_readback}) {
        const auto pixels = gpu.ReadTexture(*readback);
        Check(std::all_of(pixels.begin(), pixels.end(), [](uint8_t value) { return value == 0x7f; }),
            "an unavailable role never writes a retained old NR color back to either game surface");
      }
    };
    seed_paused_colors();
    input.hudless = nullptr;
    const auto before_missing_calls = model_calls;
    const auto missing = service();
    // PREPARED is the Service recording contract; the selected color group is not ready.
    Check(missing == fg::Result::PREPARED
        && dual.adaptive_role_unavailable && !dual.adaptive_session->prepared
        && dual.adaptive_session->status.reason == addon::adaptive_resolution::Reason::UNSUPPORTED
        && !dual.adaptive_session->failed && model_calls == before_missing_calls
        && native::features.at(dual.roles[0].Handle()).adaptive_cache == primary
        && native::features.at(dual.roles[1].Handle()).adaptive_cache == auxiliary
        && native::nr_features_created == role_models && native::worksets_created == role_worksets,
        "a temporarily absent role closes the ready gate and reports input unavailability while retaining compatible allocations");
    check_paused_colors();
    input.hudless = hudless.Get();
    const auto short_resume_ns = native::SteadyNowNs();
    Check(service() == fg::Result::SUBMITTED && dual.adaptive_session->prepared
        && !dual.adaptive_role_unavailable && reset_pass_masks[0] == 3 && reset_pass_masks[1] == 3
        && dual.adaptive_session->status.ready_levels == 2
        && dual.adaptive_session->stable_after_ns >= short_resume_ns + addon::adaptive_resolution::WARMUP_NS
        && native::nr_features_created == role_models && native::worksets_created == role_worksets,
        "both retained colors and both Passes resume with Reset and one new warmup without additional preparation allocation");
    const auto short_resume_stable_ns = dual.adaptive_session->stable_after_ns;
    Check(service() == fg::Result::SUBMITTED && dual.adaptive_session->stable_after_ns == short_resume_stable_ns,
        "a subsequent healthy frame does not restart the resumed collection's warmup");

    seed_paused_colors();
    input.hudless = nullptr;
    Check(service() == fg::Result::PREPARED, "a second role interruption begins a retained pause");
    const auto paused_stable_ns = dual.adaptive_session->stable_after_ns;
    const auto paused_calls = model_calls;
    const auto paused_preparation_calls = preparation_calls;
    const auto paused_source_successes = native::SourceSuccessfulEvaluations();
    for (uint32_t frame = 0; frame <= native::kContractDebounceFrames * 8; ++frame) {
      Check(service() == fg::Result::PREPARED && dual.adaptive_role_unavailable
          && !dual.adaptive_session->prepared && !dual.adaptive_session->failed && !dual.adaptive_session->trial
          && dual.adaptive_session->status.reason == addon::adaptive_resolution::Reason::UNSUPPORTED
          && dual.adaptive_session->status.ready_levels == 0
          && dual.adaptive_session->stable_after_ns == paused_stable_ns
          && model_calls == paused_calls && preparation_calls == paused_preparation_calls
          && native::SourceSuccessfulEvaluations() == paused_source_successes
          && native::features.at(dual.roles[0].Handle()).adaptive_cache == primary
          && native::features.at(dual.roles[1].Handle()).adaptive_cache == auxiliary
          && native::nr_features_created == role_models && native::worksets_created == role_worksets,
          "persistent missing input stays unavailable beyond 240 frames without execution, preparation, allocation, or warmup refresh");
    }
    check_paused_colors();
    input.hudless = hudless.Get();
    const auto long_resume_ns = native::SteadyNowNs();
    Check(service() == fg::Result::SUBMITTED && dual.adaptive_session->prepared
        && !dual.adaptive_role_unavailable && !dual.adaptive_session->failed
        && reset_pass_masks[0] == 3 && reset_pass_masks[1] == 3
        && dual.adaptive_session->status.ready_levels == 2
        && dual.adaptive_session->stable_after_ns >= long_resume_ns + addon::adaptive_resolution::WARMUP_NS
        && native::nr_features_created == role_models && native::worksets_created == role_worksets,
        "a long missing-input pause resumes both complete color/Pass groups with Reset and retained allocations");
    const auto long_resume_stable_ns = dual.adaptive_session->stable_after_ns;
    Check(service() == fg::Result::SUBMITTED && dual.adaptive_session->stable_after_ns == long_resume_stable_ns,
        "continuous healthy output after a long pause preserves the resumed warmup deadline");

    const auto dual_models = native::nr_features_created;
    const auto dual_worksets = native::worksets_created;
    for (const auto* setting : {&native::kSetPassStructure[0], &native::kSetPassSkin[0], &native::kSetPassFollow[0]}) {
      addon::gpu_comparison::Identity before, resources_before, after, resources_after;
      native::comparison_ui::CaptureSettings(&before);
      native::comparison_ui::CaptureAdaptiveSettings(&resources_before);
      const auto saved = setting->Get();
      setting->Store(saved == 0.f ? 1.f : 0.f);
      native::comparison_ui::CaptureSettings(&after);
      native::comparison_ui::CaptureAdaptiveSettings(&resources_after);
      Check(before.settings_hash != after.settings_hash && resources_before.settings_hash == resources_after.settings_hash,
          "per-Pass steering aliases and model-steering inheritance remain in measurements but preserve allocation identity");
      setting->Store(saved);
    }
    ComPtr<ID3D12CommandAllocator> held_allocator;
    ComPtr<ID3D12GraphicsCommandList> held_list;
    Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&held_allocator))), "dual-color held allocator");
    Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        held_allocator.Get(), nullptr, IID_PPV_ARGS(&held_list))), "dual-color held list");
    Check(native::InstallCommandListStateHooks(held_list.Get()), "dual-color held-list hooks");
    const auto held_model = auxiliary->tiers[1].state.slots[0].handle;
    addon::submission::TrackUse(held_list.Get(), held_model);
    native::kSetAdaptiveMinScale.Store(.5f);
    service();
    Check(!dual.adaptive_session->prepared && !dual.adaptive_session->trial
        && primary->active == 0 && auxiliary->active == 0,
        "a missing selected endpoint closes the whole-group gate before automatic switching");
    for (uint32_t frame = 0; frame < 20; ++frame) service();
    Check(primary->tiers[1].ready && auxiliary->tiers[1].ready && dual.adaptive_session->prepared
        && dual.adaptive_session->status.ready_levels == 2
        && primary->ladder.sizes[1] == dual.adaptive_session->minimum
        && auxiliary->ladder.sizes[1] == dual.adaptive_session->minimum
        && primary->active == 0 && auxiliary->active == 0
        && native::nr_features_created == dual_models + 4 && native::worksets_created == dual_worksets + 2
        && !addon::submission::ResourceReleasable(held_model),
        "new independent complete-size roles become ready while exact lifetime proof retains an unrelated retired old model");
    dual.adaptive_session->Request(1, native::SteadyNowNs());
    Check(service() == fg::Result::SUBMITTED && primary->active == 1 && auxiliary->active == 1
        && dual.adaptive_session->status.applied[0].width == 8 && dual.adaptive_session->status.applied[1].width == 8
        && native::nr_features_created == dual_models + 4,
        "both colors and both Passes switch to the already resident endpoint as one application frame");
    Check(SUCCEEDED(held_list->Close()) && SUCCEEDED(held_allocator->Reset())
        && SUCCEEDED(held_list->Reset(held_allocator.Get(), nullptr)),
        "the fixture ends the old auxiliary lifetime without lending its receipt to the replacement");
    native::kSetAdaptiveMinScale.Store(.8f);
    service();
    Check(primary->active == 0 && auxiliary->active == 0
        && dual.adaptive_session->status.applied[0].width == 16 && !dual.adaptive_session->failed,
        "a new minimum restores the complete legal upper group while the new selected endpoint prepares");

    native::kSetPassPreset[0].Store(1.f);
    native::ApplySettingEffect(native::kSetPassPreset[0].effect);
    Check(native::features.at(dual.roles[0].Handle()).adaptive_cache == nullptr
        && native::features.at(dual.roles[1].Handle()).adaptive_cache == nullptr,
        "a changed Create model contract retires both color-role caches");
    for (uint32_t frame = 0; frame < 20; ++frame) service();
    Check(native::nr_features_created > dual_models + 4 && !dual.adaptive_session->failed
        && native::features.at(dual.roles[0].Handle()).adaptive_cache != primary
        && native::features.at(dual.roles[1].Handle()).adaptive_cache != auxiliary,
        "both colors rebuild clean model contracts after the explicit model-selection change");
    const auto previous_primary = native::features.at(dual.roles[0].Handle()).adaptive_cache;
    input.hudless = nullptr;
    Check(service() == fg::Result::PREPARED && dual.adaptive_role_unavailable
        && dual.adaptive_session->status.reason == addon::adaptive_resolution::Reason::UNSUPPORTED,
        "a real contract transition starts while the old required role is unavailable");
    ++input.epoch;
    for (uint32_t frame = 0; frame < 24; ++frame) service();
    Check(!dual.adaptive_role_unavailable && dual.epoch == input.epoch
        && dual.adaptive_session != nullptr && dual.adaptive_session->prepared
        && dual.adaptive_required_roles == 1 && !dual.adaptive_session->failed
        && native::features.at(dual.roles[0].Handle()).adaptive_cache != previous_primary,
        "a real epoch change retires the unavailable old contract and prepares the new single-role group");
    const auto epoch_primary = native::features.at(dual.roles[0].Handle()).adaptive_cache;
    input.color_width = input.color_height = 8;
    for (uint32_t frame = 0; frame < 24; ++frame) service();
    auto& changed = native::fg_service::streams.at({false, input.context});
    Check(changed.adaptive_session != nullptr && changed.adaptive_session->prepared
        && changed.adaptive_required_roles == 1
        && !changed.adaptive_role_unavailable
        && native::features.at(changed.roles[0].Handle()).adaptive_cache != epoch_primary
        && !changed.adaptive_session->failed,
        "a real same-epoch color geometry change prepares its new contract instead of waiting for an obsolete role");

    native::fg_service::Retire();
    gpu.Submit();
    native::DrainRetiredResources(true);
    native::nr_adaptive_selected_levels.store(5);
    native::nr_adaptive_min_scale.store(.33f);
    native::kSetPassPreset[0].Store(2.f);
    input.context = 0xADAFA;
    input.application_frame = 0;
    input.color_width = input.color_height = 16;
    input.hudless = hudless.Get();
    const auto before_many_models = native::nr_features_created;
    const auto before_many_worksets = native::worksets_created;
    for (uint32_t preparation = 0; preparation < 80; ++preparation) {
      service();
      auto& preparing = native::fg_service::streams.at({false, input.context});
      if (preparing.adaptive_session != nullptr && preparing.adaptive_session->prepared) break;
      for (auto& role : preparing.roles) {
        if (const auto feature = native::features.find(role.Handle()); feature != native::features.end()
            && feature->second.adaptive_cache != nullptr) {
          Check(feature->second.adaptive_cache->active == 0 && !preparing.adaptive_session->trial,
              "neither color requests an unready member of the complete five-level group");
        }
      }
    }
    auto& many = native::fg_service::streams.at({false, input.context});
    Check(many.adaptive_session->prepared && native::adaptive_resident_worksets == 10
        && native::EffectiveAdaptiveWorksetLimit() == 14 && native::max_worksets.load() == 4
        && native::AdaptiveResidentWorksetCount(many.adaptive_session) == 10
        && native::nr_features_created == before_many_models + 20
        && native::worksets_created == before_many_worksets + 10,
        "five complete-size levels reserve both real color worksets and both independent model Passes per role");
    auto primary_readback = gpu.CreateTextureTransfer(color.Get());
    auto auxiliary_readback = gpu.CreateTextureTransfer(hudless.Get());
    const auto many_models = native::nr_features_created;
    const auto many_worksets = native::worksets_created;
    for (size_t level = 0; level < many.adaptive_session->ladder.count; ++level) {
      for (auto* surface : {color.Get(), hudless.Get()}) {
        gpu.Transition(surface, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Write(surface, 0x7f);
        gpu.Transition(surface, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
      }
      gpu.Submit();
      many.adaptive_session->Request(level, native::SteadyNowNs());
      Check(service() == fg::Result::SUBMITTED && many.fence->GetCompletedValue() != UINT64_MAX
          && many.fence->GetCompletedValue() >= many.submitted && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
          "every five-level color pair finishes its actual WARP submission with a healthy Fence");
      for (auto& role : many.roles) {
        const auto& feature = native::features.at(role.Handle());
        Check(feature.adaptive_cache->active == level
            && feature.input_width == many.adaptive_session->ladder.sizes[level][0].width
            && feature.slots[1].create_input_width == many.adaptive_session->ladder.sizes[level][1].width,
            "both roles use the selected first- and second-Pass working dimensions");
      }
      gpu.Transition(color.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.RecordTextureReadback(color.Get(), primary_readback);
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
      gpu.Transition(hudless.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.RecordTextureReadback(hudless.Get(), auxiliary_readback);
      gpu.Transition(hudless.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, NATIVE_INPUT_STATE);
      gpu.Submit();
      for (const auto* readback : {&primary_readback, &auxiliary_readback}) {
        const auto pixels = gpu.ReadTexture(*readback);
        bool changed = false;
        for (size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
          changed |= pixels[pixel] != 0x7f || pixels[pixel + 1] != 0x7f || pixels[pixel + 2] != 0x7f;
        }
        Check(changed, "each role writes actual GPU model output back at every selected level");
      }
    }
    Check(native::nr_features_created == many_models && native::worksets_created == many_worksets,
        "five-level paired steady switches require no new Feature or working surfaces");
    native::nr_adaptive_selected_levels.store(0);
    Check(service() == fg::Result::SUBMITTED && many.adaptive_session->prepared
        && many.adaptive_session->ladder.count == 2 && !many.adaptive_session->failed
        && many.adaptive_session->preparation_reason != addon::adaptive_resolution::Reason::BUDGET
        && native::AdaptiveResidentWorksetCount(many.adaptive_session) == 4
        && native::EffectiveAdaptiveWorksetLimit() == 4
        && native::nr_features_created == many_models && native::worksets_created == many_worksets,
        "five-to-Default reconciles both role selections before budget adjudication and reuses the retained endpoints");
    input.hudless = nullptr;
    Check(service() == fg::Result::PREPARED && many.adaptive_role_unavailable,
        "the selected collection is paused for missing input before Off");
    native::nr_adaptive_enabled.store(false);
    Check(service() == fg::Result::SUBMITTED && !many.adaptive_role_unavailable
        && native::adaptive_resident_session.expired()
        && native::adaptive_resident_worksets == 0
        && native::features.at(many.roles[0].Handle()).adaptive_cache == nullptr
        && native::features.at(many.roles[1].Handle()).adaptive_cache == nullptr,
        "Off releases both selected-role reservations after the larger collection");
    std::cout << "ADAPTIVE_SELECTED evidence=WARP+MOCK levels=5 roles=2 Passes=2 Features=20 selected_worksets=10 ordinary_limit=4 steady_allocations=0 native_NGX=not_run\n";

    model_parameter_observer = {};
    native::fg_service::Retire();
    gpu.Submit();
    native::DrainRetiredResources(true);
    Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR,
        "adaptive service lifecycle observer detach transaction");
    Check(DetourDetach(reinterpret_cast<PVOID*>(&real_close), reinterpret_cast<PVOID>(ObserveClose)) == NO_ERROR
        && DetourDetach(reinterpret_cast<PVOID*>(&real_reset), reinterpret_cast<PVOID>(ObserveReset)) == NO_ERROR,
        "adaptive service lifecycle observer detach");
    Check(DetourTransactionCommit() == NO_ERROR, "adaptive service lifecycle observer detach commit");
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "adaptive service debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          std::cerr << "ADAPTIVE_DEBUG message_id=" << message->ID << " severity=" << message->Severity
              << " description=" << message->pDescription << '\n';
        }
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "PASS adaptive single/dual-color two-Pass Service evidence=WARP+MOCK frames=" << input.application_frame
        << " source_successes=" << native::SourceSuccessfulEvaluations() << " model_calls=" << model_calls
        << " preparation_calls=" << preparation_calls << " preparation_failures=" << preparation_failures << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "adaptive-fg-service: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace adaptive_fg_service_cases
