/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <atomic>
#include <exception>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace adaptive_inline_cases {
namespace addon = renodx::addons::dlss5;

using CreatePipelineFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
CreatePipelineFn real_create_pipeline = nullptr;
std::atomic_uint32_t pipeline_calls = 0;
std::atomic_uint32_t fail_pipeline_call = 0;
std::atomic<HANDLE> async_entered = nullptr;
std::atomic<HANDLE> async_release = nullptr;
std::atomic<HANDLE> async_worker_thread = nullptr;
std::atomic_uint32_t async_pipeline_calls = 0;
std::atomic_uint32_t async_band_calls = 0;
std::atomic<ID3D12RootSignature*> async_pipeline_root = nullptr;
std::atomic_uint32_t async_threads_started = 0;
std::atomic_uint32_t async_threads_joined = 0;
std::atomic_bool fail_async_pipeline = false;
std::atomic_bool async_gate_failed = false;
using CreateThreadFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
CreateThreadFn real_create_thread = CreateThread;

HANDLE WINAPI ObserveCreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack_size,
    LPTHREAD_START_ROUTINE entry, LPVOID argument, DWORD flags, LPDWORD id) {
  HANDLE thread = real_create_thread(attributes, stack_size, entry, argument, flags, id);
  if (thread != nullptr && entry == native::CreateAsyncAutoscalePipeline) {
    ++async_threads_started;
    HANDLE proof = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &proof, SYNCHRONIZE, FALSE, 0)) {
      async_gate_failed = true;
    }
    async_worker_thread.store(proof);
  }
  return thread;
}

struct AsyncPipelineGate {
  HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool thread_observer_attached = false;

  AsyncPipelineGate() {
    async_entered.store(entered);
    async_release.store(release);
  }

  bool Finish() {
    if (release != nullptr) SetEvent(release);
    const HANDLE worker = async_worker_thread.exchange(nullptr);
    if (worker == nullptr) return !async_gate_failed.load();
    const DWORD wait = WaitForSingleObject(worker, 15000);
    if (wait != WAIT_OBJECT_0) {
      async_worker_thread.store(worker);
      return false;
    }
    CloseHandle(worker);
    ++async_threads_joined;
    return true;
  }

  void Reset() {
    Check(Finish(), "private PSO previous real worker exited");
    Check(ResetEvent(entered) && ResetEvent(release), "private PSO event reset");
  }

  ~AsyncPipelineGate() {
    native::async_autoscale_preparation = false;
    native::CancelAsyncAutoscalePreparation();
    const bool finished = Finish();
    if (finished) {
      {
        addon::RuntimeLock lock(native::runtime_mutex);
        native::DiscardAsyncAutoscalePreparation();
      }
      async_entered.store(nullptr);
      async_release.store(nullptr);
      if (entered != nullptr) CloseHandle(entered);
      if (release != nullptr) CloseHandle(release);
      if (thread_observer_attached) {
        const bool detached = DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_create_thread), reinterpret_cast<PVOID>(ObserveCreateThread)) == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR;
        if (!detached) std::cerr << "ADAPTIVE_ASYNC thread_observer_detach_failed=1\n";
      }
    } else {
      std::cerr << "ADAPTIVE_ASYNC worker_cleanup_timeout=1 events_kept_open=1\n";
    }
  }
};

HRESULT STDMETHODCALLTYPE ObserveCreatePipeline(ID3D12Device* device,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** pipeline) {
  const auto call = ++pipeline_calls;
  const bool autoscale = desc->CS.BytecodeLength == __v6_autoscale.size()
      && std::memcmp(desc->CS.pShaderBytecode, __v6_autoscale.data(), __v6_autoscale.size()) == 0;
  const bool look_band = desc->CS.BytecodeLength == __look_band.size()
      && std::memcmp(desc->CS.pShaderBytecode, __look_band.data(), __look_band.size()) == 0;
  if (async_release.load() != nullptr && (autoscale || look_band)) {
    ++async_pipeline_calls;
    if (look_band) {
      ++async_band_calls;
    }
    async_pipeline_root.store(desc->pRootSignature);
    SetEvent(async_entered.load());
    if (WaitForSingleObject(async_release.load(), 10000) != WAIT_OBJECT_0) {
      async_gate_failed = true;
      *pipeline = nullptr;
      return E_ABORT;
    }
    if (fail_async_pipeline.exchange(false)) {
      *pipeline = nullptr;
      return E_FAIL;
    }
  }
  if (call == fail_pipeline_call.load()) {
    *pipeline = nullptr;
    return E_FAIL;
  }
  return real_create_pipeline(device, desc, iid, pipeline);
}

using CreateResourceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
CreateResourceFn real_create_resource = nullptr;
uint32_t fail_resource_phase = 0;

HRESULT STDMETHODCALLTYPE ObserveCreateResource(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, REFIID iid, void** resource) {
  if (native::AdaptivePreparationActive()
      && ((fail_resource_phase == 1 && native::preparing_adaptive_frame == nullptr)
          || (fail_resource_phase == 2 && native::preparing_adaptive_frame != nullptr
              && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER))) {
    fail_resource_phase = 0;
    *resource = nullptr;
    return E_OUTOFMEMORY;
  }
  return real_create_resource(device, heap, flags, desc, state, clear, iid, resource);
}

int Run() {
  try {
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(false, 16, 16);
    model_gpu = &gpu;
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    Check(debug != nullptr && info != nullptr, "adaptive inline requires the D3D12 debug layer and InfoQueue");
    std::cout << "ADAPTIVE_INLINE debug_layer=1 info_queue=1\n";
    native::enabled = true;
    native::hooks_enabled = true;
    Check(native::dedupe_mode.load() == native::defaults::kDedupeMode,
        "inline adaptive uses the production default deduplication policy");
    native::cost_meter_enabled = false;
    native::gpu_timers_enabled = false;
    native::stack_passes = 1;
    native::nr_follow_input_res = false;
    native::nr_resolution_scale = 1.f;
    native::codec_mode = 0;
    native::feed_mode = 0;
    native::look_mode = false;
    native::detail_stability = 0;
    native::chained_temporal_history = true;
    native::transfer_strength = 1.f;
    native::color_strength = 1.f;
    native::control_mask_enabled = true;
    native::nr_adaptive_selected_levels = 0;
    native::nr_adaptive_min_scale = .33f;
    native::nr_adaptive_target_fps = 116.f;
    Check(native::adaptive_feature_mode.load() == native::AdaptiveFeatureMode::OFF, "Feature capacity experiment defaults to disabled");
    Check(!native::nr_adaptive_limited_residency.load(), "limited residency experiment defaults to disabled");
    native::max_worksets = 4;
    native::OnNativeFgOptions(false);
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
    native::successful_sr_device = gpu.device.Get();
    native::direct_runtime_requirements.dual_models = true;
    native::direct_runtime_requirements.file_version = "310.9.16.0";
    native::parameter_runtime_ready = true;
    native::core_allocate_parameters = addon::bridge::AllocateOwnedParameters;
    native::core_destroy_parameters = addon::bridge::DestroyOwnedParameters;
    Check(native::InstallQueueCompletionHooks(gpu.queue.Get()), "inline adaptive real queue hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "inline adaptive real list hooks");
    const auto texture = [&](DXGI_FORMAT format, bool writable, uint32_t size = 16) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = size, .Height = size, .DepthOrArraySize = 1, .MipLevels = 1,
          .Format = format, .SampleDesc = {1, 0},
          .Flags = (writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
      ComPtr<ID3D12Resource> resource;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
          &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))), "inline adaptive texture");
      gpu.Write(resource.Get(), 0);
      gpu.Transition(resource.Get(), D3D12_RESOURCE_STATE_COMMON,
          writable ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : NATIVE_INPUT_STATE);
      return resource;
    };
    std::array colors{texture(DXGI_FORMAT_R8G8B8A8_UNORM, false), texture(DXGI_FORMAT_R8G8B8A8_UNORM, false)};
    std::array outputs{texture(DXGI_FORMAT_R8G8B8A8_UNORM, true), texture(DXGI_FORMAT_R8G8B8A8_UNORM, true)};
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, false);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT, false);
    gpu.Submit();
    const auto submit = [&](bool reset = true) {
      Check(SUCCEEDED(gpu.list->Close()), "inline adaptive recording close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "inline adaptive Fence signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "inline adaptive Fence event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = (SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0, "inline adaptive bounded GPU completion");
      if (reset) {
        Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
            "inline adaptive completed recording reset");
      }
      native::DrainRetiredResources(true);
      addon::gpu_timers::Poll(true, false);
      gpu.uploads.clear();
    };
    addon::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    parameters.Set(NVSDK_NGX_Parameter_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 16u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 16u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
    // A genuinely cold adaptive source, before any manual NR warm-up. Count
    // the real WARP device API; the only mocked failure is one explicit PSO.
    {
      real_create_pipeline = reinterpret_cast<CreatePipelineFn>((*reinterpret_cast<void***>(gpu.device.Get()))[11]);
      Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
          && DetourAttach(reinterpret_cast<PVOID*>(&real_create_pipeline), reinterpret_cast<PVOID>(ObserveCreatePipeline)) == NO_ERROR
          && DetourTransactionCommit() == NO_ERROR, "cold inline PSO observer attach");
      auto cold_color = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, false);
      auto cold_output = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, true);
      parameters.Set(NVSDK_NGX_Parameter_Color, cold_color.Get());
      parameters.Set(NVSDK_NGX_Parameter_Output, cold_output.Get());
      parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
      native::hook_point = native::kHookUpscaled;
      native::stack_passes = 2;
      native::nr_adaptive_enabled = true;
      const auto* cold_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA080});
      native::RegisterCreatedFeatureLocked(cold_handle, native::kFeatureDlss, &parameters);
      const auto models_before = next_handle;
      const auto evaluates_before = model_calls;
      const auto worksets_before = native::worksets_created;
      uint32_t cold_sources = 0;
      const auto cold_record = [&](bool models_only = false) {
        ++cold_sources;
        ++native::present_generation;
        const auto pipelines_before = pipeline_calls.load();
        native::NrRecording recording;
        bool accepted = false;
        {
          const addon::EvaluateChainScope chain;
          const addon::SourceStageScope source(cold_handle, true);
          const native::PipelinePreparationScope opportunity;
          accepted = native::ProcessInline(gpu.list.Get(), cold_handle, &parameters, nullptr, nullptr, &recording, models_only);
          if (!accepted && native::enabled.load()
              && (pipeline_calls != pipelines_before || native::codec_pipeline.preparation_failed)) {
            native::NrRecording second;
            Check(!native::ProcessInline(gpu.list.Get(), cold_handle, &parameters, nullptr, nullptr, &second, true)
                && !second.prepared && !second.model_invoked && !second.model_recorded && !second.output_ready
                && !second.group_complete && second.model_evaluate_mask == 0 && second.model_pass_mask == 0,
                "model-only recursion cannot bypass the shared cold pipeline gate");
          }
        }
        Check(pipeline_calls - pipelines_before <= 1, "one cold source creates at most one actual compute pipeline");
        if (pipeline_calls != pipelines_before || native::features.at(cold_handle).adaptive_session->failed) {
          Check(!accepted && !recording.prepared && !recording.model_invoked && !recording.model_recorded
              && !recording.output_ready && !recording.group_complete && recording.model_evaluate_mask == 0
              && recording.model_pass_mask == 0 && next_handle == models_before && model_calls == evaluates_before
              && native::worksets_created == worksets_before && !native::features.at(cold_handle).adaptive_session->prepared,
              "pipeline pending/failure has zero model creation, evaluation, scratch allocation and ready publication");
        }
        std::cout << "ADAPTIVE_PIPELINE source=" << cold_sources << " actual_PSO=" << pipeline_calls - pipelines_before
            << " models=" << next_handle - models_before << " evaluates=" << model_calls - evaluates_before
            << " worksets=" << native::worksets_created - worksets_before << " accepted=" << accepted << '\n';
        submit();
        return accepted;
      };
      cold_record();
      cold_record();
      const auto* first_pipeline = native::codec_pipeline.linearize;
      fail_pipeline_call = pipeline_calls + 1;
      cold_record();
      Check(native::codec_pipeline.preparation_failed && native::features.at(cold_handle).adaptive_session->failed,
          "one actual PSO API failure latches the current preparation");
      const auto first_failure_calls = pipeline_calls.load();
      const auto* second_source = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA081});
      native::RegisterCreatedFeatureLocked(second_source, native::kFeatureDlss, &parameters);
      {
        const addon::EvaluateChainScope chain;
        const addon::SourceStageScope source(second_source, true);
        ++native::present_generation;
        Check(!native::ProcessInline(gpu.list.Get(), second_source, &parameters)
            && !native::features.at(second_source).adaptive_session->failed && pipeline_calls == first_failure_calls + 1,
            "a new real source Session can resume one bounded PSO preparation opportunity");
      }
      cold_record();
      Check(pipeline_calls == first_failure_calls + 1,
          "returning to the old failed source cannot advance or repeatedly retry the new owner's preparation");
      native::ReleaseFeatureState(native::features.at(second_source));
      native::features.erase(second_source);
      const auto failed_calls = pipeline_calls.load();
      native::nr_adaptive_target_fps = 115.f;
      for (uint32_t repeat = 0; repeat < 3; ++repeat) cold_record();
      Check(pipeline_calls == failed_calls && native::codec_pipeline.linearize == first_pipeline,
          "FPS edits and repeated sources neither retry failure nor release successful partial pipelines");
      native::nr_adaptive_enabled = false;
      cold_record();
      native::nr_adaptive_enabled = true;
      cold_record();
      Check(!native::codec_pipeline.preparation_failed && pipeline_calls == failed_calls + 1,
          "an explicit adaptive Off/On permits one bounded preparation retry");
      native::nr_adaptive_enabled = false;
      cold_record();
      Check(pipeline_calls == failed_calls + 2, "turning automatic sizing off cannot synchronously fill remaining pipelines");
      native::enabled = false;
      cold_record();
      Check(pipeline_calls == failed_calls + 2, "NR Off stops all pipeline preparation immediately");
      native::enabled = true;
      native::nr_adaptive_enabled = true;
      for (uint32_t step = 0; step < 16 && pipeline_calls < 15; ++step) cold_record();
      // Nine v6 + four look + one ControlMask; one additional API attempt failed.
      Check(pipeline_calls == 15 && next_handle == models_before && model_calls == evaluates_before,
          "the complete fourteen-PSO family finishes before the first NGX Create/Evaluate");
      for (uint32_t step = 0; step < 32; ++step) cold_record();
      Check(native::features.at(cold_handle).adaptive_session->prepared && model_calls > evaluates_before
          && pipeline_calls == 15, "ready two-Pass operation reuses every prepared pipeline");
      {
        Check(!native::async_autoscale_preparation.load(), "private autoscale experiment defaults to disabled");
        AsyncPipelineGate gate;
        Check(gate.entered != nullptr && gate.release != nullptr, "private PSO deterministic events");
        Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_create_thread), reinterpret_cast<PVOID>(ObserveCreateThread)) == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR, "private PSO actual worker thread exit observer attach");
        gate.thread_observer_attached = true;
        ComPtr<ID3D12PipelineState> synchronous_autoscale = native::codec_pipeline.autoscale;
        const auto original_session = native::features.at(cold_handle).adaptive_session;
        addon::adaptive_resolution::Frame async_frame{
            .session = original_session, .preparation_epoch = original_session->preparation_epoch, .enabled = true};
        const auto pipeline_source = [&] {
          addon::RuntimeLock lock(native::runtime_mutex);
          const native::PipelinePreparationScope opportunity;
          const auto result = native::PrepareAdaptivePipelines(gpu.device.Get(), async_frame, 1, true);
          return std::pair{result, native::pipeline_preparation_opportunity->created};
        };
        const auto wait_entered = [&] {
          Check(WaitForSingleObject(gate.entered, 10000) == WAIT_OBJECT_0
              && async_worker_thread.load() != nullptr, "private PSO production Start created a real worker");
        };
        const auto meter = std::find_if(native::worksets.begin(), native::worksets.end(), [&](const auto& entry) {
          return entry.first.handle == cold_handle && entry.second.norm_scale != nullptr && entry.second.norm_commit != nullptr;
        });
        Check(meter != native::worksets.end(), "private PSO equivalence uses the existing real HDR workset");
        const auto sample_autoscale = [&](ID3D12PipelineState* pipeline) {
          auto& resources = meter->second;
          const auto previous_state = resources.norm_scale_state;
          native::Transition(gpu.list.Get(), resources.norm_scale, previous_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          native::UavBarrier(gpu.list.Get(), resources.norm_commit);
          native::BindCodecV6(gpu.list.Get(), resources, pipeline,
              native::kDescriptorMeterSet, native::kDescriptorNormScaleUav,
              1, 1, resources.width, resources.height, 0, 0, 1, 1, 1, 1, 0.f, 0.f, 0.f, 0.f, 1u, 0.f, 0.f);
          const float snap = 1.f;
          gpu.list->SetComputeRoot32BitConstants(2, 1, &snap,
              offsetof(native::V6CodecConstants, snap_now) / sizeof(uint32_t));
          gpu.list->Dispatch(1, 1, 1);
          auto transfer = gpu.CreateTextureTransfer(resources.norm_scale);
          native::Transition(gpu.list.Get(), resources.norm_scale,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          gpu.RecordTextureReadback(resources.norm_scale, transfer);
          native::Transition(gpu.list.Get(), resources.norm_scale, D3D12_RESOURCE_STATE_COPY_SOURCE, previous_state);
          submit();
          return gpu.ReadTexture(transfer);
        };
        const auto synchronous_pixels = sample_autoscale(synchronous_autoscale.Get());
        Check(std::any_of(synchronous_pixels.begin(), synchronous_pixels.end(), [](uint8_t value) { return value != 0; }),
            "synchronous autoscale comparison dispatched and returned nonzero scale data");
        native::async_autoscale_preparation = true;
        native::ReleaseCom(native::codec_pipeline.autoscale);
        Check(pipeline_source().first == native::PipelinePreparationResult::PENDING
            && native::async_autoscale_task != nullptr && async_pipeline_calls == 0,
            "a source registers only the private target and waits for the existing Present start boundary");
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        const auto waiting_models = next_handle;
        const auto waiting_evaluates = model_calls;
        const auto waiting_worksets = native::worksets_created;
        {
          const addon::EvaluateChainScope chain;
          const addon::SourceStageScope source(cold_handle, true);
          native::NrRecording recording;
          Check(!native::ProcessInline(gpu.list.Get(), cold_handle, &parameters, nullptr, nullptr, &recording)
              && !recording.model_invoked && !recording.output_ready
              && next_handle == waiting_models && model_calls == waiting_evaluates
              && native::worksets_created == waiting_worksets
              && WaitForSingleObject(gate.release, 0) == WAIT_TIMEOUT && !async_gate_failed.load(),
              "a real blocked PSO worker leaves source pending with zero NGX/scratch work and no source wait");
        }
        submit();
        Check(gate.Finish(), "private PSO real worker exited before result admission");
        const auto accepted = pipeline_source();
        auto* accepted_pipeline = native::codec_pipeline.autoscale;
        Check(accepted.first == native::PipelinePreparationResult::PENDING && accepted.second
            && accepted_pipeline != nullptr && async_pipeline_calls == 1,
            "one successful private result consumes its source opportunity before any NGX Create");
        Check(pipeline_source().first == native::PipelinePreparationResult::READY
            && native::codec_pipeline.autoscale == accepted_pipeline && async_pipeline_calls == 1
            && next_handle == waiting_models && model_calls == waiting_evaluates,
            "the result is accepted once and the next source sees a complete unchanged pipeline cache");
        Check(sample_autoscale(accepted_pipeline) == synchronous_pixels,
            "private and synchronous PSOs use the exact embedded shader and produce identical WARP autoscale pixels");

        gate.Reset();
        native::ReleaseCom(native::codec_pipeline.autoscale);
        async_frame.enabled = false;
        async_frame.session.reset();
        native::codec_pipeline.preparation_session.reset();
        Check(pipeline_source().first == native::PipelinePreparationResult::PENDING, "manual upper can register without a Session owner");
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        Check(gate.Finish(), "manual-upper private worker exits");
        pipeline_source();
        Check(pipeline_source().first == native::PipelinePreparationResult::READY,
            "disabling only Adaptive cannot strand the legal manual upper behind a missing weak Session");
        gate.Reset();
        native::ReleaseCom(native::codec_pipeline.autoscale);
        pipeline_source();
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        fail_async_pipeline = true;
        Check(gate.Finish(), "ownerless failure worker actually exits");
        Check(pipeline_source().first == native::PipelinePreparationResult::FAILED
            && native::codec_pipeline.preparation_failed && async_frame.session == nullptr,
            "an ownerless manual preparation failure latches its device attempt without dereferencing a missing Session");
        const auto ownerless_failure_calls = async_pipeline_calls.load();
        pipeline_source();
        Check(async_pipeline_calls == ownerless_failure_calls, "ownerless failure cannot requeue every source");

        gate.Reset();
        async_frame.session = original_session;
        async_frame.preparation_epoch = original_session->preparation_epoch;
        async_frame.enabled = true;
        native::ReleaseCom(native::codec_pipeline.autoscale);
        pipeline_source();
        native::CancelAsyncAutoscalePreparation();
        const auto canceled_calls = async_pipeline_calls.load();
        native::StartAsyncAutoscalePreparation();
        Check(native::async_autoscale_task == nullptr && async_pipeline_calls == canceled_calls,
            "a canceled queued task is reclaimed before module/thread startup");
        pipeline_source();
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        auto* retained_root = native::codec_pipeline.root_signature;
        native::disable_release_mode = 0;
        native::enabled = false;
        native::ServiceDisabledNr();
        Check(!native::disabled_resources_released.load() && native::codec_pipeline.root_signature == retained_root,
            "NR Off cancels a running private task at release=0 without claiming exit or clearing retained cache");
        Check(gate.Finish(), "canceled private worker actually exits");
        native::ServiceDisabledNr();
        Check(native::async_autoscale_task == nullptr && native::codec_pipeline.autoscale == nullptr
            && native::codec_pipeline.root_signature == retained_root, "the canceled late PSO is never admitted");
        native::enabled = true;
        gate.Reset();
        pipeline_source();
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        Check(gate.Finish(), "reenabled private worker exits");
        pipeline_source();
        Check(pipeline_source().first == native::PipelinePreparationResult::READY,
            "ordinary re-enable recovers with a new valid private attempt");

        gate.Reset();
        native::ReleaseCom(native::codec_pipeline.autoscale);
        pipeline_source();
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        async_frame.session = std::make_shared<addon::adaptive_resolution::Session>(*original_session);
        async_frame.preparation_epoch = async_frame.session->preparation_epoch;
        pipeline_source();
        fail_async_pipeline = true;
        Check(gate.Finish(), "obsolete attempt failure worker exits");
        pipeline_source();
        Check(!native::codec_pipeline.preparation_failed && !async_frame.session->failed
            && native::async_autoscale_task != nullptr && !native::async_autoscale_task->started,
            "an old Session failure is recorded and discarded without poisoning the current frame or blocking its one attempt");
        gate.Reset();
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        fail_async_pipeline = true;
        Check(gate.Finish(), "current attempt failure worker exits");
        Check(pipeline_source().first == native::PipelinePreparationResult::FAILED
            && native::codec_pipeline.preparation_failed && async_frame.session->failed && !original_session->failed,
            "only the matching current attempt latches pipeline and Session failure");
        const auto failed_async_calls = async_pipeline_calls.load();
        pipeline_source();
        pipeline_source();
        Check(async_pipeline_calls == failed_async_calls && native::async_autoscale_task == nullptr && !async_gate_failed.load(),
            "failed preparation neither requeues every source nor leaves a stuck worker gate");
        native::codec_pipeline.preparation_failed = false;
        native::codec_pipeline.preparation_session = original_session;
        Check(SUCCEEDED(synchronous_autoscale.CopyTo(&native::codec_pipeline.autoscale)), "restore the original completed autoscale cache");
        async_frame.session = original_session;
        async_frame.preparation_epoch = original_session->preparation_epoch;
        ComPtr<ID3D12PipelineState> synchronous_band = native::codec_pipeline.look.band;
        std::array band_inputs{
            gpu.Texture(16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST),
            gpu.Texture(16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST)};
        for (uint32_t source = 0; source < band_inputs.size(); ++source) {
          std::array<uint16_t, 16 * 16 * 4> pixels{};
          for (uint32_t pixel = 0; pixel < 16 * 16; ++pixel) {
            for (uint32_t channel = 0; channel < 3; ++channel) {
              pixels[pixel * 4 + channel] = (source == 0 ? 0x3400u : (pixel % 3 == 0 ? 0x3A00u : 0x3800u));
            }
            pixels[pixel * 4 + 3] = 0x3C00u;
          }
          auto transfer = gpu.CreateTextureTransfer(band_inputs[source].Get());
          gpu.UploadTexture(band_inputs[source].Get(), transfer,
              reinterpret_cast<const uint8_t*>(pixels.data()), sizeof(pixels), 16 * 8);
          gpu.Transition(band_inputs[source].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        std::array band_surfaces{
            texture(DXGI_FORMAT_R32G32B32A32_FLOAT, true, 8),
            texture(DXGI_FORMAT_R32G32B32A32_FLOAT, true, 8),
            texture(DXGI_FORMAT_R32G32_FLOAT, true, 8),
            texture(DXGI_FORMAT_R32G32_FLOAT, true, 8)};
        const D3D12_DESCRIPTOR_HEAP_DESC band_heap_desc{
            .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, .NumDescriptors = 14,
            .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        ComPtr<ID3D12DescriptorHeap> band_heap;
        Check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&band_heap_desc, IID_PPV_ARGS(&band_heap))), "look bands comparison heap");
        const auto band_stride = gpu.device->GetDescriptorHandleIncrementSize(band_heap_desc.Type);
        const auto band_cpu = [&](uint32_t slot) {
          return D3D12_CPU_DESCRIPTOR_HANDLE{band_heap->GetCPUDescriptorHandleForHeapStart().ptr + slot * band_stride};
        };
        const auto band_gpu = [&](uint32_t slot) {
          return D3D12_GPU_DESCRIPTOR_HANDLE{band_heap->GetGPUDescriptorHandleForHeapStart().ptr + slot * band_stride};
        };
        const D3D12_SHADER_RESOURCE_VIEW_DESC band_srv{
            .Format = DXGI_FORMAT_R16G16B16A16_FLOAT, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Texture2D = {.MipLevels = 1}};
        for (uint32_t slot = 0; slot < 4; ++slot) {
          gpu.device->CreateShaderResourceView(band_inputs[slot == 2 ? 1 : 0].Get(), &band_srv, band_cpu(slot));
        }
        auto band_motion_srv = band_srv;
        band_motion_srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        gpu.device->CreateShaderResourceView(nullptr, &band_motion_srv, band_cpu(13));
        for (uint32_t slot = 0; slot < addon::look::kUavCount; ++slot) {
          D3D12_UNORDERED_ACCESS_VIEW_DESC uav{
              .Format = DXGI_FORMAT_R16G16B16A16_FLOAT, .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
          ID3D12Resource* surface = nullptr;
          if (slot >= addon::look::kUavBandA && slot <= addon::look::kUavBandMaxB) {
            surface = band_surfaces[slot - addon::look::kUavBandA].Get();
            uav.Format = surface->GetDesc().Format;
          } else if (slot == addon::look::kUavTraceBlocks) {
            uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
          } else if (slot == addon::look::kUavTraceHistogram) {
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer = {.NumElements = addon::look::kTraceWords, .Flags = D3D12_BUFFER_UAV_FLAG_RAW};
          }
          gpu.device->CreateUnorderedAccessView(surface, nullptr, &uav, band_cpu(4 + slot));
        }
        const addon::look::Bindings band_bindings{
            .heap = band_heap.Get(), .codec_set = band_gpu(0), .look_set = band_gpu(0),
            .uav_table = band_gpu(4), .motion = band_gpu(13),
            .history_in = meter->second.norm_commit->GetGPUVirtualAddress(),
            .history_out = meter->second.norm_commit->GetGPUVirtualAddress()};
        const auto sample_band = [&](ID3D12PipelineState* pipeline) {
          auto constants = addon::look::MakeConstants({.tone = .5f},
              {.nr_width = 16, .nr_height = 16, .full_width = 16, .full_height = 16});
          constants.size[0] = constants.size[1] = 8;
          for (uint32_t mode = 0; mode < 5; ++mode) {
            constants.mode = mode;
            addon::look::Bind(gpu.list.Get(), native::codec_pipeline.look, pipeline,
                band_bindings, band_bindings.codec_set, constants);
            gpu.list->Dispatch(1, 1, 1);
            addon::look::UavBarrierAll(gpu.list.Get());
          }
          std::array<d3d12_test::TextureTransfer, 4> transfers;
          for (uint32_t surface = 0; surface < band_surfaces.size(); ++surface) {
            transfers[surface] = gpu.CreateTextureTransfer(band_surfaces[surface].Get());
            gpu.Transition(band_surfaces[surface].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gpu.RecordTextureReadback(band_surfaces[surface].Get(), transfers[surface]);
            gpu.Transition(band_surfaces[surface].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          }
          submit();
          std::vector<uint8_t> pixels;
          for (const auto& transfer : transfers) {
            const auto surface = gpu.ReadTexture(transfer);
            pixels.insert(pixels.end(), surface.begin(), surface.end());
          }
          return pixels;
        };
        const auto synchronous_band_pixels = sample_band(synchronous_band.Get());
        Check(std::any_of(synchronous_band_pixels.begin(), synchronous_band_pixels.end(), [](uint8_t value) { return value != 0; }),
            "synchronous look bands comparison dispatches all five modes and reads nonzero band/extrema data");
        gate.Reset();
        native::ReleaseCom(native::codec_pipeline.look.band);
        Check(pipeline_source().first == native::PipelinePreparationResult::PENDING
            && native::async_autoscale_task != nullptr
            && native::async_autoscale_task->target_identity == &native::codec_pipeline.look.band
            && native::async_autoscale_task->root_identity == native::codec_pipeline.look.root_signature
            && native::async_autoscale_task->root_identity != native::codec_pipeline.root_signature,
            "look bands registers its exact shader slot and distinct Look root in the existing single task");
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        native::async_autoscale_preparation = false;
        Check(pipeline_source().first == native::PipelinePreparationResult::PENDING
            && native::codec_pipeline.look.band == nullptr && async_band_calls == 1
            && async_pipeline_root.load() == native::codec_pipeline.look.root_signature
            && next_handle == waiting_models && model_calls == waiting_evaluates
            && native::worksets_created == waiting_worksets,
            "pending look bands cannot become ready or fall back to synchronous Create when the private flag changes");
        native::async_autoscale_preparation = true;
        Check(gate.Finish(), "look bands worker exits before admission");
        const auto band_accepted = pipeline_source();
        auto* accepted_band = native::codec_pipeline.look.band;
        Check(band_accepted.first == native::PipelinePreparationResult::PENDING && band_accepted.second
            && accepted_band != nullptr && native::async_autoscale_task == nullptr,
            "look bands accepts once at a source opportunity without starting NGX");
        Check(pipeline_source().first == native::PipelinePreparationResult::READY
            && native::codec_pipeline.look.band == accepted_band && async_band_calls == 1
            && sample_band(accepted_band) == synchronous_band_pixels,
            "private Look-root bands produces identical nonzero WARP band/extrema pixels with no second admission");

        gate.Reset();
        native::ReleaseCom(native::codec_pipeline.look.band);
        pipeline_source();
        native::CancelAsyncAutoscalePreparation();
        native::StartAsyncAutoscalePreparation();
        Check(native::async_autoscale_task == nullptr && async_band_calls == 1, "queued look bands cancels before worker startup");
        pipeline_source();
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        auto* retained_look_root = native::codec_pipeline.look.root_signature;
        native::enabled = false;
        native::ServiceDisabledNr();
        Check(!native::disabled_resources_released.load() && native::codec_pipeline.look.root_signature == retained_look_root,
            "NR Off does not release the Look root while its canceled private worker is still running");
        Check(gate.Finish(), "canceled look bands worker exits");
        native::ServiceDisabledNr();
        Check(native::async_autoscale_task == nullptr && native::codec_pipeline.look.band == nullptr
            && native::codec_pipeline.look.root_signature == retained_look_root, "late canceled look bands cannot populate its retained slot");
        native::enabled = true;

        gate.Reset();
        pipeline_source();
        const auto band_task = native::async_autoscale_task;
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        native::ReleaseCom(native::codec_pipeline.autoscale);
        const auto other_target_pending = pipeline_source();
        Check(other_target_pending.first == native::PipelinePreparationResult::PENDING && !other_target_pending.second
            && native::async_autoscale_task == band_task && native::codec_pipeline.autoscale == nullptr,
            "a missing autoscale slot cannot replace or synchronously bypass the in-flight Look bands task");
        Check(gate.Finish(), "re-enabled look bands worker exits");
        const auto other_target_accepted = pipeline_source();
        auto* recovered_band = native::codec_pipeline.look.band;
        Check(other_target_accepted.first == native::PipelinePreparationResult::PENDING && other_target_accepted.second
            && recovered_band != nullptr && native::codec_pipeline.autoscale == nullptr && native::async_autoscale_task == nullptr,
            "an autoscale caller admits the healthy Look result only into its own slot and consumes this source");
        gate.Reset();
        pipeline_source();
        Check(native::async_autoscale_task != nullptr
            && native::async_autoscale_task->target_identity == &native::codec_pipeline.autoscale
            && native::async_autoscale_task->root_identity == native::codec_pipeline.root_signature,
            "the second private PSO starts only after the first target was admitted and its worker exited");
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        Check(gate.Finish(), "serial autoscale worker exits");
        pipeline_source();
        Check(pipeline_source().first == native::PipelinePreparationResult::READY
            && native::codec_pipeline.look.band == recovered_band,
            "serial Look and autoscale preparation recovers both distinct roots without replacing the healthy first target");

        gate.Reset();
        native::ReleaseCom(native::codec_pipeline.look.band);
        pipeline_source();
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        async_frame.session = std::make_shared<addon::adaptive_resolution::Session>(*original_session);
        async_frame.preparation_epoch = async_frame.session->preparation_epoch;
        pipeline_source();
        fail_async_pipeline = true;
        Check(gate.Finish(), "obsolete Look attempt failure worker exits");
        pipeline_source();
        Check(!native::codec_pipeline.preparation_failed && !async_frame.session->failed
            && native::async_autoscale_task != nullptr && !native::async_autoscale_task->started,
            "an obsolete Look failure cannot poison the new Session or consume its legitimate attempt");
        gate.Reset();
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        fail_async_pipeline = true;
        Check(gate.Finish(), "current Look attempt failure worker exits");
        Check(pipeline_source().first == native::PipelinePreparationResult::FAILED
            && native::codec_pipeline.preparation_failed && async_frame.session->failed && !original_session->failed,
            "Look failure belongs only to the matching current preparation Session");
        const auto failed_band_calls = async_band_calls.load();
        pipeline_source();
        pipeline_source();
        Check(async_band_calls == failed_band_calls && native::async_autoscale_task == nullptr,
            "a current Look failure cannot requeue each source");
        gate.Reset();
        async_frame.session = std::make_shared<addon::adaptive_resolution::Session>(*original_session);
        async_frame.preparation_epoch = async_frame.session->preparation_epoch;
        pipeline_source();
        native::StartAsyncAutoscalePreparation();
        wait_entered();
        Check(gate.Finish(), "new valid Look attempt worker exits");
        pipeline_source();
        Check(pipeline_source().first == native::PipelinePreparationResult::READY && !async_frame.session->failed
            && !native::codec_pipeline.preparation_failed && async_band_calls == failed_band_calls + 1,
            "an explicitly new healthy Session recovers from the bounded Look preparation failure");
        native::codec_pipeline.preparation_session = original_session;
        Check(async_threads_started == async_threads_joined && !async_gate_failed.load(),
            "every autoscale and Look private worker has an OS thread exit proof");
        std::cout << "ADAPTIVE_ASYNC evidence=WARP+MOCK real_threads=" << async_threads_started.load()
            << " thread_exit_proofs=" << async_threads_joined.load()
            << " look_band_calls=" << async_band_calls.load()
            << " pending_zero_NGX=1 admitted_once=1 pixel_equal=1 look_band_modes=5 look_pixel_equal=1 distinct_roots=1 serial_targets=1 ownerless_manual=1 off_late_rejected=1 stale_failure_isolated=1 current_failure_bounded=1\n";
      }
      {
        real_create_resource = reinterpret_cast<CreateResourceFn>((*reinterpret_cast<void***>(gpu.device.Get()))[27]);
        Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_create_resource), reinterpret_cast<PVOID>(ObserveCreateResource)) == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR, "adaptive allocation-failure observer attach");
        const auto pressure_record = [&](bool reset = true) {
          ++native::present_generation;
          bool accepted = false;
          {
            const addon::EvaluateChainScope chain;
            const addon::SourceStageScope source(cold_handle, true);
            accepted = native::ProcessInline(gpu.list.Get(), cold_handle, &parameters);
          }
          submit(reset);
          return accepted;
        };
        native::nr_adaptive_selected_levels = 2;
        native::RecreateFeatures();
        submit();
        const auto before_upper_oom = next_handle;
        const bool look_before = native::look_unavailable;
        fail_resource_phase = 1;
        for (uint32_t step = 0; step < 8; ++step) pressure_record();
        Check(fail_resource_phase == 0 && next_handle == before_upper_oom
            && !native::features.at(cold_handle).adaptive_session->prepared
            && native::features.at(cold_handle).adaptive_session->status.reason == addon::adaptive_resolution::Reason::BUDGET
            && native::look_unavailable == look_before,
            "first-upper resource OOM pauses every subsequent source without creating any model or disabling the ordinary Look stage");
        native::RecreateFeatures();
        submit();
        for (uint32_t step = 0; step < 32; ++step) pressure_record();
        auto cache = native::features.at(cold_handle).adaptive_cache;
        Check(native::features.at(cold_handle).adaptive_session->prepared && cache->count == 2,
            "explicit recovery after first-upper OOM prepares a complete two-Pass collection");
        const auto* upper = native::features.at(cold_handle).slots[0].handle;
        const auto* standby = cache->tiers[1].state.slots[0].handle;
        const auto released_before = std::count(model_released_handles.begin(), model_released_handles.end(), standby);
        addon::submission::TrackUse(gpu.list.Get(), standby);
        addon::submission::TrackUse(gpu.list.Get(), cache->tiers[1].recording_token.get());
        pressure_record(false);
        auto parked_list = gpu.list;
        auto parked_allocator = gpu.allocator;
        Check(!addon::submission::ResourceReleasable(standby),
            "the completed parked recording genuinely retains the standby before allocation failure");
        ComPtr<ID3D12CommandAllocator> next_allocator;
        ComPtr<ID3D12GraphicsCommandList> next_list;
        Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&next_allocator))), "resource-pressure recovery allocator");
        Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            next_allocator.Get(), nullptr, IID_PPV_ARGS(&next_list))), "resource-pressure recovery list");
        gpu.allocator = next_allocator;
        gpu.list = next_list;
        Check(native::InstallCommandListStateHooks(gpu.list.Get()), "resource-pressure recovery list hooks");
        native::kSetAdaptiveLevelCount.Store(3.f);
        const auto before_standby_oom = next_handle;
        fail_resource_phase = 2;
        for (uint32_t step = 0; step < 12 && fail_resource_phase != 0; ++step) pressure_record();
        Check(fail_resource_phase == 0 && next_handle == before_standby_oom && cache->budget_blocked
            && cache->standby_retired && !native::features.at(cold_handle).adaptive_session->prepared
            && native::features.at(cold_handle).slots[0].handle == upper
            && native::AdaptiveRetirementPending(cold_handle),
            "standby Look allocation OOM stops before NGX Create and retires the unused collection while retaining the healthy upper");
        Check(std::count(model_released_handles.begin(), model_released_handles.end(), standby) == released_before,
            "a GPU-complete but replayable recording still protects the failed collection's standby model");
        native::disable_release_mode = 0;
        native::enabled = false;
        native::ServiceDisabledNr();
        Check(native::features.at(cold_handle).slots[0].handle == upper,
            "NRDisableRelease=0 still retains the healthy upper while NR is Off");
        native::enabled = true;
        native::RecreateFeatures();
        const auto before_retry = next_handle;
        for (uint32_t step = 0; step < 4; ++step) {
          Check(!pressure_record(), "re-enable keeps the host output while its failed collection is still replayable");
        }
        Check(next_handle == before_retry && native::AdaptiveRetirementPending(cold_handle),
            "re-enable never overlaps new models with the failed collection's necessary retirements");
        Check(SUCCEEDED(parked_allocator->Reset()) && SUCCEEDED(parked_list->Reset(parked_allocator.Get(), nullptr))
            && SUCCEEDED(parked_list->Close()), "discard only the already completed parked recording");
        native::DrainRetiredResources(true);
        Check(!native::AdaptiveRetirementPending(cold_handle)
            && std::count(model_released_handles.begin(), model_released_handles.end(), standby) == released_before + 1,
            "the failed standby releases after its exact recording closes");
        for (uint32_t step = 0; step < 40; ++step) pressure_record();
        Check(native::features.at(cold_handle).adaptive_session->prepared
            && !native::features.at(cold_handle).adaptive_retirement_blocked,
            "ordinary inline re-enable clears its retirement gate and fully recovers without weakening the proof");
        Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_create_resource), reinterpret_cast<PVOID>(ObserveCreateResource)) == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR, "adaptive allocation-failure observer detach");
        native::nr_adaptive_selected_levels = 0;
      }
      Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
          && DetourDetach(reinterpret_cast<PVOID*>(&real_create_pipeline), reinterpret_cast<PVOID>(ObserveCreatePipeline)) == NO_ERROR
          && DetourTransactionCommit() == NO_ERROR, "cold inline PSO observer detach");
      native::nr_adaptive_enabled = false;
      native::ReleaseAdaptiveCache(&native::features.at(cold_handle), true);
      native::ReleaseAllNrSlots(native::features.at(cold_handle), true);
      native::ReleaseCom(native::features.at(cold_handle).norm.commit);
      for (auto it = native::worksets.begin(); it != native::worksets.end();) {
        if (it->first.handle != cold_handle) {
          ++it;
          continue;
        }
        const auto key = it++->first;
        native::RetireWorkset(key);
      }
      native::features.erase(cold_handle);
      submit();
      native::ReleaseCodecPipeline();
      native::stack_passes = 1;
      native::nr_adaptive_target_fps = 116.f;
      parameters.Set(NVSDK_NGX_Parameter_Color, colors[0].Get());
      parameters.Set(NVSDK_NGX_Parameter_Output, outputs[0].Get());
      parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    }
    uint32_t active_successes = 0;
    uint32_t preparation_calls = 0;
    uint32_t preparation_failures = 0;
    const NVSDK_NGX_Handle* expected_reset_model = nullptr;
    bool observed_expected_reset = false;
    model_parameter_observer = [&](const NVSDK_NGX_Handle* model, const NVSDK_NGX_Parameter* model_parameters) {
      if (model == expected_reset_model && native::preparing_adaptive_frame == nullptr) {
        observed_expected_reset |= native::GetInt(model_parameters, "DLSSNR.Reset") != 0;
      }
      const bool failing = model_calls + 1 == fail_call;
      if (native::preparing_adaptive_frame != nullptr) {
        ++preparation_calls;
        preparation_failures += failing;
      } else if (!failing) {
        ++active_successes;
      }
    };
    for (const auto hook : {native::kHookUpscaled, native::kHookRender}) {
      native::hook_point = hook;
      native::nr_adaptive_enabled = false;
      const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA100} + hook);
      native::RegisterCreatedFeatureLocked(handle, native::kFeatureDlss, &parameters);
      uint32_t frame = 0;
      const auto evaluate = [&] {
        ++native::present_generation;
        const auto before_source = native::SourceSuccessfulEvaluations();
        const auto before_active = active_successes;
        const auto before_inline = native::successful_evaluations.load();
        const auto before_pre_sr = native::successful_pre_sr_evaluations.load();
        const uint32_t output = frame++ % 2;
        parameters.Set(NVSDK_NGX_Parameter_Color, colors[output].Get());
        parameters.Set(NVSDK_NGX_Parameter_Output, outputs[output].Get());
        bool accepted = false;
        {
          const addon::EvaluateChainScope chain;
          const addon::SourceStageScope source(handle, true);
          if (hook == native::kHookRender) {
            native::PreSrSwap swap;
            accepted = native::ProcessInlinePreSR(gpu.list.Get(), handle, &parameters, swap);
            native::RestorePreSrColor(&parameters, swap);
          } else {
            accepted = native::ProcessInline(gpu.list.Get(), handle, &parameters);
          }
        }
        const uint64_t completed_active = active_successes - before_active;
        Check(native::SourceSuccessfulEvaluations() == before_source + completed_active,
            "inline source successes exclude additional preparation Evaluate calls");
        Check(native::successful_evaluations.load() == before_inline
                + (hook == native::kHookUpscaled ? completed_active : 0)
            && native::successful_pre_sr_evaluations.load() == before_pre_sr
                + (hook == native::kHookRender ? completed_active : 0),
            "only the active insertion point contributes its successful source frame");
        return accepted;
      };
      const auto record = [&](bool reset = true) {
        const bool accepted = evaluate();
        submit(reset);
        return accepted;
      };
      const auto* occupied_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA200} + hook);
      Check(native::EnsureWorkset(gpu.device.Get(), occupied_owner, outputs[0].Get(), 16, 16, 16, 16,
          DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0) != nullptr, "diagnostic pressure uses a real WARP workset");
      native::features[occupied_owner].adaptive_cache = std::make_shared<native::AdaptiveFeatureCache>();
      native::features.at(occupied_owner).adaptive_cache->last_used_ns = native::SteadyNowNs();
      native::features.at(occupied_owner).adaptive_cache->resident = true;
      native::features.at(occupied_owner).adaptive_cache->count = 1;
      native::features.at(occupied_owner).adaptive_cache->output_identities.insert(reinterpret_cast<uintptr_t>(outputs[0].Get()));
      native::max_worksets = 1;
      const auto calls_before = model_calls;
      const auto setup_failures = addon::nr_decline_counts[static_cast<size_t>(addon::NrDeclineReason::kWorksetSetupFailed)].load();
      Check(!record(), "real capacity pressure rejects setup before model evaluation");
      auto& failed_slot = native::features.at(handle).slots[0];
      Check(failed_slot.failed && failed_slot.fail_count == 0 && model_calls == calls_before
          && addon::nr_decline_counts[static_cast<size_t>(addon::NrDeclineReason::kWorksetSetupFailed)].load() == setup_failures + 1,
          "capacity rejection reaches the unchanged setup-failure latch");
      Check(std::any_of(captured_logs.begin(), captured_logs.end(), [](const auto& entry) {
        return entry.second.find("reason=adaptive_owners_active workset_create_api=not_called") != std::string::npos;
      }), "setup diagnostics identify active cached owners and an uncalled workset creation API");
      Check(!record() && failed_slot.failed && failed_slot.fail_count == 1,
          "the original first retry remains immediate and records its next failure");
      const auto failure_ns = failed_slot.failed_ns;
      const auto backoffs = addon::nr_decline_counts[static_cast<size_t>(addon::NrDeclineReason::kFailureBackoff)].load();
      native::RetireWorkset({occupied_owner, outputs[0].Get()});
      native::features.erase(occupied_owner);
      native::DrainRetiredResources(true);
      native::max_worksets = 4;
      Check(!record() && failed_slot.failed && failed_slot.failed_ns == failure_ns && failed_slot.fail_count == 1
          && model_calls == calls_before
          && addon::nr_decline_counts[static_cast<size_t>(addon::NrDeclineReason::kFailureBackoff)].load() == backoffs + 1,
          "releasing pressure does not bypass the original one-second feature backoff");
      failed_slot.failed_ns = native::SteadyNowNs() - native::kFeatureRetryFirstNs - 1;
      record();
      Check(!failed_slot.failed,
          "the expired original backoff retries after pressure releases");
      std::cout << "DIAGNOSTIC_SETUP evidence=WARP+MOCK hook=" << hook
          << " reason=adaptive_owners_active workset_create_api=not_called backoff=unchanged recovery=passed\n";
      for (uint32_t warmup = 0; warmup < 8; ++warmup) record();
      Check(record(false), "manual inline NR records successfully before parking its actual GPU-complete list");
      auto parked_list = gpu.list;
      auto parked_allocator = gpu.allocator;
      const auto* parked_model = native::features.at(handle).slots[0].handle;
      const auto parked_releases = std::count(model_released_handles.begin(), model_released_handles.end(), parked_model);
      const auto parked_recording = addon::submission::ObserveRecording(parked_list.Get());
      Check(parked_recording->submitted.load() && !parked_recording->closed.load()
          && !addon::submission::ResourceReleasable(parked_model),
          "the completed active recording remains replayable and retains its model lifetime");
      ComPtr<ID3D12CommandAllocator> next_allocator;
      ComPtr<ID3D12GraphicsCommandList> next_list;
      Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&next_allocator))), "inline adaptive second allocator");
      Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
          next_allocator.Get(), nullptr, IID_PPV_ARGS(&next_list))), "inline adaptive second list");
      gpu.allocator = next_allocator;
      gpu.list = next_list;
      Check(native::InstallCommandListStateHooks(gpu.list.Get()), "inline adaptive second-list hooks");
      native::nr_adaptive_enabled = true;
      const auto before_preparation = preparation_calls;
      for (uint32_t warmup = 0; warmup < 32; ++warmup) record();
      auto& feature = native::features.at(handle);
      auto cache = feature.adaptive_cache;
      std::cout << "ADAPTIVE_INLINE hook=" << hook << " outputs=" << (cache ? cache->output_count : 0)
          << " tiers=" << (cache ? cache->count : 0) << " failed=" << feature.adaptive_session->failed
          << " reason=" << addon::adaptive_resolution::ReasonText(feature.adaptive_session->status.reason) << '\n';
      Check(cache != nullptr && cache->count == 2 && cache->output_count == 2,
          "inline A/B rotation shares the original four-workset budget across two tiers");
      if (hook == native::kHookRender) {
        for (const auto& [key, resources] : native::worksets) {
          if (key.handle != handle || !key.pre_sr) continue;
          Check(resources.adaptive_recording_token != nullptr
              && resources.adaptive_recording_token == cache->tiers[key.adaptive_slot].recording_token,
              "the actual Render workset owns the selected Render tier's recording token");
        }
        auto nested_color = texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
        auto nested = std::make_shared<native::FeatureState>();
        nested->adaptive_session = feature.adaptive_session;
        nested->adaptive_cache = std::make_shared<native::AdaptiveFeatureCache>();
        auto& nested_cache = *nested->adaptive_cache;
        nested_cache.owner = handle;
        nested_cache.pre_sr = true;
        nested_cache.count = 1;
        nested_cache.tiers[0].recording_token = std::make_shared<uint8_t>(0);
        const auto nested_token = nested_cache.tiers[0].recording_token;
        auto previous_render = std::exchange(feature.render_session, nested);
        native::max_worksets = 5;
        auto* nested_workset = native::EnsureWorkset(gpu.device.Get(), handle, nested_color.Get(),
            16, 16, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, true);
        Check(nested_workset != nullptr && nested_workset->adaptive_recording_token == nested_token
            && nested_workset->adaptive_recording_token != cache->tiers[0].recording_token,
            "nested Render allocation takes its real owner token instead of the parent's adaptive cache");
        native::RetireFailedAdaptivePreparation(nested.get());
        native::max_worksets = 4;
        Check(native::AdaptiveRetirementPending(handle)
            && std::count_if(native::worksets.begin(), native::worksets.end(), [&](const auto& workset) {
                 return native::AdaptiveCacheOwnsWorkset(*cache, workset.first, workset.second);
               }) == 4,
            "failed nested Render retirement tracks its actual scratch and preserves the parent's four live worksets");
        submit();
        Check(!native::AdaptiveRetirementPending(handle), "nested Render scratch completes its own exact retirement");
        feature.render_session = std::move(previous_render);
      }
      {
        const addon::adaptive_resolution::Frame adaptive_frame{.slot = 1, .enabled = true};
        const native::AdaptiveFrameScope scope(&adaptive_frame);
        const auto created = native::worksets_created;
        const char* rejection = "old_reason";
        Check(native::EnsureWorkset(gpu.device.Get(), occupied_owner, outputs[0].Get(), 16, 16, 16, 16,
            DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, false, &rejection) == nullptr
            && rejection != nullptr && std::strcmp(rejection, "adaptive_capacity") == 0
            && native::worksets_created == created,
            "full adaptive capacity exposes its pre-allocation rejection without creating resources");
      }
      native::PollAdaptiveCache(cache.get());
      Check(!cache->failed && !feature.adaptive_session->failed && cache->tiers[0].ready && cache->tiers[1].ready,
          "Render/Upscaled standby tiers complete both output recordings without permanent pause");
      Check(preparation_calls > before_preparation && feature.adaptive_session->status.preparation_cpu_ms > 0.,
          "standby Evaluate calls execute and retain their preparation cost separately from source successes");
      Check(!parked_recording->closed.load() && !addon::submission::ResourceReleasable(parked_model),
          "standby preparation preserves the active tier's replayable recording protection");
      if (hook == native::kHookUpscaled) {
        const auto created_before = native::nr_features_created;
        for (const auto mode : {native::AdaptiveFeatureMode::UPPER_CAPACITY,
                               native::AdaptiveFeatureMode::UPPER_CAPACITY_ON_DEMAND,
                               native::AdaptiveFeatureMode::UPPER_CAPACITY_ADJACENT}) {
          native::adaptive_feature_mode = mode;
          Check(record() && record() && feature.adaptive_cache == cache && cache->count == 2
              && cache->tiers[1].state.slots[0].create_input_width == 5
              && native::nr_features_created == created_before,
              "upper-capacity modes leave the existing R8 rotating-output path and its original small Create capacity unchanged");
        }
        native::adaptive_feature_mode = native::AdaptiveFeatureMode::OFF;
      }
      {
        const auto set = [](const native::Setting& setting, float value) {
          setting.Store(value);
          native::ApplySettingEffect(setting.effect);
        };
        const auto saved_style = native::kSetStyle.Get();
        const auto upper_model = feature.slots[0].handle;
        const auto lower_model = cache->tiers[1].state.slots[0].handle;
        const auto allocations = native::worksets_created;
        const auto models = native::nr_features_created;
        addon::gpu_comparison::Identity before, resources_before, after, resources_after;
        native::comparison_ui::CaptureSettings(&before);
        native::comparison_ui::CaptureAdaptiveSettings(&resources_before);
        set(native::kSetStyle, saved_style == 0.f ? 1.f : 0.f);
        Check(record() && record(), "Evaluate-only steering keeps both active outputs running");
        native::comparison_ui::CaptureSettings(&after);
        native::comparison_ui::CaptureAdaptiveSettings(&resources_after);
        const auto reset_epoch = native::history_reset_epoch.load();
        Check(feature.adaptive_cache == cache && feature.slots[0].handle == upper_model
            && cache->tiers[1].state.slots[0].handle == lower_model
            && native::nr_features_created == models && native::worksets_created == allocations
            && before.settings_hash != after.settings_hash && resources_before.settings_hash == resources_after.settings_hash
            && feature.slots[0].acknowledged_reset_epoch == reset_epoch
            && cache->tiers[1].state.slots[0].acknowledged_reset_epoch != reset_epoch,
            "steering retains compatible allocations and measurement identity while each tier consumes its own History reset");
        feature.adaptive_session->Request(1, native::SteadyNowNs());
        Check(record() && record() && cache->active == 1 && feature.slots[0].handle == lower_model
            && feature.slots[0].acknowledged_reset_epoch == reset_epoch && native::nr_features_created == models,
            "the retained lower model applies the pending steering reset when it is activated");
        feature.adaptive_session->Request(0, native::SteadyNowNs());
        Check(record() && record(), "steering reuse returns to its original upper model");
        set(native::kSetTransferStrength, .5f);
        Check(record() && record() && feature.adaptive_cache == cache && feature.slots[0].handle == upper_model
            && native::history_reset_epoch.load() == reset_epoch && native::nr_features_created == models
            && native::worksets_created == allocations,
            "resolve-only strength changes keep all allocations and do not request a model History reset");

        const auto saved_target = native::kSetAdaptiveTargetFps.Get();
        set(native::kSetAdaptiveTargetFps, saved_target + 1.f);
        Check(record() && record() && feature.adaptive_cache == cache
            && native::nr_features_created == models && native::worksets_created == allocations,
            "target FPS edits retain the entire selected collection");
        set(native::kSetAdaptiveTargetFps, saved_target);
        for (const auto* setting : {&native::kSetMonitorKey, &native::kSetMonitorModeKey}) {
          const auto saved = setting->Get();
          set(*setting, saved == 0.f ? 120.f : 0.f);
          Check(record() && record() && native::nr_features_created == models && native::worksets_created == allocations,
              "monitor hotkeys preserve all selected Feature and workset allocations");
          set(*setting, saved);
        }
        for (const auto& [setting, value] : std::array{
                 std::pair{&native::kSetControlMaskRed, .8f},
                 std::pair{&native::kSetControlMaskGreen, .7f},
                 std::pair{&native::kSetControlMaskBlue, .6f},
                 std::pair{&native::kSetControlMaskDepthNear, .8f},
                 std::pair{&native::kSetControlMaskDepthFar, .99999f},
                 std::pair{&native::kSetControlMaskMotionStart, 0.f},
                 std::pair{&native::kSetControlMaskMotionEnd, 1.f}}) {
          const auto saved = setting->Get();
          set(*setting, value);
          Check(record() && record() && native::nr_features_created == models && native::worksets_created == allocations,
              "RGB and guide thresholds update the actual mask without replacing selected allocations");
          set(*setting, saved);
        }
        const auto now = native::SteadyNowNs();
        cache->last_used_ns = now - 6'000'000'000;
        for (auto& [key, resources] : native::worksets) {
          if (key.handle == handle) resources.last_used_ns = now - 6'000'000'000;
        }
        native::max_worksets = 5;
        Check(native::EnsureWorkset(gpu.device.Get(), occupied_owner, outputs[0].Get(), 16, 16, 16, 16,
            DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0) != nullptr, "a temporary stream owns one real recent workset");
        native::RetireSupersededWorksets(now);
        Check(feature.adaptive_cache == cache && cache->resident
            && std::count_if(native::worksets.begin(), native::worksets.end(),
                [&](const auto& workset) { return workset.first.handle == handle; }) == 4,
            "another active stream cannot retire a selected owner paused longer than five seconds");
        native::RetireWorkset({occupied_owner, outputs[0].Get()});
        native::DrainRetiredResources(true);
        native::max_worksets = 4;
        const auto after_temporary = native::worksets_created;
        Check(native::EnsureWorkset(gpu.device.Get(), occupied_owner, outputs[0].Get(), 16, 16, 16, 16,
            DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0) == nullptr && native::worksets_created == after_temporary,
            "the LRU applies the same resident protection after the five-second idle boundary");
        Check(record() && record() && feature.adaptive_cache == cache && native::nr_features_created == models,
            "the original owner resumes its retained collection without a new Feature");
        const auto before_three_models = native::nr_features_created;
        const auto before_three_worksets = native::worksets_created;
        set(native::kSetAdaptiveLevelCount, 3.f);
        for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
        Check(feature.adaptive_session->settings.selected_levels == 3 && feature.adaptive_session->prepared
            && cache->count == 3 && native::nr_features_created == before_three_models + 1
            && native::worksets_created == before_three_worksets + 2
            && native::adaptive_resident_worksets == 6 && native::EffectiveAdaptiveWorksetLimit() == 10
            && native::max_worksets.load() == 4,
            "three explicit levels own all six real rotating-output worksets without changing the ordinary four-workset setting");
        const auto three_models = native::nr_features_created;
        set(native::kSetAdaptiveLevelCount, 0.f);
        native::max_worksets = 3;
        Check(record() && record() && feature.adaptive_session->status.reason == addon::adaptive_resolution::Reason::BUDGET
            && !feature.adaptive_session->prepared && native::nr_features_created == three_models,
            "Default still rejects a complete two-output collection outside its actual ordinary quota");
        set(native::kSetAdaptiveTargetFps, saved_target + 1.f);
        Check(record() && record() && feature.adaptive_session->status.reason == addon::adaptive_resolution::Reason::BUDGET
            && native::nr_features_created == three_models, "target FPS does not retry a denied collection");
        native::max_worksets = 4;
        set(native::kSetAdaptiveLevelCount, 2.f);
        set(native::kSetAdaptiveTargetFps, saved_target);
        for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
        Check(feature.adaptive_cache == cache && feature.adaptive_session->prepared
            && cache->count == 2 && native::nr_features_created == three_models + 1
            && feature.slots[0].handle == upper_model,
            "an explicit selection rechecks budget and rebuilds the retired lower tier while retaining its healthy upper");
        const auto recovered_models = native::nr_features_created;

        set(native::kSetAdaptiveMinScale, .5f);
        record();
        Check(!feature.adaptive_session->prepared && !feature.adaptive_session->trial && cache->active == 0,
            "a committed missing endpoint enters preparation before any automatic request");
        for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
        Check(feature.adaptive_cache == cache && feature.adaptive_session->prepared && cache->tiers[1].ready
            && cache->ladder.sizes[1][0].width == 8 && feature.slots[0].handle == upper_model
            && native::nr_features_created == recovered_models + 1,
            "editing the minimum prepares the missing endpoint while retaining the complete upper allocation");
        feature.adaptive_session->Request(1, native::SteadyNowNs());
        Check(record() && record() && cache->active == 1 && feature.input_width == 8,
            "the prepared endpoint is available immediately when it is requested");
        feature.adaptive_session->Request(0, native::SteadyNowNs());
        Check(record() && record() && cache->active == 0, "the legal upper remains available during selection edits");
        const auto reused_upper = cache->tiers[1].state.slots[0].handle;
        const auto before_reindex_models = native::nr_features_created;
        expected_reset_model = reused_upper;
        observed_expected_reset = false;
        {
          std::unique_lock lock(native::present_restore_configuration.mutex);
          native::nr_resolution_scale.store(.5f);
          ++native::nr_resolution_commit_epochs[0];
        }
        for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
        Check(feature.adaptive_cache == cache && feature.slots[0].handle == reused_upper
            && feature.input_width == 8 && observed_expected_reset && feature.adaptive_session->prepared
            && native::nr_features_created == before_reindex_models + 1,
            "a previously lower complete-size tuple becomes the manual upper with its original Feature and a local History reset");
        expected_reset_model = nullptr;
        {
          std::unique_lock lock(native::present_restore_configuration.mutex);
          native::nr_resolution_scale.store(1.f);
          ++native::nr_resolution_commit_epochs[0];
        }
        set(native::kSetAdaptiveMinScale, .33f);
        for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
        set(native::kSetAdaptiveLevelCount, 0.f);

        set(native::kSetStyle, saved_style);
        set(native::kSetTransferStrength, 1.f);
        Check(record() && record(), "reuse regression restores the original Evaluate and resolve controls");
        native::adaptive_feature_mode = native::AdaptiveFeatureMode::OFF;
        const auto saved_preset = native::kSetPreset.Get();
        native::comparison_ui::CaptureAdaptiveSettings(&resources_before);
        native::kSetPreset.Store(saved_preset == 1.f ? 2.f : 1.f);
        native::comparison_ui::CaptureAdaptiveSettings(&resources_after);
        Check(resources_before.settings_hash != resources_after.settings_hash,
            "model selection remains part of the allocation identity");
        native::kSetPreset.Store(saved_preset);
        const auto settled_models = native::nr_features_created;
        Check(record() && record() && feature.adaptive_cache == cache && cache->active == 0
            && cache->tiers[1].ready && cache->ladder.sizes[1][0].width == 5
            && native::nr_features_created == settled_models,
            "both outputs consume the restored measurement epoch with the original lower tier ready and no Create");
      }
      if (hook == native::kHookRender) {
        auto running_list = gpu.list;
        auto running_allocator = gpu.allocator;
        ComPtr<ID3D12CommandAllocator> pending_allocator;
        ComPtr<ID3D12GraphicsCommandList> pending_list;
        Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&pending_allocator))), "Render target-tier pending allocator");
        Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            pending_allocator.Get(), nullptr, IID_PPV_ARGS(&pending_list))), "Render target-tier pending list");
        gpu.allocator = pending_allocator;
        gpu.list = pending_list;
        Check(native::InstallCommandListStateHooks(gpu.list.Get()), "Render target-tier pending-list hooks");
        native::ActivateAdaptiveTier(&feature, 1);
        feature.adaptive_session->Request(1, native::SteadyNowNs());
        const auto before_pending = model_calls;
        Check(evaluate() && model_calls == before_pending + 1,
            "the target tier owns an actual NR recording that is deliberately left unsubmitted");
        const auto target_recording = addon::submission::ObserveRecording(gpu.list.Get());
        Check(!target_recording->submitted.load() && !target_recording->closed.load(),
            "target-tier History is reserved by its own open unsubmitted recording");
        gpu.allocator = running_allocator;
        gpu.list = running_list;
        native::ActivateAdaptiveTier(&feature, 0);
        feature.adaptive_session->Request(1, native::SteadyNowNs());
        const auto before_blocked = model_calls;
        Check(!evaluate() && model_calls == before_blocked,
            "Render selects and checks the requested tier before recording: its own pending History prevents every NR call");
        Check(!feature.adaptive_session->failed && !cache->failed,
            "the requested tier's temporary History wait does not latch an execution failure");
        gpu.allocator = pending_allocator;
        gpu.list = pending_list;
        submit();
        gpu.allocator = running_allocator;
        gpu.list = running_list;
        Check(record() && record(), "Render resumes the requested tier after its own pending producer submits");
        feature.adaptive_session->Request(0, native::SteadyNowNs());
        Check(record() && record() && cache->active == 0,
            "Render returns to the original active tier after the target-tier admission check");
      }
      const auto creates = native::nr_features_created;
      feature.adaptive_session->Request(1, native::SteadyNowNs());
      Check(record() && record(), "both rotating outputs accept their prepared lower tier");
      Check(cache->active == 1 && feature.adaptive_session->status.applied[0].width == 5
          && native::nr_features_created == creates,
          "inline lower tier actually applies its requested size without cold Create");
      feature.adaptive_session->Request(0, native::SteadyNowNs());
      Check(record() && record() && cache->active == 0 && native::nr_features_created == creates,
          "inline prepared upper tier restores without cold Create");
      const auto deadline = native::SteadyNowNs() + 5'000'000'000;
      uint64_t serial = 0;
      while (feature.adaptive_session->level == 0 && native::SteadyNowNs() < deadline) {
        const auto now = native::SteadyNowNs();
        {
          std::unique_lock lock(addon::output_framerate::internal::state.mutex);
          auto& estimate = addon::output_framerate::internal::state.stream;
          estimate.previous_ns = now;
          estimate.snapshot = {.available = true, .source = addon::output_framerate::Source::PRESENT,
              .source_generation = 1, .configuration_generation = native::configuration_generation.load(),
              .window_serial = ++serial, .window_begin_ns = now - 500'000'000, .window_end_ns = now,
              .fps = 50., .fg_off = true};
        }
        record();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      std::cout << "ADAPTIVE_INLINE controller hook=" << hook << " level=" << feature.adaptive_session->level
          << " gpu_samples=" << feature.adaptive_session->gpu_count
          << " applied_width=" << feature.adaptive_session->status.applied[0].width
          << " reason=" << addon::adaptive_resolution::ReasonText(feature.adaptive_session->status.reason) << '\n';
      Check(feature.adaptive_session->level == 1 && cache->active == 1
          && feature.adaptive_session->status.applied[0].width == 5 && native::nr_features_created == creates,
          "real GPU samples and mocked low output FPS drive the production inline controller into its ready lower tier");
      {
        std::unique_lock lock(addon::output_framerate::internal::state.mutex);
        addon::output_framerate::internal::state.stream = {};
      }
      auto& failing_tier = cache->tiers[(cache->active + 1) % cache->count];
      failing_tier.output_completions.clear();
      failing_tier.ready = false;
      const auto before_failure = model_calls;
      const auto before_preparation_failure = preparation_failures;
      fail_call = model_calls + 2;  // Active output succeeds, then standby Evaluate fails.
      Check(record() && model_calls == before_failure + 2 && cache->failed && feature.adaptive_session->failed,
          "a true standby API failure preserves the successful active output and pauses adaptive preparation");
      Check(preparation_failures == before_preparation_failure + 1,
          "the standby API failure is observed even though it contributes no successful source frame");
      fail_call = 0;
      const auto before_recreate = native::configuration_generation.load();
      native::RecreateFeatures();
      Check(native::configuration_generation.load() == before_recreate + 1 && feature.adaptive_cache == nullptr,
          "the production clear-failure action retires the inline failed tier set");
      for (uint32_t warmup = 0; warmup < 32; ++warmup) record();
      auto recovered_cache = feature.adaptive_cache;
      native::PollAdaptiveCache(recovered_cache.get());
      Check(recovered_cache != cache && !recovered_cache->failed && !feature.adaptive_session->failed
          && recovered_cache->tiers[0].ready && recovered_cache->tiers[1].ready
          && feature.adaptive_session->status.applied[0].width == 16,
          "Render/Upscaled clear-failure rebuilds GPU-ready tiers and resumes the saved upper working size");
      feature.adaptive_session->Request(1, native::SteadyNowNs());
      Check(record() && record() && recovered_cache->active == 1
          && feature.adaptive_session->status.applied[0].width == 5,
          "the recovered inline source runs its real lower working size before adaptive Off");
      native::nr_adaptive_enabled = false;
      Check(record() && record() && feature.adaptive_cache == nullptr
          && feature.adaptive_session->status.applied[0].width == 16 && native::nr_resolution_scale.load() == 1.f,
          "adaptive Off restores the unchanged manual upper size after recovery");
      native::nr_adaptive_enabled = true;
      for (uint32_t warmup = 0; warmup < 32; ++warmup) record();
      Check(feature.adaptive_cache != nullptr && !feature.adaptive_session->failed
          && feature.adaptive_session->status.applied[0].width == 16,
          "adaptive On resumes the same inline source after Off without changing the saved upper limit");
      native::kSetAdaptiveMinScale.Store(1.f);
      native::RecreateFeatures();
      for (uint32_t warmup = 0; warmup < 12; ++warmup) record();
      const auto single_tier = feature.adaptive_cache;
      Check(single_tier != nullptr && single_tier->count == 1 && single_tier->tiers[0].ready,
          "a source first prepared at a 100-percent minimum owns only its upper physical tier");
      const auto single_model = feature.slots[0].handle;
      const auto single_models = native::nr_features_created;
      const auto single_worksets = native::worksets_created;
      native::kSetAdaptiveMinScale.Store(.5f);
      record();
      Check(feature.adaptive_cache == single_tier && single_tier->count == 2
          && feature.adaptive_session->ladder.count == 2 && !feature.adaptive_session->prepared
          && feature.slots[0].handle == single_model,
          "leaving a one-size range starts one explicit preparation and retains the original upper Feature");
      for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
      Check(feature.adaptive_session->prepared && single_tier->active == 0 && single_tier->tiers[1].ready
          && native::nr_features_created == single_models + 1 && native::worksets_created == single_worksets + 2,
          "the full newly selected set completes both rotating outputs before automatic control resumes");
      feature.adaptive_session->Request(1, native::SteadyNowNs());
      Check(record() && record() && single_tier->active == 1
          && feature.adaptive_session->status.applied[0].width == 8
          && single_tier->tiers[0].state.slots[0].handle == single_model,
          "the newly prepared lower level switches without replacing the original upper allocation");

      native::kSetAdaptiveMinScale.Store(.33f);
      for (uint32_t warmup = 0; warmup < 24; ++warmup) record();
      // Start at the unchanged output: B/A/B/A completes the replacement standby
      // on the fourth call, before the next source can consume its ready receipt.
      if (frame % 2 == 0) {
        record();
      }
      auto binding_cache = feature.adaptive_cache;
      const auto binding_models = native::nr_features_created;
      const auto binding_worksets = native::worksets_created;
      auto* rebound_resource = (hook == native::kHookUpscaled ? outputs[0].Get() : colors[0].Get());
      uint64_t original_binding_identity = 0;
      UINT identity_size = sizeof(original_binding_identity);
      Check(SUCCEEDED(rebound_resource->GetPrivateData(native::kAdaptiveOutputIdentityGuid,
          &identity_size, &original_binding_identity)) && original_binding_identity != 0,
          "the current output carries a non-owning object-attached identity");
      const auto changed_binding_identity = native::next_adaptive_output_identity++;
      Check(SUCCEEDED(rebound_resource->SetPrivateData(native::kAdaptiveOutputIdentityGuid,
          sizeof(changed_binding_identity), &changed_binding_identity)), "MOCK same-address replacement identity");
      ++native::present_generation;
      native::AdaptiveOutputCount(&feature, handle, rebound_resource, hook == native::kHookRender,
          16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, rebound_resource);
      Check(!feature.adaptive_output_blocked
          && binding_cache->previous_output_bindings.contains(original_binding_identity),
          "a new cookie at the same address is admitted only after healthy exact completion");
      for (const auto& [key, resources] : native::worksets) {
        if (key.handle != handle || key.resource != rebound_resource) continue;
        Check(std::none_of(std::begin(resources.bound_sources), std::end(resources.bound_sources),
            [&](const auto* source) { return source == rebound_resource; }),
            "same-address replacement invalidates source-view pointer shortcuts before the next descriptor bind");
      }
      for (uint32_t step = 0; step < 4; ++step) record();
      Check(!feature.adaptive_session->prepared && native::nr_features_created == binding_models
          && native::worksets_created == binding_worksets,
          "B/A/B/A completes replacement work without allocating while owner readiness awaits the next source");
      Check(record(), "the next source consumes the replacement standby completion");
      Check(feature.adaptive_session->prepared && native::nr_features_created == binding_models
          && native::worksets_created == binding_worksets,
          "MOCK identity replacement reacquires real WARP readiness without allocating a new tier");
      const auto previous_bindings = binding_cache->previous_output_bindings.size();
      auto third = texture(DXGI_FORMAT_R8G8B8A8_UNORM, hook == native::kHookUpscaled);
      gpu.Submit();
      const auto output_references_before = third->AddRef();
      third->Release();
      auto& changed_output = (hook == native::kHookUpscaled ? outputs[0] : colors[0]);
      auto original_output = changed_output;
      changed_output = third;
      for (uint32_t step = 0; step < 4; ++step) record();
      const auto output_references_after = third->AddRef();
      third->Release();
      if (hook == native::kHookUpscaled) {
        Check(output_references_after == output_references_before + 1,
            "output identity adds no COM ownership beyond the fixture's own current-output reference");
      }
      Check(feature.adaptive_cache == binding_cache && binding_cache->previous_output_bindings.size() == previous_bindings + 1
          && native::nr_features_created == binding_models && native::worksets_created == binding_worksets,
          "a proven same-geometry binding replacement refreshes receipts and references without replacing Feature or scratch");
      changed_output = original_output;
      for (uint32_t step = 0; step < 4; ++step) record();
      Check(binding_cache->budget_blocked && feature.adaptive_session->status.reason == addon::adaptive_resolution::Reason::BUDGET
          && native::nr_features_created == binding_models && native::worksets_created == binding_worksets,
          "a previously rebound output returning in A/B/C/A locks budget instead of starting a rolling preparation cycle");
      const auto previous_target = native::nr_adaptive_target_fps.load();
      native::nr_adaptive_target_fps.store(previous_target + 1.f);
      for (uint32_t step = 0; step < 8; ++step) record();
      Check(binding_cache->budget_blocked && native::nr_features_created == binding_models
          && native::worksets_created == binding_worksets,
          "target FPS and repeated source observations cannot restart a rejected output-rotation contract");
      native::nr_adaptive_target_fps.store(previous_target);

      std::cout << "ADAPTIVE_INLINE accounting hook=" << hook << " frames=" << frame
          << " source_successes=" << native::SourceSuccessfulEvaluations()
          << " model_calls=" << model_calls << " preparation_calls=" << preparation_calls
          << " preparation_failures=" << preparation_failures << '\n';
      native::ForgetSourceHandleLocked(handle);
      submit();
      Check(!addon::submission::ResourceReleasable(parked_model),
          "source retirement cannot release a model referenced by the parked active recording");
      Check(SUCCEEDED(parked_allocator->Reset()) && SUCCEEDED(parked_list->Reset(parked_allocator.Get(), nullptr)),
          "fixture closes its parked recording only after preparation and source-retirement assertions");
      native::DrainRetiredResources(true);
      Check(addon::submission::ResourceReleasable(parked_model)
          && std::count(model_released_handles.begin(), model_released_handles.end(), parked_model) == parked_releases + 1,
          "closing the fixture's completed parked recording permits exactly one normal model release");
    }
    model_parameter_observer = {};
    {
      native::hook_point = native::kHookUpscaled;
      native::nr_adaptive_enabled = true;
      native::nr_adaptive_selected_levels = addon::adaptive_resolution::MAX_PREPARED_LEVELS;
      native::nr_adaptive_min_scale = .33f;
      native::nr_resolution_scale = 1.f;
      native::max_worksets = 4;
      std::array wide_colors{texture(DXGI_FORMAT_R8G8B8A8_UNORM, false, 64),
                            texture(DXGI_FORMAT_R8G8B8A8_UNORM, false, 64)};
      std::array wide_outputs{texture(DXGI_FORMAT_R8G8B8A8_UNORM, true, 64),
                             texture(DXGI_FORMAT_R8G8B8A8_UNORM, true, 64)};
      auto wide_motion = texture(DXGI_FORMAT_R16G16_FLOAT, false, 64);
      auto wide_depth = texture(DXGI_FORMAT_R32_FLOAT, false, 64);
      gpu.Submit();
      std::array output_readbacks{gpu.CreateTextureTransfer(wide_outputs[0].Get()),
                                 gpu.CreateTextureTransfer(wide_outputs[1].Get())};
      addon::bridge::OwnedParameters wide_parameters;
      wide_parameters.Set(NVSDK_NGX_Parameter_Depth, wide_depth.Get());
      wide_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, wide_motion.Get());
      for (const auto* key : {NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
               NVSDK_NGX_Parameter_OutWidth, NVSDK_NGX_Parameter_OutHeight,
               NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
               NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height}) {
        wide_parameters.Set(key, 64u);
      }
      wide_parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
      wide_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
      wide_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
      wide_parameters.Set(NVSDK_NGX_Parameter_Reset, 0u);
      const auto* wide_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA800});
      native::RegisterCreatedFeatureLocked(wide_handle, native::kFeatureDlss, &wide_parameters);
      const auto before_wide_models = native::nr_features_created;
      uint32_t wide_frame = 0;
      const auto wide_record = [&](bool verify_writeback = false, bool reset = true) {
        ++native::present_generation;
        const uint32_t output = wide_frame++ % 2;
        if (verify_writeback) {
          gpu.Transition(wide_outputs[output].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_COMMON);
          gpu.Write(wide_outputs[output].Get(), 0x7f);
          gpu.Transition(wide_outputs[output].Get(), D3D12_RESOURCE_STATE_COMMON,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        wide_parameters.Set(NVSDK_NGX_Parameter_Color, wide_colors[output].Get());
        wide_parameters.Set(NVSDK_NGX_Parameter_Output, wide_outputs[output].Get());
        bool accepted = false;
        {
          const addon::EvaluateChainScope chain;
          const addon::SourceStageScope source(wide_handle, true);
          if (native::hook_point.load() == native::kHookRender) {
            native::PreSrSwap swap;
            accepted = native::ProcessInlinePreSR(gpu.list.Get(), wide_handle, &wide_parameters, swap);
            native::RestorePreSrColor(&wide_parameters, swap);
          } else {
            accepted = native::ProcessInline(gpu.list.Get(), wide_handle, &wide_parameters);
          }
        }
        if (verify_writeback) {
          gpu.Transition(wide_outputs[output].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
          gpu.RecordTextureReadback(wide_outputs[output].Get(), output_readbacks[output]);
          gpu.Transition(wide_outputs[output].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        submit(reset);
        Check(SUCCEEDED(gpu.device->GetDeviceRemovedReason())
            && gpu.fence->GetCompletedValue() != UINT64_MAX && gpu.fence->GetCompletedValue() >= gpu.serial,
            "every selected-level step finishes on a healthy WARP Fence");
        if (verify_writeback) {
          const auto pixels = gpu.ReadTexture(output_readbacks[output]);
          bool changed = false;
          for (size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
            changed |= pixels[pixel] != 0x7f || pixels[pixel + 1] != 0x7f || pixels[pixel + 2] != 0x7f;
          }
          Check(accepted && changed, "the selected level writes actual GPU model output back to the requested game surface");
        }
        return accepted;
      };
      bool ordinary_pressure = false;
      for (uint32_t preparation = 0; preparation < 128; ++preparation) {
        wide_record();
        auto& wide = native::features.at(wide_handle);
        if (!wide.adaptive_session->prepared) {
          Check(wide.adaptive_cache->active == 0 && !wide.adaptive_session->trial,
              "the full fifteen-size collection closes automatic decisions until every output is ready");
        }
        if (!ordinary_pressure && wide.adaptive_cache->resident) {
          const auto owned = native::AdaptiveResidentWorksetCount(wide.adaptive_session);
          for (uint32_t stream = 0; stream < 5; ++stream) {
            const auto* ordinary = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA810} + stream);
            Check(native::EnsureWorkset(gpu.device.Get(), ordinary, wide_outputs[0].Get(), 16, 16, 16, 16,
                DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0) != nullptr,
                "ordinary traffic can use and recycle its original bounded pool during preparation");
          }
          Check(native::AdaptiveResidentWorksetCount(wide.adaptive_session) == owned
              && native::worksets.size() - owned == native::max_worksets.load()
              && native::EffectiveAdaptiveWorksetLimit() == 34,
              "unbuilt selected slots stay reserved while ordinary traffic remains at its original four-workset quota");
          ordinary_pressure = true;
        }
        if (wide.adaptive_session->prepared) break;
      }
      auto& wide = native::features.at(wide_handle);
      const auto collection = wide.adaptive_cache;
      Check(wide.adaptive_session->prepared && collection->count == 15 && collection->output_count == 2
          && native::AdaptiveResidentWorksetCount(wide.adaptive_session) == 30
          && native::adaptive_resident_worksets == 30 && native::max_worksets.load() == 4
          && native::nr_features_created == before_wide_models + 15,
          "all fifteen complete-size tuples own real Features and both rotating-output worksets within their explicit reservation");
      const auto steady_models = native::nr_features_created;
      const auto steady_worksets = native::worksets_created;
      for (size_t level = 0; level < collection->count; ++level) {
        wide.adaptive_session->Request(level, native::SteadyNowNs());
        wide_record(true);
        wide_record(true);
        Check(collection->active == level && wide.input_width == collection->ladder.sizes[level][0].width
            && wide.input_height == collection->ladder.sizes[level][0].height
            && wide.adaptive_session->status.applied == collection->ladder.sizes[level],
            "each selected level evaluates at its actual complete working dimensions on both outputs");
      }
      for (const size_t level : {14u, 0u, 7u, 14u, 0u}) {
        wide.adaptive_session->Request(level, native::SteadyNowNs());
        wide_record(true);
        wide_record(true);
      }
      Check(native::nr_features_created == steady_models && native::worksets_created == steady_worksets,
          "all-level and immediate fifteen-level round trips perform no new Feature or workset allocations");
      const auto* mode_upper = wide.slots[0].handle;
      // This GPU host has no ReShade configuration exports. The separate real
      // configuration/ImGui fixtures cover Commit, persistence and profile undo.
      native::kSetAdaptiveLimitedResidency.Store(1.f);
      native::ApplySettingEffect(native::kSetAdaptiveLimitedResidency.effect);
      for (uint32_t source = 0; source < 16; ++source) {
        wide_record();
        if (wide.adaptive_session->TargetReady()
            && native::AdaptiveResidentWorksetCount(wide.adaptive_session) == 2) break;
      }
      std::cerr << "ADAPTIVE_MODE to_limited cache_same=" << (wide.adaptive_cache == collection)
          << " requested_mode=" << native::nr_adaptive_limited_residency.load()
          << " session_mode=" << wide.adaptive_session->settings.limited_residency
          << " logical=" << collection->count << " active=" << collection->active
          << " ready=" << wide.adaptive_session->TargetReady() << " full=" << wide.adaptive_session->prepared
          << " failed=" << wide.adaptive_session->failed << " budget=" << collection->budget_blocked
          << " epoch=" << wide.adaptive_session->preparation_epoch << '/' << collection->preparation_epoch
          << " resident=" << native::AdaptiveResidentWorksetCount(wide.adaptive_session)
          << " retiring=" << native::AdaptiveSessionRetirementPending(wide.adaptive_session)
          << " upper_same=" << (wide.slots[0].handle == mode_upper)
          << " models=" << native::nr_features_created << '/' << steady_models
          << " reason=" << addon::adaptive_resolution::ReasonText(wide.adaptive_session->status.reason) << '\n';
      Check(wide.adaptive_cache == collection && collection->active == 0 && collection->count == 15
          && wide.adaptive_session->TargetReady() && !wide.adaptive_session->prepared
          && wide.slots[0].handle == mode_upper && native::nr_features_created == steady_models
          && native::AdaptiveResidentWorksetCount(wide.adaptive_session) == 2,
          "the registered on-demand mode retires optional allocations in the same cache without reducing its logical ladder");
      native::kSetAdaptiveLimitedResidency.Store(0.f);
      native::ApplySettingEffect(native::kSetAdaptiveLimitedResidency.effect);
      wide_record();
      Check(wide.adaptive_cache == collection && !wide.adaptive_session->prepared && collection->active == 0
          && wide.slots[0].handle == mode_upper,
          "switching back to full preparation closes the ready gate and retains the compatible actual upper");
      for (uint32_t source = 0; source < 128 && !wide.adaptive_session->prepared; ++source) wide_record();
      Check(wide.adaptive_cache == collection && wide.adaptive_session->prepared && collection->count == 15
          && wide.slots[0].handle == mode_upper && native::AdaptiveResidentWorksetCount(wide.adaptive_session) == 30
          && native::nr_features_created == steady_models + 14,
          "full preparation rebuilds only the fourteen evicted models and requires both outputs of all fifteen levels");
      native::nr_adaptive_enabled = false;
      wide_record();
      wide_record();
      Check(wide.adaptive_cache == nullptr && native::adaptive_resident_session.expired()
          && native::adaptive_resident_worksets == 0
          && std::count_if(native::worksets.begin(), native::worksets.end(),
              [&](const auto& workset) { return workset.first.handle == wide_handle; }) == 2,
          "Off releases the explicit reservation and retires every standby while retaining the legal manual upper");
      for (uint32_t stream = 0; stream < 5; ++stream) {
        const auto* ordinary = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA810} + stream);
        native::RetireWorkset({ordinary, wide_outputs[0].Get()});
      }
      submit();
      native::hook_point = native::kHookRender;
      native::RecreateFeatures();
      native::nr_adaptive_selected_levels = 3;
      native::nr_adaptive_enabled = true;
      for (uint32_t preparation = 0; preparation < 64; ++preparation) {
        wide_record();
        if (wide.adaptive_session->prepared) break;
      }
      Check(wide.adaptive_session->prepared && wide.adaptive_cache->pre_sr
          && native::AdaptiveResidentWorksetCount(wide.adaptive_session) == 6
          && std::count_if(native::worksets.begin(), native::worksets.end(), [&](const auto& workset) {
               return workset.first.handle == wide_handle && !workset.first.pre_sr;
             }) == 2,
          "ordinary PostSR scratch survives Off and a Render switch without consuming the six selected Render slots");
      for (uint32_t stream = 0; stream < 4; ++stream) {
        const auto* ordinary = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA810} + stream);
        Check(native::EnsureWorkset(gpu.device.Get(), ordinary, wide_outputs[0].Get(), 16, 16, 16, 16,
            DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0) != nullptr, "ordinary Stage leftovers remain eligible for normal LRU");
      }
      Check(native::AdaptiveResidentWorksetCount(wide.adaptive_session) == 6
          && std::none_of(native::worksets.begin(), native::worksets.end(), [&](const auto& workset) {
               return workset.first.handle == wide_handle && !workset.first.pre_sr;
             }), "the pool can recycle old PostSR scratch while every selected Render workset stays resident");

      native::RecreateFeatures();
      submit();
      native::nr_adaptive_limited_residency = true;
      native::nr_adaptive_selected_levels = 15;
      const auto before_limited_models = native::nr_features_created;
      for (uint32_t source = 0; source < 32; ++source) {
        wide_record();
        if (wide.adaptive_session->TargetReady()) break;
      }
      auto limited = wide.adaptive_cache;
      std::cout << "ADAPTIVE_LIMITED initial logical=" << (limited != nullptr ? limited->count : 0)
          << " ready=" << wide.adaptive_session->TargetReady() << " failed=" << wide.adaptive_session->failed
          << " active=" << (limited != nullptr ? limited->active : 0)
          << " reserved=" << native::adaptive_resident_worksets
          << " resident=" << native::AdaptiveResidentWorksetCount(wide.adaptive_session)
          << " pool=" << native::worksets.size() << '/' << native::EffectiveAdaptiveWorksetLimit()
          << " new_models=" << native::nr_features_created - before_limited_models
          << " reason=" << addon::adaptive_resolution::ReasonText(wide.adaptive_session->status.reason) << '\n';
      Check(limited != nullptr && limited->count == 15 && wide.adaptive_session->TargetReady()
          && !wide.adaptive_session->prepared && limited->active == 0
          && native::nr_features_created == before_limited_models + 1
          && native::AdaptiveResidentWorksetCount(wide.adaptive_session) == 2,
          "limited residency retains fifteen logical sizes but initially creates only the complete upper on both outputs");
      const auto upper_applied = wide.adaptive_session->status.applied;
      wide.adaptive_session->Request(6, native::SteadyNowNs());
      wide_record();
      Check(wide.adaptive_session->settings.prepared_levels == 15 && wide.adaptive_session->level == 6
          && !wide.adaptive_session->TargetReady() && limited->active == 0
          && wide.adaptive_session->status.applied == upper_applied && wide.adaptive_session->applied_ns == 0,
          "a cold target preserves the actually applied upper until both output receipts are healthy");
      for (uint32_t source = 0; source < 32 && limited->active != 6; ++source) wide_record();
      Check(limited->active == 6 && wide.adaptive_session->TargetReady()
          && wide.adaptive_session->status.applied == limited->ladder.sizes[6]
          && native::nr_features_created == before_limited_models + 2,
          "the requested cold Render tier applies only after its complete output group is ready");

      wide_record(false, false);
      auto retained_allocator = gpu.allocator;
      auto retained_list = gpu.list;
      const auto* retained_model = wide.slots[0].handle;
      Check(!addon::submission::ResourceReleasable(retained_model),
          "the limited active tier is still protected by its completed replayable host recording");
      Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&gpu.allocator)))
          && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list)))
          && native::InstallCommandListStateHooks(gpu.list.Get()), "limited residency replacement recording");
      wide.adaptive_session->Request(0, native::SteadyNowNs());
      wide_record();
      wide_record();
      Check(limited->active == 0 && native::AdaptiveRetirementPending(wide_handle)
          && !wide.adaptive_session->failed && !limited->standby_retired,
          "ordinary eviction leaves the healthy upper running while the old host recording remains replayable");
      const auto waiting_models = native::nr_features_created;
      wide.adaptive_session->Request(10, native::SteadyNowNs());
      wide_record();
      wide_record();
      Check(limited->active == 0 && !wide.adaptive_session->TargetReady()
          && native::nr_features_created == waiting_models && native::AdaptiveRetirementPending(wide_handle),
          "a later cold request cannot allocate over an unretired ordinary eviction");
      wide.adaptive_session->Request(0, native::SteadyNowNs());
      wide_record();
      wide_record();
      Check(limited->active == 0 && wide.adaptive_session->TargetReady()
          && native::nr_features_created == waiting_models,
          "a reverse request can use the protected upper without waiting for or creating the canceled target");
      Check(SUCCEEDED(retained_allocator->Reset())
          && SUCCEEDED(retained_list->Reset(retained_allocator.Get(), nullptr)),
          "only the legitimate host Reset ends the retired tier's replay eligibility");
      native::DrainRetiredResources(true);
      Check(!native::AdaptiveRetirementPending(wide_handle)
          && addon::submission::ResourceReleasable(retained_model), "exact retirement completes before the next cold admission");
      wide.adaptive_session->Request(10, native::SteadyNowNs());
      for (uint32_t source = 0; source < 32 && limited->active != 10; ++source) wide_record();
      Check(limited->active == 10 && wide.adaptive_session->TargetReady()
          && wide.adaptive_session->status.applied == limited->ladder.sizes[10]
          && native::nr_features_created == waiting_models + 1 && !wide.adaptive_session->prepared,
          "a retired slot can be reused for one new complete target without claiming full-ladder preparedness");
      wide_record(false, false);
      auto off_retained_allocator = gpu.allocator;
      auto off_retained_list = gpu.list;
      const auto* off_retained_model = wide.slots[0].handle;
      // The earlier retained list was Reset above and is still open and empty.
      gpu.allocator = retained_allocator;
      gpu.list = retained_list;
      wide.adaptive_session->Request(0, native::SteadyNowNs());
      wide_record();
      wide_record();
      Check(limited->active == 0 && native::AdaptiveRetirementPending(wide_handle)
          && !addon::submission::ResourceReleasable(off_retained_model),
          "the final optional target genuinely remains replayable while the healthy upper is active");
      native::nr_adaptive_enabled = false;
      const bool off_accepted = wide_record();
      Check(off_accepted && wide.adaptive_cache == limited
          && wide.adaptive_session->status.applied == wide.adaptive_session->status.upper
          && native::AdaptiveRetirementPending(wide_handle)
          && !addon::submission::ResourceReleasable(off_retained_model),
          "disabling Adaptive renders with the retained healthy upper without releasing the old replayable target");
      Check(wide_record() && wide.adaptive_cache == limited
          && wide.adaptive_session->status.applied == wide.adaptive_session->status.upper
          && native::AdaptiveRetirementPending(wide_handle)
          && !addon::submission::ResourceReleasable(off_retained_model)
          && native::nr_features_created == waiting_models + 1,
          "successive Adaptive-Off frames keep the upper contract without recreating or releasing pending resources");
      Check(SUCCEEDED(off_retained_allocator->Reset())
          && SUCCEEDED(off_retained_list->Reset(off_retained_allocator.Get(), nullptr))
          && SUCCEEDED(off_retained_list->Close()), "discard the final retired recording only after its real Fence");
      native::DrainRetiredResources(true);
      wide_record();
      Check(wide.adaptive_cache == nullptr && wide.adaptive_session->status.applied == wide.adaptive_session->status.upper,
          "limited adaptive Off restores the retained manual upper and retires optional tiers");
      native::nr_adaptive_limited_residency = false;
      native::ForgetSourceHandleLocked(wide_handle);
      for (uint32_t stream = 0; stream < 4; ++stream) {
        const auto* ordinary = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA810} + stream);
        native::RetireWorkset({ordinary, wide_outputs[0].Get()});
      }
      submit();
      native::nr_adaptive_selected_levels = 0;
      std::cout << "ADAPTIVE_SELECTED evidence=WARP+MOCK dimensions=64x64 levels=15 outputs=2 Features=15 selected_worksets=30 ordinary_limit=4 steady_allocations=0 native_NGX=not_run\n";
    }

    native::successful_sr_device = nullptr;
    {
      ID3D12RootSignature* root = nullptr;
      const auto before = captured_logs.size();
      Check(!native::CreateRootSignature(nullptr, E_INVALIDARG, nullptr, nullptr, &root, L"mock",
          "MOCK serialization failed", "mock creation failed"), "MOCK serialization failure remains a failure");
      Check(captured_logs.size() == before + 1
          && captured_logs.back().second.find("api=D3D12SerializeRootSignature hr=0x80070057") != std::string::npos,
          "MOCK HRESULT input is logged unchanged with its actual API label");
      std::cout << "DIAGNOSTIC_API evidence=MOCK api=D3D12SerializeRootSignature hr=0x80070057 illegal_D3D12_call=none\n";
      for (auto& sample : addon::gpu_timers::stage_stats) {
        sample = {};
      }
      for (auto& sample : addon::gpu_timers::execution_stats) {
        sample = {};
      }
      for (auto& sample : addon::gpu_timers::completed_stats) {
        sample = {};
      }
      native::card_observation.completions = {};
      addon::NrVerdict verdict{.state = addon::NrState::kOff, .api = "d3d12", .path = "inline"};
      auto tick_ns = native::SteadyNowNs();
      native::last_emitted_card = 0xff;
      native::card_line_tokens = 1;
      native::card_line_refill_ns = 0;
      const auto count_lines = [](const char* text) {
        return std::count_if(captured_logs.begin(), captured_logs.end(), [&](const auto& entry) {
          return entry.second.find(text) != std::string::npos;
        });
      };
      const auto cards_before = count_lines("NR-CARD v1 ");
      const auto evidence_before = count_lines("NR-CARD evidence ");
      native::MaybeEmitStatusCard(verdict, tick_ns);
      Check(count_lines("NR-CARD v1 ") == cards_before + 1 && count_lines("NR-CARD evidence ") == evidence_before + 1
          && captured_logs.back().second.find("gpu_candidate_completed_age_ms=unavailable") != std::string::npos,
          "production card change emits one companion evidence line with explicit missing GPU evidence");
      verdict.eligible = 10;
      tick_ns += 100'000'000LL;
      native::MaybeEmitStatusCard(verdict, tick_ns);
      Check(count_lines("NR-CARD evidence ") == evidence_before + 1,
          "changing counts and ages alone cannot bypass production card deduplication");
      native::card_line_tokens = 0;
      native::card_line_refill_ns = tick_ns;
      verdict.state = addon::NrState::kEngaged;
      native::MaybeEmitStatusCard(verdict, tick_ns);
      Check(count_lines("NR-CARD v1 ") == cards_before + 1 && count_lines("NR-CARD evidence ") == evidence_before + 1,
          "production token exhaustion suppresses both the card and evidence lines");
    }
    if (info != nullptr) {
      for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
        SIZE_T bytes = 0;
        info->GetMessage(index, nullptr, &bytes);
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(SUCCEEDED(info->GetMessage(index, message, &bytes)), "inline adaptive debug message");
        Check(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
      }
    }
    std::cout << "PASS adaptive Render/Upscaled two-output preparation and ready-tier switching evidence=WARP+MOCK\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "adaptive-inline: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace adaptive_inline_cases
