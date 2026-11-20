#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <DirectXPackedVector.h>
#include <psapi.h>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <utils/hash.hpp>

namespace control_mask_gpu_cases {
namespace addon = renodx::addons::dlss5::internal;

using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
inline DispatchFn real_dispatch = nullptr;
inline uint32_t mask_dispatches = 0;
inline ID3D12PipelineState* observed_pipeline = nullptr;
inline d3d12_test::TimestampQueries* native_cost_queries = nullptr;
inline ID3D12GraphicsCommandList* native_cost_list = nullptr;
inline UINT native_cost_interval = 0;
inline uint32_t native_cost_dispatches = 0;
inline ID3D12GraphicsCommandList* native_single_feature_dispatch_list = nullptr;
inline std::vector<std::array<uint32_t, 3>> native_single_feature_dispatches;
inline uint32_t native_single_feature_dispatch_overflow = 0;
using PipelineFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
inline PipelineFn real_pipeline = nullptr;
using AllocateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, REFIID, void**);
inline AllocateFn real_allocate = nullptr;
inline bool fail_mask_allocation = false;
inline uint32_t allocation_failures = 0;
using RootFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
using PsoFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using HeapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
using SignalFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
using CompletedFn = UINT64(STDMETHODCALLTYPE*)(ID3D12Fence*);
using RemovedFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*);
inline RootFn real_root = nullptr;
inline PsoFn real_pso = nullptr;
inline HeapFn real_heap = nullptr;
inline SignalFn real_signal = nullptr;
inline CompletedFn real_completed = nullptr;
inline RemovedFn real_removed = nullptr;
inline ID3D12Device* removed_device = nullptr;
inline uint32_t runtime_initializations = 0;
inline uint32_t runtime_shutdowns = 0;
inline uint32_t source_evaluate_calls = 0;
inline uint32_t source_evaluate_c_calls = 0;
inline ID3D12Resource* source_evaluate_color = nullptr;
inline bool fail_source_evaluate = false;
enum class Fault { NONE, ROOT, PSO, HEAP };
inline Fault fault = Fault::NONE;
inline uint32_t injected_failures = 0;
inline ID3D12Fence* signal_fault_fence = nullptr;
inline ID3D12Fence* removed_fence = nullptr;
inline int reservation_fault_stage = -1;

HRESULT STDMETHODCALLTYPE CreateRoot(ID3D12Device* device, UINT node, const void* blob, SIZE_T bytes,
                                     REFIID iid, void** result) {
  if (fault == Fault::ROOT) {
    ++injected_failures;
    *result = nullptr;
    return E_OUTOFMEMORY;
  }
  return real_root(device, node, blob, bytes, iid, result);
}
HRESULT STDMETHODCALLTYPE CreatePso(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                    REFIID iid, void** result) {
  if (fault == Fault::PSO) {
    ++injected_failures;
    *result = nullptr;
    return E_OUTOFMEMORY;
  }
  return real_pso(device, desc, iid, result);
}
HRESULT STDMETHODCALLTYPE CreateHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                     REFIID iid, void** result) {
  if (fault == Fault::HEAP) {
    ++injected_failures;
    *result = nullptr;
    return E_OUTOFMEMORY;
  }
  return real_heap(device, desc, iid, result);
}
HRESULT STDMETHODCALLTYPE Signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
  if (fence == signal_fault_fence) {
    ++injected_failures;
    signal_fault_fence = nullptr;
    return E_FAIL;
  }
  return real_signal(queue, fence, value);
}
UINT64 STDMETHODCALLTYPE Completed(ID3D12Fence* fence) {
  if (removed_device != nullptr) {
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(fence->GetDevice(IID_PPV_ARGS(&device))) && device.Get() == removed_device) return UINT64_MAX;
  }
  return fence == removed_fence ? UINT64_MAX : real_completed(fence);
}
HRESULT STDMETHODCALLTYPE RemovedReason(ID3D12Device* device) {
  return device == removed_device ? DXGI_ERROR_DEVICE_REMOVED : real_removed(device);
}
NVSDK_NGX_Result NVSDK_CONV SourceEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  ++source_evaluate_calls;
  source_evaluate_color = addon::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Color);
  return fail_source_evaluate ? NVSDK_NGX_Result_FAIL_UnableToInitializeFeature
                              : NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV SourceEvaluateC(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback_C) {
  ++source_evaluate_c_calls;
  return addon::HookedEvaluateFeatureSlot<0>(list, handle, parameters, nullptr);
}
NVSDK_NGX_Result NVSDK_CONV InitializeRuntime(unsigned long long, const wchar_t*, ID3D12Device*,
                                             NVSDK_NGX_Version, const NVSDK_NGX_Parameter*) {
  ++runtime_initializations;
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV ShutdownRuntime(ID3D12Device*) {
  ++runtime_shutdowns;
  return NVSDK_NGX_Result_Success;
}
void ReservationFault(unsigned stage) {
  if (reservation_fault_stage == static_cast<int>(stage)) {
    reservation_fault_stage = -1;
    ++injected_failures;
    throw std::bad_alloc();
  }
}

inline bool wrapper_returned = false;
inline uint32_t wrapper_post_reads = 0;
inline bool wrapper_credential_seen = false;
inline bool wrapper_probe_adoption = false;
inline bool wrapper_adopted = false;
inline ID3D12GraphicsCommandList* wrapper_list = nullptr;
inline uint32_t failed_core_creates = 0;
inline uint32_t forbidden_fallback_creates = 0;
struct ProviderCalls {
  uint32_t allocations = 0, destroys = 0, creates = 0, evaluates = 0, releases = 0;
  std::set<const NVSDK_NGX_Parameter*> parameters;
  std::set<const NVSDK_NGX_Handle*> handles;
  std::set<const NVSDK_NGX_Handle*> evaluated_handles;
};
inline ProviderCalls provider_core, provider_signed;
inline bool fail_private_init = false, fail_private_create = false, fail_private_shutdown = false;
inline uint32_t provider_shared_shutdowns = 0;
inline uint64_t provider_next_device_id = 0;
inline std::map<uint64_t, bool> provider_shared_contexts, provider_private_contexts;
// Synthetic test key only: the production path never reads or writes NGX keys.
inline constexpr GUID kProviderDeviceId{0xc2dc4be9, 0x6d82, 0x4c3e, {0x9d, 0x7b, 0x55, 0x1e, 0x13, 0x7f, 0x42, 0x91}};

uint64_t ProviderDeviceId(ID3D12Device* device) {
  uint64_t id = 0;
  UINT bytes = sizeof(id);
  if (FAILED(device->GetPrivateData(kProviderDeviceId, &bytes, &id))) {
    id = ++provider_next_device_id;
    Check(SUCCEEDED(device->SetPrivateData(kProviderDeviceId, sizeof(id), &id)), "provider synthetic device ID");
  }
  return id;
}

NVSDK_NGX_Result NVSDK_CONV ProviderSharedInit(unsigned long long, const wchar_t*, ID3D12Device* device,
    NVSDK_NGX_Version, const NVSDK_NGX_Parameter*) {
  ++runtime_initializations;
  provider_shared_contexts.try_emplace(ProviderDeviceId(device), false);
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV ProviderSharedShutdown(ID3D12Device* device) {
  ++provider_shared_shutdowns;
  provider_shared_contexts.erase(ProviderDeviceId(device));
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV ProviderCoreAllocate(NVSDK_NGX_Parameter** parameters) {
  ++provider_core.allocations;
  const auto result = renodx::addons::dlss5::bridge::AllocateOwnedParameters(parameters);
  if (NVSDK_NGX_SUCCEED(result)) provider_core.parameters.insert(*parameters);
  return result;
}
NVSDK_NGX_Result NVSDK_CONV ProviderCoreDestroy(NVSDK_NGX_Parameter* parameters) {
  Check(provider_core.parameters.erase(parameters) == 1, "old Core destroys only its own parameter block");
  ++provider_core.destroys;
  return renodx::addons::dlss5::bridge::DestroyOwnedParameters(parameters);
}
NVSDK_NGX_Result NVSDK_CONV ProviderCoreCreate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  Check(provider_core.parameters.contains(parameters), "Core Create uses Core parameters");
  ++provider_core.creates;
  const auto result = CreateModel(list, feature, parameters, handle);
  provider_core.handles.insert(*handle);
  return result;
}
NVSDK_NGX_Result NVSDK_CONV ProviderCoreEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  Check(provider_core.handles.contains(handle) && provider_core.parameters.contains(parameters),
      "ordinary inline Evaluate keeps its Core handle and parameters");
  ++provider_core.evaluates;
  provider_core.evaluated_handles.insert(handle);
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV ProviderCoreRelease(NVSDK_NGX_Handle* handle) {
  Check(provider_core.handles.erase(handle) == 1, "old Core releases only its own handle");
  ++provider_core.releases;
  return ReleaseModel(handle);
}
NVSDK_NGX_Result NVSDK_CONV ProviderSignedInit(unsigned long long, const wchar_t*, ID3D12Device* device,
    NVSDK_NGX_Version, const NVSDK_NGX_Parameter*) {
  ++runtime_initializations;
  if (fail_private_init) return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  provider_private_contexts.try_emplace(ProviderDeviceId(device), false);
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV ProviderSignedShutdown(ID3D12Device* device) {
  ++runtime_shutdowns;
  if (fail_private_shutdown) return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  provider_private_contexts.erase(ProviderDeviceId(device));
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV ProviderSignedCreate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  Check(!provider_core.parameters.contains(parameters)
      && addon::core_allocate_parameters == renodx::addons::dlss5::bridge::AllocateOwnedParameters
      && addon::core_destroy_parameters == renodx::addons::dlss5::bridge::DestroyOwnedParameters,
      "private signed Create uses the complete OwnedParameters pair, never Core");
  ComPtr<ID3D12Device> device;
  Check(SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&device))), "private Create obtains its actual list device");
  const auto device_id = ProviderDeviceId(device.Get());
  Check(addon::direct_api.private_instance && provider_private_contexts.contains(device_id)
      && !provider_private_contexts.at(device_id) && provider_shared_contexts.at(device_id),
      "private Create uses its own native cache while the shared proxy context remains unchanged");
  ++provider_signed.creates;
  if (fail_private_create) {
    *handle = nullptr;
    return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  }
  const auto result = CreateModel(list, feature, parameters, handle);
  provider_signed.handles.insert(*handle);
  return result;
}
NVSDK_NGX_Result NVSDK_CONV ProviderSignedEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  Check(provider_signed.handles.contains(handle) && !provider_core.parameters.contains(parameters),
      "private signed Evaluate keeps its direct handle and OwnedParameters");
  ++provider_signed.evaluates;
  provider_signed.evaluated_handles.insert(handle);
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV ProviderSignedRelease(NVSDK_NGX_Handle* handle) {
  Check(provider_signed.handles.erase(handle) == 1, "signed provider releases only its own handle");
  ++provider_signed.releases;
  return ReleaseModel(handle);
}

struct CredentialParameters : NVSDK_NGX_Parameter {
  renodx::addons::dlss5::bridge::OwnedParameters values;

  // Observe the production after-path through its ordinary Output query,
  // after the real-export substitute has returned Success.
#define NR_CREDENTIAL_PARAMETER(type) \
  void Set(const char* key, type value) override { values.Set(key, value); } \
  NVSDK_NGX_Result Get(const char* key, type* value) const override { \
    if (wrapper_returned && std::strcmp(key, NVSDK_NGX_Parameter_Output) == 0) { \
      ++wrapper_post_reads; \
      wrapper_credential_seen |= addon::successful_sr_device != nullptr; \
      if (wrapper_probe_adoption) { \
        wrapper_adopted |= addon::EnsureDirectRuntime(wrapper_list); \
      } \
    } \
    return values.Get(key, value); \
  }
  NR_CREDENTIAL_PARAMETER(unsigned long long)
  NR_CREDENTIAL_PARAMETER(float)
  NR_CREDENTIAL_PARAMETER(double)
  NR_CREDENTIAL_PARAMETER(unsigned int)
  NR_CREDENTIAL_PARAMETER(int)
  NR_CREDENTIAL_PARAMETER(ID3D11Resource*)
  NR_CREDENTIAL_PARAMETER(ID3D12Resource*)
  NR_CREDENTIAL_PARAMETER(void*)
#undef NR_CREDENTIAL_PARAMETER
  void Reset() override { values.Reset(); }
};

NVSDK_NGX_Result NVSDK_CONV SuccessfulHostEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
  wrapper_returned = true;
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV SuccessfulHostEvaluateC(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C) {
  wrapper_returned = true;
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV FailCoreCreate(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
  ++failed_core_creates;
  *handle = nullptr;
  return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
}
NVSDK_NGX_Result NVSDK_CONV ForbiddenFallbackCreate(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    const NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
  ++forbidden_fallback_creates;
  *handle = nullptr;
  return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
}

void RunBorrowedWrapperClassification(Gpu& gpu) {
  const auto slot = addon::ngx_slot_real[0];
  const bool enabled = addon::enabled.exchange(true);
  const bool device_seen = addon::d3d12_device_seen.exchange(true);
  const bool foreign_seen = addon::foreign_nr_seen.exchange(true);
  const bool foreign_yield = addon::foreign_nr_yield.exchange(true);
  addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(SuccessfulHostEvaluate);
  addon::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(SuccessfulHostEvaluateC);
  wrapper_list = gpu.list.Get();
  CredentialParameters parameters;
  const auto* handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC0BE001});
  for (const bool c_export : {false, true}) {
    // Unknown, guessed DLSS, guessed RR, observed non-SR, observed SR/RR.
    for (unsigned source = 0; source < 6; ++source) {
      if (source == 1 || source == 2) {
        addon::features[handle].source_feature = (source == 1 ? addon::kFeatureDlss : addon::kFeatureDlssd);
        addon::features[handle].source_type_confirmed = true;
      } else if (source >= 3) {
        addon::created_feature_ids[handle] = (source == 3 ? NVSDK_NGX_Feature_FrameGeneration
            : source == 4 ? addon::kFeatureDlss : addon::kFeatureDlssd);
      }
      wrapper_returned = wrapper_credential_seen = wrapper_adopted = false;
      wrapper_post_reads = 0;
      wrapper_probe_adoption = source < 3;
      addon::last_present_steady_ns = addon::SteadyNowNs();
      const auto result = (c_export
          ? addon::HookedEvaluateFeatureCSlot<0>(gpu.list.Get(), handle, &parameters, nullptr)
          : addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), handle, &parameters, nullptr));
      Check(NVSDK_NGX_SUCCEED(result) && wrapper_returned, "wrapper preserves successful unknown/known Evaluate");
      if (source < 3) {
        Check(wrapper_post_reads != 0 && !wrapper_credential_seen && !wrapper_adopted
                  && addon::direct_device == nullptr && addon::direct_load_state == addon::DirectLoadState::Unknown,
              "successful unknown or guessed SR Evaluate cannot lend Core through either wrapper");
      } else {
        Check(wrapper_credential_seen == (source >= 4), "only observed SR/RR Create can grant wrapper credential");
      }
      Check(addon::successful_sr_device == nullptr, "wrapper credential ends with callback");
      addon::ForgetSourceHandleLocked(handle);
    }
  }
  wrapper_list = nullptr;
  wrapper_probe_adoption = false;
  addon::ngx_slot_real[0] = slot;
  addon::enabled = enabled;
  addon::d3d12_device_seen = device_seen;
  addon::foreign_nr_seen = foreign_seen;
  addon::foreign_nr_yield = foreign_yield;
  std::cout << "PASS borrowed Core C++/C successful unknown/guessed/non-SR/observed SR classification evidence=MOCK+GPU\n";
}

void RunBorrowedCore(Gpu& gpu, bool hardware, bool second_adapter) {
  Check(!addon::control_mask_use_core_context.load(), "Core diagnostic defaults off");
  const auto* published = addon::ngx_loader_published.load();
  addon::NgxLoaderSymbols symbols;
  symbols.nr_ready = symbols.core_ready = true;
  symbols.nr_module = symbols.core = GetModuleHandleW(nullptr);
  symbols.init = InitializeRuntime;
  symbols.shutdown = ShutdownRuntime;
  symbols.core_allocate = renodx::addons::dlss5::bridge::AllocateOwnedParameters;
  symbols.core_destroy = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
  symbols.core_create = CreateModel;
  symbols.core_evaluate = EvaluateModel;
  symbols.core_release = ReleaseModel;
  symbols.nr_requirements.file_version = "310.9.16.0";
  addon::ngx_loader_published.store(&symbols);
  addon::control_mask_use_core_context = true;
  addon::hook_point = addon::kHookUpscaled;
  addon::stack_passes = 1;
  addon::control_mask_enabled = false;
  RunBorrowedWrapperClassification(gpu);
  addon::last_result = NVSDK_NGX_Result_FAIL_InvalidParameter;
  const auto last = addon::last_result.load();
  Check(!addon::EnsureDirectRuntime(gpu.list.Get()) && !addon::LoadDirectApi()
            && addon::direct_load_state == addon::DirectLoadState::Unknown,
        "resolved exports without successful SR cannot adopt Core");
  {
    addon::SuccessfulSrCoreScope sr(gpu.list.Get());
    for (unsigned missing = 0; missing < 7; ++missing) {
      const auto complete = symbols;
      switch (missing) {
        case 0: symbols.core_allocate = nullptr; break;
        case 1: symbols.core_destroy = nullptr; break;
        case 2: symbols.core_create = nullptr; break;
        case 3: symbols.core_evaluate = nullptr; break;
        case 4: symbols.core_release = nullptr; break;
        case 5: symbols.core_ready = false; break;
        case 6: symbols.nr_requirements.file_version = "310.9.16.1"; break;
      }
      Check(!addon::EnsureDirectRuntime(gpu.list.Get())
                && addon::direct_load_state == addon::DirectLoadState::Unknown
                && addon::get_module_filename_iat == nullptr,
            "incomplete Core or wrong runtime cannot touch identity or adopt");
      symbols = complete;
    }
    Check(addon::EnsureDirectRuntime(gpu.list.Get()) && addon::direct_api.borrowed_core_context
              && addon::parameter_runtime_via_core && addon::direct_device == gpu.device.Get()
              && addon::direct_api.init == nullptr && addon::direct_api.shutdown == nullptr
              && addon::last_result.load() == last,
          "none baseline adopts exact SR device without fabricating Init result");
    Check(!addon::EnsureDirectRuntime(gpu.list.Get(), nullptr, true)
        && addon::direct_api.borrowed_core_context && addon::parameter_runtime_via_core,
        "private native request cannot override the explicit borrowed-Core diagnostic");
    {
      addon::SuccessfulSrCoreScope invalid(nullptr);
      Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "nested empty credential shadows ready device");
    }
    Check(addon::successful_sr_device == gpu.device.Get()
              && addon::EnsureDirectRuntime(gpu.list.Get()), "nested credential restores outer SR");
    {
      renodx::addons::dlss5::DirectCallScope own;
      Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "own NGX reentry cannot borrow inherited credential");
    }
    for (unsigned missing = 0; missing < 5; ++missing) {
      const auto complete = symbols;
      switch (missing) {
        case 0: symbols.core_allocate = nullptr; break;
        case 1: symbols.core_destroy = nullptr; break;
        case 2: symbols.core_create = nullptr; break;
        case 3: symbols.core_evaluate = nullptr; break;
        case 4: symbols.core_release = nullptr; break;
      }
      Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "ready fast path still requires every Core export");
      symbols = complete;
    }
    addon::control_mask_use_core_context = false;
    Check(!addon::EnsureDirectRuntime(gpu.list.Get()) && !addon::LoadDirectApi(),
          "borrowed ready backend cannot hot-switch to legacy");
    addon::control_mask_use_core_context = true;
    addon::direct_api.borrowed_core_context = false;
    Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "legacy ready backend cannot hot-switch to borrowed");
    addon::direct_api.borrowed_core_context = true;
    addon::hook_point = addon::kHookPresent;
    Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "Present cannot borrow current SR context");
    addon::hook_point = addon::kHookUpscaled;
    addon::stack_passes = 2;
    Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "stack cannot borrow P0 context");
    addon::stack_passes = 1;
    if (second_adapter && hardware) {
      Gpu other(false);
      Check(other.device.Get() != gpu.device.Get(), "distinct adapter device");
      Check(!addon::EnsureDirectRuntime(other.list.Get()), "SR credential cannot lend another device");
      {
        addon::SuccessfulSrCoreScope nested(other.list.Get());
        Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "nested SR cannot borrow outer device");
        Check(addon::EnsureDirectRuntime(other.list.Get()), "device change adopts only nested successful SR face");
      }
      Check(addon::EnsureDirectRuntime(gpu.list.Get()), "outer credential restored after device change");
    } else if (second_adapter) {
      std::cout << "CASE location=borrowed-core-device-change evidence=NOT_RUN recovery=requires-explicit-hardware-case\n";
    }
    addon::FeatureState state;
    addon::FinalResources resources;
    Check(addon::InstallQueueCompletionHooks(gpu.queue.Get()), "borrowed Core production queue tracking");
    Check(addon::EnsureNrFeature(gpu.list.Get(), state, 0, resources, 8, 8, 8, 8)
              && state.slots[0].via_core, "borrow creates plugin NR through Core");
    Check(!addon::Shutdown(false) && addon::direct_device == gpu.device.Get(),
          "borrowed teardown still retains unsubmitted feature recording");
    // Complete and Reset the real recording before releasing its mock feature.
    gpu.Submit();
    const auto releases = model_released_handles.size();
    addon::ReleaseNrSlot(state, 0, false);
    Check(state.slots[0].handle == nullptr && state.slots[0].parameters == nullptr
              && model_released_handles.size() == releases + 1, "borrow releases plugin feature and parameters");
    addon::core_create_feature = FailCoreCreate;
    addon::direct_api.create = ForbiddenFallbackCreate;
    addon::FeatureState rejected;
    Check(!addon::EnsureNrFeature(gpu.list.Get(), rejected, 0, resources, 8, 8, 8, 8)
              && rejected.slots[0].failed && rejected.slots[0].parameters == nullptr
              && failed_core_creates == 1 && forbidden_fallback_creates == 0
              && runtime_initializations == 0 && addon::get_module_filename_iat == nullptr,
          "borrowed Core Create failure releases parameters without fallback/identity/Init");
  }
  Check(!addon::EnsureDirectRuntime(gpu.list.Get()), "ready Core cannot serve unscoped fallback");
  Check(addon::Shutdown(false) && addon::direct_device == nullptr
            && runtime_initializations == 0 && runtime_shutdowns == 0
            && addon::get_module_filename_iat == nullptr,
        "full borrowed teardown makes no SDK Init/Shutdown or identity calls");
  addon::control_mask_use_core_context = false;
  {
    // The same WARP device/list is sufficient to expose a provider mismatch:
    // canonical device equality cannot authorize using Core on a private list.
    symbols.core_allocate = ProviderCoreAllocate;
    symbols.core_destroy = ProviderCoreDestroy;
    symbols.core_create = ProviderCoreCreate;
    symbols.core_evaluate = ProviderCoreEvaluate;
    symbols.core_release = ProviderCoreRelease;
    symbols.init = ProviderSharedInit;
    symbols.create = ForbiddenFallbackCreate;
    symbols.evaluate = ProviderCoreEvaluate;
    symbols.release = ProviderCoreRelease;
    symbols.shutdown = ProviderSharedShutdown;
    symbols.native_ready = true;
    symbols.native_api = {.private_instance = true, .module = GetModuleHandleW(nullptr),
        .init = ProviderSignedInit, .create = ProviderSignedCreate, .evaluate = ProviderSignedEvaluate,
        .release = ProviderSignedRelease, .shutdown = ProviderSignedShutdown};
    const auto device_id = ProviderDeviceId(gpu.device.Get());
    provider_shared_contexts[device_id] = true;  // The game initialized its proxy face first.
    provider_private_contexts.clear();
    provider_shared_shutdowns = 0;
    const auto configure_runtime = [&] {
      addon::ngx_loader_published.store(&symbols);
      addon::direct_api = {.module = GetModuleHandleW(nullptr), .init = ProviderSharedInit,
          .create = ForbiddenFallbackCreate, .evaluate = ProviderCoreEvaluate,
          .release = ProviderCoreRelease, .shutdown = ProviderSharedShutdown};
      addon::direct_load_state = addon::DirectLoadState::Ready;
      addon::direct_runtime_failed = false;
      addon::native_runtime_unload_safe = false;
    };
    configure_runtime();
    runtime_initializations = runtime_shutdowns = 0;
    Check(addon::InstallQueueCompletionHooks(gpu.queue.Get())
        && addon::InstallCommandListStateHooks(gpu.list.Get()), "private provider production recording hooks");
    addon::enabled = addon::hooks_enabled = true;
    addon::hook_point = addon::kHookUpscaled;
    addon::stack_passes = 2;
    addon::pass_stage[0] = 0;
    addon::nr_follow_input_res = false;
    addon::nr_resolution_scale = 1.f;
    addon::codec_mode = addon::feed_mode = 0;
    addon::look_mode = false;
    addon::intensity = .5f;
    addon::transfer_strength = addon::color_strength = 1.f;
    addon::detail_stability = 0;
    addon::control_mask_enabled = false;
    addon::dedupe_mode = renodx::addons::dlss5::kDedupePerEvaluate;
    const auto texture = [&](DXGI_FORMAT format, bool output) {
      const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
          .SampleDesc = {1, 0}, .Flags = (output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
      ComPtr<ID3D12Resource> resource;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
          output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : NATIVE_INPUT_STATE, nullptr,
          IID_PPV_ARGS(&resource))), "provider test WARP texture");
      return resource;
    };
    auto color = texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto output = texture(DXGI_FORMAT_R8G8B8A8_UNORM, true);
    auto depth = texture(DXGI_FORMAT_R32_FLOAT, false);
    auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, false);
    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    parameters.Set(NVSDK_NGX_Parameter_Width, 8u);
    parameters.Set(NVSDK_NGX_Parameter_Height, 8u);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 8u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 8u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 8u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 8u);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC0BE002});
    addon::RegisterCreatedFeatureLocked(owner, addon::kFeatureDlss, &parameters);
    bool touched = false;
    bool recorded = false;
    // Preserve production's create-now/evaluate-next-recording boundary for
    // both Passes. The last recording stays open for the pending-switch check.
    for (uint32_t frame = 0; frame < 3; ++frame) {
      recorded = addon::RecordInline(gpu.list.Get(), owner, &parameters, &touched);
      if (frame != 2) gpu.Submit();
    }
    Check(recorded && provider_core.allocations == 2 && provider_core.creates == 2
        && provider_core.evaluated_handles.size() == 2
        && provider_signed.creates == 0 && runtime_initializations == 1
        && provider_shared_contexts.at(device_id) && provider_private_contexts.empty(),
        "ordinary inline keeps Core calls and repeated native Init preserves the shared proxy cache");
    addon::ReleaseNrSlot(addon::features.at(owner), 1, true);
    const auto old_destroy = addon::core_destroy_parameters;
    const auto core_evaluates = provider_core.evaluates;
    const auto device_changes = addon::nr_runtime_device_changes.load();
    auto decline = renodx::addons::dlss5::NrDeclineReason::kNrRuntimeUnavailable;
    Check(!addon::EnsureDirectRuntime(gpu.list.Get(), &decline, true)
        && decline == renodx::addons::dlss5::NrDeclineReason::kRuntimeSwitchPending
        && addon::core_destroy_parameters == old_destroy && provider_core.releases == 0
        && provider_core.destroys == 0 && runtime_initializations == 1 && runtime_shutdowns == 0,
        "same-interface provider switch retains live and retired Core objects while work is pending");
    Check(SUCCEEDED(gpu.list->Close()), "provider recording Close");
    Check(!addon::EnsureDirectRuntime(gpu.list.Get(), &decline, true)
        && provider_core.releases == 0 && addon::core_destroy_parameters == old_destroy,
        "Close without submission cannot authorize a provider switch");
    ID3D12CommandList* lists[]{gpu.list.Get()};
    const auto pending = renodx::addons::dlss5::submission::BeginSubmission(1, lists);
    gpu.queue->ExecuteCommandLists(1, lists);
    gpu.submission_pending = true;
    Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "provider recording completion signal");
    renodx::addons::dlss5::submission::CompleteSubmission(pending, gpu.fence.Get(), gpu.serial);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr, "provider recording completion event");
    const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
    const auto wait = (SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
    CloseHandle(event);
    Check(wait == WAIT_OBJECT_0 && gpu.fence->GetCompletedValue() != UINT64_MAX,
        "provider recording completes on WARP");
    gpu.submission_pending = false;
    Check(addon::TeardownProof() == renodx::addons::dlss5::submission::RecordedWork::kReplayable
        && !addon::EnsureDirectRuntime(gpu.list.Get(), &decline, true)
        && provider_core.releases == 0 && provider_core.destroys == 0
        && addon::core_destroy_parameters == old_destroy,
        "completed but replayable Core recording retains the old provider and allocator");
    Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
        "provider recording legitimate Reset");
    renodx::addons::dlss5::submission::OnCommandListReset(gpu.list.Get());
    Check(addon::EnsureDirectRuntime(gpu.list.Get(), &decline, true)
        && !addon::parameter_runtime_via_core && provider_core.releases == 2 && provider_core.destroys == 2
        && provider_core.handles.empty() && provider_core.parameters.empty()
        && runtime_initializations == 2 && runtime_shutdowns == 0 && provider_shared_shutdowns == 0
        && addon::direct_api.private_instance && provider_shared_contexts.at(device_id)
        && provider_private_contexts.contains(device_id) && !provider_private_contexts.at(device_id)
        && addon::nr_runtime_device_changes.load() == device_changes,
        "Core-to-private switch retires OLD objects, initializes its own cache, and never shuts down the game context");
    addon::hook_point = addon::kHookPresent;
    addon::PresentTarget target{.output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        .workset_key = output.Get(), .private_signed_runtime = true};
    for (uint32_t frame = 0; frame < 3; ++frame) {
      recorded = addon::RecordInline(gpu.list.Get(), owner, &parameters, &touched, &target);
      if (frame != 2) gpu.Submit();
    }
    Check(recorded && provider_signed.creates == 2 && provider_signed.evaluated_handles.size() == 2
        && provider_core.allocations == 2 && provider_core.creates == 2 && provider_core.evaluates == core_evaluates
        && addon::EnsureDirectRuntime(gpu.list.Get()) && runtime_initializations == 2,
        "private Present and every stacked/nested Ensure remain signed without new Core calls or Init");
    gpu.Submit();
    fail_private_shutdown = true;
    const auto private_shutdowns_before = runtime_shutdowns;
    const auto private_api_before = addon::direct_api;
    auto** private_iat_before = addon::get_module_filename_iat;
    Check(!addon::EnsureDirectRuntime(gpu.list.Get(), nullptr, false)
        && addon::direct_api.module == private_api_before.module && addon::direct_api.private_instance
        && addon::direct_device == gpu.device.Get() && addon::get_module_filename_iat == private_iat_before
        && runtime_shutdowns == private_shutdowns_before + 1 && provider_private_contexts.contains(device_id)
        && provider_shared_shutdowns == 0,
        "failed private Shutdown retains its API, device, context and IAT instead of switching providers");
    Check(!addon::EnsureDirectRuntime(gpu.list.Get()) && runtime_shutdowns == private_shutdowns_before + 1,
        "a context with failed Shutdown cannot resume private evaluations");
    Check(!addon::Shutdown(false) && !addon::shutting_down.load()
        && addon::direct_api.module == private_api_before.module && addon::direct_api.private_instance
        && addon::direct_device == gpu.device.Get() && addon::get_module_filename_iat == private_iat_before
        && runtime_shutdowns == private_shutdowns_before + 2 && provider_private_contexts.contains(device_id),
        "failed full Shutdown retains the private context and cannot claim teardown success");
    fail_private_shutdown = false;
    Check(addon::EnsureDirectRuntime(gpu.list.Get(), nullptr, false)
        && !addon::direct_api.private_instance && addon::parameter_runtime_via_core
        && provider_signed.releases == 2 && provider_signed.handles.empty()
        && provider_private_contexts.empty() && provider_shared_contexts.at(device_id)
        && runtime_shutdowns == private_shutdowns_before + 3 && provider_shared_shutdowns == 0,
        "explicit ordinary selection retires private objects with their provider and preserves the game cache");
    Check(addon::EnsureDirectRuntime(gpu.list.Get(), nullptr, true)
        && addon::direct_api.private_instance && provider_private_contexts.contains(device_id)
        && provider_shared_contexts.at(device_id) && provider_shared_shutdowns == 0,
        "private OFF-to-ON starts only its own native context again");
    addon::direct_api.module = nullptr;  // The fixture borrowed the EXE, never a loader-owned reference.
    Check(addon::Shutdown(false) && provider_signed.releases == 2 && provider_signed.handles.empty()
        && runtime_shutdowns == private_shutdowns_before + 4 && provider_private_contexts.empty() && provider_shared_shutdowns == 0
        && provider_shared_contexts.at(device_id), "private teardown shuts down only its own context");
    for (const bool fail_init : {true, false}) {
      configure_runtime();
      fail_private_init = fail_init;
      fail_private_create = !fail_init;
      const auto core_creates = provider_core.creates;
      const auto creates = provider_signed.creates;
      const bool initialized = addon::EnsureDirectRuntime(gpu.list.Get(), nullptr, true);
      if (fail_init) {
        Check(!initialized && !addon::EnsureDirectRuntime(gpu.list.Get())
            && provider_signed.creates == creates && provider_core.creates == core_creates,
            "private signed Init failure never falls back to Core");
        Check(addon::EnsureDirectRuntime(gpu.list.Get(), nullptr, false)
            && !addon::direct_api.private_instance && !addon::direct_runtime_failed,
            "a private Init failure does not poison an explicitly selected ordinary provider");
        const auto initializations = runtime_initializations;
        Check(!addon::EnsureDirectRuntime(gpu.list.Get(), nullptr, true)
            && !addon::direct_api.private_instance && runtime_initializations == initializations
            && addon::native_runtime_failed_instance == symbols.native_api.module,
            "ordinary-to-private routing cannot retry a failed private context in the same image generation");
      } else {
        addon::FeatureState rejected;
        addon::FinalResources resources;
        Check(initialized && !addon::EnsureNrFeature(gpu.list.Get(), rejected, 0, resources, 8, 8, 8, 8)
            && rejected.slots[0].failed && rejected.slots[0].parameters == nullptr
            && rejected.slots[0].handle == nullptr && provider_signed.creates == creates + 1
            && provider_core.creates == core_creates && provider_signed.handles.empty(),
            "private signed Create failure destroys its OwnedParameters and does not call Core");
      }
      addon::direct_api.module = nullptr;
      Check(addon::Shutdown(false), "failed private provider test cleanup");
    }
    configure_runtime();
    symbols.native_ready = false;
    symbols.native_failed = true;
    const auto ordinary_module = addon::direct_api.module;
    Check(!addon::EnsureDirectRuntime(gpu.list.Get(), nullptr, true)
        && addon::direct_api.module == ordinary_module && !addon::direct_api.private_instance
        && addon::EnsureDirectRuntime(gpu.list.Get(), nullptr, false) && provider_shared_shutdowns == 0,
        "failed private image preparation retains the ordinary API without an implicit shared fallback");
    addon::direct_api.module = nullptr;
    Check(addon::Shutdown(false) && provider_shared_shutdowns == 0
        && provider_shared_contexts.at(device_id), "ordinary teardown retains the game's shared context");
    symbols.native_ready = true;
    symbols.native_failed = false;
    fail_private_init = fail_private_create = false;
    runtime_initializations = runtime_shutdowns = 0;
    addon::stack_passes = 1;
    addon::hook_point = addon::kHookUpscaled;
    std::cout << "PASS private signed provider exact-interface Core switch pending/replayable/Reset nested stack failure evidence=WARP+MOCK real_NGX=NOT_RUN\n";
  }
  addon::ngx_loader_published.store(published);
  std::cout << "PASS borrowed Core credential/exports/backend/teardown evidence=MOCK+GPU no real NGX\n";
}

void STDMETHODCALLTYPE ObservePipeline(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline) {
  if (model_gpu != nullptr && list == model_gpu->list.Get()) {
    observed_pipeline = pipeline;
  }
  real_pipeline(list, pipeline);
}

void STDMETHODCALLTYPE ObserveDispatch(ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z) {
  if (list == native_single_feature_dispatch_list) {
    if (native_single_feature_dispatches.size() < native_single_feature_dispatches.capacity()) {
      native_single_feature_dispatches.push_back({x, y, z});
    } else {
      ++native_single_feature_dispatch_overflow;
    }
  }
  if (native_cost_queries != nullptr && list == native_cost_list
      && observed_pipeline == addon::codec_pipeline.control_mask) {
    ++native_cost_dispatches;
    if (native_cost_dispatches == 1) {
      native_cost_queries->Begin(list, native_cost_interval + 2);
      real_dispatch(list, x, y, z);
      list->EndQuery(native_cost_queries->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                     2 * (native_cost_interval + 2) + 1);
      return;
    }
  }
  if (model_gpu != nullptr && list == model_gpu->list.Get()
      && addon::codec_pipeline.control_mask != nullptr
      && observed_pipeline == addon::codec_pipeline.control_mask) {
    ++mask_dispatches;
  }
  real_dispatch(list, x, y, z);
}

HRESULT STDMETHODCALLTYPE Allocate(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, REFIID iid, void** resource) {
  if (fail_mask_allocation && desc->Format == DXGI_FORMAT_R8G8B8A8_UNORM
      && state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS
      && (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
    ++allocation_failures;
    *resource = nullptr;
    return E_OUTOFMEMORY;
  }
  return real_allocate(device, heap, flags, desc, state, clear, iid, resource);
}

void RunInline(Gpu& gpu, bool hardware, bool safety) {
  const bool saved_motion_enabled = addon::control_mask_motion_enabled.load();
  const float saved_motion_start = addon::control_mask_motion_start.exchange(2.f);
  const float saved_motion_end = addon::control_mask_motion_end.exchange(16.f);
  using renodx::addons::dlss5::NrDeclineReason;
  using renodx::addons::dlss5::kNrDeclineTags;
  Check(static_cast<size_t>(NrDeclineReason::kControlMaskSubmissionsUnproven)
                == static_cast<size_t>(NrDeclineReason::kStageResolutionBackend) + 1
            && std::strcmp(kNrDeclineTags[static_cast<size_t>(NrDeclineReason::kWorksetSetupFailed)], "setup_failed") == 0
            && std::strcmp(kNrDeclineTags[static_cast<size_t>(NrDeclineReason::kControlMaskSubmissionsUnproven)], "control_mask_unproven") == 0
            && std::strcmp(kNrDeclineTags[static_cast<size_t>(NrDeclineReason::kControlMaskReuseNotReady)], "control_mask_reuse_not_ready") == 0,
        "ControlMask decline tags append to the taxonomy and preserve setup_failed");
  model_gpu = &gpu;
  addon::enabled = true;
  addon::hooks_enabled = true;
  addon::dedupe_mode = renodx::addons::dlss5::kDedupePerEvaluate;
  addon::hook_point = addon::kHookUpscaled;
  addon::stack_passes = 1;
  addon::nr_follow_input_res = false;
  addon::nr_resolution_scale = 1.f;
  addon::feed_mode = 0;
  addon::look_mode = false;
  addon::detail_stability = 1;
  addon::transfer_strength = 1.f;
  addon::color_strength = 1.f;
  addon::intensity = .5f;
  addon::preset = 2;
  addon::codec_mode = 0;
  addon::direct_api.module = GetModuleHandleW(nullptr);
  addon::direct_api.create = CreateModel;
  addon::direct_api.evaluate = EvaluateModel;
  addon::direct_api.release = ReleaseModel;
  addon::direct_load_state = addon::DirectLoadState::Ready;
  addon::direct_device = gpu.device.Get();
  addon::direct_device->AddRef();
  addon::direct_device_native = gpu.device.Get();
  addon::direct_runtime_requirements.dual_models = true;
  addon::direct_runtime_requirements.file_version = "310.9.16.0";
  addon::parameter_runtime_ready = true;
  addon::core_allocate_parameters = renodx::addons::dlss5::bridge::AllocateOwnedParameters;
  addon::core_destroy_parameters = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
  const auto texture = [&](DXGI_FORMAT format, bool uav) {
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .Format = format,
        .SampleDesc = {1, 0}, .Flags = (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE)};
    ComPtr<ID3D12Resource> resource;
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))), "ControlMask inline texture");
    return resource;
  };
  auto source = texture(DXGI_FORMAT_R8G8B8A8_UNORM, false);
  auto output = texture(DXGI_FORMAT_R8G8B8A8_UNORM, true);
  auto depth = texture(DXGI_FORMAT_R32_FLOAT, false);
  auto motion = texture(DXGI_FORMAT_R16G16_FLOAT, false);
  gpu.Write(source.Get(), 96);
  gpu.Write(output.Get(), 96);
  gpu.Write(depth.Get(), 0);
  gpu.Write(motion.Get(), 0x50);
  gpu.Transition(source.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
  gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
  gpu.Transition(motion.Get(), D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
  gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  renodx::addons::dlss5::bridge::OwnedParameters parameters;
  parameters.Set(NVSDK_NGX_Parameter_Color, source.Get());
  parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
  parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
  parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
  parameters.Set(NVSDK_NGX_Parameter_Width, 8u);
  parameters.Set(NVSDK_NGX_Parameter_Height, 8u);
  parameters.Set(NVSDK_NGX_Parameter_OutWidth, 8u);
  parameters.Set(NVSDK_NGX_Parameter_OutHeight, 8u);
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 8u);
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 8u);
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
      static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
  parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
  parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
  const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04A6});
  addon::RegisterCreatedFeatureLocked(owner, addon::kFeatureDlss, &parameters);
  real_dispatch = reinterpret_cast<DispatchFn>((*reinterpret_cast<void***>(gpu.list.Get()))[14]);
  real_pipeline = reinterpret_cast<PipelineFn>((*reinterpret_cast<void***>(gpu.list.Get()))[25]);
  real_allocate = reinterpret_cast<AllocateFn>((*reinterpret_cast<void***>(gpu.device.Get()))[27]);
  real_root = reinterpret_cast<RootFn>((*reinterpret_cast<void***>(gpu.device.Get()))[16]);
  real_pso = reinterpret_cast<PsoFn>((*reinterpret_cast<void***>(gpu.device.Get()))[11]);
  real_heap = reinterpret_cast<HeapFn>((*reinterpret_cast<void***>(gpu.device.Get()))[14]);
  real_signal = reinterpret_cast<SignalFn>((*reinterpret_cast<void***>(gpu.queue.Get()))[14]);
  real_completed = reinterpret_cast<CompletedFn>((*reinterpret_cast<void***>(gpu.fence.Get()))[8]);
  real_removed = reinterpret_cast<RemovedFn>((*reinterpret_cast<void***>(gpu.device.Get()))[37]);
  Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR,
        "ControlMask observer transaction");
  Check(DetourAttach(reinterpret_cast<PVOID*>(&real_dispatch), reinterpret_cast<PVOID>(ObserveDispatch)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_pipeline), reinterpret_cast<PVOID>(ObservePipeline)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_allocate), reinterpret_cast<PVOID>(Allocate)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_root), reinterpret_cast<PVOID>(CreateRoot)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_pso), reinterpret_cast<PVOID>(CreatePso)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_heap), reinterpret_cast<PVOID>(CreateHeap)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_signal), reinterpret_cast<PVOID>(Signal)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_completed), reinterpret_cast<PVOID>(Completed)) == NO_ERROR
            && DetourAttach(reinterpret_cast<PVOID*>(&real_removed), reinterpret_cast<PVOID>(RemovedReason)) == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR, "ControlMask observers attached");
  const auto record = [&] {
    const auto before = model_calls;
    const auto gate_before = addon::control_mask_reuse_stats;
    addon::ProcessInline(gpu.list.Get(), owner, &parameters);
    Check(addon::control_mask_reuse_stats.checks - gate_before.checks <= 1
              && addon::control_mask_reuse_stats.admitted - gate_before.admitted
                  <= addon::control_mask_reuse_stats.checks - gate_before.checks,
          "one ProcessInline contributes at most one coherent reuse check and admission");
    gpu.Submit();
    addon::DrainRetiredResources(true);
    Check(model_calls <= before + addon::stack_passes.load(),
          "NR Evaluate count stays within the configured stack, including failures");
    return model_calls - before;
  };
  const auto mature = [&] {
    for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
      if (record() == 1) return;
    }
    Check(false, "ControlMask production path matures within bounded attempts");
  };
  for (const bool seed_mask : {false, true}) {
    if (!safety) break;
    Check(!addon::control_mask_use_core_context.load() && !addon::direct_api.borrowed_core_context,
          "stage-switch regression uses the owned backend with the legacy P0 flag off");
    const auto saved_export = addon::ngx_slot_real[0].evaluate;
    addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(SourceEvaluate);
    addon::hook_point = addon::kHookRender;
    addon::control_mask_enabled = seed_mask;
    addon::configuration_generation.fetch_add(1);
    bool render_ready = false;
    for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
      const auto before = model_calls;
      addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
      gpu.Submit();
      addon::DrainRetiredResources(true);
      if (model_calls == before + 1) {
        render_ready = true;
        break;
      }
    }
    Check(render_ready && (addon::features.at(owner).control_mask_stream != nullptr) == seed_mask,
          "Render seeds ordered History only when a mask was actually requested");
    addon::hook_point = addon::kHookUpscaled;
    addon::control_mask_enabled = false;
    addon::configuration_generation.fetch_add(1);
    const auto saved_observer = model_parameter_observer;
    uint32_t credential_evaluations = 0;
    model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block) {
      Check(!seed_mask || addon::successful_sr_device == gpu.device.Get(),
            "owned NR receives the current successful host SR device through the real wrapper");
      Check(addon::GetD3D12Resource(block, "DLSSNR.ControlMask") == nullptr,
            "every unmasked stack pass has a null ControlMask binding");
      ++credential_evaluations;
    };
    const auto dispatch_before = mask_dispatches;
    for (uint32_t passes : {1u, 2u}) {
      addon::stack_passes = passes;
      addon::configuration_generation.fetch_add(1);
      uint32_t stable_frames = 0;
      uint64_t stable_workset = 0;
      for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
        const auto before = model_calls;
        const auto result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
        Check(NVSDK_NGX_SUCCEED(result) && addon::successful_sr_device == nullptr,
              "owned wrapper preserves host SR success and ends its device credential");
        gpu.Submit();
        addon::DrainRetiredResources(true);
        if (model_calls != before + passes) continue;
        const auto& workset = addon::worksets.at({owner, output.Get(), false});
        if (stable_frames == 0) stable_workset = workset.id;
        Check(workset.id == stable_workset && workset.control_mask == nullptr
                  && (addon::features.at(owner).control_mask_stream != nullptr) == seed_mask
                  && mask_dispatches == dispatch_before,
              "mask-off Upscaled retains ordered History and reuses its unmasked workset");
        if (++stable_frames == 3) break;
      }
      std::cout << "CASE owned-mask-off Render->Upscaled seed_mask=" << seed_mask << " passes=" << passes
                << " completed_frames=" << stable_frames << " evidence=MOCK+GPU" << std::endl;
      Check(stable_frames == 3,
            "owned mask-off Upscaled resumes one/two-pass NR after Render without reuse_not_ready starvation");
    }
    Check(credential_evaluations >= 9, "both owned stack sizes evaluate with wrapper-granted credentials");
    const auto before_negative = model_calls;
    fail_source_evaluate = true;
    const auto failed_sr = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
    fail_source_evaluate = false;
    Check(NVSDK_NGX_FAILED(failed_sr) && model_calls == before_negative
              && addon::successful_sr_device == nullptr,
          "failed host SR cannot grant an owned Upscaled credential or Evaluate NR");
    if (seed_mask) {
      addon::created_feature_ids.erase(owner);
      const auto unknown_sr = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
      Check(NVSDK_NGX_SUCCEED(unknown_sr) && model_calls == before_negative
                && addon::successful_sr_device == nullptr,
            "successful guessed SR without observed Create cannot reuse an existing owned stream");
      addon::created_feature_ids[owner] = addon::kFeatureDlss;
    }
    gpu.Submit();
    addon::DrainRetiredResources(true);
    model_parameter_observer = saved_observer;
    addon::ngx_slot_real[0].evaluate = saved_export;
    addon::stack_passes = 1;
    addon::ForgetSourceHandleLocked(owner);
    addon::DrainRetiredResources(true);
    addon::RegisterCreatedFeatureLocked(owner, addon::kFeatureDlss, &parameters);
    addon::configuration_generation.fetch_add(1);
  }
  const auto default_off_dispatches = mask_dispatches;
  const auto default_off_reuse = addon::control_mask_reuse_stats;
  addon::control_mask_enabled = false;
  mature();
  Check(mask_dispatches == default_off_dispatches, "default-off ProcessInline records zero ControlMask dispatches");
  Check(addon::control_mask_reuse_stats.checks == default_off_reuse.checks
            && addon::control_mask_reuse_stats.admitted == default_off_reuse.admitted,
        "default-off without old mask objects contributes no reuse checks or admissions");
  Check(addon::worksets.at({owner, output.Get(), false}).control_mask == nullptr,
        "default-off production path allocates no mask");
  Check(addon::worksets.at({owner, output.Get(), false}).recording == nullptr
            && addon::features.at(owner).slots[0].recording == nullptr,
        "default-off NR allocates no ControlMask recording observations");
  addon::control_mask_enabled = true;
  addon::control_mask_motion_enabled = true;
  const auto verify_motion_mask = [&](ID3D12Resource* mask, uint8_t expected_red) {
    Check(mask != nullptr, "production motion route supplies a mask for readback");
    auto transfer = gpu.CreateTextureTransfer(mask);
    gpu.Transition(mask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.RecordTextureReadback(mask, transfer);
    gpu.Transition(mask, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.Submit();
    const auto bytes = gpu.ReadTexture(transfer);
    for (size_t pixel = 0; pixel < bytes.size(); pixel += 4) {
      Check(bytes[pixel] == expected_red && bytes[pixel + 1] == 255
                && bytes[pixel + 2] == 255 && bytes[pixel + 3] == 255,
            "production motion route attenuates R and preserves G/B/A; missing provenance restores manual R");
    }
  };
  const auto set_motion_texels = [&](uint16_t x, uint16_t y) {
    std::array<uint16_t, 8 * 8 * 2> vectors{};
    for (size_t pixel = 0; pixel < vectors.size(); pixel += 2) {
      vectors[pixel] = x;
      vectors[pixel + 1] = y;
    }
    auto transfer = gpu.CreateTextureTransfer(motion.Get());
    gpu.Transition(motion.Get(), NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.UploadTexture(motion.Get(), transfer, reinterpret_cast<const uint8_t*>(vectors.data()), sizeof(vectors), 8 * 4);
    gpu.Transition(motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
    gpu.Submit();
  };
  bool saw_mask = false;
  model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block) {
    saw_mask = addon::GetD3D12Resource(block, "DLSSNR.ControlMask") != nullptr;
  };
  mature();
  Check(saw_mask && mask_dispatches > 0, "production Evaluate receives generated mask");
  const auto previous_evaluate = addon::ngx_slot_real[0].evaluate;
  const auto previous_evaluate_c = addon::ngx_slot_real[0].evaluate_c;
  addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(SourceEvaluate);
  addon::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(SourceEvaluateC);
  const auto exercise_hook = [&](bool c_export) {
    const auto source_before = source_evaluate_calls;
    const auto source_c_before = source_evaluate_c_calls;
    const auto model_before = model_calls;
    const auto dispatch_before = mask_dispatches;
    gpu.list->ClearState(nullptr);
    const auto result = c_export
        ? addon::HookedEvaluateFeatureCSlot<0>(gpu.list.Get(), owner, &parameters, nullptr)
        : addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
    Check(NVSDK_NGX_SUCCEED(result), "production NGX evaluate wrapper preserves a successful source result");
    Check(source_evaluate_calls == source_before + 1
              && source_evaluate_c_calls == source_c_before + (c_export ? 1u : 0u),
          "production evaluate wrapper calls its source once, including C-to-C++ delegation");
    gpu.Submit();
    addon::DrainRetiredResources(true);
    Check(model_calls == model_before + 1,
          "production evaluate wrapper records one real NR path");
    Check(mask_dispatches == dispatch_before + 1,
          "production evaluate wrapper records one ControlMask dispatch");
    const auto& wrapper_slot = addon::features.at(owner).slots[0];
    Check(!wrapper_slot.control_mask_bound
              && addon::GetD3D12Resource(wrapper_slot.parameters, "DLSSNR.ControlMask") == nullptr,
          "production evaluate wrapper clears the transient mask binding");
    std::cout << "CASE source=MOCK wrapper=" << (c_export ? "C+Ex/C++" : "Ex/C++")
              << " inner_ngx=MOCK completion=Fence" << std::endl;
  };
  exercise_hook(false);
  exercise_hook(true);
  // FP16 (3,12) with scale (-2,0.5) gives sqrt(72) pixels/frame: R=142.
  // Both scale components affect this non-saturated result.
  set_motion_texels(0x4200, 0x4a00);
  {
    const auto* present_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04AA});
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, -2.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 0.5f);
    addon::RegisterCreatedFeatureLocked(present_owner, addon::kFeatureDlss, &parameters);
    const uint32_t saved_control_mask_mode = addon::control_mask_mode.exchange(addon::kControlMaskDepthFar);
    auto present_output = texture(DXGI_FORMAT_R8G8B8A8_UNORM, true);
    gpu.Write(present_output.Get(), 96);
    parameters.Set(NVSDK_NGX_Parameter_Output, present_output.Get());
    addon::PresentTarget target{};
    target.workset_key = source.Get();
    target.domain = renodx::addons::dlss5::present_restore_scope::Domain::FINAL_PRESENT;
    target.guide_serial = 1;
    const auto saved_parameter_observer = model_parameter_observer;
    ID3D12Resource* present_mask = nullptr;
    model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block) {
      present_mask = addon::GetD3D12Resource(block, "DLSSNR.ControlMask");
    };
    bool present_ready = false;
    for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
      const auto model_before = model_calls;
      bool touched = false;
      const bool ran = addon::ProcessInline(gpu.list.Get(), present_owner, &parameters, &touched, &target);
      gpu.Submit();
      addon::DrainRetiredResources(true);
      if (!ran || model_calls != model_before + 1) continue;
      Check(touched && present_mask != nullptr,
            "ordinary final-Present production evaluation binds its generated ControlMask");
      present_ready = true;
      break;
    }
    Check(present_ready, "ordinary final-Present ControlMask route matures within bounded attempts");
    auto& feature = addon::features.at(present_owner);
    Check(!feature.slots[0].control_mask_bound
              && addon::GetD3D12Resource(feature.slots[0].parameters, "DLSSNR.ControlMask") == nullptr,
          "ordinary final-Present evaluation clears its transient ControlMask binding");
    Check(addon::worksets.at({present_owner, source.Get(), false}).control_mask == present_mask,
          "ordinary final-Present mask workset remains owned after Fence completion");
    verify_motion_mask(present_mask, 142);
    std::cout << "CASE path=D3D12-final-Present gpu=" << (hardware ? "NATIVE_GPU" : "WARP")
              << " ngx=MOCK completion=Fence mask=bound+cleared" << std::endl;
    target.guide_serial = 0;
    for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
      const auto model_before = model_calls;
      bool touched = false;
      present_mask = nullptr;
      addon::ProcessInline(gpu.list.Get(), present_owner, &parameters, &touched, &target);
      gpu.Submit();
      addon::DrainRetiredResources(true);
      if (model_calls == model_before + 1) {
        Check(present_mask != nullptr,
              "final Present without a depth-guide serial keeps manual mask settings active");
        verify_motion_mask(present_mask, 255);
        break;
      }
    }
    Check(present_mask != nullptr && !feature.slots[0].control_mask_bound
              && addon::GetD3D12Resource(feature.slots[0].parameters, "DLSSNR.ControlMask") == nullptr,
          "final Present fallback clears its transient ControlMask binding");
    const std::array pre_fg_roles{
        std::pair{renodx::addons::dlss5::present_restore_scope::Domain::PRE_FG_BACKBUFFER, uintptr_t{0xC04AB}},
        std::pair{renodx::addons::dlss5::present_restore_scope::Domain::PRE_FG_HUDLESS, uintptr_t{0xC04AC}}};
    for (const auto& [domain, owner_value] : pre_fg_roles) {
      const auto* role_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(owner_value);
      addon::RegisterCreatedFeatureLocked(role_owner, addon::kFeatureDlss, &parameters);
      auto role_output = texture(DXGI_FORMAT_R8G8B8A8_UNORM, true);
      gpu.Write(role_output.Get(), 96);
      parameters.Set(NVSDK_NGX_Parameter_Output, role_output.Get());
      addon::PresentTarget role_target = target;
      role_target.domain = domain;
      role_target.guide_serial = 1;
      bool role_ready = false;
      for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
        const auto model_before = model_calls;
        bool touched = false;
        present_mask = nullptr;
        const bool ran = addon::ProcessInline(gpu.list.Get(), role_owner, &parameters, &touched, &role_target);
        gpu.Submit();
        addon::DrainRetiredResources(true);
        if (!ran || model_calls != model_before + 1) continue;
        Check(touched && present_mask != nullptr,
              "pre-FG Backbuffer/HUDLess evaluation binds its generated depth ControlMask");
        role_ready = true;
        break;
      }
      Check(role_ready, "pre-FG Backbuffer/HUDLess ControlMask route matures within bounded attempts");
      auto& role_feature = addon::features.at(role_owner);
      Check(!role_feature.slots[0].control_mask_bound
                && addon::GetD3D12Resource(role_feature.slots[0].parameters, "DLSSNR.ControlMask") == nullptr
                && addon::worksets.at({role_owner, source.Get(), false}).control_mask == present_mask,
            "pre-FG role clears its NGX binding while retaining the completed mask workset");
      verify_motion_mask(present_mask, 142);
      std::cout << "CASE path=FG-input-role domain=" << static_cast<uint32_t>(domain)
                << " gpu=" << (hardware ? "NATIVE_GPU" : "WARP")
                << " ngx=MOCK completion=Fence mask=bound+cleared" << std::endl;
      role_target.guide_serial = 0;
      present_mask = nullptr;
      const auto model_before_missing_guide = model_calls;
      addon::ProcessInline(gpu.list.Get(), role_owner, &parameters, nullptr, &role_target);
      gpu.Submit();
      addon::DrainRetiredResources(true);
      Check(present_mask != nullptr && model_calls == model_before_missing_guide + 1
                && !role_feature.slots[0].control_mask_bound
                && addon::GetD3D12Resource(role_feature.slots[0].parameters, "DLSSNR.ControlMask") == nullptr,
            "pre-FG role without guide provenance keeps manual settings and clears the transient binding");
      verify_motion_mask(present_mask, 255);
      parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
      addon::ForgetSourceHandleLocked(role_owner);
      addon::DrainRetiredResources(true);
    }
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    model_parameter_observer = saved_parameter_observer;
    addon::control_mask_mode.store(saved_control_mask_mode);
    addon::ForgetSourceHandleLocked(present_owner);
    addon::DrainRetiredResources(true);
  }
  {
    const uint32_t saved_hook_point = addon::hook_point.exchange(addon::kHookRender);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, -2.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 0.5f);
    const uint32_t saved_stack_passes = addon::stack_passes.exchange(2);
    const uint32_t saved_second_stage = addon::pass_stage[0].exchange(2);
    addon::configuration_generation.fetch_add(1);
    const auto mixed_plan = addon::EvaluationStagePlan(owner);
    Check(mixed_plan.valid && mixed_plan.mixed
              && mixed_plan.At(renodx::addons::dlss5::stage_plan::Stage::RENDER).count == 1
              && mixed_plan.At(renodx::addons::dlss5::stage_plan::Stage::UPSCALED).count == 1,
          "Render-to-Upscaled ControlMask case uses the existing legal stage plan");
    const auto saved_parameter_observer = model_parameter_observer;
    std::vector<const NVSDK_NGX_Handle*> mixed_model_handles;
    std::vector<ID3D12Resource*> mixed_masks;
    model_parameter_observer = [&](const NVSDK_NGX_Handle* model_handle, const NVSDK_NGX_Parameter* block) {
      mixed_model_handles.push_back(model_handle);
      mixed_masks.push_back(addon::GetD3D12Resource(block, "DLSSNR.ControlMask"));
    };
    bool mixed_chain_ready = false;
    for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
      mixed_model_handles.clear();
      mixed_masks.clear();
      source_evaluate_color = nullptr;
      const uint32_t model_before = model_calls;
      const uint32_t dispatch_before = mask_dispatches;
      gpu.list->ClearState(nullptr);
      const auto result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
      Check(NVSDK_NGX_SUCCEED(result), "mixed Render-to-Upscaled host SR completes");
      gpu.Submit();
      addon::DrainRetiredResources(true);
      if (model_calls != model_before + 2 || mask_dispatches != dispatch_before + 2) continue;
      auto& feature = addon::features.at(owner);
      Check(feature.render_session != nullptr && feature.render_session.get() != &feature
                && mixed_model_handles.size() == 2 && mixed_masks.size() == 2
                && mixed_model_handles[0] == feature.render_session->slots[0].handle
                && mixed_model_handles[1] == feature.slots[0].handle,
            "Render and Upscaled execute distinct Feature histories in stage order");
      const auto& render_workset = addon::worksets.at({owner, source.Get(), true});
      Check(render_workset.pre_sr_color != nullptr
                && source_evaluate_color == render_workset.pre_sr_color
                && addon::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == source.Get(),
            "successful host SR consumes the Render result and the wrapper restores game Color");
      Check(mixed_masks[0] != nullptr && mixed_masks[1] != nullptr
                && mixed_masks[0] != mixed_masks[1]
                && mixed_masks[0]->GetDesc().Width == 8 && mixed_masks[1]->GetDesc().Width == 8
                && !feature.render_session->slots[0].control_mask_bound
                && !feature.slots[0].control_mask_bound,
            "each stage binds its own generated mask and clears the transient NGX parameter");
      mixed_chain_ready = true;
      break;
    }
    Check(mixed_chain_ready, "Render-to-Upscaled ControlMask chain matures within bounded attempts");
    verify_motion_mask(mixed_masks[0], 142);
    verify_motion_mask(mixed_masks[1], 142);
    set_motion_texels(0x5050, 0x5050);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    std::cout << "CASE motion-routes=Render+Upscaled+Present+FG-roles rgba=verified"
                 " motion=(3,12) scale=-2,0.5 expected_R=142 missing_provenance=manual_R" << std::endl;
    {
      ComPtr<ID3D12CommandAllocator> alternate_allocator;
      ComPtr<ID3D12GraphicsCommandList> alternate_list;
      Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&alternate_allocator))), "alternate Render allocator");
      Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
          alternate_allocator.Get(), nullptr, IID_PPV_ARGS(&alternate_list))), "alternate Render list");
      const auto gate_before = addon::control_mask_reuse_stats;
      const auto declines_before = renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
          renodx::addons::dlss5::NrDeclineReason::kControlMaskReuseNotReady)].load();
      mixed_model_handles.clear();
      mixed_masks.clear();
      gpu.list->ClearState(nullptr);
      addon::PreSrSwap recorded_render;
      Check(addon::ProcessInlinePreSR(gpu.list.Get(), owner, &parameters, recorded_render)
                && recorded_render.result != nullptr && mixed_model_handles.size() == 1
                && mixed_masks.size() == 1 && mixed_masks[0] != nullptr,
            "Render ControlMask records on the first command list");
      Check(addon::control_mask_reuse_stats.checks == gate_before.checks + 1
                && addon::control_mask_reuse_stats.admitted == gate_before.admitted + 1,
            "Render enters the per-object reuse admission gate");
      const auto calls_before_rejection = model_calls;
      const auto admitted_before_rejection = addon::control_mask_reuse_stats.admitted;
      addon::PreSrSwap rejected_render;
      Check(!addon::ProcessInlinePreSR(alternate_list.Get(), owner, &parameters, rejected_render)
                && rejected_render.result == nullptr && model_calls == calls_before_rejection
                && addon::control_mask_reuse_stats.checks == gate_before.checks + 2
                && addon::control_mask_reuse_stats.admitted == admitted_before_rejection
                && renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
                    renodx::addons::dlss5::NrDeclineReason::kControlMaskReuseNotReady)].load() == declines_before + 1,
            "a second command list cannot reuse an open Render Feature or workset");
      Check(SUCCEEDED(alternate_list->Close()), "close rejected alternate Render list");
      addon::RestorePreSrColor(&parameters, recorded_render);
      gpu.Submit();
      addon::DrainRetiredResources(true);
    }
    {
      auto& feature = addon::features.at(owner);
      mixed_model_handles.clear();
      mixed_masks.clear();
      source_evaluate_color = nullptr;
      const uint32_t model_before = model_calls;
      const uint32_t dispatch_before = mask_dispatches;
      fail_source_evaluate = true;
      gpu.list->ClearState(nullptr);
      const auto result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
      fail_source_evaluate = false;
      Check(NVSDK_NGX_FAILED(result), "injected host SR failure is returned to the caller");
      gpu.Submit();
      addon::DrainRetiredResources(true);
      Check(model_calls == model_before + 1 && mask_dispatches == dispatch_before + 1
                && mixed_model_handles.size() == 1
                && mixed_model_handles[0] == feature.render_session->slots[0].handle
                && mixed_masks.size() == 1 && mixed_masks[0] != nullptr
                && !feature.render_session->slots[0].control_mask_bound
                && !feature.slots[0].control_mask_bound,
            "failed host SR runs only its Render predecessor and leaves both mask bindings clear");
    }
    {
      auto& feature = addon::features.at(owner);
      const uint32_t previous_render_handle = *reinterpret_cast<const uint32_t*>(feature.render_session->slots[0].handle);
      const uint32_t previous_upscaled_handle = *reinterpret_cast<const uint32_t*>(feature.slots[0].handle);
      const uint64_t previous_render_workset = addon::worksets.at({owner, source.Get(), true}).id;
      const uint64_t previous_upscaled_workset = addon::worksets.at({owner, output.Get(), false}).id;
      const uint32_t model_before_discard = model_calls;
      const uint32_t dispatch_before_discard = mask_dispatches;
      gpu.list->ClearState(nullptr);
      const auto result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
      Check(NVSDK_NGX_SUCCEED(result) && model_calls == model_before_discard + 2
                && mask_dispatches == dispatch_before_discard + 2,
            "mature mixed chain records both stage histories before discard");
      Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
                && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
            "discard unsubmitted mixed Render recording");
      submission::OnCommandListReset(gpu.list.Get());
      submission::PruneCompletedGenerations();

      const size_t evaluations_before_recovery = model_evaluated_handles.size();
      bool discarded_chain_recovered = false;
      for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
        mixed_model_handles.clear();
        mixed_masks.clear();
        const uint32_t model_before = model_calls;
        const uint32_t dispatch_before = mask_dispatches;
        gpu.list->ClearState(nullptr);
        const auto recovered = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
        Check(NVSDK_NGX_SUCCEED(recovered), "discard recovery preserves host SR success");
        gpu.Submit();
        addon::DrainRetiredResources(true);
        if (model_calls != model_before + 2 || mask_dispatches != dispatch_before + 2) continue;
        // Allocators may reuse freed COM/model addresses; compare their generation IDs.
        Check(*reinterpret_cast<const uint32_t*>(feature.render_session->slots[0].handle) != previous_render_handle
                  && *reinterpret_cast<const uint32_t*>(feature.slots[0].handle) != previous_upscaled_handle,
              "discarded Render and Upscaled Features are both recreated");
        Check(addon::worksets.at({owner, source.Get(), true}).id != previous_render_workset
                  && addon::worksets.at({owner, source.Get(), true}).control_mask != nullptr,
              "discarded Render mask workset is rebuilt");
        Check(addon::worksets.at({owner, output.Get(), false}).id != previous_upscaled_workset
                  && addon::worksets.at({owner, output.Get(), false}).control_mask != nullptr,
              "discarded Upscaled mask workset is rebuilt");
        const auto first_reset_for = [&](const NVSDK_NGX_Handle* target) {
          for (size_t evaluation = evaluations_before_recovery;
               evaluation < model_evaluated_handles.size(); ++evaluation) {
            if (model_evaluated_handles[evaluation] == target) return model_resets[evaluation];
          }
          return -1;
        };
        Check(first_reset_for(feature.render_session->slots[0].handle) == 1
                  && first_reset_for(feature.slots[0].handle) == 1,
              "each recreated stage Feature receives Reset on its first Evaluate");
        discarded_chain_recovered = true;
        break;
      }
      Check(discarded_chain_recovered,
            "discarded mixed chain recovers within bounded frame-ahead maturation");
    }
    source_evaluate_color = nullptr;
    model_parameter_observer = saved_parameter_observer;
    addon::hook_point.store(saved_hook_point);
    addon::stack_passes.store(saved_stack_passes);
    addon::pass_stage[0].store(saved_second_stage);
    addon::configuration_generation.fetch_add(1);
    // The following cases call ProcessInline directly, outside a host SR callback.
    // End the wrapper fixture's ordered History before starting those cases.
    addon::ForgetSourceHandleLocked(owner);
    addon::DrainRetiredResources(true);
    addon::RegisterCreatedFeatureLocked(owner, addon::kFeatureDlss, &parameters);
    mature();
  }
  addon::ngx_slot_real[0].evaluate = previous_evaluate;
  addon::ngx_slot_real[0].evaluate_c = previous_evaluate_c;
  auto& slot = addon::features.at(owner).slots[0];
  Check(!slot.control_mask_bound
            && addon::GetD3D12Resource(slot.parameters, "DLSSNR.ControlMask") == nullptr,
        "completed Evaluate leaves no stale mask in reusable NGX parameters");
  for (uint32_t mode : {addon::kControlMaskNeutral, addon::kControlMaskRect,
                       addon::kControlMaskGreenRect, addon::kControlMaskBlueRect,
                       addon::kControlMaskDepthFar}) {
    addon::control_mask_mode = mode;
    const auto before = mask_dispatches;
    Check(record() == 1 && mask_dispatches == before + 1 && !slot.control_mask_bound,
          "mode switch has one mask dispatch, one Evaluate and no stale binding");
    Check(addon::worksets.at({owner, output.Get(), false}).control_mask_state
                  == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          "mode switch restores mask UAV state after Evaluate");
  }
  addon::control_mask_mode = addon::kControlMaskDepthFar;
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 9u);
  const auto depth_misfit_dispatches = mask_dispatches;
  const auto depth_misfit_models = model_calls;
  saw_mask = true;
  mature();
  Check(model_calls == depth_misfit_models + 1 && mask_dispatches == depth_misfit_dispatches + 1
            && saw_mask && !slot.control_mask_bound,
        "invalid current depth subrect skips only the depth rule and clears its Evaluate binding");
  parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
  const auto depth_recovery_dispatches = mask_dispatches;
  mature();
  Check(mask_dispatches == depth_recovery_dispatches + 1 && saw_mask,
        "valid current depth subrect resumes the depth rule without stale state");
  if (safety) {
    namespace shot = renodx::addons::dlss5::screenshot;
    const auto previous_source = shot::source_mode.exchange(2);
    const bool previous_planes = shot::stage_planes_enabled.exchange(true);
    Check(shot::RequestCapture() && record() == 1, "evaluate a masked model diagnostic frame");
    {
      const auto& workset = addon::worksets.at({owner, output.Get(), false});
      std::scoped_lock lock(shot::internal::state_mutex);
      Check(shot::internal::pending.has_value(), "masked model diagnostic is pending");
      auto& capture = *shot::internal::pending;
      Check(capture.planes.size() == 2 && capture.planes.front().name == "control_mask"
                && capture.planes.front().recorded
                && capture.planes.front().source_identity == reinterpret_cast<uintptr_t>(workset.control_mask)
                && capture.planes.back().name == "control_mask_depth"
                && capture.planes.back().recorded
                && capture.planes.back().source_identity == reinterpret_cast<uintptr_t>(depth.Get())
                && workset.control_mask_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            "model diagnostic copies the evaluated ControlMask and depth, restoring resource states");
      shot::internal::ReleaseReadbacks(capture);
      shot::internal::pending.reset();
      shot::internal::has_pending.store(false);
      shot::internal::stage_busy.store(false);
    }
    shot::source_mode.store(previous_source);
    shot::stage_planes_enabled.store(previous_planes);
  }
  const auto run_stack_case = [&](uint32_t pass_count, uint32_t resolution_mode,
                                  float resolution_scale, uint32_t expected_second_width, bool motion_case = false) {
    const uint32_t saved_stack_passes = addon::stack_passes.exchange(pass_count);
    const bool saved_follow_input = addon::nr_follow_input_res.exchange(false);
    const float saved_scale = addon::nr_resolution_scale.exchange(1.f);
    const uint32_t saved_pass_mode = addon::pass_resolution_mode[0].exchange(resolution_mode);
    const float saved_pass_scale = addon::pass_resolution_scale[0].exchange(resolution_scale);
    const bool saved_motion = addon::control_mask_motion_enabled.exchange(motion_case);
    auto& second_mask = addon::pass_control_mask[0];
    const bool saved_mask_follow = second_mask.follow.load();
    const addon::ControlMaskParameters saved_second{
        second_mask.mode.load(), second_mask.blend_mode.load(),
        {second_mask.red.load(), second_mask.green.load(), second_mask.blue.load()},
        second_mask.depth_near.load(), second_mask.depth_far.load(),
        second_mask.motion_enabled.load(), second_mask.motion_start.load(), second_mask.motion_end.load()};
    if (motion_case) {
      second_mask.follow = false;
      second_mask.mode = addon::kControlMaskRgb;
      second_mask.blend_mode = 3;
      second_mask.red = 0.5f;
      second_mask.green = 0.25f;
      second_mask.blue = 0.75f;
      second_mask.motion_enabled = false;
    }
    addon::configuration_generation.fetch_add(1);
    const auto saved_parameter_observer = model_parameter_observer;
    std::vector<ID3D12Resource*> stack_masks;
    std::vector<d3d12_test::TextureTransfer> stack_readbacks;
    model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block) {
      auto* mask = addon::GetD3D12Resource(block, "DLSSNR.ControlMask");
      stack_masks.push_back(mask);
      if (motion_case) {
        const size_t pass = (stack_masks.size() - 1) % pass_count;
        Check(mask != nullptr && addon::GetUInt(block, "DLSSNR.MaskBlendMode") == (pass == 1 ? 3u : addon::control_mask_blend_mode.load()),
            "each motion-mask pass consumes its own Blend mode");
        stack_readbacks.push_back(gpu.CreateTextureTransfer(mask));
        gpu.Transition(mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(mask, stack_readbacks.back());
        gpu.Transition(mask, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      }
    };
    bool stack_ready = false;
    for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
      const auto dispatch_before = mask_dispatches;
      const auto masks_before = stack_masks.size();
      stack_readbacks.clear();
      if (record() == pass_count) {
        Check(mask_dispatches == dispatch_before + pass_count
                  && stack_masks.size() == masks_before + pass_count,
              "each Feature Slot generates and binds its own depth mask");
        ID3D12Resource* const first_mask = stack_masks[masks_before];
        for (uint32_t pass = 0; pass < pass_count; ++pass) {
          ID3D12Resource* const pass_mask = stack_masks[masks_before + pass];
          Check(pass_mask != nullptr, "each evaluated Feature Slot receives a ControlMask resource");
          const auto mask_desc = pass_mask->GetDesc();
          const uint32_t expected_width = (pass == 1 ? expected_second_width : 8u);
          Check(mask_desc.Width == expected_width && mask_desc.Height == expected_width,
                "each Feature Slot receives a mask at its configured working resolution");
          if (motion_case) {
            const auto bytes = gpu.ReadTexture(stack_readbacks[pass]);
            const std::array<int, 4> expected = (pass == 1 ? std::array<int, 4>{128, 64, 191, 255}
                                                                         : std::array<int, 4>{0, 255, 255, 255});
            for (size_t component = 0; component < bytes.size(); ++component) {
              if (std::abs(static_cast<int>(bytes[component]) - expected[component % 4]) > 1) {
                std::cerr << "STACK_MOTION mismatch pass=" << pass << " component=" << component
                          << " actual=" << static_cast<int>(bytes[component])
                          << " expected=" << expected[component % 4] << '\n';
              }
              Check(std::abs(static_cast<int>(bytes[component]) - expected[component % 4]) <= 1,
                  "same-size passes consume distinct motion/manual RGB masks before the shared workset is overwritten");
            }
          }
          if (pass == 0 || expected_second_width == 8) {
            Check(pass_mask == first_mask,
                  "same-size Feature Slots share their mask workset");
          } else if (pass == 1) {
            Check(pass_mask != first_mask,
                  "different-size Feature Slots receive separate mask worksets");
          }
        }
        const auto& stack_feature = addon::features.at(owner);
        for (uint32_t pass = 0; pass < pass_count; ++pass) {
          Check(!stack_feature.slots[pass].control_mask_bound,
                "each stack Evaluate clears its transient mask binding");
        }
        stack_ready = true;
        break;
      }
    }
    Check(stack_ready, "ControlMask stack matures within bounded attempts");
    model_parameter_observer = saved_parameter_observer;
    addon::stack_passes.store(saved_stack_passes);
    addon::nr_follow_input_res.store(saved_follow_input);
    addon::nr_resolution_scale.store(saved_scale);
    addon::pass_resolution_mode[0].store(saved_pass_mode);
    addon::pass_resolution_scale[0].store(saved_pass_scale);
    addon::control_mask_motion_enabled = saved_motion;
    second_mask.follow = saved_mask_follow;
    second_mask.mode = saved_second.mode;
    second_mask.blend_mode = saved_second.blend_mode;
    second_mask.red = saved_second.rgb[0];
    second_mask.green = saved_second.rgb[1];
    second_mask.blue = saved_second.rgb[2];
    second_mask.motion_enabled = saved_second.motion_enabled;
    addon::configuration_generation.fetch_add(1);
    mature();
  };
  run_stack_case(2, 0, 1.f, 8);
  run_stack_case(2, 0, 1.f, 8, true);
  run_stack_case(2, 2, 0.5f, 4, true);
  run_stack_case(3, 0, 1.f, 8);
  run_stack_case(4, 0, 1.f, 8);
  {
    const uint32_t saved_stack_passes = addon::stack_passes.exchange(2);
    const uint32_t saved_pass_mode = addon::pass_resolution_mode[0].exchange(2);
    const float saved_pass_scale = addon::pass_resolution_scale[0].exchange(0.5f);
    addon::configuration_generation.fetch_add(1);
    bool stack_ready = false;
    for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
      if (record() == 2) {
        stack_ready = true;
        break;
      }
    }
    Check(stack_ready, "heterogeneous two-pass chain is ready for a mid-stack failure");
    std::vector<ID3D12Resource*> failed_stack_masks;
    const auto saved_parameter_observer = model_parameter_observer;
    model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter* block) {
      failed_stack_masks.push_back(addon::GetD3D12Resource(block, "DLSSNR.ControlMask"));
    };
    const auto before_failure = stage_execution_cases::Capture(
        gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.Submit();
    const uint32_t dispatches_before_failure = mask_dispatches;
    fail_call = model_calls + 2;
    Check(record() == 2, "second-pass NGX failure stops the remaining stack");
    fail_call = 0;
    const auto after_failure = stage_execution_cases::Capture(
        gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.Submit();
    Check(before_failure.Pixels() == after_failure.Pixels(),
          "mid-stack failure preserves the game's output");
    Check(mask_dispatches == dispatches_before_failure + 2 && failed_stack_masks.size() == 2
              && failed_stack_masks[0] != nullptr && failed_stack_masks[1] != nullptr
              && failed_stack_masks[0] != failed_stack_masks[1]
              && failed_stack_masks[0]->GetDesc().Width == 8
              && failed_stack_masks[1]->GetDesc().Width == 4,
          "different-size masks are bound for both attempted passes before failure");
    auto& failed_stack = addon::features.at(owner);
    Check(!failed_stack.slots[0].control_mask_bound
              && !failed_stack.slots[1].control_mask_bound
              && !failed_stack.slots[2].control_mask_bound
              && failed_stack.slots[1].failed && failed_stack.slots[2].pending_reset,
          "mid-stack failure clears bindings and resets only unexecuted later history");
    failed_stack.slots[1].failed_ns -= addon::kFeatureRetryMaxNs;
    bool recovered_stack = false;
    for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
      if (record() == 2) {
        recovered_stack = true;
        break;
      }
    }
    Check(recovered_stack, "two-pass ControlMask chain recovers after the failed pass is recreated");
    model_parameter_observer = saved_parameter_observer;
    addon::stack_passes.store(saved_stack_passes);
    addon::pass_resolution_mode[0].store(saved_pass_mode);
    addon::pass_resolution_scale[0].store(saved_pass_scale);
    addon::configuration_generation.fetch_add(1);
    mature();
  }
  const auto enabled_dispatches = mask_dispatches;
  addon::control_mask_enabled = false;
  mature();
  Check(!saw_mask && mask_dispatches == enabled_dispatches,
        "switching off clears binding and stops mask dispatches");
  const auto off_workset = addon::worksets.at({owner, output.Get(), false}).id;
  for (uint32_t frame = 0; frame < 3; ++frame) {
    Check(record() == 1 && addon::worksets.at({owner, output.Get(), false}).id == off_workset,
          "unmasked frames reuse their workset after switching off ControlMask");
  }
  addon::control_mask_enabled = true;
  addon::direct_runtime_requirements.file_version = "310.9.16.1";
  mature();
  Check(!saw_mask && mask_dispatches == enabled_dispatches,
        "nonexact runtime records no mask dispatch and passes no mask");
  addon::direct_runtime_requirements.file_version = "310.9.16.0";
  fail_mask_allocation = true;
  const auto before_failure = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  Check(record() == 0 && allocation_failures == 1, "mask allocation failure skips NR without retry");
  const auto after_failure = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  Check(before_failure.Pixels() == after_failure.Pixels() && slot.pending_reset && !slot.control_mask_bound,
        "allocation failure preserves game output, clears mask and arms history reset");
  fail_mask_allocation = false;
  mature();
  Check(model_resets.back() == 1, "allocation recovery resets History through production retry");
  std::cout << "CASE location=mask-allocation evidence=MOCK+GPU size=8x8 output=preserved binding=clear recovery=production-retry\n";
  for (const auto injected : {Fault::ROOT, Fault::PSO, Fault::HEAP}) {
    if (injected == Fault::ROOT) addon::ReleaseCodecPipeline();
    else if (injected == Fault::PSO) addon::ReleaseCom(addon::codec_pipeline.control_mask);
    else addon::RetireWorkset({owner, output.Get(), false});
    const auto before = injected_failures;
    fault = injected;
    Check(record() == 0 && injected_failures > before && slot.failed && !slot.control_mask_bound,
          "setup creation failure skips model and clears mask");
    fault = Fault::NONE;
    // Advance only the existing failure clock; never clear the production latch.
    slot.failed_ns -= addon::kFeatureRetryMaxNs;
    mature();
    Check(model_resets.back() == 1, "setup retry sends History reset");
    std::cout << "CASE location=create-" << static_cast<int>(injected)
              << " evidence=MOCK+GPU size=8x8 binding=clear recovery=expired-production-backoff\n";
  }
  fail_call = model_calls + 1;
  const auto before_evaluate_failure = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  const auto gate_before_failure = addon::control_mask_reuse_stats;
  Check(record() == 1, "NGX failure has exactly one Evaluate attempt");
  Check(addon::control_mask_reuse_stats.checks == gate_before_failure.checks + 1
            && addon::control_mask_reuse_stats.admitted == gate_before_failure.admitted + 1,
        "reuse gate admission is counted even when the subsequent NGX Evaluate fails");
  const auto after_evaluate_failure = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  Check(before_evaluate_failure.Pixels() == after_evaluate_failure.Pixels()
            && !addon::features.at(owner).slots[0].control_mask_bound,
        "NGX failure preserves game output and clears the old mask");
  fail_call = 0;
  mature();
  Check(model_resets.back() == 1, "NGX failure recovers with History reset");
  uint32_t exception_attempts = 0;
  model_parameter_observer = [&](const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*) {
    ++exception_attempts;
    throw std::runtime_error("injected ControlMask NGX exception");
  };
  const auto before_exception = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  record();
  const auto after_exception = stage_execution_cases::Capture(gpu, output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  Check(exception_attempts == 1 && before_exception.Pixels() == after_exception.Pixels()
            && !slot.control_mask_bound, "NGX exception clears mask without retry or output change");
  model_parameter_observer = {};
  mature();
  Check(model_resets.back() == 1, "NGX exception recovers with History reset");
  int unknown_object = 0;
  const auto& completed_workset = addon::worksets.at({owner, output.Get(), false});
  Check(submission::CanReuseRecordingObjects(
            addon::CollectWorksetGpuObjects(completed_workset),
            submission::RecordingIdentity(gpu.list.Get()), completed_workset.recording),
        "pruned healthy completed recording retains positive object provenance");
  Check(!submission::CanReuseRecordingObjects({&unknown_object},
            submission::RecordingIdentity(gpu.list.Get()), completed_workset.recording),
        "untracked object cannot borrow another object's completed receipt");
  std::cout << "CASE location=NGX-return-and-exception evidence=MOCK+GPU size=8x8 output=preserved binding=clear recovery=production-retry\n";

  // A second list cannot reuse state whose first recording has no submission.
  ComPtr<ID3D12CommandAllocator> other_allocator;
  ComPtr<ID3D12GraphicsCommandList> other_list;
  Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
      IID_PPV_ARGS(&other_allocator))), "second allocator");
  Check(SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
      other_allocator.Get(), nullptr, IID_PPV_ARGS(&other_list))), "second list");
  const addon::WorksetKey key{owner, output.Get(), false};
  const auto assert_gate = [&](ID3D12GraphicsCommandList* list, bool admitted) {
    const auto before = addon::control_mask_reuse_stats;
    const auto declines = renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
        renodx::addons::dlss5::NrDeclineReason::kControlMaskReuseNotReady)].load();
    addon::ProcessInline(list, owner, &parameters);
    Check(addon::control_mask_reuse_stats.checks == before.checks + 1
              && addon::control_mask_reuse_stats.admitted == before.admitted + (admitted ? 1 : 0)
              && renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
                  renodx::addons::dlss5::NrDeclineReason::kControlMaskReuseNotReady)].load() == declines + (admitted ? 0 : 1),
          "actual reuse gate counts one check and exactly one admission or classified rejection");
  };
  const auto before_parked = model_calls;
  addon::ProcessInline(gpu.list.Get(), owner, &parameters);
  Check(model_calls == before_parked + 1, "first parked recording evaluates");
  addon::ProcessInline(gpu.list.Get(), owner, &parameters);
  Check(model_calls == before_parked + 2, "exclusive same recording permits ordered reuse");
  auto* parked_mask = addon::worksets.at(key).control_mask;
  Check(!addon::ProcessInline(other_list.Get(), owner, &parameters) && model_calls == before_parked + 2,
        "unsubmitted recording rejects second list reuse without another Evaluate");
  // Real Close/Reset discards work. Do not submit commands with stale before-states.
  Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
            && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "discard parked recording");
  submission::OnCommandListReset(gpu.list.Get());
  submission::PruneCompletedGenerations();
  const auto old_budget = addon::max_worksets.load();
  addon::max_worksets = 1;
  ++addon::present_generation;
  auto output_b = texture(DXGI_FORMAT_R8G8B8A8_UNORM, true);
  gpu.Transition(output_b.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  parameters.Set(NVSDK_NGX_Parameter_Output, output_b.Get());
  mature();
  Check(addon::worksets.at({owner, output_b.Get(), false}).control_mask != parked_mask
            && model_resets.back() == 1, "discarded A cannot re-key into B with stale state or History");
  parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
  // Evict A under another owner before A gets a chance to inspect its old key.
  addon::ProcessInline(gpu.list.Get(), owner, &parameters);
  Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
            && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "discard before owner eviction");
  submission::OnCommandListReset(gpu.list.Get());
  submission::PruneCompletedGenerations();
  const auto previous_handle = slot.handle;
  const auto* other_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04A7});
  Check(addon::EnsureWorkset(gpu.device.Get(), other_owner, output_b.Get(), 8, 8, 8, 8,
      DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true) != nullptr,
      "other owner evicts discarded owner A workset");
  addon::max_worksets = old_budget;
  mature();
  Check(slot.handle != previous_handle && model_resets.back() == 1,
        "owner History reset survives eviction of its discarded workset");
  std::cout << "CASE location=unsubmitted-and-reset-rekey evidence=MOCK+GPU size=8x8 resource=fresh binding=clear recovery=discard-and-recreate\n";

  // Hold one actual queue, then record the next frame without waiting for it.
  ComPtr<ID3D12Fence> gate;
  Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "GPU delay gate");
  const auto wait_real = [&] {
    Check(SUCCEEDED(real_signal(gpu.queue.Get(), gpu.fence.Get(), ++gpu.serial)), "test-owned completion signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr, "test-owned completion event");
    const auto hr = gpu.fence->SetEventOnCompletion(gpu.serial, event);
    const auto wait = SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
    CloseHandle(event);
    Check(wait == WAIT_OBJECT_0 && real_completed(gpu.fence.Get()) >= gpu.serial,
          "independent real fence finishes actual GPU work before cleanup or removal mock");
  };
  Check(SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "queue waits on test gate");
  const auto queued_before = model_calls;
  addon::ProcessInline(gpu.list.Get(), owner, &parameters);
  Check(SUCCEEDED(gpu.list->Close()), "queued frame Close");
  ID3D12CommandList* queued[]{gpu.list.Get()};
  gpu.queue->ExecuteCommandLists(1, queued);
  auto* queued_mask = addon::worksets.at(key).control_mask;
  auto* queued_history = slot.handle;
  try {
    Check(queued_mask != nullptr && queued_history != nullptr,
          "in-flight ControlMask frame owns mask and NR History");
    Check(!submission::HasUnprovenSubmissions(), "valid delayed Fence is not an unknown submission");
    const auto saved_list = gpu.list;
    gpu.list = other_list;
    const auto toggle_calls = model_calls;
    const auto toggle_dispatches = mask_dispatches;
    addon::control_mask_enabled = false;
    assert_gate(gpu.list.Get(), false);
    Check(model_calls == toggle_calls && mask_dispatches == toggle_dispatches
              && addon::worksets.at(key).control_mask == queued_mask
              && slot.handle == queued_history
              && !submission::ResourceReleasable(queued_mask)
              && !submission::ResourceReleasable(queued_history),
          "OFF during pending masked work retains its mask and NR History without evaluating");
    addon::control_mask_enabled = true;
    assert_gate(gpu.list.Get(), false);
    Check(model_calls == toggle_calls && mask_dispatches == toggle_dispatches
              && addon::worksets.at(key).control_mask == queued_mask
              && slot.handle == queued_history
              && !submission::ResourceReleasable(queued_mask)
              && !submission::ResourceReleasable(queued_history)
              && gate->GetCompletedValue() < 1,
          "ON again returns while the queue is pending and preserves the same mask and NR History");
    gpu.list = saved_list;
    Check(model_calls == queued_before + 1 && addon::worksets.at(key).control_mask == queued_mask,
          "healthy pending submission rejects reuse without being classified unknown");
    parameters.Set(NVSDK_NGX_Parameter_Output, output_b.Get());
    assert_gate(other_list.Get(), false);
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    Check(model_calls == queued_before + 1,
          "rotating output cannot bypass shared owner NR History completion gate");
    Check(SUCCEEDED(other_list->Close()), "next queued frame Close");
    ID3D12CommandList* next[]{other_list.Get()};
    gpu.queue->ExecuteCommandLists(1, next);
    Check(!submission::ResourceReleasable(queued_mask), "delayed work remains retained");
  } catch (...) { gate->Signal(1); wait_real(); throw; }
  Check(SUCCEEDED(gate->Signal(1)), "release healthy queue gate");
  wait_real();
  Check(SUCCEEDED(other_list->Reset(other_allocator.Get(), nullptr)), "open empty rejected list");
  const auto before_replayable = model_calls;
  assert_gate(other_list.Get(), false);
  Check(model_calls == before_replayable,
        "Fence completion without Reset still rejects replayable prior recording");
  Check(SUCCEEDED(other_list->Close()), "close replayability probe without addon work");
  Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "reset completed frame");
  Check(SUCCEEDED(other_allocator->Reset()) && SUCCEEDED(other_list->Reset(other_allocator.Get(), nullptr)), "reset completed second frame");
  submission::OnCommandListReset(gpu.list.Get());
  submission::OnCommandListReset(other_list.Get());
  addon::DrainRetiredResources(true);
  Check(submission::ResourceReleasable(queued_mask), "real delayed fence then Reset releases use");
  const auto gate_before_reset_recovery = addon::control_mask_reuse_stats;
  Check(record() == 1 && addon::worksets.at(key).control_mask == queued_mask,
        "healthy completion plus Reset permits next recording without rebuilding");
  Check(addon::control_mask_reuse_stats.checks == gate_before_reset_recovery.checks + 1
            && addon::control_mask_reuse_stats.admitted == gate_before_reset_recovery.admitted + 1,
        "Reset recovery contributes exactly one admitted reuse check");
  std::cout << "CASE location=healthy-queued evidence=GPU+MOCK size=8x8 pending=declined replayable=declined complete-closed=reused hot_toggle=ON-OFF-ON resource=mask+history-retained no_cpu_wait=1\n";

  // Exercise the actual detour's exception boundary, not manual null receipts.
  for (int stage : {0, 1, 2}) {
    const auto before = model_calls;
    addon::ProcessInline(gpu.list.Get(), owner, &parameters);
    Check(model_calls == before + 1, "fault case records actual mask and model");
    auto* mask = addon::worksets.at(key).control_mask;
    Check(SUCCEEDED(gpu.list->Close()), "fault recording Close");
    ComPtr<ID3D12Fence> delayed;
    Check(SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&delayed))), "fault GPU gate");
    Check(SUCCEEDED(gpu.queue->Wait(delayed.Get(), 1)), "delay actual fault submission");
    const auto failures_before = injected_failures;
    if (stage < 2) {
      reservation_fault_stage = stage;
      submission::reservation_fault = ReservationFault;
    } else {
      std::lock_guard lock(renodx::addons::dlss5::queue_completion_mutex);
      signal_fault_fence = renodx::addons::dlss5::queue_completions.at(gpu.queue.Get()).fence;
    }
    ID3D12CommandList* lists[]{gpu.list.Get()};
    gpu.queue->ExecuteCommandLists(1, lists);
    submission::reservation_fault = nullptr;
    try {
      Check(injected_failures == failures_before + 1, "production submit fault was reached");
      Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "Reset in-flight list without resetting allocator");
      submission::OnCommandListReset(gpu.list.Get());
      submission::PruneCompletedGenerations();
      Check(submission::HasUnprovenSubmissions(), "allocation or Signal fault is a device admission barrier");
      Check(!submission::ResourceReleasable(mask), "failed bookkeeping or Signal retains actual in-flight mask");
      const auto failed_calls = model_calls;
      const auto gate_before_unknown = addon::control_mask_reuse_stats;
      const auto unknown_before = renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
          renodx::addons::dlss5::NrDeclineReason::kControlMaskSubmissionsUnproven)].load();
      Check(!addon::ProcessInline(gpu.list.Get(), owner, &parameters) && model_calls == failed_calls,
            "unknown submit proof forbids reusing mask and model");
      Check(addon::control_mask_reuse_stats.checks == gate_before_unknown.checks
                && addon::control_mask_reuse_stats.admitted == gate_before_unknown.admitted
                && renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
                    renodx::addons::dlss5::NrDeclineReason::kControlMaskSubmissionsUnproven)].load() == unknown_before + 1,
            "early unknown decline counts once and is excluded from the later reuse denominator");
      addon::hook_point = addon::kHookRender;
      addon::PreSrSwap swap;
      Check(!addon::ProcessInlinePreSR(gpu.list.Get(), owner, &parameters, swap)
                && addon::control_mask_reuse_stats.checks == gate_before_unknown.checks
                && addon::control_mask_reuse_stats.admitted == gate_before_unknown.admitted
                && renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(
                    renodx::addons::dlss5::NrDeclineReason::kControlMaskSubmissionsUnproven)].load() == unknown_before + 2,
            "PreSR unknown rejection shares the classification and never enters the Upscaled reuse denominator");
      addon::hook_point = addon::kHookUpscaled;
      addon::teardown_pending = true;
      addon::teardown_other_device_waiting = true;
      addon::teardown_pending_since_ns = addon::SteadyNowNs() - 60'000'000'000LL;
      Check(!addon::ServicePendingTeardown() && addon::worksets.contains(key),
            "actual pending teardown cannot release aged unfinished work");
    } catch (...) { delayed->Signal(1); wait_real(); throw; }
    delayed->Signal(1);
    wait_real();
    // Test fence proves physical safety only: the production receipt stays unknown.
    Check(!submission::ResourceReleasable(mask) && !addon::ServicePendingTeardown(),
          "test-owned GPU completion does not repair production unknown proof");
    addon::teardown_pending = false;
    addon::teardown_other_device_waiting = false;
    const auto* replacement_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04A8});
    addon::RegisterCreatedFeatureLocked(replacement_owner, addon::kFeatureDlss, &parameters);
    const auto assert_bounded = [&](const NVSDK_NGX_Handle* candidate) {
      const auto creates = addon::worksets_created;
      const auto models = next_handle;
      const auto calls = model_calls;
      const auto dispatches = mask_dispatches;
      const auto active = addon::worksets.size();
      const auto retired = addon::retired_final_resources.size();
      const auto retired_models = addon::retired_nr_features.size();
      uint64_t retained_bytes = 0;
      for (const auto& entry : addon::retired_final_resources) retained_bytes += entry.resources.allocated_bytes;
      for (uint32_t attempt = 0; attempt != 16; ++attempt) {
        ++addon::present_generation;
        // Turning the option off cannot make the old unknown work disappear.
        addon::control_mask_enabled = (attempt % 2 == 0);
        parameters.Set(NVSDK_NGX_Parameter_Output, (attempt % 2 == 0 ? output.Get() : output_b.Get()));
        addon::ProcessInline(gpu.list.Get(), candidate, &parameters);
      }
      addon::control_mask_enabled = true;
      parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
      uint64_t final_bytes = 0;
      for (const auto& entry : addon::retired_final_resources) final_bytes += entry.resources.allocated_bytes;
      Check(addon::worksets_created == creates && next_handle == models && model_calls == calls
                && mask_dispatches == dispatches && addon::worksets.size() == active
                && addon::retired_final_resources.size() == retired
                && addon::retired_nr_features.size() == retired_models && final_bytes == retained_bytes,
            "unknown submission blocks device-wide Owner/output/option churn without new GPU resources");
    };
    assert_bounded(replacement_owner);
    addon::ForgetSourceHandleLocked(owner);
    addon::RegisterCreatedFeatureLocked(owner, addon::kFeatureDlss, &parameters);
    Check(!submission::ResourceReleasable(mask), "source release retains unknown mask");
    assert_bounded(owner);
    assert_bounded(replacement_owner);
    const auto retained_count = addon::retired_final_resources.size();
    Check(addon::EnsureWorkset(gpu.device.Get(), replacement_owner, output_b.Get(), 8, 8, 8, 8,
              DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true) == nullptr
              && addon::retired_final_resources.size() == retained_count,
          "direct workset allocation cannot bypass retained unknown submission");
    std::cout << "CASE location=unknown-owner-churn stage=" << stage
              << " evidence=MOCK+GPU size=8x8 attempts=48 new_resources=0 resource=retained"
                 " recovery=requires-production-proof-or-terminal-device-lifecycle\n";
    // Explicit isolated fixture reset after real GPU completion, never a product recovery.
    submission::ResetAll();
    addon::RetireWorkset(key);
    addon::ReleaseNrSlot(addon::features.at(owner), 0, true);
    addon::DrainRetiredResources(true);
    renodx::addons::dlss5::ReleaseQueueCompletions();
    Check(SUCCEEDED(gpu.list->Close()) && SUCCEEDED(gpu.allocator->Reset())
              && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "safe fixture reset");
    mature();
    std::cout << "CASE location=" << (stage < 2 ? "submission-reservation-allocation" : "post-execute-signal")
              << " stage=" << stage << " evidence=MOCK+GPU size=8x8 resource=retained proof=unknown recovery=isolated-fixture-reset-after-independent-fence\n";
  }
  // Keep a real completed recording replayable: neither teardown entry nor
  // a new runtime may discard it just because another device is waiting.
  namespace timers = renodx::addons::dlss5::gpu_timers;
  Check(timers::Begin(gpu.list.Get(), gpu.device.Get(), "postSR", 8, 8, 0, 1, false), "removal completion-only sample");
  addon::ProcessInline(gpu.list.Get(), owner, &parameters);
  timers::End(gpu.list.Get());
  Check(SUCCEEDED(gpu.list->Close()), "replayable removal recording Close");
  ID3D12CommandList* removal_lists[]{gpu.list.Get()};
  gpu.queue->ExecuteCommandLists(1, removal_lists);
  wait_real();
  addon::teardown_pending = true;
  addon::teardown_other_device_waiting = true;
  addon::teardown_pending_since_ns = addon::SteadyNowNs() - 60'000'000'000LL;
  Check(!addon::ServicePendingTeardown() && !addon::Shutdown(false)
            && addon::worksets.contains(key), "all teardown entries retain completed replayable list");
  // D3D12CreateDevice may return the existing device on the same adapter.
  // Only the explicit hardware case may use WARP as a distinct second adapter.
  // A software-only case must never create a hardware device for recovery.
  std::unique_ptr<Gpu> recovery_gpu;
  if (safety && hardware) recovery_gpu = std::make_unique<Gpu>(false);
  if (recovery_gpu != nullptr) {
  auto& recovered = *recovery_gpu;
  Check(recovered.device.Get() != gpu.device.Get(), "recovery uses a genuinely distinct device");
  addon::direct_api.init = InitializeRuntime;
  addon::direct_api.shutdown = ShutdownRuntime;
  Check(!addon::EnsureDirectRuntime(recovered.list.Get()) && runtime_initializations == 0,
        "runtime switch retains replayable old work despite new device");
  addon::teardown_pending = false;
  addon::teardown_other_device_waiting = false;
  std::cout << "CASE location=replayable-teardown-and-runtime-switch evidence=GPU+MOCK size=8x8 resource=retained recovery=close-recording\n";
  // Removal substitutes only API observations after independent real completion.
  {
    namespace leases = renodx::addons::dlss5;
    leases::GpuLease unknown;
    Check(leases::QueryGpuLease(unknown) == leases::GpuLeaseState::kPending, "unknown lease stays pending");
    leases::GpuLease removed;
    removed.coverage_complete = true;
    removed.waits.emplace_back(leases::FenceRef(gpu.fence.Get()), gpu.serial);
    auto* production_fence = leases::queue_completions.at(gpu.queue.Get()).fence;
    removed_fence = production_fence;
    removed_device = gpu.device.Get();
    removed.waits.clear();
    removed.waits.emplace_back(leases::FenceRef(production_fence), 1);
    Check(leases::QueryGpuLease(removed) == leases::GpuLeaseState::kDeviceRemoved
              && !leases::GpuLeaseCompleted(removed), "removed fence cannot prove successful execution");
    const auto calls = model_calls;
    Check(!addon::ProcessInline(gpu.list.Get(), owner, &parameters) && model_calls == calls,
          "removed device cannot evaluate again");
    addon::RetireWorkset(key);
    addon::ReleaseNrSlot(addon::features.at(owner), 0, true);
    addon::DrainRetiredResources(true);
    timers::Poll(true, false);
    Check(!timers::execution_stats[1].valid
              && timers::execution_stats[1].state == timers::SampleState::DEVICE_REMOVED,
          "production completion consumer rejects removed sample instead of successful readback");
    Check(addon::retired_final_resources.empty() && addon::retired_nr_features.empty()
              && addon::TeardownProof() == submission::RecordedWork::kComplete,
          "production removal retires dead-device resources without requiring old-list Reset");
    Check(addon::EnsureDirectRuntime(recovered.list.Get()) && addon::direct_device == recovered.device.Get()
               && runtime_initializations == 1 && runtime_shutdowns == 0,
          "production runtime switch initializes a distinct legal device generation");
    removed_fence = nullptr;
    removed_device = nullptr;
    auto* rebuilt = addon::EnsureWorkset(recovered.device.Get(), owner, output.Get(), 8, 8, 8, 8,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true);
    Check(rebuilt != nullptr && addon::EnsureControlMaskPipeline(recovered.device.Get()),
          "new device rebuilds actual mask resources and PSO");
    addon::TrackWorksetRecording(recovered.list.Get(), *rebuilt);
    ID3D12DescriptorHeap* heaps[]{rebuilt->descriptors};
    recovered.list->SetDescriptorHeaps(1, heaps);
    recovered.list->SetComputeRootSignature(addon::codec_pipeline.root_signature);
    recovered.list->SetPipelineState(addon::codec_pipeline.control_mask);
    recovered.list->SetComputeRootDescriptorTable(1, addon::DescriptorHeapSlot(
        rebuilt->descriptors->GetGPUDescriptorHandleForHeapStart(), addon::kDescriptorControlMaskUav, rebuilt->descriptor_size));
    recovered.list->SetComputeRootDescriptorTable(3, addon::DescriptorHeapSlot(
        rebuilt->descriptors->GetGPUDescriptorHandleForHeapStart(), addon::kDescriptorControlMaskDepthNullSrv,
        rebuilt->descriptor_size));
    recovered.list->SetComputeRootDescriptorTable(4, addon::DescriptorHeapSlot(
        rebuilt->descriptors->GetGPUDescriptorHandleForHeapStart(), addon::kDescriptorControlMaskMotionNullSrv,
        rebuilt->descriptor_size));
    recovered.list->SetComputeRootDescriptorTable(5, addon::DescriptorHeapSlot(
        rebuilt->descriptors->GetGPUDescriptorHandleForHeapStart(), addon::kDescriptorControlMaskDepthProofNullCbv,
        rebuilt->descriptor_size));
    const std::array<uint32_t, 28> constants{8, 8, addon::kControlMaskNeutral, 4, 4, 0, 0, 0,
        0, 0, 0, 0, 0, std::bit_cast<uint32_t>(0.7f), std::bit_cast<uint32_t>(0.9f), 0};
    recovered.list->SetComputeRoot32BitConstants(2, static_cast<UINT>(constants.size()), constants.data(), 0);
    recovered.list->Dispatch(1, 1, 1);
    recovered.Transition(rebuilt->control_mask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto transfer = recovered.CreateTextureTransfer(rebuilt->control_mask);
    recovered.RecordTextureReadback(rebuilt->control_mask, transfer);
    recovered.Submit();
    const auto pixels = recovered.ReadTexture(transfer);
    Check(std::all_of(pixels.begin(), pixels.end(), [](uint8_t value) { return value == 255; }),
          "new device generation dispatch completes with neutral mask bytes");
  }
  std::cout << "CASE location=device-removal evidence=MOCK+GPU size=8x8 actual_gpu=already-completed old_device_evaluates=0 old_device_reset=0 recovery=distinct-adapter-device-new-PSO-resource-dispatch runtime=MOCK\n";
  } else {
    // The ordinary WARP case remains usable on machines without NVIDIA.
    Check(SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)), "close replay lifetime in WARP-only case");
    submission::OnCommandListReset(gpu.list.Get());
    addon::teardown_pending = false;
    addon::teardown_other_device_waiting = false;
    std::cout << "CASE location=device-removal evidence=NOT_RUN recovery=requires-explicit-hardware-safety-case\n";
  }
  Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_dispatch), reinterpret_cast<PVOID>(ObserveDispatch)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_pipeline), reinterpret_cast<PVOID>(ObservePipeline)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_allocate), reinterpret_cast<PVOID>(Allocate)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_root), reinterpret_cast<PVOID>(CreateRoot)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_pso), reinterpret_cast<PVOID>(CreatePso)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_heap), reinterpret_cast<PVOID>(CreateHeap)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_signal), reinterpret_cast<PVOID>(Signal)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_completed), reinterpret_cast<PVOID>(Completed)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_removed), reinterpret_cast<PVOID>(RemovedReason)) == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR, "ControlMask observers detached");
  std::cout << "PASS ControlMask ProcessInline default-off dispatch, allocation/NGX failure and no-retry contracts\n";
  addon::control_mask_motion_enabled = saved_motion_enabled;
  addon::control_mask_motion_start = saved_motion_start;
  addon::control_mask_motion_end = saved_motion_end;
}

std::vector<uint8_t> ReadPixels(Gpu& gpu, ID3D12Resource* texture) {
  const auto desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  uint64_t bytes = 0;
  gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
  gpu.Transition(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  const D3D12_TEXTURE_COPY_LOCATION source{.pResource = texture,
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = readback.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  gpu.Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  gpu.Submit();
  uint8_t* mapped = nullptr;
  Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "ControlMask readback map");
  std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
  for (UINT y = 0; y < desc.Height; ++y) {
    std::memcpy(pixels.data() + static_cast<size_t>(y) * desc.Width * 4,
        mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
        static_cast<size_t>(desc.Width) * 4);
  }
  readback->Unmap(0, nullptr);
  return pixels;
}

void Dispatch(Gpu& gpu, ID3D12Resource* texture, ID3D12DescriptorHeap* heap,
              uint32_t mode, uint32_t rect_width, uint32_t rect_height,
              float red, float green, float blue, ID3D12Resource* depth = nullptr,
              uint32_t depth_base_x = 0, uint32_t depth_base_y = 0,
              uint32_t depth_width = 0, uint32_t depth_height = 0,
              bool depth_inverted = false, float depth_near = 0.7f, float depth_far = 0.9f) {
  ID3D12DescriptorHeap* heaps[]{heap};
  gpu.list->SetDescriptorHeaps(1, heaps);
  gpu.list->SetComputeRootSignature(addon::codec_pipeline.root_signature);
  gpu.list->SetPipelineState(addon::codec_pipeline.control_mask);
  auto uav = heap->GetGPUDescriptorHandleForHeapStart();
  uav.ptr += static_cast<UINT64>(addon::kDescriptorControlMaskUav)
      * gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  gpu.list->SetComputeRootDescriptorTable(1, uav);
  auto srv = heap->GetGPUDescriptorHandleForHeapStart();
  const uint32_t depth_descriptor = depth != nullptr
      ? addon::kDescriptorControlMaskDepthSrv
      : addon::kDescriptorControlMaskDepthNullSrv;
  srv.ptr += static_cast<UINT64>(depth_descriptor)
      * gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  gpu.list->SetComputeRootDescriptorTable(3, srv);
  gpu.list->SetComputeRootDescriptorTable(4, addon::DescriptorHeapSlot(
      heap->GetGPUDescriptorHandleForHeapStart(), addon::kDescriptorControlMaskMotionNullSrv,
      gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));
  gpu.list->SetComputeRootDescriptorTable(5, addon::DescriptorHeapSlot(
      heap->GetGPUDescriptorHandleForHeapStart(), addon::kDescriptorControlMaskDepthProofNullCbv,
      gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));
  const std::array<uint32_t, 28> constants{
      static_cast<uint32_t>(texture->GetDesc().Width), texture->GetDesc().Height,
      mode, rect_width, rect_height,
      std::bit_cast<uint32_t>(red), std::bit_cast<uint32_t>(green), std::bit_cast<uint32_t>(blue),
      depth_base_x, depth_base_y, depth_width, depth_height, depth_inverted ? 1u : 0u,
      std::bit_cast<uint32_t>(depth_near), std::bit_cast<uint32_t>(depth_far), 0};
  gpu.list->SetComputeRoot32BitConstants(2, static_cast<UINT>(constants.size()), constants.data(), 0);
  gpu.list->Dispatch((static_cast<UINT>(texture->GetDesc().Width) + 15) / 16,
                    (texture->GetDesc().Height + 15) / 16, 1);
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = texture;
  gpu.list->ResourceBarrier(1, &barrier);
}

void RunJitterBoundaryContract() {
  renodx::addons::dlss5::bridge::OwnedParameters parameters;
  addon::FeatureState feature{};
  auto& slot = feature.slots[0];
  addon::FinalResources resources{};
  slot.parameters = &parameters;
  struct JitterCase {
    const char* name;
    bool source_x;
    bool source_y;
    float x;
    float y;
  };
  bool contract_ok = true;
  bool ever_present = false;
  // Reuse one NR block without Reset, as consecutive evaluations do.
  for (const auto& test : std::array{
           JitterCase{"never-present", false, false, 0.f, 0.f},
           JitterCase{"never-partial-x", true, false, 0.25f, 0.f},
           JitterCase{"never-partial-y", false, true, 0.f, -0.5f},
           JitterCase{"present", true, true, 0.375f, -0.25f},
           JitterCase{"absent", false, false, 0.f, 0.f},
           JitterCase{"absent-again", false, false, 0.f, 0.f},
           JitterCase{"partial-source-x", true, false, -0.125f, 0.f},
           JitterCase{"partial-source-x-again", true, false, 0.75f, 0.f},
           JitterCase{"partial-source-y", false, true, 0.f, 0.625f},
           JitterCase{"explicit-zero", true, true, 0.f, 0.f},
           JitterCase{"absent-after-zero", false, false, 0.f, 0.f},
           JitterCase{"present-again", true, true, -0.625f, 0.125f},
           JitterCase{"present-to-partial-x", true, false, 0.25f, 0.f},
           JitterCase{"present-before-partial-y", true, true, 0.375f, -0.5f},
           JitterCase{"present-to-partial-y", false, true, 0.f, -0.25f}}) {
    renodx::addons::dlss5::bridge::OwnedParameters source;
    if (test.source_x) {
      source.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, test.x);
    }
    if (test.source_y) {
      source.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, test.y);
    }
    const auto current = addon::DeriveFeatureState(&source);
    addon::EvaluationContract frame{};
    frame.has_jitter = current.has_jitter;
    frame.jitter_x = current.jitter_x;
    frame.jitter_y = current.jitter_y;
    addon::SetEvaluationParameters(feature, slot, 0, 0, resources, frame, 8, 8, 8, 8, false, 0);
    float x = 0.f;
    float y = 0.f;
    const bool has_x = NVSDK_NGX_SUCCEED(parameters.Get("DLSSNR.JitterOffsetX", &x));
    const bool has_y = NVSDK_NGX_SUCCEED(parameters.Get("DLSSNR.JitterOffsetY", &y));
    bool stage_ok = current.has_jitter == (test.source_x && test.source_y)
        && slot.jitter_bound == current.has_jitter;
    if (current.has_jitter) {
      stage_ok &= has_x && has_y && x == test.x && y == test.y;
      ever_present = true;
    } else if (!ever_present) {
      stage_ok &= !has_x && !has_y;
    } else {
      stage_ok &= (!has_x || x == 0.f) && (!has_y || y == 0.f);
    }
    std::cout << "JITTER evidence=MOCK stage=" << test.name << " has_jitter=" << current.has_jitter
              << " has_x=" << has_x << " x=" << x << " has_y=" << has_y << " y=" << y
              << " contract=" << (stage_ok ? "PASS" : "FAIL") << '\n';
    contract_ok &= stage_ok;
  }
  // Mask binding and jitter belong to each parameter slot independently.
  renodx::addons::dlss5::bridge::OwnedParameters other_parameters;
  auto& other_slot = feature.slots[1];
  other_slot.parameters = &other_parameters;
  addon::EvaluationContract frame{};
  addon::SetEvaluationParameters(feature, other_slot, 1, 0, resources, frame, 8, 8, 8, 8, true, 0);
  float x = 0.f;
  float y = 0.f;
  const bool other_never_present = !NVSDK_NGX_SUCCEED(other_parameters.Get("DLSSNR.JitterOffsetX", &x))
      && !NVSDK_NGX_SUCCEED(other_parameters.Get("DLSSNR.JitterOffsetY", &y));
  frame.has_jitter = true;
  frame.jitter_x = 0.5f;
  frame.jitter_y = -0.75f;
  addon::SetEvaluationParameters(feature, other_slot, 1, 0, resources, frame, 8, 8, 8, 8, true, 0);
  frame.has_jitter = false;
  addon::SetEvaluationParameters(feature, slot, 0, 0, resources, frame, 8, 8, 8, 8, false, 0);
  const bool isolated = other_never_present && other_slot.control_mask_bound && !slot.control_mask_bound
      && other_slot.jitter_bound && !slot.jitter_bound
      && NVSDK_NGX_SUCCEED(other_parameters.Get("DLSSNR.JitterOffsetX", &x)) && x == 0.5f
      && NVSDK_NGX_SUCCEED(other_parameters.Get("DLSSNR.JitterOffsetY", &y)) && y == -0.75f;
  std::cout << "JITTER evidence=MOCK stage=mask-slot-isolation contract=" << (isolated ? "PASS" : "FAIL") << '\n';
  contract_ok &= isolated;
  // The ordinary release destroys its owned block. The empty release must
  // also discard binding state before a fresh block is attached to that slot.
  const auto saved_destroy = addon::core_destroy_parameters;
  addon::core_destroy_parameters = renodx::addons::dlss5::bridge::DestroyOwnedParameters;
  for (const bool empty_release : {false, true}) {
    auto& released_slot = feature.slots[2];
    Check(NVSDK_NGX_SUCCEED(renodx::addons::dlss5::bridge::AllocateOwnedParameters(&released_slot.parameters)),
          "allocate jitter release fixture");
    frame.has_jitter = true;
    addon::SetEvaluationParameters(feature, released_slot, 2, 0, resources, frame, 8, 8, 8, 8, false, 0);
    if (empty_release) {
      renodx::addons::dlss5::bridge::DestroyOwnedParameters(released_slot.parameters);
      released_slot.parameters = nullptr;
    }
    addon::ReleaseNrSlot(feature, 2, false);
    const bool released_clean = !released_slot.jitter_bound && released_slot.parameters == nullptr;
    Check(NVSDK_NGX_SUCCEED(renodx::addons::dlss5::bridge::AllocateOwnedParameters(&released_slot.parameters)),
          "allocate fresh jitter release fixture");
    frame.has_jitter = false;
    addon::SetEvaluationParameters(feature, released_slot, 2, 0, resources, frame, 8, 8, 8, 8, false, 0);
    const bool fresh_unset = released_clean && !released_slot.jitter_bound
        && !NVSDK_NGX_SUCCEED(released_slot.parameters->Get("DLSSNR.JitterOffsetX", &x))
        && !NVSDK_NGX_SUCCEED(released_slot.parameters->Get("DLSSNR.JitterOffsetY", &y));
    std::cout << "JITTER evidence=MOCK stage=" << (empty_release ? "empty-release" : "normal-release")
              << " contract=" << (fresh_unset ? "PASS" : "FAIL") << '\n';
    contract_ok &= fresh_unset;
    addon::ReleaseNrSlot(feature, 2, false);
  }
  addon::core_destroy_parameters = saved_destroy;
  other_slot.parameters = nullptr;
  Check(contract_ok, "reused NR parameter block must not retain nonzero jitter after source becomes absent or partial");
}

int RunSafety(bool hardware, bool safety = true) {
  try {
    RunJitterBoundaryContract();
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Gpu gpu(hardware);
    RunBorrowedCore(gpu, hardware, safety);
    ComPtr<ID3D12InfoQueue> info;
    gpu.device.As(&info);
    std::cout << "ControlMask WARP debug_layer=" << (debug != nullptr) << '\n';
    Check(addon::InstallQueueCompletionHooks(gpu.queue.Get()), "ControlMask queue completion hooks");
    Check(addon::InstallCommandListStateHooks(gpu.list.Get()), "ControlMask command-list state hooks");
    Check(addon::EnsureControlMaskPipeline(gpu.device.Get()), "production ControlMask shader pipeline creates on WARP");
    const D3D12_HEAP_PROPERTIES properties{.Type = D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC texture_desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .SampleDesc = {1, 0},
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ComPtr<ID3D12Resource> texture;
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
        &texture_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&texture))), "ControlMask RGBA8 UAV allocation");
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = addon::kDescriptorHeapSize,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> heap;
    Check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap))), "ControlMask workset descriptor heap");
    D3D12_SHADER_RESOURCE_VIEW_DESC null_depth_srv{.Format = DXGI_FORMAT_R32_FLOAT,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    null_depth_srv.Texture2D.MipLevels = 1;
    gpu.device->CreateShaderResourceView(nullptr, &null_depth_srv,
        addon::DescriptorHeapSlot(heap->GetCPUDescriptorHandleForHeapStart(),
            addon::kDescriptorControlMaskDepthNullSrv,
            gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));
    null_depth_srv.Format = DXGI_FORMAT_R16G16_FLOAT;
    gpu.device->CreateShaderResourceView(nullptr, &null_depth_srv,
        addon::DescriptorHeapSlot(heap->GetCPUDescriptorHandleForHeapStart(),
            addon::kDescriptorControlMaskMotionNullSrv,
            gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));
    gpu.device->CreateConstantBufferView(nullptr,
        addon::DescriptorHeapSlot(heap->GetCPUDescriptorHandleForHeapStart(),
            addon::kDescriptorControlMaskDepthProofNullCbv,
            gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)));
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D};
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(addon::kDescriptorControlMaskUav)
        * gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, cpu);

    Dispatch(gpu, texture.Get(), heap.Get(), addon::kControlMaskRgb, 0, 0, 0.25f, 0.5f, 0.75f);
    auto pixels = ReadPixels(gpu, texture.Get());
    auto pixel = [&](uint32_t x, uint32_t y) { return pixels.data() + (y * 8 + x) * 4; };
    Check(pixel(7, 7)[0] == 64 && (pixel(7, 7)[1] == 128 || pixel(7, 7)[1] == 127)
              && pixel(7, 7)[2] == 191 && pixel(7, 7)[3] == 255,
          "fixed RGB mask writes expected RGBA8 texels");
    Dispatch(gpu, texture.Get(), heap.Get(), addon::kControlMaskNeutral, 0, 0, 0.f, 0.f, 0.f);
    pixels = ReadPixels(gpu, texture.Get());
    Check(pixel(0, 0)[0] == 255 && pixel(0, 0)[1] == 255
              && pixel(0, 0)[2] == 255 && pixel(0, 0)[3] == 255,
          "neutral mask is full one in all channels");
    Dispatch(gpu, texture.Get(), heap.Get(), addon::kControlMaskRect, 4, 4, 1.f, 0.5f, 0.f);
    pixels = ReadPixels(gpu, texture.Get());
    Check(pixel(1, 1)[0] == 0 && pixel(1, 1)[1] == 0
              && pixel(1, 1)[2] == 0 && pixel(1, 1)[3] == 255,
          "local test rectangle is zero RGB with opaque alpha");
    Check(pixel(6, 6)[0] == 255 && (pixel(6, 6)[1] == 128 || pixel(6, 6)[1] == 127)
              && pixel(6, 6)[2] == 0 && pixel(6, 6)[3] == 255,
          "pixels outside test rectangle retain fixed RGB");

    for (const auto& size : {std::array<uint32_t, 2>{8, 8}, {19, 17}, {1, 3}, {3, 1}}) {
      auto probe_desc = texture_desc;
      probe_desc.Width = size[0];
      probe_desc.Height = size[1];
      ComPtr<ID3D12Resource> probe;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
          &probe_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
          IID_PPV_ARGS(&probe))), "ControlMask boundary probe allocation");
      gpu.device->CreateUnorderedAccessView(probe.Get(), nullptr, &view, cpu);
      for (uint32_t mode : {addon::kControlMaskRgb, addon::kControlMaskNeutral, addon::kControlMaskRect,
                            addon::kControlMaskGreenRect, addon::kControlMaskBlueRect}) {
        for (const auto& rgb : {std::array<float, 3>{0.f, 0.f, 0.f}, {0.25f, 0.5f, 0.75f}}) {
          // New probes derive their half-size region from the texture, ignoring legacy rectangle constants.
          Dispatch(gpu, probe.Get(), heap.Get(), mode, 1, 1, rgb[0], rgb[1], rgb[2]);
          const auto probe_pixels = ReadPixels(gpu, probe.Get());
          const std::array<uint8_t, 4> fixed_rgb = (rgb[0] == 0.f
              ? std::array<uint8_t, 4>{0, 0, 0, 255}
              : std::array<uint8_t, 4>{64, 128, 191, 255});
          for (uint32_t y = 0; y < size[1]; ++y) {
            for (uint32_t x = 0; x < size[0]; ++x) {
              auto expected = fixed_rgb;
              if (mode == addon::kControlMaskNeutral) {
                expected = {255, 255, 255, 255};
              } else if (mode == addon::kControlMaskRect && x == 0 && y == 0) {
                expected = {0, 0, 0, 255};
              } else if (mode == addon::kControlMaskGreenRect || mode == addon::kControlMaskBlueRect) {
                expected = {255, 255, 255, 255};
                if (x < size[0] / 2 && y < size[1] / 2) {
                  expected[mode == addon::kControlMaskGreenRect ? 1 : 2] = 0;
                }
              }
              const size_t offset = (static_cast<size_t>(y) * size[0] + x) * 4;
              bool matches = true;
              for (size_t channel = 0; channel < 4; ++channel) {
                const auto actual = probe_pixels[offset + channel];
                matches &= actual == expected[channel] || (expected[channel] == 128 && actual == 127);
              }
              Check(matches,
                    "all mask texels match channel, origin, half-open boundary, RGB independence and alpha contracts");
            }
          }
        }
      }
    }
    gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, cpu);

    auto depth_input = gpu.Texture(7, 3, DXGI_FORMAT_R32_FLOAT);
    std::array<float, 21> depth_values{};
    depth_values[7 + 1] = 0.1f;
    depth_values[7 + 2] = 0.4f;
    depth_values[7 + 3] = 0.6f;
    depth_values[7 + 4] = 0.9f;
    depth_values[7 + 5] = 1.25f;
    auto depth_transfer = gpu.CreateTextureTransfer(depth_input.Get());
    gpu.Transition(depth_input.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.UploadTexture(depth_input.Get(), depth_transfer,
        reinterpret_cast<const uint8_t*>(depth_values.data()), sizeof(depth_values), 7 * sizeof(float));
    gpu.Transition(depth_input.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.Submit();

    auto depth_mask_desc = texture_desc;
    depth_mask_desc.Width = 5;
    depth_mask_desc.Height = 1;
    ComPtr<ID3D12Resource> depth_mask;
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
        &depth_mask_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&depth_mask))), "ControlMask depth-far output texture allocation");
    gpu.device->CreateUnorderedAccessView(depth_mask.Get(), nullptr, &view, cpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC depth_srv{.Format = DXGI_FORMAT_R32_FLOAT,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    depth_srv.Texture2D.MipLevels = 1;
    auto depth_cpu = heap->GetCPUDescriptorHandleForHeapStart();
    depth_cpu.ptr += static_cast<SIZE_T>(addon::kDescriptorControlMaskDepthSrv)
        * gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    gpu.device->CreateShaderResourceView(depth_input.Get(), &depth_srv, depth_cpu);
    Dispatch(gpu, depth_mask.Get(), heap.Get(), addon::kControlMaskDepthFar, 0, 0, 1.f, 0.5f, 0.25f,
            depth_input.Get(), 1, 1, 5, 1, false, 0.25f, 0.75f);
    const auto depth_pixels = ReadPixels(gpu, depth_mask.Get());
    const std::array<uint8_t, 5> expected_depth_red{255, 200, 55, 0, 255};
    for (uint32_t x = 0; x < expected_depth_red.size(); ++x) {
      const size_t offset = static_cast<size_t>(x) * 4;
      Check(std::abs(static_cast<int>(depth_pixels[offset]) - expected_depth_red[x]) <= 1
                && (depth_pixels[offset + 1] == 127 || depth_pixels[offset + 1] == 128)
                && depth_pixels[offset + 2] == 64 && depth_pixels[offset + 3] == 255,
            "depth-far mask uses the current nonzero depth subrect, soft range, R-only modulation and invalid-value pass-through");
    }
    Dispatch(gpu, depth_mask.Get(), heap.Get(), addon::kControlMaskDepthFar, 0, 0, 1.f, 0.5f, 0.25f,
            depth_input.Get(), 1, 1, 5, 1, true, 0.25f, 0.75f);
    const auto inverted_depth_pixels = ReadPixels(gpu, depth_mask.Get());
    const std::array<uint8_t, 5> expected_inverted_red{0, 55, 200, 255, 255};
    for (uint32_t x = 0; x < expected_inverted_red.size(); ++x) {
      Check(std::abs(static_cast<int>(inverted_depth_pixels[static_cast<size_t>(x) * 4])
                     - expected_inverted_red[x]) <= 1,
            "depth-far mask follows DepthInverted while leaving invalid values unclassified");
    }
    gpu.device->CreateUnorderedAccessView(texture.Get(), nullptr, &view, cpu);

    {
      const auto* motion_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04AF});
      const addon::WorksetKey motion_key{motion_owner, texture.Get(), false};
      auto* workset = addon::EnsureWorkset(gpu.device.Get(), motion_owner, texture.Get(), 5, 1, 5, 1,
          DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true);
      Check(workset != nullptr && workset->control_mask != nullptr, "motion mask fixture owns a production workset");
      auto vectors = gpu.Texture(14, 4, DXGI_FORMAT_R32G32_FLOAT);
      const auto vector_desc = vectors->GetDesc();
      const auto depth_desc = depth_input->GetDesc();
      const float saved_multiplier_x = addon::motion_scale_x_multiplier.exchange(1.f);
      const float saved_multiplier_y = addon::motion_scale_y_multiplier.exchange(1.f);
      float migrated_start = 2.f;
      float migrated_end = 16.f;
      Check(addon::MigrateControlMaskMotionDefaults(&migrated_start, &migrated_end)
                && migrated_start == 16.f && migrated_end == 160.f,
            "exact old motion defaults migrate together");
      for (const auto custom : {std::pair{2.f, 174.56f}, std::pair{4.f, 16.f}}) {
        migrated_start = custom.first;
        migrated_end = custom.second;
        Check(!addon::MigrateControlMaskMotionDefaults(&migrated_start, &migrated_end)
                  && migrated_start == custom.first && migrated_end == custom.second,
              "custom motion ranges remain intact during migration");
      }
      struct MotionCase {
        const char* name;
        bool display_grid;
        float scale_x;
        bool depth;
        bool motion;
        bool missing_motion;
        bool missing_depth;
        std::array<int, 5> red;
        bool vertical = false;
        bool invalid_window = false;
        bool neutral = false;
      };
      const MotionCase cases[]{
          {"low-res", false, 1.f, false, true, false, false, {204, 0, 102, 204, 204}},
          {"negative-scale", false, -1.f, false, true, false, false, {204, 0, 102, 204, 204}},
          {"display-res-subrect", true, 1.f, false, true, false, false, {204, 0, 102, 204, 204}},
          {"vertical-display", true, 1.f, false, true, false, false, {204, 0, 102, 204, 204}, true},
          {"depth-overlap", true, -1.f, true, true, false, false, {204, 0, 44, 0, 204}},
          {"motion-missing", false, 1.f, true, true, true, false, {204, 160, 44, 0, 204}},
          {"depth-missing", false, 1.f, true, true, false, true, {204, 0, 102, 204, 204}},
          {"motion-off", false, 1.f, false, false, false, false, {204, 204, 204, 204, 204}},
          {"invalid-scale", false, std::numeric_limits<float>::infinity(), false, true, false, false,
              {204, 204, 204, 204, 204}},
          {"invalid-window", false, 1.f, false, true, false, false, {204, 204, 204, 204, 204}, false, true},
          {"neutral-hidden-rgb", false, 1.f, false, true, false, false,
              {255, 0, 128, 255, 255}, false, false, true}};
      bool first_upload = true;
      for (const auto& test : cases) {
        addon::EvaluationContract frame{};
        frame.input_width = 5;
        frame.input_height = 1;
        frame.motion = (test.missing_motion ? nullptr : vectors.Get());
        frame.motion_x = 3;
        frame.motion_y = 1;
        frame.depth_x = 1;
        frame.depth_y = 1;
        frame.depth = (test.missing_depth ? nullptr : depth_input.Get());
        frame.create_flags = (test.display_grid ? 0u : static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
        const auto window = addon::ResolveMotionWindow(frame.create_flags, frame.motion_x, frame.motion_y,
            5, 1, 10, 2, vector_desc);
        frame.motion_width = window.width;
        frame.motion_height = window.height;
        frame.motion_scale_x = test.scale_x;
        frame.motion_scale_y = -1.f;
        const std::array<float, 5> speed{0.f, 16.f, 9.f, 2.f, std::numeric_limits<float>::quiet_NaN()};
        std::array<float, 14 * 4 * 2> vector_data{};
        for (uint32_t y = 0; y < frame.motion_height; ++y) {
          for (uint32_t x = 0; x < frame.motion_width; ++x) {
            const size_t index = (static_cast<size_t>(y + frame.motion_y) * 14 + x + frame.motion_x) * 2;
            const float value = speed[x * 5 / frame.motion_width];
            if (test.vertical) {
              vector_data[index + 1] = value * static_cast<float>(frame.motion_height);
            } else {
              vector_data[index] = value * static_cast<float>(frame.motion_width) / 5.f;
            }
          }
        }
        auto transfer = gpu.CreateTextureTransfer(vectors.Get());
        gpu.Transition(vectors.Get(), first_upload ? D3D12_RESOURCE_STATE_COMMON
                                                  : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.UploadTexture(vectors.Get(), transfer, reinterpret_cast<const uint8_t*>(vector_data.data()),
            sizeof(vector_data), 14 * 2 * sizeof(float));
        gpu.Transition(vectors.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        first_upload = false;
        const addon::ControlMaskParameters settings{
            test.neutral ? addon::kControlMaskNeutral : (test.depth ? addon::kControlMaskDepthFar : addon::kControlMaskRgb),
            2, {0.8f, 0.5f, 0.25f}, 0.25f, 0.75f, test.motion, 2.f, 16.f};
        if (test.invalid_window) {
          frame.motion_x = 13;
        }
        Check(addon::DispatchControlMask(gpu.device.Get(), gpu.list.Get(), *workset, frame.depth,
                  depth_desc, frame, 5, 1, settings),
              "production mask generator accepts independent motion/depth rule inputs");
        gpu.Transition(workset->control_mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        workset->control_mask_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const auto bytes = ReadPixels(gpu, workset->control_mask);
        const int expected_green = (test.neutral ? 255 : 128);
        const int expected_blue = (test.neutral ? 255 : 64);
        for (size_t x = 0; x < test.red.size(); ++x) {
          if (std::abs(static_cast<int>(bytes[x * 4]) - test.red[x]) > 1
              || std::abs(static_cast<int>(bytes[x * 4 + 1]) - expected_green) > 1
              || bytes[x * 4 + 2] != expected_blue || bytes[x * 4 + 3] != 255) {
            std::cerr << "MOTION_MASK mismatch case=" << test.name << " x=" << x
                      << " actual=" << static_cast<int>(bytes[x * 4]) << ',' << static_cast<int>(bytes[x * 4 + 1])
                      << ',' << static_cast<int>(bytes[x * 4 + 2]) << ',' << static_cast<int>(bytes[x * 4 + 3])
                      << " expected_red=" << test.red[x] << '\n';
          }
          Check(std::abs(static_cast<int>(bytes[x * 4]) - test.red[x]) <= 1
                    && std::abs(static_cast<int>(bytes[x * 4 + 1]) - expected_green) <= 1
                    && bytes[x * 4 + 2] == expected_blue && bytes[x * 4 + 3] == 255,
                test.name);
        }
        std::cout << "MOTION_MASK case=" << test.name << " size=5x1 motion_base=3,1 grid="
                  << frame.motion_width << 'x' << frame.motion_height << " rgba8=PASS\n";
      }
      // Keep the same guide vectors and manual upper bound while only the
      // automatic NR working size changes. Both axes retain reference pixels.
      for (const bool vertical : {false, true}) {
        addon::EvaluationContract frame{};
        frame.input_width = frame.motion_width = 10;
        frame.input_height = frame.motion_height = 2;
        frame.motion = vectors.Get();
        frame.motion_x = 3;
        frame.motion_y = 1;
        frame.motion_scale_x = 1.f;
        frame.motion_scale_y = -1.f;
        std::array<float, 14 * 4 * 2> vector_data{};
        constexpr std::array<float, 5> speeds{0.f, 16.f, 9.f, 2.f, 0.f};
        for (uint32_t y = 0; y < 2; ++y) {
          for (uint32_t x = 0; x < 10; ++x) {
            const size_t index = (static_cast<size_t>(y + 1) * 14 + x + 3) * 2;
            vector_data[index + (vertical ? 1 : 0)] = speeds[x / 2];
          }
        }
        auto transfer = gpu.CreateTextureTransfer(vectors.Get());
        gpu.Transition(vectors.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.UploadTexture(vectors.Get(), transfer, reinterpret_cast<const uint8_t*>(vector_data.data()),
            sizeof(vector_data), 14 * 2 * sizeof(float));
        gpu.Transition(vectors.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        constexpr std::array<int, 5> expected_red{204, 0, 102, 204, 204};
        for (const uint32_t current_width : {10u, 5u}) {
          const uint32_t current_height = current_width / 5;
          workset = addon::EnsureWorkset(gpu.device.Get(), motion_owner, texture.Get(),
              current_width, current_height, 10, 2, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false,
              nullptr, nullptr, true);
          Check(workset != nullptr && workset->control_mask != nullptr, "automatic motion reference mask allocation");
          const addon::ControlMaskParameters settings{addon::kControlMaskRgb, 2,
              {0.8f, 0.5f, 0.25f}, 0.25f, 0.75f, true, 2.f, 16.f};
          Check(addon::DispatchControlMask(gpu.device.Get(), gpu.list.Get(), *workset, nullptr,
                    depth_desc, frame, current_width, current_height, settings, false, true, 10, 2),
                "automatic mask uses the fixed manual upper-bound motion reference");
          gpu.Transition(workset->control_mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          workset->control_mask_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
          const auto bytes = ReadPixels(gpu, workset->control_mask);
          for (uint32_t y = 0; y < current_height; ++y) {
            for (uint32_t x = 0; x < current_width; ++x) {
              const size_t pixel = (static_cast<size_t>(y) * current_width + x) * 4;
              Check(std::abs(static_cast<int>(bytes[pixel]) - expected_red[x * 5 / current_width]) <= 1
                        && std::abs(static_cast<int>(bytes[pixel + 1]) - 128) <= 1
                        && bytes[pixel + 2] == 64 && bytes[pixel + 3] == 255,
                    "automatic resize preserves reference-pixel attenuation and G/B/alpha");
            }
          }
          std::cout << "MOTION_MASK auto_reference=10x2 current=" << current_width << 'x' << current_height
                    << " axis=" << (vertical ? "y" : "x") << " rgba8=PASS adapter="
                    << (hardware ? "NATIVE_GPU" : "WARP") << " nr=MOCK\n";
        }
      }
      // A compressed guide grid retains the scene's normalized MV contract:
      // scale/window cancel, leaving 3840x2160 reference pixels per frame.
      addon::EvaluationContract frame{};
      frame.input_width = frame.motion_width = 10;
      frame.input_height = frame.motion_height = 2;
      frame.motion = vectors.Get();
      frame.motion_x = 3;
      frame.motion_y = 1;
      frame.motion_scale_x = 10.f;
      frame.motion_scale_y = 2.f;
      workset = addon::EnsureWorkset(gpu.device.Get(), motion_owner, texture.Get(),
          10, 2, 10, 2, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true);
      Check(workset != nullptr, "default motion sequence owns its mask dimensions");
      const addon::ControlMaskParameters settings{
          .mode = addon::kControlMaskRgb, .blend_mode = 0, .rgb = {1.f, 0.5f, 0.25f},
          .depth_near = 0.f, .depth_far = 1.f, .motion_enabled = true};
      constexpr std::array<float, 13> sequence{0.f, 8.f, 16.f, 32.f, 64.f, 96.f, 160.f,
          96.f, 64.f, 32.f, 16.f, 8.f, 0.f};
      constexpr std::array<int, 13> expected_red{255, 255, 255, 246, 189, 106, 0,
          106, 189, 246, 255, 255, 255};
      for (const bool vertical : {false, true}) {
        for (size_t step = 0; step < sequence.size(); ++step) {
          std::array<float, 14 * 4 * 2> vector_data{};
          for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t x = 0; x < 10; ++x) {
              const size_t index = (static_cast<size_t>(y + 1) * 14 + x + 3) * 2;
              vector_data[index + (vertical ? 1 : 0)] = sequence[step] / (vertical ? 2160.f : 3840.f);
            }
          }
          auto transfer = gpu.CreateTextureTransfer(vectors.Get());
          gpu.Transition(vectors.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
          gpu.UploadTexture(vectors.Get(), transfer, reinterpret_cast<const uint8_t*>(vector_data.data()),
              sizeof(vector_data), 14 * 2 * sizeof(float));
          gpu.Transition(vectors.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          Check(addon::DispatchControlMask(gpu.device.Get(), gpu.list.Get(), *workset, nullptr,
                    depth_desc, frame, 10, 2, settings, false, true, 3840, 2160),
                "default motion sequence dispatch uses normalized vectors in 4K reference pixels");
          gpu.Transition(workset->control_mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          workset->control_mask_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
          const auto bytes = ReadPixels(gpu, workset->control_mask);
          for (size_t pixel = 0; pixel < bytes.size(); pixel += 4) {
            Check(std::abs(static_cast<int>(bytes[pixel]) - expected_red[step]) <= 1
                      && std::abs(static_cast<int>(bytes[pixel + 1]) - 128) <= 1
                      && bytes[pixel + 2] == 64 && bytes[pixel + 3] == 255,
                  "default thresholds preserve gentle motion, fade continuously and restore stopped input");
          }
          std::cout << "MOTION_MASK default_sequence axis=" << (vertical ? "y" : "x")
                    << " reference=3840x2160 speed=" << sequence[step]
                    << " red=" << static_cast<int>(bytes[0]) << " rgba8=PASS\n";
        }
      }
      addon::motion_scale_x_multiplier = saved_multiplier_x;
      addon::motion_scale_y_multiplier = saved_multiplier_y;
      addon::RetireWorkset(motion_key);
      addon::DrainRetiredResources(true);
    }

    const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04A5});
    const auto* disabled_owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04A4});
    const addon::WorksetKey workset_key{owner, texture.Get(), false};
    const addon::WorksetKey disabled_key{disabled_owner, texture.Get(), false};
    auto* disabled = addon::EnsureWorkset(gpu.device.Get(), disabled_owner, texture.Get(), 8, 8, 8, 8,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false);
    Check(disabled != nullptr && disabled->control_mask == nullptr
              && !addon::control_mask_enabled.load(),
          "default-off workset allocates no ControlMask resource");
    addon::RetireWorkset(disabled_key);
    addon::DrainRetiredResources(true);
    auto* first = addon::EnsureWorkset(gpu.device.Get(), owner, texture.Get(), 8, 8, 8, 8,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true);
    Check(first != nullptr && first->control_mask != nullptr
              && first->allocated_bytes >= 8u * 8u * 4u,
          "enabled workset owns and accounts for its RGBA8 mask");
    auto* first_mask = first->control_mask;
    addon::TrackWorksetRecording(gpu.list.Get(), *first);
    auto* resized = addon::EnsureWorkset(gpu.device.Get(), owner, texture.Get(), 4, 4, 8, 8,
        DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1, 0, false, nullptr, nullptr, true);
    Check(resized != nullptr && resized->control_mask != nullptr
              && resized->control_mask != first_mask,
          "workset size change replaces the mask resource");
    addon::DrainRetiredResources(true);
    Check(std::any_of(addon::retired_final_resources.begin(), addon::retired_final_resources.end(),
              [&](const auto& retired) { return retired.resources.control_mask == first_mask; }),
          "recorded but unsubmitted old mask cannot retire early");
    gpu.Submit();
    addon::DrainRetiredResources(true);
    Check(std::none_of(addon::retired_final_resources.begin(), addon::retired_final_resources.end(),
              [&](const auto& retired) { return retired.resources.control_mask == first_mask; }),
          "old mask retires only after its recorded submission completes");
    addon::RetireWorkset(workset_key);
    addon::DrainRetiredResources(true);

    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    addon::FeatureState feature{};
    addon::NrFeatureSlot slot{};
    addon::FinalResources resources{};
    addon::EvaluationContract frame{};
    resources.control_mask = texture.Get();
    const bool old_auto_mask = addon::use_auto_mask.exchange(true);
    slot.parameters = &parameters;
    for (uint32_t blend = 0; blend < 4; ++blend) {
      addon::SetEvaluationParameters(feature, slot, 0, 0, resources, frame,
                                     8, 8, 8, 8, true, blend);
      Check(addon::GetD3D12Resource(&parameters, "DLSSNR.ControlMask") == texture.Get()
                && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectBaseX") == 0
                && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectBaseY") == 0
                && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectWidth") == 8
                && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectHeight") == 8
                && addon::GetUInt(&parameters, "DLSSNR.MaskBlendMode") == blend
                && addon::GetInt(&parameters, "DLSSNR.UseAutoMask") == 1,
            "NGX block binds mask/subrect/blend mode and preserves AutoMask");
    }
    addon::SetEvaluationParameters(feature, slot, 0, 0, resources, frame, 8, 8, 8, 8, false, 0);
    Check(addon::GetD3D12Resource(&parameters, "DLSSNR.ControlMask") == nullptr
              && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectBaseX") == 0
              && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectBaseY") == 0
              && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectWidth") == 0
              && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectHeight") == 0
              && !slot.control_mask_bound,
          "previous mask binding is cleared when the feature is disabled");
    addon::use_auto_mask.store(old_auto_mask);
    feature.slots[1].control_mask_bound = true;
    addon::ReleaseNrSlot(feature, 1, false);
    Check(!feature.slots[1].control_mask_bound,
          "released parameter slot forgets its prior mask binding");
    RunInline(gpu, hardware, safety);
    if (info != nullptr) {
      for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0;
        info->GetMessage(i, nullptr, &bytes);
        std::vector<uint8_t> buffer(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        Check(SUCCEEDED(info->GetMessage(i, message, &bytes)), "ControlMask debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
          std::cerr << message->pDescription << '\n';
          Check(false, "ControlMask D3D12 validation error");
        }
      }
    }
    std::cout << "PASS ControlMask safety adapter=" << (hardware ? "NATIVE_GPU" : "WARP")
              << " model=MOCK; NVIDIA runtime not exercised\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL ControlMask: " << error.what() << std::endl;
    return 1;
  }
}

#if defined(NR_NEXUS_TEST_NATIVE_NGX)
inline addon::DirectEvaluateFn native_runtime_evaluate = nullptr;
inline std::vector<ID3D12Resource*> native_runtime_masks;
inline std::vector<NVSDK_NGX_Result> native_runtime_results;
inline std::vector<int> native_runtime_resets;
inline bool native_cost_observe = false;
inline Gpu* native_motion_gpu = nullptr;
inline const d3d12_test::TextureTransfer* native_motion_sr_readback = nullptr;
inline const d3d12_test::TextureTransfer* native_motion_proxy_readback = nullptr;
inline const d3d12_test::TextureTransfer* native_motion_neural_readback = nullptr;
inline uint32_t native_motion_sr_calls = 0;
struct NativeRenderSrInput {
  ID3D12Resource* original = nullptr;
  const d3d12_test::TextureTransfer* readback = nullptr;
  bool bound_stand_in = false;
};
inline NativeRenderSrInput* native_render_sr_input = nullptr;
// Only the history experiment overrides NR guides; SR always consumes its
// original per-frame parameters and owns a separate SDK history.
inline ID3D12Resource* native_history_motion = nullptr;
inline int native_history_reset = -1;
inline d3d12_test::TimestampQueries* native_history_queries = nullptr;
inline UINT native_history_interval = 0;
inline nlohmann::json native_history_parameters;
inline bool native_cache_observe = false;
inline bool native_cache_cost = false;
inline bool native_cache_two_tiers = false;
inline const NVSDK_NGX_Handle* native_cache_two_tier_owner = nullptr;
inline std::chrono::steady_clock::time_point native_cache_nr_cpu_begin;
inline bool native_cache_pq = false;
inline addon::DirectCreateFn native_resize_create = nullptr;
inline addon::DirectReleaseFn native_resize_release = nullptr;
inline double native_resize_create_ms = 0;
inline double native_resize_release_ms = 0;
inline double native_resize_host_ms = 0;
inline double native_resize_evaluate_ms = 0;
inline uint32_t native_resize_creates = 0;
inline uint32_t native_resize_releases = 0;
inline bool native_resize_ready = false;
inline bool native_resize_create_runtime_locked = false;
inline bool native_resize_create_ngx_call = false;
inline bool native_resize_demand_prepare = false;
inline bool native_resize_selected_sequence = false;
inline PsoFn native_limited_pso = nullptr;
inline DWORD native_limited_main_thread_id = 0;
inline const std::array<std::span<const uint8_t>, 2> native_limited_bytecodes{__v6_autoscale, __look_band};
inline const std::array<ID3D12PipelineState**, 2> native_limited_slots{
    &addon::codec_pipeline.autoscale, &addon::codec_pipeline.look.band};
inline constexpr std::array<const char*, 2> native_limited_target_names{"autoscale", "look_bands"};
struct NativeLimitedPsoCall {
  HANDLE worker = nullptr;
  DWORD worker_id = 0;
  int64_t observed_begin_ns = 0;
  int64_t begin_ns = 0, end_ns = 0;
  HRESULT result = E_PENDING;
  uintptr_t pipeline = 0;
  bool api_returned = false;
  bool api_threw = false;
  bool worker_exited = false;
};
struct NativeLimitedPsoCalls {
  std::shared_mutex mutex;
  std::vector<NativeLimitedPsoCall> calls;
};
inline std::array<NativeLimitedPsoCalls, 2> native_limited_targets;

HRESULT STDMETHODCALLTYPE ObserveNativeLimitedPso(ID3D12Device* device,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID iid, void** result) {
  size_t target = 0;
  for (; target < native_limited_bytecodes.size(); ++target) {
    const auto bytecode = native_limited_bytecodes[target];
    if (desc->CS.BytecodeLength == bytecode.size()
        && std::memcmp(desc->CS.pShaderBytecode, bytecode.data(), bytecode.size()) == 0) break;
  }
  if (target == native_limited_bytecodes.size()) return native_limited_pso(device, desc, iid, result);
  auto& observation = native_limited_targets[target];
  const DWORD worker_id = GetCurrentThreadId();
  const HANDLE worker = (worker_id != native_limited_main_thread_id
      ? OpenThread(SYNCHRONIZE, FALSE, worker_id) : nullptr);
  size_t call_index;
  try {
    std::unique_lock lock(observation.mutex);
    call_index = observation.calls.size();
    observation.calls.push_back({.worker = worker, .worker_id = worker_id,
        .observed_begin_ns = addon::SteadyNowNs()});
  } catch (...) {
    if (worker != nullptr) CloseHandle(worker);
    throw;
  }
  const int64_t begin_ns = addon::SteadyNowNs();
  HRESULT status;
  try {
    status = native_limited_pso(device, desc, iid, result);
  } catch (...) {
    const int64_t end_ns = addon::SteadyNowNs();
    std::unique_lock lock(observation.mutex);
    auto& call = observation.calls[call_index];
    call.begin_ns = begin_ns;
    call.end_ns = end_ns;
    call.api_threw = true;
    throw;
  }
  const int64_t end_ns = addon::SteadyNowNs();
  {
    std::unique_lock lock(observation.mutex);
    auto& call = observation.calls[call_index];
    call.begin_ns = begin_ns;
    call.end_ns = end_ns;
    call.result = status;
    call.pipeline = reinterpret_cast<uintptr_t>(SUCCEEDED(status) && result != nullptr ? *result : nullptr);
    call.api_returned = true;
  }
  return status;
}

struct NativeLimitedAutoscaleProbe {
  bool attached = false;
  bool exercised = false;
  bool asynchronous = true;
  std::array<std::shared_ptr<addon::AsyncAutoscalePreparation>, 2> tasks;
  std::array<uint32_t, 2> pending_source_returns{};
  std::array<bool, 2> accepted{};
  std::array<int64_t, 2> accepted_observed_ns{};
  std::array<uint32_t, 2> accepted_source_call{UINT32_MAX, UINT32_MAX};

  void Attach(ID3D12Device* device, bool use_async) {
    asynchronous = use_async;
    native_limited_main_thread_id = GetCurrentThreadId();
    native_limited_pso = reinterpret_cast<PsoFn>((*reinterpret_cast<void***>(device))[11]);
    Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
        && DetourAttach(reinterpret_cast<PVOID*>(&native_limited_pso), reinterpret_cast<PVOID>(ObserveNativeLimitedPso)) == NO_ERROR
        && DetourTransactionCommit() == NO_ERROR, "limited native private PSO observer attach");
    attached = true;
    exercised = true;
  }

  bool Finish() {
    if (!attached) return true;
    addon::CancelAsyncAutoscalePreparation();
    const auto deadline = GetTickCount64() + 10000;
    bool task_finished = false;
    do {
      {
        renodx::addons::dlss5::RuntimeLock lock(addon::runtime_mutex);
        task_finished = addon::DiscardAsyncAutoscalePreparation();
      }
      if (!task_finished) Sleep(1);
    } while (!task_finished && GetTickCount64() < deadline);
    if (!task_finished) return false;
    bool workers_exited = true;
    for (auto& observation : native_limited_targets) {
      std::unique_lock lock(observation.mutex);
      for (auto& call : observation.calls) {
        if (call.worker_id == native_limited_main_thread_id) continue;
        if (call.worker != nullptr) {
          call.worker_exited = call.worker_id != GetCurrentThreadId()
              && WaitForSingleObject(call.worker, 10000) == WAIT_OBJECT_0;
          if (call.worker_exited || call.worker_id == GetCurrentThreadId()) {
            CloseHandle(call.worker);
            call.worker = nullptr;
          }
        }
        workers_exited &= call.worker_exited;
      }
    }
    if (!workers_exited) return false;
    const bool detached = DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
        && DetourDetach(reinterpret_cast<PVOID*>(&native_limited_pso), reinterpret_cast<PVOID>(ObserveNativeLimitedPso)) == NO_ERROR
        && DetourTransactionCommit() == NO_ERROR;
    if (detached) attached = false;
    return detached;
  }

  nlohmann::json Snapshot() const {
    auto observed_tasks = tasks;
    {
      renodx::addons::dlss5::RuntimeLock lock(addon::runtime_mutex);
      const auto task = addon::async_autoscale_task;
      if (task != nullptr) {
        for (size_t target = 0; target < native_limited_slots.size(); ++target) {
          if (task->target_identity == native_limited_slots[target]) observed_tasks[target] = task;
        }
      }
    }
    nlohmann::json snapshot{{"schema", "nr-nexus-native-private-pso-v2"}, {"mode", asynchronous ? "async" : "sync"},
        {"scope", "separate fixed shader/slot targets; API intervals exclude observer locking; sync intervals are nested in main-source traces, async intervals run on workers; no nested sums; acceptance is observed after source return"}};
    for (size_t target = 0; target < native_limited_targets.size(); ++target) {
      auto& entry = snapshot["targets"][native_limited_target_names[target]];
      entry = {{"main_thread_id", native_limited_main_thread_id}, {"accepted", accepted[target]},
          {"admission_source", asynchronous ? "private task result to production slot" : "synchronous API result to production slot"},
          {"accepted_observed_ns", accepted_observed_ns[target]},
          {"accepted_source_call", accepted[target] ? nlohmann::json(accepted_source_call[target]) : nlohmann::json(nullptr)},
          {"pending_source_returns_before_worker_complete", pending_source_returns[target]},
          {"pending_overlap_observation", asynchronous
                  ? (pending_source_returns[target] != 0 ? "Observed" : "NotObserved; scheduler timing, no product failure; deterministic WARP coverage is separate")
                  : "NotApplicable: synchronous source call"},
          {"api_calls", nlohmann::json::array()}};
      {
        auto& observation = native_limited_targets[target];
        std::shared_lock lock(observation.mutex);
        entry["calls"] = observation.calls.size();
        for (const auto& call : observation.calls) {
          const bool api_finished = call.api_returned || call.api_threw;
          const bool source_thread = call.worker_id == native_limited_main_thread_id;
          entry["api_calls"].push_back({{"thread_id", call.worker_id},
              {"execution_thread", source_thread ? "main source" : "worker"},
              {"worker_thread_id", source_thread ? nlohmann::json(nullptr) : nlohmann::json(call.worker_id)},
              {"begin_ns", call.begin_ns}, {"end_ns", call.end_ns}, {"api_finished", api_finished},
              {"api_returned", call.api_returned}, {"api_threw", call.api_threw},
              {"api_cpu_ms", (api_finished && call.end_ns >= call.begin_ns
                      ? nlohmann::json((call.end_ns - call.begin_ns) / 1.e6) : nlohmann::json(nullptr))},
              {"inflight_observer_elapsed_ms", (!api_finished
                      ? nlohmann::json((addon::SteadyNowNs() - call.observed_begin_ns) / 1.e6) : nlohmann::json(nullptr))},
              {"result", call.api_returned ? nlohmann::json(call.result) : nlohmann::json(nullptr)},
              {"pipeline", call.pipeline},
              {"worker_exited", source_thread ? nlohmann::json(nullptr) : nlohmann::json(call.worker_exited)},
              {"worker_exit_status", source_thread ? "NotApplicable: synchronous source call" : (call.worker_exited ? "Exited" : "NotProven")}});
        }
      }
      const auto task = observed_tasks[target];
      if (task != nullptr) {
        std::shared_lock lock(task->mutex);
        entry["task"] = {{"started", task->started}, {"complete", task->complete}, {"result", task->result},
            {"operation", task->operation}, {"cache_epoch", task->cache_epoch},
            {"canceled_at_snapshot", task->cancellation->load() != task->cancellation_epoch}};
      } else {
        entry["task"] = nullptr;
      }
    }
    return snapshot;
  }

  ~NativeLimitedAutoscaleProbe() {
    if (!Finish()) std::cerr << "LIMITED_NATIVE async observer/worker cleanup failed\n";
    if (exercised) {
      addon::async_autoscale_preparation = false;
      addon::nr_adaptive_limited_residency = false;
    }
  }
};
inline bool native_resize_create_gpu = false;
inline std::vector<nlohmann::json> native_resize_create_calls;
inline std::vector<uintptr_t> native_resize_models;
inline std::vector<bool> native_resize_preparing;
inline std::vector<double> native_resize_evaluate_cpu_ms;
inline bool native_resize_fixed_input = false;
inline bool native_resize_observe_parameters = false;
inline uint32_t native_resize_frame = UINT32_MAX;
inline uint32_t native_resize_fixed_input_copies = 0;
inline double native_resize_fixed_input_cpu_ms = 0.;
inline std::vector<nlohmann::json> native_resize_parameter_snapshots;
struct NativeResizeFixedInput {
  d3d12_test::TextureTransfer transfer;
  uint32_t crc32 = 0;
};
inline std::map<std::pair<uint32_t, uint32_t>, NativeResizeFixedInput> native_resize_fixed_inputs;
inline const std::atomic_bool* native_cold_worker_sdk_active = nullptr;
inline uint64_t native_cold_serial_timeouts_before = 0;
inline bool native_cold_sdk_entry_refused = false;
inline int64_t native_cold_sr_enter_ns = 0;
inline bool native_cold_sr_runtime_locked = false;
inline uint32_t native_cold_sr_ngx_depth = 0;

NVSDK_NGX_Result NVSDK_CONV ObserveNativeResizeCreate(
    ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature,
    const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  native_resize_create_runtime_locked = renodx::addons::dlss5::runtime_lock::State::HeldByThisThread();
  native_resize_create_ngx_call = renodx::addons::dlss5::ngx_serial::depth != 0;
  const bool timed = native_resize_create_gpu && native_cost_queries != nullptr && list == native_cost_list;
  if (timed) {
    Check(native_resize_creates == 0, "consecutive demand records at most one actual Create per source frame");
    native_cost_queries->Begin(list, native_cost_interval + 3);
  }
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
  addon::ColdCpuTraceScope sdk_trace("SDK.CoreCreateFeature");
#endif
  const auto start = std::chrono::steady_clock::now();
  const auto result = native_resize_create(list, feature, parameters, handle);
  const auto end = std::chrono::steady_clock::now();
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
  sdk_trace.End();
#endif
  const double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
  if (timed) {
    list->EndQuery(native_cost_queries->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * (native_cost_interval + 3) + 1);
  }
  native_resize_create_ms += elapsed_ms;
  ++native_resize_creates;
  if (native_resize_demand_prepare || native_resize_selected_sequence) {
    native_resize_create_calls.push_back({{"feature", static_cast<uint32_t>(feature)},
        {"result", static_cast<uint32_t>(result)}, {"model", reinterpret_cast<uintptr_t>(handle != nullptr ? *handle : nullptr)},
        {"input", {addon::GetUInt(parameters, NVSDK_NGX_Parameter_Width), addon::GetUInt(parameters, NVSDK_NGX_Parameter_Height)}},
        {"output", {addon::GetUInt(parameters, NVSDK_NGX_Parameter_OutWidth), addon::GetUInt(parameters, NVSDK_NGX_Parameter_OutHeight)}},
        {"cpu_ms", elapsed_ms}, {"begin_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count()},
        {"end_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count()},
        {"runtime_mutex_held", native_resize_create_runtime_locked}, {"ngx_call_active", native_resize_create_ngx_call}});
    const auto* frame = (addon::preparing_adaptive_frame != nullptr ? addon::preparing_adaptive_frame : addon::active_adaptive_frame);
    if (frame != nullptr && frame->session != nullptr) {
      const auto& session = *frame->session;
      native_resize_create_calls.back()["session"] = {{"requested_slot", session.level}, {"source_active_slot", session.active_level},
          {"preparing_slot", frame->slot}, {"preparing", frame->preparing}, {"trial", session.trial},
          {"blocked", session.blocked}, {"defer_preparation", session.defer_preparation}};
    }
  }
  return result;
}

NVSDK_NGX_Result NVSDK_CONV ObserveNativeResizeRelease(NVSDK_NGX_Handle* handle) {
  const auto start = std::chrono::steady_clock::now();
  const auto result = native_resize_release(handle);
  native_resize_release_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  ++native_resize_releases;
  return result;
}

NVSDK_NGX_Result NVSDK_CONV ObserveNativeHostEvaluate(
    ID3D12GraphicsCommandList* command_list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback progress) {
  if (native_cold_worker_sdk_active != nullptr) {
    // Call's bounded timeout normally forwards without serialization. This
    // probe must refuse that fallback before entering the actual SDK.
    if (native_cold_worker_sdk_active->load(std::memory_order_acquire)
        || renodx::addons::dlss5::ngx_serial::serial_timeouts.load() != native_cold_serial_timeouts_before) {
      native_cold_sdk_entry_refused = true;
      return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    }
    native_cold_sr_enter_ns = addon::SteadyNowNs();
    native_cold_sr_runtime_locked = renodx::addons::dlss5::HoldingRuntimeLock();
    native_cold_sr_ngx_depth = renodx::addons::dlss5::ngx_serial::depth;
    if (native_cost_queries != nullptr) {
      native_cost_queries->Begin(command_list, native_cost_interval + 2);
    }
  }
  ++native_motion_sr_calls;
  if (native_render_sr_input != nullptr) {
    auto& input = *native_render_sr_input;
    auto* color = addon::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Color);
    const auto workset = addon::worksets.find({handle, input.original, true});
    if (color != nullptr && color != input.original && workset != addon::worksets.end()
        && color == workset->second.pre_sr_color) {
      input.bound_stand_in = true;
      native_motion_gpu->Retain(color);
      native_motion_gpu->Transition(color, addon::kPreSrReadable, D3D12_RESOURCE_STATE_COPY_SOURCE);
      native_motion_gpu->RecordTextureReadback(color, *input.readback);
      native_motion_gpu->Transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE, addon::kPreSrReadable);
    }
  }
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
  addon::ColdCpuTraceScope sdk_trace("SDK.SREvaluateFeature");
#endif
  if (native_history_queries != nullptr) {
    native_history_queries->Begin(command_list, native_history_interval);
  }
  const auto start = std::chrono::steady_clock::now();
  const auto result = NVSDK_NGX_D3D12_EvaluateFeature(command_list, handle, parameters, progress);
  if (native_history_queries != nullptr) {
    native_history_queries->EndAndResolve(command_list, native_history_interval);
  }
  native_resize_host_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
  sdk_trace.End();
#endif
  if (native_cold_worker_sdk_active != nullptr && native_cost_queries != nullptr) {
    command_list->EndQuery(native_cost_queries->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                          2 * (native_cost_interval + 2) + 1);
  }
  if (NVSDK_NGX_SUCCEED(result) && native_motion_sr_readback != nullptr) {
    auto* output = addon::GetD3D12Resource(parameters, NVSDK_NGX_Parameter_Output);
    native_motion_gpu->Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    native_motion_gpu->RecordTextureReadback(output, *native_motion_sr_readback);
    native_motion_gpu->Transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (NVSDK_NGX_SUCCEED(result) && native_cache_observe && !native_cache_pq && native_history_queries != nullptr) {
    // Exclude real SR and its diagnostic snapshot from the full NR chain.
    native_history_queries->Begin(command_list, native_history_interval + 2);
  }
  if (native_cache_two_tiers) {
    native_cache_nr_cpu_begin = std::chrono::steady_clock::now();
  }
  return result;
}

void NVSDK_CONV NativeNgxSdkLog(
    const char*, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {}

NVSDK_NGX_Result NVSDK_CONV ObserveNativeRuntimeEvaluate(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback progress) {
  if (native_history_motion != nullptr || (native_cache_observe && !native_cache_cost)) {
    const auto wrapper_reset = addon::GetInt(parameters, "DLSSNR.Reset", -1);
    auto* experiment_parameters = const_cast<NVSDK_NGX_Parameter*>(parameters);
    if (native_history_motion != nullptr) {
      experiment_parameters->Set("DLSSNR.MVec", native_history_motion);
    }
    if (native_history_reset >= 0) {
      experiment_parameters->Set("DLSSNR.Reset", native_history_reset);
    }
    native_history_parameters = {{"handle", reinterpret_cast<uintptr_t>(handle)},
        {"wrapper_reset", wrapper_reset},
        {"reset", addon::GetInt(parameters, "DLSSNR.Reset", -1)},
        {"motion", reinterpret_cast<uintptr_t>(addon::GetD3D12Resource(parameters, "DLSSNR.MVec"))},
        {"depth", reinterpret_cast<uintptr_t>(addon::GetD3D12Resource(parameters, "DLSSNR.Depth"))}};
    const auto motion_desc = addon::GetD3D12Resource(parameters, "DLSSNR.MVec")->GetDesc();
    native_history_parameters["motion_resource"] = {{"width", motion_desc.Width}, {"height", motion_desc.Height},
        {"format", static_cast<uint32_t>(motion_desc.Format)},
        {"format_name", motion_desc.Format == DXGI_FORMAT_R32G32_FLOAT ? "R32G32_FLOAT" : "R16G16_FLOAT"}};
    for (const auto* key : {"DLSSNR.MVecScaleX", "DLSSNR.MVecScaleY", "DLSSNR.JitterOffsetX", "DLSSNR.JitterOffsetY"}) {
      float value = 0.f;
      const auto result = parameters->Get(key, &value);
      native_history_parameters[key] = {{"present", NVSDK_NGX_SUCCEED(result)}, {"value", value}};
    }
    for (const auto* key : {"DLSSNR.InputWidth", "DLSSNR.InputHeight", "DLSSNR.OutputWidth", "DLSSNR.OutputHeight",
             "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY", "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight",
             "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY", "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight"}) {
      native_history_parameters[key] = addon::GetUInt(parameters, key);
    }
  }
  native_runtime_masks.push_back(addon::GetD3D12Resource(parameters, "DLSSNR.ControlMask"));
  if (native_cost_observe) {
    native_runtime_resets.push_back(addon::GetInt(parameters, "DLSSNR.Reset", -1));
  }
  if (native_motion_proxy_readback != nullptr) {
    auto* proxy = addon::GetD3D12Resource(parameters, "DLSSNR.Color");
    native_motion_gpu->Transition(proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    native_motion_gpu->RecordTextureReadback(proxy, *native_motion_proxy_readback);
    native_motion_gpu->Transition(proxy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  const bool timed = native_cost_queries != nullptr && command_list == native_cost_list;
  const bool timed_evaluate = timed && (native_runtime_masks.size() == 1
      || (native_resize_ready && native_runtime_masks.size() == 2));
  const UINT evaluate_interval = native_cost_interval + static_cast<UINT>(native_runtime_masks.size());
  if (timed_evaluate) {
    native_cost_queries->Begin(command_list, evaluate_interval);
  }
  if (native_resize_fixed_input) {
    const auto replacement_begin = std::chrono::steady_clock::now();
    Check(native_motion_gpu != nullptr && command_list == native_motion_gpu->list.Get(),
          "fixed-input probe records on the existing fixture list");
    auto* proxy = addon::GetD3D12Resource(parameters, "DLSSNR.Color");
    Check(proxy != nullptr, "fixed-input probe has the actual Core Color resource");
    const auto desc = proxy->GetDesc();
    const uint32_t width = addon::GetUInt(parameters, NVSDK_NGX_Parameter_Width);
    const uint32_t height = addon::GetUInt(parameters, NVSDK_NGX_Parameter_Height);
    Check(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
              && desc.MipLevels == 1 && desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1
              && width > 1 && height > 1 && desc.Width == width && desc.Height == height
              && addon::GetUInt(parameters, "DLSSNR.ColorSubrectBaseX") == 0
              && addon::GetUInt(parameters, "DLSSNR.ColorSubrectBaseY") == 0
              && addon::GetUInt(parameters, "DLSSNR.ColorSubrectWidth") == width
              && addon::GetUInt(parameters, "DLSSNR.ColorSubrectHeight") == height,
          "fixed-input probe requires coherent actual FP16 dimensions and a full zero-origin Color ROI");
    auto& input = native_resize_fixed_inputs[{width, height}];
    native_motion_gpu->Retain(proxy);
    native_motion_gpu->Transition(proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (input.transfer.upload == nullptr) {
      input.transfer = native_motion_gpu->CreateTextureTransfer(proxy);
      input.transfer.readback.Reset();
      std::vector<uint16_t> pixels(static_cast<size_t>(width) * height * 4);
      for (uint32_t y = 0; y < height; ++y) {
        const float v = static_cast<float>(y) / (height - 1);
        for (uint32_t x = 0; x < width; ++x) {
          const float u = static_cast<float>(x) / (width - 1);
          const size_t pixel = (static_cast<size_t>(y) * width + x) * 4;
          pixels[pixel] = DirectX::PackedVector::XMConvertFloatToHalf(0.12f + 0.65f * u);
          pixels[pixel + 1] = DirectX::PackedVector::XMConvertFloatToHalf(0.1f + 0.65f * v);
          pixels[pixel + 2] = DirectX::PackedVector::XMConvertFloatToHalf(0.15f + 0.5f * (1.f - u * v));
          pixels[pixel + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
        }
      }
      input.crc32 = renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(pixels.data()), pixels.size() * sizeof(uint16_t));
      native_motion_gpu->UploadTexture(proxy, input.transfer, reinterpret_cast<const uint8_t*>(pixels.data()),
          pixels.size() * sizeof(uint16_t), static_cast<uint64_t>(width) * 8);
    } else {
      native_motion_gpu->Retain(input.transfer.upload.Get());
      const D3D12_TEXTURE_COPY_LOCATION source{.pResource = input.transfer.upload.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = input.transfer.footprint};
      const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = proxy, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    submission::TrackUse(command_list, proxy);
    submission::TrackUse(command_list, input.transfer.upload.Get());
    native_motion_gpu->Transition(proxy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ++native_resize_fixed_input_copies;
    native_resize_fixed_input_cpu_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - replacement_begin).count();
  }
  const bool observe_parameters = native_resize_observe_parameters && native_resize_frame >= 285 && native_resize_frame <= 299;
  if (observe_parameters) {
    nlohmann::json snapshot{{"frame", native_resize_frame}, {"phase", native_resize_frame / 60},
        {"preparing", addon::preparing_adaptive_frame != nullptr}, {"fixed_input", native_resize_fixed_input},
        {"history_reset_epoch", addon::history_reset_epoch.load()},
        {"identities", {{"model", reinterpret_cast<uintptr_t>(handle)}}}};
    for (const auto* key : {NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
             NVSDK_NGX_Parameter_OutWidth, NVSDK_NGX_Parameter_OutHeight, "DLSSNR.Width", "DLSSNR.Height",
             "DLSSNR.InputWidth", "DLSSNR.InputHeight", "DLSSNR.OutputWidth", "DLSSNR.OutputHeight",
             "DLSSNR.Output.Width", "DLSSNR.Output.Height", "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
             "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight", "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
             "DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight", "DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
             "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight", "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
             "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight", "DLSSNR.ControlMaskSubrectWidth", "DLSSNR.ControlMaskSubrectHeight",
             "DLSSNR.Style", "DLSSNR.Hint.Render.Preset"}) {
      uint32_t value = 0;
      const bool available = addon::GetUIntIfPresent(parameters, key, &value);
      snapshot["uint_parameters"][key] = {{"available", available}, {"value", available ? nlohmann::json(value) : nlohmann::json(nullptr)}};
    }
    for (const auto* key : {"DLSSNR.Reset", "DLSSNR.Enabled", "DLSSNR.DepthInverted", "DLSSNR.UseAutoMask", "DLSSNR.UICorrection",
             NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, NVSDK_NGX_Parameter_Reset, NVSDK_NGX_Parameter_PerfQualityValue}) {
      int value = 0;
      const bool available = NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(const_cast<NVSDK_NGX_Parameter*>(parameters), key, &value));
      snapshot["int_parameters"][key] = {{"available", available}, {"value", available ? nlohmann::json(value) : nlohmann::json(nullptr)}};
    }
    for (const auto* key : {"DLSSNR.ScalingRatio", "DLSSNR.Scale", "DLSSNR.JitterOffsetX", "DLSSNR.JitterOffsetY",
             "DLSSNR.MVecScaleX", "DLSSNR.MVecScaleY", "DLSSNR.Intensity", "DLSSNR.LocalToneStrength",
             "DLSSNR.LocalStructureStrength", "DLSSNR.SkinStructureStrength",
             NVSDK_NGX_Parameter_Jitter_Offset_X, NVSDK_NGX_Parameter_Jitter_Offset_Y,
             NVSDK_NGX_Parameter_MV_Scale_X, NVSDK_NGX_Parameter_MV_Scale_Y, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,
             NVSDK_NGX_Parameter_DLSS_Pre_Exposure, NVSDK_NGX_Parameter_DLSS_Exposure_Scale}) {
      float value = 0.f;
      const bool available = addon::GetFloatIfPresent(parameters, key, value);
      snapshot["float_parameters"][key] = {{"available", available},
          {"value", available && std::isfinite(value) ? nlohmann::json(value) : nlohmann::json(nullptr)},
          {"finite", available && std::isfinite(value)}};
    }
    for (const auto* key : {"DLSSNR.Color", "DLSSNR.Output", "DLSSNR.MVec", "DLSSNR.Depth", "DLSSNR.ControlMask"}) {
      auto* resource = addon::GetD3D12Resource(parameters, key);
      snapshot["identities"]["resources"][key] = {{"address", reinterpret_cast<uintptr_t>(resource)}};
      if (resource != nullptr) {
        const auto desc = resource->GetDesc();
        snapshot["identities"]["resources"][key]["size"] = {desc.Width, desc.Height};
        snapshot["identities"]["resources"][key]["format"] = static_cast<uint32_t>(desc.Format);
      }
    }
    if (native_resize_fixed_input) {
      const auto& input = native_resize_fixed_inputs.at({addon::GetUInt(parameters, NVSDK_NGX_Parameter_Width),
          addon::GetUInt(parameters, NVSDK_NGX_Parameter_Height)});
      snapshot["fixed_input_expected_crc32"] = input.crc32;
    }
    native_resize_parameter_snapshots.push_back(std::move(snapshot));
  }
  if (native_resize_selected_sequence) {
    if (native_cache_two_tiers && addon::preparing_adaptive_frame != nullptr) {
      const auto source = addon::features.find(native_cache_two_tier_owner);
      Check(source != addon::features.end() && source->second.slots[0].edit_reuse == nullptr,
            "real standby Evaluate never owns or consumes an edit cache");
    }
    auto* color = addon::GetD3D12Resource(parameters, "DLSSNR.Color");
    auto* output = addon::GetD3D12Resource(parameters, "DLSSNR.Output");
    const uint32_t width = addon::GetUInt(parameters, NVSDK_NGX_Parameter_Width);
    const uint32_t height = addon::GetUInt(parameters, NVSDK_NGX_Parameter_Height);
    Check(color != nullptr && output != nullptr && width != 0 && height != 0
              && color->GetDesc().Width == width && color->GetDesc().Height == height
              && output->GetDesc().Width == width && output->GetDesc().Height == height,
          "selected native Evaluate binds the actual complete working dimensions");
    nlohmann::json snapshot{{"model", reinterpret_cast<uintptr_t>(handle)},
        {"preparing", addon::preparing_adaptive_frame != nullptr}, {"working_size", {width, height}},
        {"color", reinterpret_cast<uintptr_t>(color)}, {"output", reinterpret_cast<uintptr_t>(output)},
        {"reset", addon::GetInt(parameters, "DLSSNR.Reset", -1)}};
    if (native_cache_two_tiers) {
      for (const auto* key : {"DLSSNR.InputWidth", "DLSSNR.InputHeight", "DLSSNR.OutputWidth", "DLSSNR.OutputHeight",
               "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight", "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight"}) {
        snapshot["parameters"][key] = addon::GetUInt(parameters, key);
      }
      for (const auto* key : {"DLSSNR.Color", "DLSSNR.Output", "DLSSNR.MVec", "DLSSNR.Depth"}) {
        auto* resource = addon::GetD3D12Resource(parameters, key);
        Check(resource != nullptr, "two-tier native history has complete model resources");
        const auto desc = resource->GetDesc();
        snapshot["resources"][key] = {{"address", reinterpret_cast<uintptr_t>(resource)},
            {"size", {desc.Width, desc.Height}}, {"format", static_cast<uint32_t>(desc.Format)}};
      }
    }
    native_resize_parameter_snapshots.push_back(std::move(snapshot));
  }
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
  addon::ColdCpuTraceScope sdk_trace("SDK.NREvaluateFeature");
#endif
  if (native_history_queries != nullptr) {
    native_history_queries->Begin(command_list, native_history_interval + 1);
  }
  const auto start = std::chrono::steady_clock::now();
  const NVSDK_NGX_Result result = native_runtime_evaluate(command_list, handle, parameters, progress);
  const double evaluate_cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
  sdk_trace.End();
#endif
  if (native_history_queries != nullptr) {
    if (native_cache_observe) {
      command_list->EndQuery(native_history_queries->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                            2 * (native_history_interval + 1) + 1);
    } else {
      native_history_queries->EndAndResolve(command_list, native_history_interval + 1);
    }
  }
  native_resize_evaluate_ms += evaluate_cpu_ms;
  if (observe_parameters) {
    native_resize_parameter_snapshots.back()["result"] = static_cast<uint32_t>(result);
  }
  if (NVSDK_NGX_SUCCEED(result) && native_motion_neural_readback != nullptr) {
    // Capture inside Evaluate's observer, before returning to RunLookStage.
    auto* neural = addon::GetD3D12Resource(parameters, "DLSSNR.Output");
    native_motion_gpu->Transition(neural, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    native_motion_gpu->RecordTextureReadback(neural, *native_motion_neural_readback);
    native_motion_gpu->Transition(neural, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  if (timed_evaluate) {
    command_list->EndQuery(native_cost_queries->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                           2 * evaluate_interval + 1);
  }
  if (native_resize_ready) {
    native_resize_models.push_back(reinterpret_cast<uintptr_t>(handle));
    native_resize_preparing.push_back(addon::preparing_adaptive_frame != nullptr);
    if (native_resize_demand_prepare || native_resize_selected_sequence) {
      native_resize_evaluate_cpu_ms.push_back(evaluate_cpu_ms);
    }
  }
  native_runtime_results.push_back(result);
  return result;
}

int RunNativeNgx(const char* project_id_path, bool hot_toggle = false,
                 uint32_t mask_mode = addon::kControlMaskDepthFar, bool cost = false,
                 bool motion_test = false, bool resize_test = false, bool history_test = false,
                 bool cache_test = false, bool jitter_test = false, bool cache_cost = false) {
  const auto* two_tiers_environment = std::getenv("NR_NEXUS_TEST_NR_CACHE_TWO_TIERS");
  const bool cache_two_tiers = cache_cost && two_tiers_environment != nullptr
      && std::strcmp(two_tiers_environment, "1") == 0;
  nlohmann::json cache_cost_report;
  std::filesystem::path cache_cost_directory;
  const auto save_cache_cost_report = [&] {
    std::ofstream file(cache_cost_directory / "nr-edit-reuse-cost.json", std::ios::binary | std::ios::trunc);
    file << cache_cost_report.dump(2) << '\n';
    file.close();
    Check(!file.fail(), "NR cache cost evidence saved");
  };
  const auto save_cache_cost_failure = [&](const char* message) {
    if (!cache_cost || cache_cost_directory.empty()) return;
    native_history_queries = nullptr;
    native_cache_observe = false;
    native_cache_cost = false;
    native_cache_two_tiers = false;
    native_cache_two_tier_owner = nullptr;
    addon::edit_reuse_adaptive_experiment = false;
    suppress_control_mask_cost_logs = false;
    cache_cost_report["status"] = "failed";
    cache_cost_report["errors"] = 1;
    cache_cost_report["failure"] = message;
    if (cache_two_tiers) {
      cache_cost_report["last_attempt"] = {{"sr_calls", native_motion_sr_calls}, {"nr_results", native_runtime_results},
          {"nr_resets", native_runtime_resets}, {"nr_models", native_resize_models}, {"nr_preparing", native_resize_preparing},
          {"create_calls", native_resize_create_calls}, {"model_parameters", native_resize_parameter_snapshots}};
    }
    try {
      save_cache_cost_report();
    } catch (...) {
      std::cerr << "FAIL saving NR cache cost failure JSON\n";
    }
  };
  const auto* cold_prepare_environment = std::getenv("NR_NEXUS_TEST_RESIZE_COLD_PREPARE");
  const bool cold_prepare = resize_test && cold_prepare_environment != nullptr
      && std::strcmp(cold_prepare_environment, "1") == 0;
  const auto* single_feature_environment = std::getenv("NR_NEXUS_TEST_RESIZE_SINGLE_FEATURE");
  const bool single_feature = resize_test && single_feature_environment != nullptr
      && (std::strcmp(single_feature_environment, "1") == 0 || std::strcmp(single_feature_environment, "2") == 0);
  const bool single_small_physical = single_feature && std::strcmp(single_feature_environment, "2") == 0;
  const bool diagnostic_probe = cold_prepare || single_feature;
  const auto* demand_prepare_environment = std::getenv("NR_NEXUS_TEST_RESIZE_DEMAND_PREPARE");
  const bool demand_prepare = resize_test && demand_prepare_environment != nullptr
      && (std::strcmp(demand_prepare_environment, "1") == 0 || std::strcmp(demand_prepare_environment, "2") == 0);
  const bool demand_adjacent = demand_prepare && std::strcmp(demand_prepare_environment, "2") == 0;
  const auto* limited_environment = std::getenv("NR_NEXUS_TEST_RESIZE_LIMITED");
  const bool automatic_limited = resize_test && limited_environment != nullptr && std::strcmp(limited_environment, "2") == 0;
  const bool limited_sequence = automatic_limited
      || (resize_test && limited_environment != nullptr && std::strcmp(limited_environment, "1") == 0);
  const auto* async_pso_environment = std::getenv("NR_NEXUS_TEST_RESIZE_ASYNC_PSO");
  const bool limited_async_pso = limited_sequence && !automatic_limited
      && (async_pso_environment == nullptr || std::strcmp(async_pso_environment, "1") == 0);
  const auto* selected_environment = std::getenv("NR_NEXUS_TEST_RESIZE_SELECTED_LEVELS");
  const bool selected_sequence = resize_test && selected_environment != nullptr;
  const uint32_t selected_levels = (selected_sequence && std::strcmp(selected_environment, "0") != 0
      ? (automatic_limited && std::strcmp(selected_environment, "3") == 0
              ? 3u : renodx::addons::dlss5::adaptive_resolution::MAX_PREPARED_LEVELS) : 0u);
  const uint32_t selected_source_limit = (automatic_limited ? 900u : 180u);
  const auto* components_environment = std::getenv("NR_NEXUS_TEST_RESIZE_COLD_COMPONENTS");
  const bool cold_components = resize_test && components_environment != nullptr && std::strcmp(components_environment, "1") == 0;
  const auto* pipeline_environment = std::getenv("NR_NEXUS_TEST_RESIZE_PIPELINE_STEPS");
  const bool pipeline_steps = resize_test && pipeline_environment != nullptr && std::strcmp(pipeline_environment, "1") == 0;
  NativeLimitedAutoscaleProbe limited_pso_probe;
  bool cold_components_failed = false;
  bool cold_teardown_started = false;
  bool demand_validation_failed = false;
  nlohmann::json resize_report;
  const auto save_limited_worker_failure = [&] {
    if (!limited_sequence || automatic_limited) return;
    auto& observation = resize_report["selected_collection"]["limited_residency"]["async_pso_failure"];
    observation["before_cleanup"] = limited_pso_probe.Snapshot();
    observation["private_worker_cleanup_completed"] = limited_pso_probe.Finish();
    observation["after_cleanup"] = limited_pso_probe.Snapshot();
    observation["scope"] = "current run only; before/after bounded private-worker cleanup, not full Runtime teardown and not evidence for prior runs";
  };
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
  const uint32_t cold_source_limit = (pipeline_steps ? addon::kContractDebounceFrames + 32u + 180u : 2u);
  std::vector<addon::ColdCpuTraceState> cold_cpu_traces(cold_source_limit);
  std::vector<bool> cold_cpu_exported(cold_source_limit, false);
  const auto export_cold_cpu_trace = [&](bool partial) {
    if (!cold_components && !pipeline_steps) return;
    for (uint32_t source_call = 0; source_call < cold_cpu_traces.size(); ++source_call) {
      if (cold_cpu_exported[source_call] || cold_cpu_traces[source_call].count == 0) continue;
      const auto& trace = cold_cpu_traces[source_call];
      bool valid = !trace.overflow && trace.current == UINT32_MAX;
      nlohmann::json spans = nlohmann::json::array();
      int64_t log_ns = 0;
      uint32_t log_count = 0;
      uint32_t pso_calls = 0, ngx_creates = 0, sr_evaluates = 0, nr_evaluates = 0;
      int64_t pso_ns = 0, ngx_create_ns = 0, sr_evaluate_ns = 0, nr_evaluate_ns = 0;
      for (uint32_t index = 0; index < trace.count; ++index) {
        const auto& span = trace.spans[index];
        const int64_t inclusive = span.end_ns - span.begin_ns;
        const bool span_valid = span.name != nullptr && inclusive >= 0 && span.children_ns >= 0
            && span.children_ns <= inclusive && (span.parent == UINT32_MAX || span.parent < index);
        valid &= span_valid;
        spans.push_back({{"id", index}, {"parent", (span.parent == UINT32_MAX ? nlohmann::json(nullptr) : nlohmann::json(span.parent))},
            {"name", (span.name != nullptr ? span.name : "invalid")},
            {"detail", (span.detail != nullptr ? nlohmann::json(span.detail) : nlohmann::json(nullptr))},
            {"begin_ns", span.begin_ns}, {"end_ns", span.end_ns}, {"inclusive_ns", inclusive},
            {"exclusive_ns", (span_valid ? nlohmann::json(inclusive - span.children_ns) : nlohmann::json(nullptr))}});
        if (span.name != nullptr && std::strcmp(span.name, "Log.emit_callback_output") == 0) {
          log_ns += inclusive;
          ++log_count;
        }
        if (pipeline_steps && span.name != nullptr) {
          if (std::strcmp(span.name, "D3D12.CreateComputePipelineState") == 0) {
            ++pso_calls;
            pso_ns += inclusive;
          } else if (std::strcmp(span.name, "SDK.CoreCreateFeature") == 0) {
            ++ngx_creates;
            ngx_create_ns += inclusive;
          } else if (std::strcmp(span.name, "SDK.SREvaluateFeature") == 0) {
            ++sr_evaluates;
            sr_evaluate_ns += inclusive;
          } else if (std::strcmp(span.name, "SDK.NREvaluateFeature") == 0) {
            ++nr_evaluates;
            nr_evaluate_ns += inclusive;
          }
        }
      }
      resize_report["cold_cpu_trace"]["frames"].push_back({{"source_call", source_call}, {"thread_id", GetCurrentThreadId()},
          {"valid", valid}, {"partial", partial}, {"overflow", trace.overflow}, {"log_emit_count", log_count},
          {"log_emit_total_ns", log_ns}, {"spans", std::move(spans)}});
      if (pipeline_steps) {
        resize_report["cold_cpu_trace"]["frames"].back().update({
            {"stage", (trace.spans[0].name != nullptr && std::strcmp(trace.spans[0].name, "prefix.source_call") == 0 ? "prefix" : "selected")},
            {"pso_create_api_calls", pso_calls}, {"pso_create_cpu_ns", pso_ns},
            {"ngx_create_calls", ngx_creates}, {"ngx_create_cpu_ns", ngx_create_ns},
            {"sr_evaluate_calls", sr_evaluates}, {"sr_evaluate_cpu_ns", sr_evaluate_ns},
            {"nr_evaluate_calls", nr_evaluates}, {"nr_evaluate_cpu_ns", nr_evaluate_ns}});
      }
      cold_cpu_exported[source_call] = true;
      cold_components_failed |= partial || !valid;
    }
  };
#endif
  try {
    Check(!cold_prepare || !single_feature, "resize diagnostic modes are separate probes");
    Check(!demand_prepare || !diagnostic_probe, "demand preparation uses the ordinary native resize fixture");
    Check(async_pso_environment == nullptr || (limited_sequence && !automatic_limited
        && (std::strcmp(async_pso_environment, "0") == 0 || std::strcmp(async_pso_environment, "1") == 0)),
        "the private PSO comparison switch accepts only 0/1 within the existing limited case 1");
    Check(!limited_sequence || (selected_sequence && !demand_prepare
        && (automatic_limited ? selected_levels == 3 && !pipeline_steps : selected_levels == 15 && pipeline_steps)),
        "limited case 1 requires selected 15/pipeline steps; automatic case 2 requires selected 3/no pipeline probe; neither uses legacy demand");
#if !defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
    Check(!cold_components && !pipeline_steps, "cold CPU probes require the dedicated NR_NEXUS_TEST_COLD_CPU_TRACE native build");
#endif
    Check(!cold_components || (selected_sequence && selected_levels == 0), "cold components requires explicit Default 0");
    if (pipeline_steps) {
      const auto* ready = std::getenv("NR_NEXUS_TEST_RESIZE_READY");
      Check(selected_sequence && (selected_levels == 0 || selected_levels == 15) && !cold_components
              && ready != nullptr && std::strcmp(ready, "1") == 0,
          "pipeline steps requires explicit Default 0 or 15, Ready 1, and no two-source cold-components probe");
    }
    if (selected_sequence) {
      Check(std::strcmp(selected_environment, "0") == 0 || std::strcmp(selected_environment, "15") == 0
          || (automatic_limited && std::strcmp(selected_environment, "3") == 0),
          "selected comparison accepts Default 0 or 15; the automatic limited single case selects three logical levels");
      Check(!diagnostic_probe && !demand_prepare, "selected comparison excludes the existing diagnostic experiments");
      for (const char* key : {"NR_NEXUS_TEST_RESIZE_SETTINGS", "NR_NEXUS_TEST_RESIZE_EXPECT_REUSE",
               "NR_NEXUS_TEST_RESIZE_REUSE_FEATURE", "NR_NEXUS_TEST_RESIZE_FIXED_INPUT"}) {
        const auto* value = std::getenv(key);
        Check(value == nullptr || std::strcmp(value, "0") == 0, "selected comparison requires the original independent-tier path");
      }
      const auto* ready = std::getenv("NR_NEXUS_TEST_RESIZE_READY");
      Check(ready == nullptr || std::strcmp(ready, "1") == 0, "selected comparison requires the complete ready collection");
    }
    cache_test |= cache_cost;
    if (cache_cost) {
      Check(two_tiers_environment == nullptr || std::strcmp(two_tiers_environment, "0") == 0 || cache_two_tiers,
            "two-tier cache cost is explicitly disabled or selected with 1");
      const char* requested_condition = std::getenv("NR_NEXUS_TEST_NR_CACHE_CONDITION");
      const std::string condition = (requested_condition != nullptr ? requested_condition : "baseline");
      Check(condition == "baseline" || condition == "cache", "known NR cache cost condition");
      const std::filesystem::path directory("cost-" + condition);
      Check(std::filesystem::create_directory(directory), "cache cost uses a new isolated condition directory");
      cache_cost_directory = directory;
      cache_cost_report = {{"schema", "nr-nexus-nr-edit-reuse-cost-v1"}, {"condition", condition},
          {"evidence", "NATIVE_GPU+NATIVE_NGX"}, {"status", "initializing"}};
    }
    const bool sequence_test = history_test || cache_test;
    const char* cache_route_environment = (cache_test ? std::getenv("NR_NEXUS_TEST_NR_CACHE_ROUTE") : nullptr);
    const std::string cache_route = (cache_cost ? (cache_two_tiers ? "inline" : "xefg-record")
        : (cache_route_environment != nullptr ? cache_route_environment : "inline"));
    Check(cache_route == "inline" || cache_route == "xefg-record" || cache_route == "render",
          "known NR cache recording route");
    const bool render_cache_test = cache_route == "render";
    const bool xefg_cache_test = cache_test && cache_route == "xefg-record";
    const char* cache_format_environment = (cache_test ? std::getenv("NR_NEXUS_TEST_NR_CACHE_FORMAT") : nullptr);
    const std::string cache_format = (cache_cost ? "scrgb"
        : (cache_format_environment != nullptr ? cache_format_environment : "scrgb"));
    Check(cache_format == "scrgb" || cache_format == "pq", "known NR cache source format");
    const bool pq_cache_test = cache_test && cache_format == "pq";
    Check(!pq_cache_test || xefg_cache_test, "PQ cache input requires the production XeFG Record route");
    native_cache_pq = pq_cache_test;
    // Resize reuses public SDK initialization and the real SR owner, without
    // running the independent paired ControlMask motion experiment.
    motion_test = motion_test || resize_test || sequence_test || jitter_test;
    if (resize_test || sequence_test || jitter_test) {
      // This headless fixture has no Present stream. Disable the production
      // game-exit heuristic so its normal submission-proven retirement runs.
      addon::shutdown_quiesce_enabled = false;
    }
    const bool single_stage = hot_toggle || cost || motion_test;
    const char* cost_mode_environment = std::getenv("NR_NEXUS_TEST_CONTROL_MASK_COST_MODE");
    const std::string cost_mode = (cost_mode_environment != nullptr ? cost_mode_environment : "depthfar");
    if (cost) {
      Check(cost_mode == "off" || cost_mode == "rgb" || cost_mode == "depthfar",
            "cost mode must be off, rgb or depthfar");
      mask_mode = (cost_mode == "rgb" ? addon::kControlMaskRgb : addon::kControlMaskDepthFar);
      addon::cost_meter_enabled = false;
      addon::gpu_timers_enabled = false;
      native_cost_observe = true;
      native_runtime_masks.reserve(2);
      native_runtime_results.reserve(2);
      native_runtime_resets.reserve(2);
    }
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    addon::addon_module = GetModuleHandleW(nullptr);
    Gpu gpu(true);
    const UINT width = (cost || cache_cost ? 3840 : (resize_test ? 1920 : 960));
    const UINT height = (cost || cache_cost ? 2160 : (resize_test ? 1080 : 540));
    const UINT render_width = (cache_cost ? 2560 : (motion_test ? (resize_test ? 1280 : 640) : width));
    const UINT render_height = (cache_cost ? 1440 : (motion_test ? (resize_test ? 720 : 360) : height));
    Check(addon::InstallQueueCompletionHooks(gpu.queue.Get()), "native ControlMask queue completion hooks");
    Check(addon::InstallCommandListStateHooks(gpu.list.Get()), "native ControlMask command-list hooks");
    Check(addon::EnsureControlMaskPipeline(gpu.device.Get()), "native ControlMask production pipeline");

    auto color = gpu.Texture(render_width, render_height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto depth = gpu.Texture(render_width, render_height, DXGI_FORMAT_R32_FLOAT);
    auto motion = gpu.Texture(render_width, render_height, DXGI_FORMAT_R16G16_FLOAT);
    D3D12_RESOURCE_DESC output_desc{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = width, .Height = height, .DepthOrArraySize = 1, .MipLevels = 1,
        .Format = DXGI_FORMAT_R16G16B16A16_FLOAT, .SampleDesc = {1, 0},
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    const D3D12_HEAP_PROPERTIES default_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> output;
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
        &output_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output))),
        "native ControlMask output allocation");
    ComPtr<ID3D12Resource> pq_source;
    if (pq_cache_test) {
      auto pq_desc = output_desc;
      pq_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
          &pq_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&pq_source))),
          "native cache actual PQ NR source allocation");
    }
    auto* cache_color = (pq_cache_test ? pq_source.Get() : output.Get());
    std::vector<uint32_t> pq_source_codes;

    const size_t pixel_count = static_cast<size_t>(width) * height;
    const size_t render_pixel_count = static_cast<size_t>(render_width) * render_height;
    std::vector<uint16_t> color_data(render_pixel_count * 4);
    std::vector<float> depth_data(render_pixel_count);
    std::vector<uint16_t> motion_data(render_pixel_count * 2, 0);
    for (UINT y = 0; y < render_height; ++y) {
      for (UINT x = 0; x < render_width; ++x) {
        const size_t pixel = static_cast<size_t>(y) * render_width + x;
        const float u = static_cast<float>(x) / (render_width - 1);
        const float v = static_cast<float>(y) / (render_height - 1);
        color_data[pixel * 4] = DirectX::PackedVector::XMConvertFloatToHalf(0.12f + 0.65f * u);
        color_data[pixel * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(0.1f + 0.65f * v);
        color_data[pixel * 4 + 2] = DirectX::PackedVector::XMConvertFloatToHalf(0.15f + 0.5f * (1.f - u * v));
        color_data[pixel * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
        depth_data[pixel] = 0.2f + 0.6f * v;
      }
    }
    auto color_transfer = gpu.CreateTextureTransfer(color.Get());
    auto depth_transfer = gpu.CreateTextureTransfer(depth.Get());
    auto motion_transfer = gpu.CreateTextureTransfer(motion.Get());
    gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.UploadTexture(color.Get(), color_transfer, reinterpret_cast<const uint8_t*>(color_data.data()),
                      color_data.size() * sizeof(uint16_t), render_width * 8);
    gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.UploadTexture(depth.Get(), depth_transfer, reinterpret_cast<const uint8_t*>(depth_data.data()),
                      depth_data.size() * sizeof(float), render_width * sizeof(float));
    gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.Transition(motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.UploadTexture(motion.Get(), motion_transfer, reinterpret_cast<const uint8_t*>(motion_data.data()),
                      motion_data.size() * sizeof(uint16_t), render_width * 4);
    gpu.Transition(motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_DEST);
    if (!motion_test) {
      gpu.list->CopyResource(output.Get(), color.Get());
    }
    gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.Submit();

    addon::PrimeNgxLoaderSymbols(gpu.device.Get());
    if (!cost && std::getenv("NR_NEXUS_TEST_DIRECT_INIT_ONLY") != nullptr) {
      std::scoped_lock runtime_lock(addon::runtime_mutex);
      const bool initialized = addon::EnsureDirectRuntime(gpu.list.Get());
      std::cout << "DIRECT_INIT_ONLY initialized=" << initialized << " result=0x"
                << std::hex << addon::last_result.load() << std::dec << '\n';
      return initialized ? 0 : 2;
    }
    const auto& symbols = addon::NgxLoaderView();
    Check(symbols.nr_ready && !symbols.nr_failed
              && symbols.nr_requirements.file_version == "310.9.16.0",
          "production loader resolves the pinned ControlMask Runtime from the isolated test directory");
    std::ifstream project_id_file(project_id_path);
    std::string project_id;
    Check(project_id_file.is_open() && std::getline(project_id_file, project_id)
              && project_id.size() == 36,
          "read the existing isolated NGX ProjectID");
    const std::wstring runtime_directory = addon::AddonDirectory().wstring();
    const wchar_t* runtime_paths[]{runtime_directory.c_str()};
    NVSDK_NGX_FeatureCommonInfo common_info{};
    common_info.PathListInfo = {runtime_paths, 1};
    common_info.LoggingInfo = {NativeNgxSdkLog, NVSDK_NGX_LOGGING_LEVEL_ON, true};
    const std::wstring application_data_path = addon::AddonDirectory().wstring();
    const auto core_init = NVSDK_NGX_D3D12_Init_with_ProjectID(
        project_id.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "NR-Nexus-ControlMask-P0-offline-1", application_data_path.c_str(),
        gpu.device.Get(), &common_info);
    if (!NVSDK_NGX_SUCCEED(core_init)) {
      std::cerr << "public NGX SDK/Core init result=0x" << std::hex
                << static_cast<uint32_t>(core_init) << std::dec << '\n';
    }
    Check(NVSDK_NGX_SUCCEED(core_init), "public SDK initializes the real NGX Core context");
    addon::PrimeNgxLoaderSymbols(gpu.device.Get());
    Check(addon::NgxLoaderView().core_ready,
          "production loader observes the SDK-initialized public NGX Core");
    {
      std::scoped_lock runtime_lock(addon::runtime_mutex);
      addon::control_mask_use_core_context = single_stage && !motion_test;
      addon::SuccessfulSrCoreScope host_sr(single_stage && !motion_test ? gpu.list.Get() : nullptr);
      if (motion_test) {
        // The public SDK already initialized this device. Exercise normal owned
        // NR slots without invoking the production snippet identity bridge.
        const auto& provider = addon::NgxLoaderView();
        addon::direct_api.module = (cache_two_tiers ? provider.core : provider.nr_module);
        addon::direct_api.allocate = provider.core_allocate;
        addon::direct_api.destroy = provider.core_destroy;
        addon::direct_api.create = provider.core_create;
        addon::direct_api.evaluate = provider.core_evaluate;
        addon::direct_api.release = provider.core_release;
        addon::direct_api.borrowed_core_context = false;
        if (cache_two_tiers) {
          Check(!addon::direct_api.private_instance && !addon::bridge_force_snippet.load(),
                "inline two-tier cost retains the real ordinary public-Core context");
        }
        addon::direct_load_state = addon::DirectLoadState::Ready;
        addon::direct_device = gpu.device.Get();
        addon::direct_device->AddRef();
        addon::direct_device_native = gpu.device.Get();
        addon::direct_runtime_requirements = provider.nr_requirements;
        addon::direct_runtime_sha256 = provider.nr_sha256;
      }
      Check(addon::EnsureDirectRuntime(gpu.list.Get()),
            "production Runtime adopts the initialized public SDK provider");
      Check(addon::parameter_runtime_via_core.load()
                && addon::core_evaluate_feature != nullptr,
            "production path adopts public Core parameters and Feature 18 calls");
      native_runtime_evaluate = addon::core_evaluate_feature;
      addon::core_evaluate_feature = ObserveNativeRuntimeEvaluate;
    }
    addon::enabled = !jitter_test;
    addon::hooks_enabled = true;
    addon::hook_point = (xefg_cache_test ? addon::kHookPresent
        : (single_stage && !render_cache_test ? addon::kHookUpscaled : addon::kHookRender));
    addon::stack_passes = (single_stage ? 1 : 2);
    addon::pass_stage[0] = (xefg_cache_test ? 0u : 2u);
    addon::configuration_generation.fetch_add(1);
    addon::nr_follow_input_res = false;
    addon::nr_resolution_scale = 1.f;
    if (render_cache_test || (cache_cost && !cache_two_tiers)) {
      addon::nr_adaptive_enabled = false;
    }
    addon::look_mode = false;
    addon::transfer_strength = 1.f;
    addon::color_strength = 1.f;
    addon::intensity = 1.f;
    addon::preset = (single_stage ? 2u : 1u);
    addon::codec_mode = 0u;
    const bool mask_enabled = !sequence_test && !jitter_test && (cost ? cost_mode != "off"
                                    : hot_toggle || std::getenv("NR_NEXUS_TEST_CONTROL_MASK_OFF") == nullptr);
    const char* mask_mode_name = (mask_mode == addon::kControlMaskRgb ? "rgb" : "depthfar");
    addon::control_mask_enabled = mask_enabled;
    addon::control_mask_mode = mask_mode;
    addon::control_mask_depth_near = 0.7f;
    addon::control_mask_depth_far = 0.9f;
    addon::control_mask_blend_mode = 0u;
    if (cache_test) {
      addon::edit_reuse_enabled = true;
      addon::native_fg_public_off = true;
      addon::native_fg_declaration = addon::NativeFgDeclaration::kOff;
      addon::cost_meter_enabled = false;
      addon::gpu_timers_enabled = false;
    }
    if (motion_test) {
      // Keep the paired proxy independent of wall-clock governor rates and
      // the preceding sequence's cut-aware normalization/Look history.
      addon::feed_mode = 1u;
      addon::norm_governor = 0u;
      addon::detail_stability = (sequence_test || jitter_test ? 1u : 0u);  // 0 Auto, 1 Off.
      addon::control_mask_mode = addon::kControlMaskRgb;
      addon::control_mask_motion_enabled = false;
      addon::control_mask_motion_start = 2.f;
      addon::control_mask_motion_end = 16.f;
      addon::use_auto_mask = false;
      native_cost_observe = true;
    }
    if (cache_two_tiers) {
      Check(!addon::edit_reuse_adaptive_experiment.load(), "two-tier native experiment starts disabled");
      renodx::addons::dlss5::gpu_timers::comparison_settings = addon::comparison_ui::CaptureSettings;
      addon::adaptive_configuration_settings = addon::comparison_ui::CaptureAdaptiveSettings;
      addon::kSetAdaptiveLevelCount.Store(2.f);
      addon::kSetAdaptiveMinScale.Store(.75f);
      addon::kSetAdaptiveEnabled.Store(1.f);
      addon::nr_adaptive_limited_residency = false;
      addon::adaptive_feature_mode = addon::AdaptiveFeatureMode::OFF;
      addon::max_worksets = 4;
      addon::edit_reuse_adaptive_experiment = true;
      native_cache_two_tiers = true;
      native_cache_cost = true;
      native_resize_ready = true;
      native_resize_selected_sequence = true;
      native_resize_create = addon::core_create_feature;
      native_resize_release = addon::core_release_feature;
      addon::core_create_feature = ObserveNativeResizeCreate;
      addon::core_release_feature = ObserveNativeResizeRelease;
    }
    if (cost) {
      addon::use_auto_mask = false;
      addon::control_mask_red = 1.f;
      addon::control_mask_green = 1.f;
      addon::control_mask_blue = 1.f;
    }

    renodx::addons::dlss5::bridge::OwnedParameters mock_parameters;
    NVSDK_NGX_Parameter* sr_parameters = nullptr;
    if (motion_test) {
      Check(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&sr_parameters))
                && sr_parameters != nullptr,
            "public SDK supplies real SR capability parameters");
    }
    auto& parameters = (motion_test ? *sr_parameters : static_cast<NVSDK_NGX_Parameter&>(mock_parameters));
    parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
    parameters.Set(NVSDK_NGX_Parameter_Output, output.Get());
    parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    parameters.Set(NVSDK_NGX_Parameter_Width, render_width);
    parameters.Set(NVSDK_NGX_Parameter_Height, render_height);
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, width);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, height);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, render_width);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, render_height);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0u);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        static_cast<uint32_t>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
    NVSDK_NGX_Handle* sr_handle = nullptr;
    const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC04A7});
    if (motion_test) {
      parameters.Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxQuality));
      parameters.Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
      parameters.Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
      parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
          static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_IsHDR
                           | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
      parameters.Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.f);
      parameters.Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.f);
      parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.f);
      parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.f);
      parameters.Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 1000.f / 60.f);
      parameters.Set(NVSDK_NGX_Parameter_Reset, 1);
      addon::ngx_slot_real[0].create = reinterpret_cast<void*>(NVSDK_NGX_D3D12_CreateFeature);
      const auto created = addon::HookedCreateFeatureSlot<0>(
          gpu.list.Get(), NVSDK_NGX_Feature_SuperSampling, &parameters, &sr_handle);
      addon::ngx_slot_real[0].create = nullptr;
      std::cout << "MOTION real SR create=0x" << std::hex << static_cast<uint32_t>(created) << std::dec << std::endl;
      Check(NVSDK_NGX_SUCCEED(created) && sr_handle != nullptr && addon::features.contains(sr_handle),
            "production Create wrapper observes a real DLSS SR feature");
      owner = sr_handle;
      if (cache_two_tiers) {
        native_cache_two_tier_owner = owner;
      }
      native_motion_gpu = &gpu;
      gpu.Submit();
    } else {
      addon::RegisterCreatedFeatureLocked(owner, addon::kFeatureDlss, &parameters);
    }
    addon::fg_service::Stream cache_stream;
    uint64_t cache_application_frame = 0;
    if (xefg_cache_test) {
      cache_stream.device = gpu.device.Get();
      cache_stream.device->AddRef();
      cache_stream.queue = gpu.queue.Get();
      cache_stream.queue->AddRef();
      cache_stream.epoch = 1;
    }
    const auto* nr_owner = (xefg_cache_test ? cache_stream.roles[0].Handle() : owner);
    const auto* nr_workset_key = (xefg_cache_test
        ? reinterpret_cast<const ID3D12Resource*>(&cache_stream.roles[0]) : (render_cache_test ? color.Get() : output.Get()));
    const auto record_xefg = [&] {
      // Exercise production Record and private-color writeback. SDK/Service
      // admission is covered by WARP+MOCK. PQ input construction submits the
      // real SR recording separately, before the contiguous NR GPU interval.
      if (pq_cache_test) {
        // Input construction only: retain real SR's FP16 snapshot, then convert
        // BT.709 scRGB (80 nits/unit) to BT.2020/PQ (10000 nits/unit).
        // The extra readback/CPU conversion/upload precedes the NR chain timer.
        auto sr_transfer = gpu.CreateTextureTransfer(output.Get());
        gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(output.Get(), sr_transfer);
        gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.Submit();
        const auto sr_bytes = gpu.ReadTexture(sr_transfer);
        pq_source_codes.resize(static_cast<size_t>(width) * height);
        const auto pack = [](double value, uint32_t maximum) {
          return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * maximum));
        };
        for (size_t pixel = 0; pixel < pq_source_codes.size(); ++pixel) {
          std::array<uint16_t, 4> half;
          std::memcpy(half.data(), sr_bytes.data() + pixel * sizeof(half), sizeof(half));
          std::array<double, 4> linear;
          for (size_t channel = 0; channel < linear.size(); ++channel) {
            linear[channel] = DirectX::PackedVector::XMConvertHalfToFloat(half[channel]);
            Check(std::isfinite(linear[channel]), "real SR supplies finite PQ conversion input");
          }
          const std::array<double, 3> bt2020{
              .6274040 * linear[0] + .3292820 * linear[1] + .0433136 * linear[2],
              .0690970 * linear[0] + .9195400 * linear[1] + .0113612 * linear[2],
              .0163916 * linear[0] + .0880132 * linear[1] + .8955950 * linear[2]};
          uint32_t packed = pack(linear[3], 3) << 30;
          for (size_t channel = 0; channel < bt2020.size(); ++channel) {
            const double power = std::pow(std::clamp(bt2020[channel] * .008, 0.0, 1.0), .1593017578125);
            const double pq = std::pow((.8359375 + 18.8515625 * power) / (1.0 + 18.6875 * power), 78.84375);
            packed |= pack(pq, 1023) << (10 * channel);
          }
          pq_source_codes[pixel] = packed;
        }
        auto transfer = gpu.CreateTextureTransfer(pq_source.Get());
        gpu.Transition(pq_source.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.UploadTexture(pq_source.Get(), transfer, reinterpret_cast<const uint8_t*>(pq_source_codes.data()),
            pq_source_codes.size() * sizeof(uint32_t), width * sizeof(uint32_t));
        gpu.Transition(pq_source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (native_cache_observe && native_history_queries != nullptr) {
          native_history_queries->Begin(gpu.list.Get(), native_history_interval + 2);
        }
      }
      renodx::addons::dlss5::fg::FrameInput input{
          .queue = gpu.queue.Get(), .backbuffer = cache_color, .depth = depth.Get(), .motion = motion.Get(),
          .backbuffer_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          .depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          .motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          .color_width = width, .color_height = height, .guide_width = render_width, .guide_height = render_height,
          .motion_scale_x = 1.f, .motion_scale_y = 1.f,
          .jitter_x = addon::GetFloat(&parameters, NVSDK_NGX_Parameter_Jitter_Offset_X),
          .jitter_y = addon::GetFloat(&parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y),
          .context = reinterpret_cast<uintptr_t>(&cache_stream), .epoch = 1,
          .application_frame = ++cache_application_frame,
          .present_id = static_cast<uint32_t>(cache_application_frame),
          .init_flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes,
          .color_space = (pq_cache_test ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709),
          .has_present_id = true, .has_constants = true,
          .reset = addon::GetInt(&parameters, NVSDK_NGX_Parameter_Reset) != 0, .fg_enabled = true};
      auto& slot = cache_stream.slots[0];
      Check(addon::fg_service::PrepareSurfaces(&cache_stream, &slot, input, false, false),
            "XeFG Record fixture prepares existing private color surfaces");
      addon::PresentTarget target;
      addon::SnapshotPresentRestoreConfiguration(&target);
      target.hdr_mode = (pq_cache_test ? 2 : 1);
      target.units = (pq_cache_test ? addon::codec::SourceUnits{addon::codec::Encoding::Pq, true, 10000.f, true}
                                   : addon::codec::SourceUnits{addon::codec::Encoding::Linear, true, 80.f, false});
      target.hdr_encoding = addon::V6EncodingConstant(target.units);
      target.hdr_dark_gate = addon::V6DarkGate(target.units);
      target.hdr_pedestal_cap = addon::codec::BlackPedestalCap(target.units);
      target.hdr_white_nits = 200.f;
      target.hdr_white_source = "fixed_xefg_record_fixture";
      target.hdr_pedestal = renodx::addons::dlss5::present_hdr_pedestal::Decide(target.hdr_pedestal.configuration,
          {.actual_present = true, .hdr_mode = target.hdr_mode, .v6_available = false,
           .encoding_valid = true, .absolute_units = true, .unit_nits = target.units.unit_nits,
           .display_codec = target.hdr_curve == 1u, .legacy_mode = target.hdr_pedestal.legacy_mode,
           .debug_bypass = target.diagnostic.bypass_hdr_pedestal});
      target.divisor = addon::codec::PresentDivisor(
          target.units, 200.f, addon::paper_white_scale.load(), addon::pq_calibration.load());
      target.swapchain_format = cache_color->GetDesc().Format;
      const renodx::addons::dlss5::EvaluateChainScope chain;
      bool ready = false;
      std::array<addon::NrRecording, 2> recordings;
      addon::fg_service::Record(&cache_stream, &slot, gpu.list.Get(), input, target, &ready, &recordings);
      if (ready) {
        addon::fg_service::CopyColor(gpu.list.Get(), cache_color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            slot.color[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0, 0, 0, 0, width, height);
      }
      return ready;
    };
    const auto restore_cost_inputs = [&] {
      // The mock host does no GPU work. Restore both inputs explicitly because
      // Upscaled NR consumes and replaces Output; Color alone is not sufficient.
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_COPY_DEST);
      const D3D12_TEXTURE_COPY_LOCATION source{
          .pResource = color_transfer.upload.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
          .PlacedFootprint = color_transfer.footprint};
      const D3D12_TEXTURE_COPY_LOCATION color_destination{
          .pResource = color.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      const D3D12_TEXTURE_COPY_LOCATION output_destination{
          .pResource = output.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      gpu.Retain(color_transfer.upload.Get());
      gpu.list->CopyTextureRegion(&color_destination, 0, 0, 0, &source, nullptr);
      gpu.list->CopyTextureRegion(&output_destination, 0, 0, 0, &source, nullptr);
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    nlohmann::json cost_report;
    nlohmann::json cost_prefix = nlohmann::json::array();
    constexpr uint32_t cost_prefix_frames = addon::kContractDebounceFrames + 8;
    bool production_chain_completed = false;
    uint32_t prefix_host_evaluates = 0;
    size_t prefix_nr_evaluates = 0;
    const auto submit_resize = [&](bool reset = true) {
      if (!demand_prepare && !selected_sequence && !cache_two_tiers) {
        gpu.Submit();
        return;
      }
      // The live queue hook owns registration. Preserve its healthy receipt
      // through production Drain/Poll before the fixture releases references.
      Check(SUCCEEDED(gpu.list->Close()), "resize recording close");
      ID3D12CommandList* lists[]{gpu.list.Get()};
      gpu.queue->ExecuteCommandLists(1, lists);
      gpu.submission_pending = true;
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "resize completion signal");
      HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr, "resize completion event");
      const auto signal = gpu.fence->SetEventOnCompletion(gpu.serial, event);
      const auto wait = (SUCCEEDED(signal) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
      CloseHandle(event);
      Check(wait == WAIT_OBJECT_0, "resize bounded GPU completion");
      const auto completed = gpu.fence->GetCompletedValue();
      Check(completed >= gpu.serial && completed != UINT64_MAX && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
            "resize submission completes on a healthy device before resetting or releasing its references");
      gpu.submission_pending = false;
      if (reset) {
        Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
              "resize completed recording reset");
      }
    };
    ComPtr<IDXGIAdapter3> selected_adapter;
    uint64_t selected_peak_local_bytes = 0;
    double selected_max_wrapper_ms = 0., selected_max_source_interval_ms = 0.;
    auto selected_prefix_begin = std::chrono::steady_clock::time_point{};
    auto selected_prefix_completed = selected_prefix_begin;
    auto selected_previous_start = selected_prefix_begin;
    const uint32_t selected_features_before = addon::nr_features_created;
    const uint64_t selected_worksets_before = addon::worksets_created;
    const auto sample_selected_memory = [&] {
      DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
      Check(SUCCEEDED(selected_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)),
          "selected comparison samples actual process local memory and budget");
      selected_peak_local_bytes = std::max(selected_peak_local_bytes, memory.CurrentUsage);
      if (resize_report.contains("selected_collection")) {
        resize_report["selected_collection"]["sampled_peak_process_local_bytes"] = selected_peak_local_bytes;
      }
      return memory;
    };
    if (selected_sequence) {
      Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&selected_adapter))),
          "selected comparison resolves the actual memory adapter before upper initialization");
      const auto memory = sample_selected_memory();
      resize_report = {{"schema", "nr-nexus-native-resize-v1"}, {"evidence", "NATIVE_GPU+NATIVE_NGX"},
          {"probe_execution", "Running"}, {"teardown_status", "NotRun"},
          {"selected_collection", {{"schema", "nr-nexus-native-selected-v1"}, {"selected_levels", selected_levels},
              {"minimum_scale", automatic_limited ? .5 : .33}, {"ordinary_workset_limit", 4}, {"source_frame_limit", selected_source_limit},
              {"prefix_frame_limit", addon::kContractDebounceFrames + 32},
              {"memory_baseline_process_local_bytes", memory.CurrentUsage}, {"memory_baseline_budget_bytes", memory.Budget},
              {"prefix_cpu_wrapper_ms", 0.}, {"prefix_cpu_create_ms", 0.}, {"prefix_cpu_release_ms", 0.},
              {"prefix_cpu_host_sr_ms", 0.}, {"prefix_cpu_nr_evaluate_ms", 0.},
              {"prefix_create_count", 0u}, {"prefix_release_count", 0u}, {"prefix_frames", nlohmann::json::array()}}}};
      addon::kSetAdaptiveEnabled.Store(pipeline_steps || automatic_limited ? 1.f : 0.f);
      addon::kSetAdaptiveLevelCount.Store(selected_levels);
      addon::kSetAdaptiveMinScale.Store(automatic_limited ? .5f : .33f);
      addon::max_worksets = 4;
      if (limited_sequence) {
        Check(width == 1920 && height == 1080 && addon::stack_passes.load() == 1
            && addon::codec_pipeline.autoscale == nullptr, "limited native case is one cold 1080p single-pass autoscale source");
        if (automatic_limited) {
          Check(!addon::async_autoscale_preparation.load(), "automatic player-mode probe leaves private asynchronous PSO preparation off");
          addon::kSetAdaptiveLimitedResidency.Store(1.f);
          addon::ApplySettingEffect(addon::kSetAdaptiveLimitedResidency.effect);
          addon::kSetAdaptiveTargetFps.Store(100.f);
        } else {
          Check(addon::codec_pipeline.look.band == nullptr && addon::async_autoscale_task == nullptr,
              "private native observation also starts with a cold look bands target and no previous task");
          addon::nr_adaptive_limited_residency = true;
          addon::async_autoscale_preparation = limited_async_pso;
          limited_pso_probe.Attach(gpu.device.Get(), limited_async_pso);
        }
        resize_report["selected_collection"]["limited_residency"] = {
            {"enabled", true}, {"async_autoscale", limited_async_pso}, {"async_look_bands", limited_async_pso},
            {"private_pso_mode", automatic_limited ? "disabled" : (limited_async_pso ? "async" : "sync")},
            {"logical_levels", selected_levels},
            {"selection_scope", automatic_limited
                ? "production Session/Controller decisions; MOCK output FPS; real SR/NR and GPU samples; no direct Request or state edits"
                : "direct Session requests; real SR/NR; no automatic FPS or dynamic visual claim"},
            {"prefix_source_cadence", "60 Hz paced source opportunities, same as the selected loop; no worker join or additional source attempts"},
            {"ready_wall_comparison_limit", "ready wall includes the 60 Hz prefix cadence; not directly comparable with the former unpaced synchronous prefix"},
            {"cache_scope", "process-cold production slots; driver/disk caches are not reset or controlled, so run order is not a cold-cache guarantee"},
            {"async_start_boundary", automatic_limited ? "NotRun: private PSO experiment remains off"
                : (limited_async_pso ? "tool calls production StartAsyncAutoscalePreparation before the next source, outside runtime_mutex"
                                     : "NotApplicable: synchronous production API calls remain inside the source")}};
      }
      if (pipeline_steps || automatic_limited) {
        // Match the later selected loop before the first source; changing
        // these identity providers midway would restart the same collection.
        renodx::addons::dlss5::gpu_timers::comparison_settings = addon::comparison_ui::CaptureSettings;
        addon::adaptive_configuration_settings = addon::comparison_ui::CaptureAdaptiveSettings;
        Check(addon::adaptive_feature_mode.load() == addon::AdaptiveFeatureMode::OFF,
            "pipeline steps retains the independent Feature path with legacy capacity experiments off");
      }
      native_resize_selected_sequence = true;
      native_resize_ready = true;
      native_resize_create = addon::core_create_feature;
      native_resize_release = addon::core_release_feature;
      addon::core_create_feature = ObserveNativeResizeCreate;
      addon::core_release_feature = ObserveNativeResizeRelease;
    }
    if (cold_components || pipeline_steps) {
      resize_report.update({{"host_sr", "NATIVE_NGX"}, {"render_size", {render_width, render_height}},
          {"output_size", {width, height}}, {"stage", "Upscaled"}, {"passes", 1}, {"colors", 1},
          {"format", "RGBA16F"}, {"requested_model", "B"}, {"internal_model", "unknown"},
          {"runtime", addon::direct_runtime_requirements.file_version}, {"driver_x100", addon::direct_runtime_requirements.driver_x100},
          {"nr_sha256", addon::direct_runtime_sha256}, {"input_hash_algorithm", "CRC32 of uploaded tightly packed bytes"},
          {"input_color_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(color_data.data()), color_data.size() * sizeof(uint16_t))},
          {"input_depth_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(depth_data.data()), depth_data.size() * sizeof(float))},
          {"input_motion_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(motion_data.data()), motion_data.size() * sizeof(uint16_t))}});
      resize_report["frames"] = nlohmann::json::array();
      if (cold_components) {
        resize_report["selected_collection"]["source_frame_limit"] = 0;
        resize_report["selected_collection"]["prefix_frame_limit"] = 2;
      }
      resize_report[cold_components ? "cold_components" : "pipeline_steps"] = {
          {"source_call_limit", (cold_components ? 2u : addon::kContractDebounceFrames + 32u + 180u)},
          {"adaptive_enabled_before_first_source", pipeline_steps}, {"complete_selected_sequence", pipeline_steps && !limited_sequence},
          {"complete_limited_target_sequence", limited_sequence},
          {"final_output_readback", false}, {"sdk_initialized_before_measurement", true},
          {"codec_root_prepared_before_measurement", addon::codec_pipeline.root_signature != nullptr},
          {"control_mask_pso_prepared_before_measurement", addon::codec_pipeline.control_mask != nullptr},
          {"external_timing_scope", "submit/fence includes Close/Execute/Signal/wait/fixture Reset; retire includes Drain/Poll/reference cleanup; record-to-submit includes export/query outside Hook"},
          {"runtime_scope", "public SDK/provider setup precedes measurement; SDK-internal lazy work remains inside its actual calls"}};
      resize_report["cold_cpu_trace"] = {{"schema", "nr-nexus-cold-cpu-trace-v1"}, {"spans_per_source_limit", 512},
          {"scope", pipeline_steps
              ? "calling thread only; 512 spans per source in bounded heap storage; active only inside each prefix/selected Hook call"
              : "calling thread only; fixed storage active only during the first two prefix Hook calls"},
          {"accounting", "inclusive spans nest; exclusive is inclusive minus direct children, retaining uninstrumented work and observer overhead; never sum inclusive parent and child values"},
          {"log_scope", "Log includes message prefix construction and synchronous ReShade callback/capture/stdout; caller formatting remains with its parent"},
          {"frames", nlohmann::json::array()}};
    }
    const uint32_t prefix_limit = (jitter_test || cache_two_tiers ? 0u
        : (cold_components ? 2u : (cost || sequence_test ? cost_prefix_frames : addon::kContractDebounceFrames + 32)));
    auto limited_prefix_deadline = std::chrono::steady_clock::now();
    for (uint32_t attempt = 0; attempt < prefix_limit; ++attempt) {
      if (limited_sequence && attempt != 0) {
        limited_prefix_deadline = std::max(limited_prefix_deadline + std::chrono::microseconds(16667),
            std::chrono::steady_clock::now());
        std::this_thread::sleep_until(limited_prefix_deadline);
      }
      native_runtime_masks.clear();
      native_runtime_results.clear();
      native_runtime_resets.clear();
      if (selected_sequence) {
        native_resize_models.clear();
        native_resize_preparing.clear();
        native_resize_evaluate_cpu_ms.clear();
        native_resize_parameter_snapshots.clear();
        native_resize_create_calls.clear();
        native_resize_create_ms = native_resize_release_ms = native_resize_host_ms = native_resize_evaluate_ms = 0.;
        native_resize_creates = native_resize_releases = 0;
        resize_report["selected_collection"]["last_recording"] = {{"stage", "upper-prefix"}, {"frame", attempt},
            {"process_local_before_record", sample_selected_memory().CurrentUsage}};
      }
      gpu.list->ClearState(nullptr);
      std::shared_ptr<addon::AsyncAutoscalePreparation> limited_pending_task;
      size_t limited_pending_target = native_limited_slots.size();
      if (limited_async_pso) {
        addon::StartAsyncAutoscalePreparation();
        limited_pending_task = addon::async_autoscale_task;
        if (limited_pending_task != nullptr) {
          for (size_t target = 0; target < native_limited_slots.size(); ++target) {
            if (limited_pending_task->target_identity != native_limited_slots[target]) continue;
            Check(limited_pending_task->bytecode.data() == native_limited_bytecodes[target].data()
                && limited_pending_task->bytecode.size() == native_limited_bytecodes[target].size()
                && (limited_pso_probe.tasks[target] == nullptr || limited_pso_probe.tasks[target] == limited_pending_task),
                "private native observation binds the actual task to its fixed shader and target slot");
            limited_pending_target = target;
            limited_pso_probe.tasks[target] = limited_pending_task;
          }
          Check(limited_pending_target < native_limited_slots.size(), "private native task has a recognized target");
        }
      }
      if (cost || sequence_test) {
        if (cost) {
          restore_cost_inputs();
        }
        parameters.Set(NVSDK_NGX_Parameter_Reset, (cache_test || attempt == 0 ? 1 : 0));
      }
      addon::ngx_slot_real[0].evaluate = (motion_test ? reinterpret_cast<void*>(ObserveNativeHostEvaluate)
                                                   : reinterpret_cast<void*>(SuccessfulHostEvaluate));
      const auto prefix_start = std::chrono::steady_clock::now();
      NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
      {
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
        const addon::ColdCpuTraceScope source_trace("prefix.source_call", nullptr,
            (cold_components || pipeline_steps) ? &cold_cpu_traces[attempt] : nullptr);
#endif
        result = (xefg_cache_test
            ? ObserveNativeHostEvaluate(gpu.list.Get(), owner, &parameters, nullptr)
            : addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr));
        if (xefg_cache_test) {
          record_xefg();
        }
      }
      const auto prefix_recorded = std::chrono::steady_clock::now();
      if (limited_pending_task != nullptr) {
        std::shared_lock lock(limited_pending_task->mutex);
        if (limited_pending_task->started && !limited_pending_task->complete) {
          ++limited_pso_probe.pending_source_returns[limited_pending_target];
          Check(native_resize_creates == 0 && native_runtime_results.empty(),
              "a native source returns while its real private PSO remains incomplete without NGX preparation");
        }
      }
      if (limited_sequence && !automatic_limited) {
        for (size_t target = 0; target < native_limited_targets.size(); ++target) {
          if (limited_pso_probe.accepted[target]
              || (limited_async_pso && limited_pso_probe.tasks[target] == nullptr)) continue;
          auto& observation = native_limited_targets[target];
          std::shared_lock lock(observation.mutex);
          for (const auto& call : observation.calls) {
            if (!call.api_returned || call.pipeline == 0 || FAILED(call.result)
                || call.pipeline != reinterpret_cast<uintptr_t>(*native_limited_slots[target])) continue;
            limited_pso_probe.accepted[target] = true;
            limited_pso_probe.accepted_observed_ns[target] = addon::SteadyNowNs();
            limited_pso_probe.accepted_source_call[target] = attempt;
          }
        }
      }
      addon::ngx_slot_real[0].evaluate = nullptr;
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
      if ((cold_components || pipeline_steps) && cold_cpu_traces[attempt].count == 0) {
        cold_components_failed = true;
      }
      export_cold_cpu_trace(false);
      if (pipeline_steps) {
        Check(!cold_components_failed && resize_report["cold_cpu_trace"]["frames"].back().at("pso_create_api_calls").get<uint32_t>() <= 1,
            "each pipeline-prefix source has a complete bounded trace and at most one actual PSO API call");
        if (resize_report["cold_cpu_trace"]["frames"].back().at("pso_create_api_calls").get<uint32_t>() != 0) {
          Check(native_resize_creates == 0 && native_runtime_results.empty(),
              "a synchronous pipeline step never shares its source with NGX Create or NR Evaluate");
        }
      }
#endif
      if (selected_sequence) {
        auto& selected = resize_report["selected_collection"];
        const double wrapper_ms = std::chrono::duration<double, std::milli>(prefix_recorded - prefix_start).count();
        const double interval_ms = (attempt == 0 ? 0.
            : std::chrono::duration<double, std::milli>(prefix_start - selected_previous_start).count());
        if (attempt == 0) {
          selected_prefix_begin = prefix_start;
        }
        selected_previous_start = prefix_start;
        selected_max_wrapper_ms = std::max(selected_max_wrapper_ms, wrapper_ms);
        selected_max_source_interval_ms = std::max(selected_max_source_interval_ms, interval_ms);
        selected["last_recording"].update({{"cpu_wrapper_ms", wrapper_ms}, {"raw_start_interval_ms", interval_ms},
            {"cpu_create_count", native_resize_creates}, {"cpu_create_ms", native_resize_create_ms},
            {"cpu_release_count", native_resize_releases}, {"cpu_release_ms", native_resize_release_ms},
            {"cpu_host_sr_ms", native_resize_host_ms}, {"cpu_nr_evaluate_ms", native_resize_evaluate_ms},
            {"nr_results", native_runtime_results}, {"create_calls", native_resize_create_calls},
            {"core_parameters", native_resize_parameter_snapshots},
            {"process_local_after_record", sample_selected_memory().CurrentUsage}});
        if (attempt != 0 && !native_runtime_results.empty()) {
          auto& previous_frame = selected["prefix_frames"].back();
          if (previous_frame.at("cpu_create_count").get<uint32_t>() != 0) {
            previous_frame["next_successful_source_start_gap_ms"] = interval_ms;
          }
        }
        const bool prefix_ok = std::all_of(native_runtime_results.begin(), native_runtime_results.end(),
                  [](const auto value) { return NVSDK_NGX_SUCCEED(value); })
                  && std::all_of(native_resize_create_calls.begin(), native_resize_create_calls.end(),
                      [](const auto& call) { return NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(call.at("result").template get<uint32_t>())); })
                  && !addon::features.at(owner).slots[0].failed;
        if (pipeline_steps) {
          const auto& feature = addon::features.at(owner);
          if (feature.adaptive_session != nullptr) {
            selected["last_recording"]["reason"] = renodx::addons::dlss5::adaptive_resolution::ReasonText(feature.adaptive_session->status.reason);
            if (feature.adaptive_session->preparation_reason == renodx::addons::dlss5::adaptive_resolution::Reason::BUDGET) {
              resize_report["probe_execution"] = "Blocked";
            }
            Check(!feature.adaptive_session->failed
                && feature.adaptive_session->preparation_reason != renodx::addons::dlss5::adaptive_resolution::Reason::BUDGET,
                "pipeline prefix stops on the first preparation failure or real budget rejection");
          }
          selected["last_recording"]["pso_create_api_calls"] = resize_report["cold_cpu_trace"]["frames"].back().at("pso_create_api_calls");
        }
        if (cold_components) {
          cold_components_failed |= !prefix_ok
              || (attempt == 0 && (native_resize_creates != 1 || !native_runtime_results.empty()));
        } else {
          Check(prefix_ok, "selected upper initialization stops on the first real Feature or Evaluate failure");
        }
        for (const auto* key : {"cpu_wrapper_ms", "cpu_create_ms", "cpu_release_ms", "cpu_host_sr_ms", "cpu_nr_evaluate_ms"}) {
          selected[std::string("prefix_") + key] = selected.at(std::string("prefix_") + key).get<double>()
              + selected.at("last_recording").at(key).get<double>();
        }
        selected["prefix_create_count"] = selected.at("prefix_create_count").get<uint32_t>() + native_resize_creates;
        selected["prefix_release_count"] = selected.at("prefix_release_count").get<uint32_t>() + native_resize_releases;
      }
      ++prefix_host_evaluates;
      prefix_nr_evaluates += native_runtime_results.size();
      if (cold_components) {
        cold_components_failed |= NVSDK_NGX_FAILED(result);
      } else {
        Check(NVSDK_NGX_SUCCEED(result), "host SR succeeds around production Render and Upscaled hooks");
      }
      if (selected_sequence) {
        const auto submit_begin = std::chrono::steady_clock::now();
        submit_resize();
        const auto gpu_completed = std::chrono::steady_clock::now();
        {
          std::scoped_lock runtime_lock(addon::runtime_mutex);
          addon::DrainRetiredResources(true);
          renodx::addons::dlss5::gpu_timers::Poll(true, false);
          gpu.uploads.clear();
          gpu.retained_resources.clear();
        }
        if (cold_components || pipeline_steps) {
          resize_report["selected_collection"]["last_recording"].update({
              {"record_to_submit_ms", std::chrono::duration<double, std::milli>(submit_begin - prefix_recorded).count()},
              {"cpu_submit_fence_ms", std::chrono::duration<double, std::milli>(gpu_completed - submit_begin).count()},
              {"cpu_retire_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gpu_completed).count()}});
        }
      } else {
        gpu.Submit();
        addon::DrainRetiredResources(true);
      }
      if (selected_sequence) {
        selected_prefix_completed = std::chrono::steady_clock::now();
        auto& selected = resize_report["selected_collection"];
        selected["prefix_wall_ms"] = std::chrono::duration<double, std::milli>(selected_prefix_completed - selected_prefix_begin).count();
        selected["last_recording"].update({{"process_local_after_drain", sample_selected_memory().CurrentUsage},
            {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()}});
        selected["prefix_frames"].push_back(selected.at("last_recording"));
      }
      if (cold_components) {
        if (attempt == 1) {
          const auto& feature = addon::features.at(owner);
          const auto workset = addon::worksets.find({owner, output.Get(), false});
          production_chain_completed = feature.slots[0].via_core && !feature.slots[0].failed
              && native_runtime_results.size() == 1 && native_runtime_results[0] == NVSDK_NGX_Result_Success
              && native_runtime_masks.size() == 1 && workset != addon::worksets.end()
              && native_runtime_masks[0] == workset->second.control_mask;
        }
        if (cold_components_failed || attempt == 1) break;
        continue;
      }
      if (cost || sequence_test) {
        cost_prefix.push_back({{"frame", attempt}, {"host_result", static_cast<uint32_t>(result)},
            {"evaluate_count", native_runtime_results.size()}, {"results", native_runtime_results},
            {"resets", native_runtime_resets}, {"fence", gpu.serial},
            {"fence_completed", gpu.fence->GetCompletedValue()}});
      }
      const size_t expected_evaluates = (single_stage ? 1u : 2u);
      if (native_runtime_masks.size() != expected_evaluates
          || native_runtime_results.size() != expected_evaluates) continue;

      auto& feature = addon::features.at(nr_owner);
      if (single_stage) {
        Check(feature.slots[0].via_core && addon::direct_api.borrowed_core_context == !motion_test,
              "motion uses the production owned backend; legacy toggle/cost use borrowed Core");
        Check(native_runtime_results[0] == NVSDK_NGX_Result_Success
                  && native_runtime_masks[0] == addon::worksets.at({nr_owner, nr_workset_key, render_cache_test}).control_mask,
              "initial native frame binds and evaluates its production mask");
        production_chain_completed = true;
        if (cost || (sequence_test && !render_cache_test)) continue;
        break;
      }
      Check(feature.render_session != nullptr
                && feature.render_session->slots[0].handle != nullptr
                && feature.slots[0].handle != nullptr,
            "real Runtime creates both production Feature 18 stage handles");
      const auto& render_workset = addon::worksets.at({owner, color.Get(), true});
      const auto& upscaled_workset = addon::worksets.at({owner, output.Get(), false});
      Check(native_runtime_results.size() == 2
                && NVSDK_NGX_SUCCEED(native_runtime_results[0])
                && NVSDK_NGX_SUCCEED(native_runtime_results[1]),
            "real Runtime Evaluate returns success for both production stages");
      if (mask_enabled) {
        Check(native_runtime_masks[0] == render_workset.control_mask
                  && native_runtime_masks[1] == upscaled_workset.control_mask
                  && native_runtime_masks[0] != nullptr && native_runtime_masks[1] != nullptr,
              "real Runtime receives each production-generated stage mask");
      } else {
        Check(native_runtime_masks[0] == nullptr && native_runtime_masks[1] == nullptr,
              "mask-off baseline leaves both NGX ControlMask bindings empty");
      }
      Check(gpu.fence->GetCompletedValue() >= gpu.serial
                && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
            "both native Feature 18 stages complete on the GPU fence");
      production_chain_completed = true;
      break;
    }
    if (!cold_components && !jitter_test && !cache_two_tiers) {
      Check(production_chain_completed,
            "real Runtime completes the requested production stages within the debounce window");
    }
    if (selected_sequence) {
      resize_report["selected_collection"]["prefix_wall_ms"] = std::chrono::duration<double, std::milli>(
          selected_prefix_completed - selected_prefix_begin).count();
      if (!cold_components && !pipeline_steps) {
        Check(resize_report["selected_collection"].at("prefix_create_count").get<uint32_t>() == 1
                  && resize_report["selected_collection"].at("prefix_release_count").get<uint32_t>() == 0,
            "selected preparation accounts for exactly one real initial upper Feature");
      }
      if (limited_sequence && !automatic_limited) {
        Check(limited_pso_probe.Finish(), "native private PSO observers detach after bounded worker cleanup");
        for (size_t target = 0; target < native_limited_targets.size(); ++target) {
          auto& observation = native_limited_targets[target];
          std::shared_lock lock(observation.mutex);
          Check(observation.calls.size() == 1 && observation.calls[0].api_returned
              && SUCCEEDED(observation.calls[0].result)
              && limited_pso_probe.accepted[target],
              "each fixed native PSO target is created once and its exact API result is accepted into its own slot");
          if (limited_async_pso) {
            Check(observation.calls[0].worker_id != native_limited_main_thread_id && observation.calls[0].worker_exited
                && limited_pso_probe.tasks[target] != nullptr,
                "each asynchronous native PSO target has its actual task and a real worker exit proof");
          } else {
            Check(observation.calls[0].worker_id == native_limited_main_thread_id
                && observation.calls[0].worker == nullptr && limited_pso_probe.tasks[target] == nullptr,
                "the synchronous native PSO baseline has no task or worker and requires no main-thread exit wait");
          }
        }
        resize_report["selected_collection"]["limited_residency"]["async_pso"] = limited_pso_probe.Snapshot();
      }
    }
    if (cold_components) {
      cold_components_failed |= !production_chain_completed || prefix_host_evaluates != 2 || prefix_nr_evaluates != 1;
      resize_report["cold_components"].update({{"measured_source_calls", prefix_host_evaluates},
          {"actual_nr_evaluates", prefix_nr_evaluates}, {"complete_native_chain", production_chain_completed},
          {"stop_reason", (cold_components_failed ? "incomplete or failed two-source trace/native contract; no retry" : "two measured source calls complete")}});
      resize_report["mode"] = "cold-two-source-components";
      resize_report["probe_execution"] = (cold_components_failed ? "Failed" : "Passed");
      addon::core_create_feature = native_resize_create;
      addon::core_release_feature = native_resize_release;
      native_resize_ready = false;
      native_resize_selected_sequence = false;
      native_resize_create_calls.clear();
      native_resize_evaluate_cpu_ms.clear();
      native_resize_parameter_snapshots.clear();
    }
    constexpr std::array<std::array<float, 2>, 4> jitter_phases{{
        {0.375f, 0.25f}, {-0.375f, 0.25f}, {0.375f, -0.25f}, {-0.375f, -0.25f}}};
    const auto save_packed_plane = [&](const std::filesystem::path& directory, const std::string& filename,
                                       const auto& data, const char* format, const char* encoding,
                                       UINT plane_width, UINT plane_height) {
      const auto byte_count = data.size() * sizeof(data[0]);
      const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
      std::ofstream file(directory / filename, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(byte_count));
      file.close();
      Check(!file.fail(), "native sequence packed plane saved");
      return nlohmann::json{{"file", filename}, {"format", format}, {"encoding", encoding},
          {"width", plane_width}, {"height", plane_height}, {"bytes", byte_count},
          {"crc32", renodx::utils::hash::ComputeCRC32(bytes, byte_count)}};
    };
    const auto runtime_identity = [&] {
      ComPtr<IDXGIAdapter1> adapter;
      Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))),
            "native sequence actual device adapter");
      DXGI_ADAPTER_DESC1 adapter_desc{};
      Check(SUCCEEDED(adapter->GetDesc1(&adapter_desc)), "native sequence adapter identity");
      LARGE_INTEGER driver_version{};
      const auto driver_status = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver_version);
      nlohmann::json identity{{"adapter", {{"vendor_id", adapter_desc.VendorId}, {"device_id", adapter_desc.DeviceId},
          {"luid_low", adapter_desc.AdapterLuid.LowPart}, {"luid_high", adapter_desc.AdapterLuid.HighPart},
          {"driver_version_raw", driver_version.QuadPart}, {"driver_status", driver_status}}},
          {"loaded_ngx_modules", nlohmann::json::array()}};
      std::array<HMODULE, 1024> modules{};
      DWORD module_bytes = 0;
      Check(EnumProcessModules(GetCurrentProcess(), modules.data(), sizeof(modules), &module_bytes)
                && module_bytes <= sizeof(modules), "native sequence loaded module inventory");
      for (size_t index = 0; index < module_bytes / sizeof(HMODULE); ++index) {
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(modules[index], path.data(), static_cast<DWORD>(path.size()));
        Check(length != 0 && length < path.size(), "native sequence loaded module path");
        const std::filesystem::path module_path(path.data());
        if (module_path.filename().wstring().find(L"ngx") == std::wstring::npos) continue;
        identity["loaded_ngx_modules"].push_back({{"path", module_path.string()},
            {"sha256", addon::ComputeRuntimeSha256(module_path)},
            {"version", addon::ReadNrRuntimeRequirements(module_path, gpu.device.Get()).file_version}});
      }
      return identity;
    };
    nlohmann::json motion_report;
    nlohmann::json history_report;
    nlohmann::json jitter_report;
    nlohmann::json render_report;
    std::vector<uint8_t> render_reset_output;
    if (render_cache_test) {
      const auto* source_bytes = reinterpret_cast<const uint8_t*>(color_data.data());
      const auto source_size = color_data.size() * sizeof(uint16_t);
      const auto source_crc = renodx::utils::hash::ComputeCRC32(source_bytes, source_size);
      auto stand_in_readback = gpu.CreateTextureTransfer(color.Get());
      auto sr_readback = gpu.CreateTextureTransfer(output.Get());
      render_report = runtime_identity();
      render_report.update({{"evidence", "NATIVE_GPU+NATIVE_NGX"}, {"stage", "Render"},
          {"requested_model", "B"}, {"internal_model", "unknown"}, {"format", "RGBA16F scRGB"},
          {"render_size", {render_width, render_height}}, {"output_size", {width, height}},
          {"jitter", {0.f, 0.f}}, {"motion", "zero non-jittered MVLowRes; scale 1,1"},
          {"source_crc32", source_crc}, {"prefix_frames", cost_prefix}, {"frames", nlohmann::json::array()}});

      // Frame 3 resets a fresh anchor that would otherwise be eligible for reuse.
      constexpr std::array resets{1, 0, 0, 1, 0};
      constexpr std::array expected_refresh{true, false, true, true, false};
      for (size_t frame = 0; frame < resets.size(); ++frame) {
        parameters.Set(NVSDK_NGX_Parameter_Reset, resets[frame]);
        const auto refreshes = addon::edit_reuse_refreshes.load();
        const auto reuses = addon::edit_reuse_reuses.load();
        native_motion_sr_calls = 0;
        native_runtime_masks.clear();
        native_runtime_results.clear();
        native_runtime_resets.clear();
        NativeRenderSrInput input{.original = color.Get(), .readback = &stand_in_readback};
        native_render_sr_input = &input;
        native_motion_sr_readback = &sr_readback;
        addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(ObserveNativeHostEvaluate);
        const auto sr_result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
        addon::ngx_slot_real[0].evaluate = nullptr;
        native_motion_sr_readback = nullptr;
        native_render_sr_input = nullptr;
        const bool color_restored = addon::GetD3D12Resource(&parameters, NVSDK_NGX_Parameter_Color) == color.Get();
        gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(color.Get(), color_transfer);
        gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gpu.Submit();
        addon::DrainRetiredResources(true);

        const auto completed = gpu.fence->GetCompletedValue();
        const auto device_status = gpu.device->GetDeviceRemovedReason();
        nlohmann::json sample{{"frame", frame}, {"source_reset", resets[frame]}, {"sr_calls", native_motion_sr_calls},
            {"sr_result", static_cast<uint32_t>(sr_result)}, {"sr_bound_stand_in", input.bound_stand_in},
            {"color_restored", color_restored}, {"nr_results", native_runtime_results}, {"nr_resets", native_runtime_resets},
            {"refreshes", addon::edit_reuse_refreshes.load() - refreshes}, {"reuses", addon::edit_reuse_reuses.load() - reuses},
            {"fence", gpu.serial}, {"completed_fence", completed}, {"device_status", device_status}};
        std::cout << "Render x2 " << sample.dump() << std::endl;
        Check(completed >= gpu.serial && completed != UINT64_MAX && SUCCEEDED(device_status),
              "Render x2 SR/NR recording completes on a healthy GPU");
        Check(NVSDK_NGX_SUCCEED(sr_result) && native_motion_sr_calls == 1 && input.bound_stand_in && color_restored,
              "Render x2 calls real SR once with this workset's stand-in and restores the original Color");
        const auto original = gpu.ReadTexture(color_transfer);
        Check(original.size() == source_size && std::memcmp(original.data(), source_bytes, source_size) == 0,
              "Render x2 preserves the original Color bytes");
        const auto stand_in = gpu.ReadTexture(stand_in_readback);
        const auto sr_output = gpu.ReadTexture(sr_readback);
        sample["stand_in_crc32"] = renodx::utils::hash::ComputeCRC32(stand_in.data(), stand_in.size());
        sample["sr_output_crc32"] = renodx::utils::hash::ComputeCRC32(sr_output.data(), sr_output.size());
        Check(expected_refresh[frame]
                  ? (native_runtime_results.size() == 1 && NVSDK_NGX_SUCCEED(native_runtime_results.front())
                     && sample["refreshes"] == 1 && sample["reuses"] == 0)
                  : (native_runtime_results.empty() && sample["refreshes"] == 0 && sample["reuses"] == 1),
              "Render x2 alternates real NR and reuse, with a fresh NR Evaluate on Reset");
        if (resets[frame] != 0) {
          Check(native_runtime_resets.size() == 1 && native_runtime_resets.front() == 1,
                "Render x2 forwards source Reset to the real NR Evaluate");
          render_reset_output = sr_output;
          render_report["reset_stand_in_differs_from_original"] = stand_in != original;
        }
        render_report["frames"].push_back(std::move(sample));
      }
    }
    constexpr uint32_t cache_cost_warmup_frames = 32;
    constexpr uint32_t cache_cost_sample_frames = 128;
    if (jitter_test) {
      const std::filesystem::path directory("jitter-calibration");
      Check(std::filesystem::create_directory(directory), "jitter calibration uses a new isolated directory");
      constexpr std::array<float, 2> edge{213.25f, 121.75f};
      jitter_report = {{"schema", "nr-nexus-sr-jitter-calibration-v1"}, {"process_id", GetCurrentProcessId()},
          {"host_sr_handle", reinterpret_cast<uintptr_t>(owner)}, {"evidence", "NATIVE_GPU+NATIVE_NGX"},
          {"dimensions", {{"render", {render_width, render_height}}, {"output", {width, height}}}},
          {"edge_render", edge}, {"runtime", addon::direct_runtime_requirements.file_version},
          {"nr_sha256", addon::direct_runtime_sha256}, {"driver_x100", addon::direct_runtime_requirements.driver_x100},
          {"provider_init", "public SDK ProjectID; real SR only; no NR Feature Evaluate"},
          {"sampling", "unit box coverage of R vertical/G horizontal steps at pixelCenter+raster_sign*jitter"},
          {"jitter_units", "render pixels"}, {"mv_jittered", false}, {"motion_scale", {1, 1}},
          {"has_jitter", true}, {"create_flags", addon::features.at(owner).create_flags},
          {"samples", nlohmann::json::array()}};
      std::fill(depth_data.begin(), depth_data.end(), 0.5f);
      std::fill(motion_data.begin(), motion_data.end(), uint16_t{0});
      auto calibration_depth = gpu.CreateTextureTransfer(depth.Get());
      gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      gpu.UploadTexture(depth.Get(), calibration_depth, reinterpret_cast<const uint8_t*>(depth_data.data()),
                        depth_data.size() * sizeof(float), render_width * sizeof(float));
      gpu.Transition(depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      jitter_report["inputs"] = {
          {"depth", save_packed_plane(directory, "depth.r32f", depth_data, "R32F", "constant normal depth", render_width, render_height)},
          {"motion", save_packed_plane(directory, "motion.rg16f", motion_data, "RG16F", "zero physical motion; no jitter term", render_width, render_height)}};
      for (uint32_t sample = 0; sample < 9; ++sample) {
        const int raster_sign = (sample == 0 ? 0 : (sample <= 4 ? -1 : 1));
        const std::array<float, 2> jitter = (sample == 0 ? std::array<float, 2>{0.f, 0.f} : jitter_phases[(sample - 1) % 4]);
        for (UINT y = 0; y < render_height; ++y) {
          for (UINT x = 0; x < render_width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * render_width + x;
            const float sample_x = static_cast<float>(x) + 0.5f + raster_sign * jitter[0];
            const float sample_y = static_cast<float>(y) + 0.5f + raster_sign * jitter[1];
            const float red = 0.1f + 0.65f * std::clamp(sample_x + 0.5f - edge[0], 0.f, 1.f);
            const float green = 0.15f + 0.6f * std::clamp(sample_y + 0.5f - edge[1], 0.f, 1.f);
            color_data[pixel * 4] = DirectX::PackedVector::XMConvertFloatToHalf(red);
            color_data[pixel * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(green);
            color_data[pixel * 4 + 2] = DirectX::PackedVector::XMConvertFloatToHalf(0.25f);
            color_data[pixel * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
          }
        }
        auto calibration_color = gpu.CreateTextureTransfer(color.Get());
        gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.UploadTexture(color.Get(), calibration_color, reinterpret_cast<const uint8_t*>(color_data.data()),
                          color_data.size() * sizeof(uint16_t), render_width * 8);
        gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        parameters.Set(NVSDK_NGX_Parameter_Reset, 1);
        parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitter[0]);
        parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitter[1]);
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
        Check(addon::GetFloat(&parameters, NVSDK_NGX_Parameter_Jitter_Offset_X, NAN) == jitter[0]
                  && addon::GetFloat(&parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, NAN) == jitter[1],
              "jitter calibration SDK parameters contain the exact two-axis offsets");
        native_motion_sr_calls = 0;
        native_runtime_results.clear();
        auto sr_readback = gpu.CreateTextureTransfer(output.Get());
        native_motion_sr_readback = &sr_readback;
        addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(ObserveNativeHostEvaluate);
        const auto result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
        addon::ngx_slot_real[0].evaluate = nullptr;
        native_motion_sr_readback = nullptr;
        Check(NVSDK_NGX_SUCCEED(result) && native_motion_sr_calls == 1 && native_runtime_results.empty(),
              "each jitter calibration sample executes exactly one real SR and zero NR");
        gpu.Submit();
        Check(gpu.fence->GetCompletedValue() >= gpu.serial && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
              "jitter calibration SR completes on a healthy GPU");
        const std::string stem = "sample-" + std::to_string(sample);
        jitter_report["samples"].push_back({{"index", sample}, {"raster_sign", raster_sign}, {"jitter", jitter},
            {"sr", {{"calls", native_motion_sr_calls}, {"result", static_cast<uint32_t>(result)},
                {"reset", addon::GetInt(&parameters, NVSDK_NGX_Parameter_Reset)}, {"nr_calls", 0}}},
            {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()},
            {"inputs", {{"color", save_packed_plane(directory, stem + "-input-color.rgba16f", color_data,
                "RGBA16F", "linear HDR; analytic box-covered steps", render_width, render_height)}}},
            {"planes", {{"sr", save_packed_plane(directory, stem + "-sr.rgba16f", gpu.ReadTexture(sr_readback),
                "RGBA16F", "linear HDR", width, height)}}}});
        std::ofstream file(directory / "sr-jitter-calibration.json", std::ios::binary | std::ios::trunc);
        file << jitter_report.dump(2) << '\n';
        file.close();
        Check(!file.fail(), "jitter calibration partial evidence saved");
      }
      jitter_report.update(runtime_identity());
      jitter_report["sequence_host_sr_evaluates"] = 9;
      jitter_report["sequence_nr_evaluates"] = 0;
    }
    if (sequence_test && !cache_cost && !render_cache_test) {
      const auto history_look = addon::LookSettingsSnapshot(true);
      Check(addon::detail_stability.load() == 1u && history_look.stabilize == 0u
                && !history_look.stabilize_detail && history_look.stabilize_alpha == 0.f,
            "history fixture disables temporal Look in the effective after-path snapshot");
      const char* requested_condition = std::getenv(cache_test
          ? "NR_NEXUS_TEST_NR_CACHE_CONDITION" : "NR_NEXUS_TEST_NR_HISTORY_CONDITION");
      const std::string condition = (requested_condition != nullptr ? requested_condition : "baseline");
      Check((cache_test ? condition == "baseline" || condition == "cache"
                       : condition == "baseline" || condition == "baseline-repeat"
                || condition == "x2-compose" || condition == "x2-reset"), "known NR sequence condition");
      const bool sparse = condition == "x2-compose" || condition == "x2-reset";
      const char* scene_environment = (cache_test ? std::getenv("NR_NEXUS_TEST_NR_CACHE_SCENE") : nullptr);
      const std::string scene_id = (scene_environment != nullptr ? scene_environment : "default");
      Check(scene_id == "default" || (cache_test && scene_id == "jitter-lighting"), "known NR cache scene");
      const bool jitter_scene = scene_id == "jitter-lighting";
      int raster_sign = 0;
      if (jitter_scene) {
        const char* sign_environment = std::getenv("NR_NEXUS_TEST_NR_RASTER_SIGN");
        Check(sign_environment != nullptr && (std::strcmp(sign_environment, "-1") == 0
                  || std::strcmp(sign_environment, "1") == 0 || std::strcmp(sign_environment, "+1") == 0),
              "jitter scene requires the calibrated NR_NEXUS_TEST_NR_RASTER_SIGN=-1 or +1");
        raster_sign = (std::strcmp(sign_environment, "-1") == 0 ? -1 : 1);
      }
      const std::filesystem::path raw_directory(cache_test && condition == "baseline" ? "cache-baseline" : condition);
      Check(std::filesystem::create_directory(raw_directory), "history output is a new isolated condition directory");
      constexpr uint32_t frame_count = 24;
      // Render-grid pixels. A nonconstant background velocity and separately
      // moving foreground make composition observably different from 2*d1.
      constexpr std::array<int, frame_count> camera_delta{
          0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 2, 3, 4, 2, 3, 4, 0, 0, 0, 0, 0, 0};
      constexpr std::array<float, 6> light{1.f, 1.15f, 1.3f, 1.1f, 0.85f, 1.f};
      constexpr std::array<float, 6> jitter_light{1.f, 1.15f, 1.15f, 1.15f, 0.85f, 0.85f};
      auto history_motion = gpu.Texture(render_width, render_height, DXGI_FORMAT_R16G16_FLOAT);
      gpu.Transition(history_motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      const UINT intervals_per_frame = (cache_test ? 3u : 2u);
      auto queries = d3d12_test::CreateTimestampQueries(gpu, frame_count * intervals_per_frame);
      std::vector<uint16_t> nr_motion_data(render_pixel_count * 2);
      std::vector<uint16_t> previous_motion(render_pixel_count * 2, 0);
      std::vector<uint8_t> surface(render_pixel_count, 0), previous_surface(render_pixel_count, 0);
      std::vector<uint8_t> validity(render_pixel_count, 0), previous_validity(render_pixel_count, 0);
      auto& report = history_report;
      report = {{"schema", (cache_test ? "nr-nexus-nr-edit-reuse-cache-v1" : "nr-nexus-nr-edit-reuse-history-v1")}, {"condition", condition},
          {"scene_id", scene_id}, {"raster_sign", raster_sign},
          {"process_id", GetCurrentProcessId()}, {"host_sr_handle", reinterpret_cast<uintptr_t>(owner)},
          {"evidence", "NATIVE_GPU+NATIVE_NGX"}, {"stage", (xefg_cache_test ? "D3D12 XeFG Record" : "D3D12 Upscaled")}, {"passes", 1},
          {"dimensions", {{"render", {render_width, render_height}}, {"output", {width, height}}}},
          {"provider_init", "public SDK ProjectID; owned production NR; independent process/Features per condition"},
          {"requested_model", "B"}, {"internal_model", "unknown"},
          {"runtime", addon::direct_runtime_requirements.file_version}, {"nr_sha256", addon::direct_runtime_sha256},
          {"driver_x100", addon::direct_runtime_requirements.driver_x100},
          {"prefix", cost_prefix}, {"prefix_host_sr_evaluates", prefix_host_evaluates},
          {"prefix_nr_evaluates", prefix_nr_evaluates}, {"jitter", (jitter_scene ? nlohmann::json("per-frame") : nlohmann::json({0, 0}))}, {"has_jitter", true},
          {"mv_jittered", false}, {"motion_scale", {1, 1}}, {"depth_convention", "normal; near 0 far 1"},
          {"motion_convention", (jitter_scene ? "physical current-to-previous render pixels; no jitter in MV; guides raster sampled at pixelCenter+raster_sign*jitter"
                                               : "current to previous render-grid pixels; integer sample positions; zero jitter")},
          {"composition", "d(t,t-2,x)=d(t,t-1,x)+d(t-1,t-2,x+d(t,t-1,x)); outside previous grid uses zero second step and valid bit1=0"},
          {"validity", "bit0: single-step same surface/in bounds; bit1: both steps valid"},
          {"feed_mode", addon::feed_mode.load()}, {"norm_governor", addon::norm_governor.load()},
          {"detail_stability", addon::detail_stability.load()}, {"timestamp_frequency_hz", queries.frequency_hz},
          {"look_snapshot", {{"stabilize", history_look.stabilize}, {"stabilize_detail", history_look.stabilize_detail},
              {"stabilize_alpha", history_look.stabilize_alpha}}},
          {"timing_scope", (cache_test
              ? (xefg_cache_test
                  ? "SR/model Evaluate and whole NR chain after real SR snapshot through Record, staging and private writeback; diagnostic readbacks excluded"
                  : "SR/model Evaluate and whole NR chain after real SR snapshot through wrapper return; final/validity readbacks excluded")
              : "individual real SR and NR Evaluate GPU intervals, excluding readback copies; diagnostic only")},
          {"raw_policy", "condition directory registered disposable by test entry; only KEEP_RAW=1 preserves complete dataset"},
          {"frames", nlohmann::json::array()}};
      if (cache_test) {
        report["route"] = cache_route;
        report["sr_format"] = "RGBA16F";
        report["sr_encoding"] = "linear BT709 scRGB; 1=80 nits";
        report["source_format"] = (pq_cache_test ? "R10G10B10A2_UNORM" : "RGBA16F");
        report["source_dxgi_format"] = static_cast<uint32_t>(cache_color->GetDesc().Format);
        report["source_encoding"] = (pq_cache_test ? "pq_bt2020_10000nits" : "linear BT709 scRGB; 1=80 nits");
        report["source_construction"] = (pq_cache_test
            ? "real per-frame SR FP16 readback -> CPU BT709 to BT2020, scale 80/10000, ST2084, round UNORM10 RGB/UNORM2 alpha -> upload; one extra completed SR submission per frame; conversion/readback/upload excluded from NR whole-chain GPU timestamps"
            : "unmodified real per-frame SR FP16 output");
        if (pq_cache_test) {
          report["timing_scope"] = "real SR/model Evaluate separately; whole NR chain starts after fixture PQ upload through production Record, staging and private writeback; CPU conversion, readbacks and PQ upload excluded";
        }
        report["xefg_scope"] = (xefg_cache_test
            ? "real SR and NR; production fg_service::Record plus private-color writeback; SDK/Service admission not exercised"
            : "not used");
        report["present_white_nits"] = (xefg_cache_test ? nlohmann::json(200) : nlohmann::json(nullptr));
        report["prefix_edit_reuse_enabled"] = true;
        report["prefix_force_reset"] = true;
        report["edit_reuse_enabled"] = condition == "cache";
        report["validity"] = "output plane uint32: 0 exact current NR source, 1 cached edit; fixture input valid remains two-step surface bits";
        if (jitter_scene) {
          report["raster_sampling"] = "color/depth/MV surface identity share pixelCenter+raster_sign*jitter; foreground edges (240.25,90.75) from frame0";
          report["input_validity_sampling"] = "render raster labels compared at integer physical displacement; output validity is the production uint32 plane";
          report["cache_motion_composition"] = "production raw guide q_prev=q+d1+jitter(t-1)-jitter(t); physical d(t,t-2)=d1+sample(previous motion,q_prev); input nr_motion plane remains the fixture single-step source";
        }
      }
      report.update(runtime_identity());
      int camera_shift = 0;
      uint32_t nr_calls = 0;
      bool previous_frame_reused = false;
      if (cache_test) addon::edit_reuse_enabled = condition == "cache";
      for (uint32_t frame = 0; frame < frame_count; ++frame) {
        const uint64_t refreshes_before = (cache_test ? addon::edit_reuse_refreshes.load() : 0);
        const uint64_t reuses_before = (cache_test ? addon::edit_reuse_reuses.load() : 0);
        const uint64_t fallbacks_before = (cache_test ? addon::edit_reuse_fallbacks.load() : 0);
        const bool refresh = !sparse || frame % 2 == 0;
        const std::array<float, 2> jitter = (jitter_scene ? jitter_phases[frame % jitter_phases.size()]
                                                       : std::array<float, 2>{0.f, 0.f});
        const bool foreground = jitter_scene || frame >= 12;
        const int foreground_left = 240 + (frame >= 12 ? 3 * static_cast<int>(frame - 12) : 0);
        const float foreground_edge_x = foreground_left + (jitter_scene ? 0.25f : 0.f);
        const float foreground_edge_y = (jitter_scene ? 90.75f : 90.f);
        const int foreground_delta = (frame > 12 ? 3 : 0);
        camera_shift += camera_delta[frame];
        const float illumination = (frame >= 18 ? (jitter_scene ? jitter_light[frame - 18] : light[frame - 18]) : 1.f);
        for (UINT y = 0; y < render_height; ++y) {
          for (UINT x = 0; x < render_width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * render_width + x;
            const float sample_x = (jitter_scene ? static_cast<float>(x) + 0.5f + raster_sign * jitter[0] : static_cast<float>(x));
            const float sample_y = (jitter_scene ? static_cast<float>(y) + 0.5f + raster_sign * jitter[1] : static_cast<float>(y));
            surface[pixel] = foreground && sample_x >= foreground_edge_x
                && sample_x < foreground_edge_x + 96.f && sample_y >= foreground_edge_y && sample_y < foreground_edge_y + 180.f;
            const float u = (sample_x - camera_shift) / render_width;
            const float v = sample_y / render_height;
            const std::array<float, 3> rgb = (surface[pixel] != 0
                ? std::array<float, 3>{0.65f, 0.18f + 0.1f * v, 0.12f}
                : std::array<float, 3>{0.35f + 0.2f * std::sin(u * 40.f) + 0.1f * v,
                      0.1f + 0.65f * v, 0.4f + 0.2f * std::cos(u * 23.f + v * 11.f)});
            for (size_t channel = 0; channel < 3; ++channel) {
              color_data[pixel * 4 + channel] = DirectX::PackedVector::XMConvertFloatToHalf(rgb[channel] * illumination);
            }
            color_data[pixel * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
            depth_data[pixel] = (surface[pixel] != 0 ? 0.2f : 0.7f + 0.1f * v);
            motion_data[pixel * 2] = DirectX::PackedVector::XMConvertFloatToHalf(
                static_cast<float>(-(surface[pixel] != 0 ? foreground_delta : camera_delta[frame])));
            motion_data[pixel * 2 + 1] = 0;
          }
        }
        size_t composition_differs_from_double = 0;
        for (UINT y = 0; y < render_height; ++y) {
          for (UINT x = 0; x < render_width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * render_width + x;
            const float d1 = DirectX::PackedVector::XMConvertHalfToFloat(motion_data[pixel * 2]);
            const int previous_x = static_cast<int>(x) + static_cast<int>(d1);
            const bool in_bounds = previous_x >= 0 && previous_x < static_cast<int>(render_width);
            const size_t previous_pixel = (in_bounds ? static_cast<size_t>(y) * render_width + previous_x : 0);
            const bool valid1 = frame > 0 && in_bounds && surface[pixel] == previous_surface[previous_pixel];
            const bool valid2 = valid1 && (previous_validity[previous_pixel] & 1) != 0;
            validity[pixel] = static_cast<uint8_t>((valid1 ? 1 : 0) | (valid2 ? 2 : 0));
            const float d2 = (in_bounds ? DirectX::PackedVector::XMConvertHalfToFloat(previous_motion[previous_pixel * 2]) : 0.f);
            const float composed = d1 + d2;
            if (valid2 && composed != 2.f * d1) {
              ++composition_differs_from_double;
            }
            nr_motion_data[pixel * 2] = DirectX::PackedVector::XMConvertFloatToHalf(
                condition == "x2-compose" && frame > 0 ? composed : d1);
            nr_motion_data[pixel * 2 + 1] = 0;
          }
        }
        auto color_upload = gpu.CreateTextureTransfer(color.Get());
        auto depth_upload = gpu.CreateTextureTransfer(depth.Get());
        auto motion_upload = gpu.CreateTextureTransfer(motion.Get());
        auto nr_motion_upload = gpu.CreateTextureTransfer(history_motion.Get());
        const auto upload = [&](ID3D12Resource* resource, d3d12_test::TextureTransfer* transfer,
                                const auto& data, UINT row_bytes) {
          gpu.Transition(resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
          gpu.UploadTexture(resource, *transfer, reinterpret_cast<const uint8_t*>(data.data()),
              data.size() * sizeof(data[0]), row_bytes);
          gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        };
        upload(color.Get(), &color_upload, color_data, render_width * 8);
        upload(depth.Get(), &depth_upload, depth_data, render_width * 4);
        upload(motion.Get(), &motion_upload, motion_data, render_width * 4);
        upload(history_motion.Get(), &nr_motion_upload, nr_motion_data, render_width * 4);
        parameters.Set(NVSDK_NGX_Parameter_Reset, (frame == 0 ? 1 : 0));
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f);
        parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
        if (jitter_scene) {
          parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitter[0]);
          parameters.Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitter[1]);
        }
        auto sr_readback = gpu.CreateTextureTransfer(output.Get());
        const auto& workset = addon::worksets.at({nr_owner, nr_workset_key, false});
        auto proxy_readback = gpu.CreateTextureTransfer(workset.proxy);
        auto neural_readback = gpu.CreateTextureTransfer(workset.nr_output);
        auto final_readback = gpu.CreateTextureTransfer(cache_color);
        native_motion_sr_readback = &sr_readback;
        native_motion_proxy_readback = (refresh && !cache_test ? &proxy_readback : nullptr);
        native_motion_neural_readback = (refresh && !cache_test ? &neural_readback : nullptr);
        native_motion_sr_calls = 0;
        native_runtime_masks.clear();
        native_runtime_results.clear();
        native_runtime_resets.clear();
        native_history_motion = (refresh && !cache_test ? history_motion.Get() : nullptr);
        native_history_reset = (condition == "x2-reset" ? 1 : -1);
        native_history_parameters = nullptr;
        native_history_queries = &queries;
        native_history_interval = frame * intervals_per_frame;
        native_cache_observe = cache_test;
        NVSDK_NGX_Result result;
        if (refresh && !xefg_cache_test) {
          addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(ObserveNativeHostEvaluate);
          result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
          addon::ngx_slot_real[0].evaluate = nullptr;
        } else {
          // Bypass NR admission without toggling enabled or retiring its history.
          result = ObserveNativeHostEvaluate(gpu.list.Get(), owner, &parameters, nullptr);
        }
        if (cache_test) {
          if (xefg_cache_test) {
            Check(record_xefg(), "XeFG Record completes each measured base frame");
          }
          queries.EndAndResolve(gpu.list.Get(), native_history_interval + 2);
          if (!native_runtime_results.empty()) {
            const UINT query = 2 * (native_history_interval + 1);
            gpu.list->ResolveQueryData(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query, 2,
                                      queries.readback.Get(), static_cast<uint64_t>(query) * sizeof(uint64_t));
          }
        }
        native_cache_observe = false;
        native_history_queries = nullptr;
        native_history_motion = nullptr;
        native_history_reset = -1;
        native_motion_sr_readback = nullptr;
        native_motion_proxy_readback = nullptr;
        native_motion_neural_readback = nullptr;
        Check(NVSDK_NGX_SUCCEED(result) && native_motion_sr_calls == 1
                  && (cache_test ? native_runtime_results.size() <= 1 : native_runtime_results.size() == (refresh ? 1u : 0u)),
              "sequence actual per-frame SR/NR cadence");
        const bool model_executed = !native_runtime_results.empty();
        if (model_executed) {
          Check(NVSDK_NGX_SUCCEED(native_runtime_results[0]) && native_runtime_resets.size() == 1,
                "history real NR Evaluate returns success");
          ++nr_calls;
        }
        ComPtr<ID3D12Resource> validity_readback;
        const auto cache_state = (cache_test ? addon::features.at(nr_owner).slots[0].edit_reuse : nullptr);
        const uint64_t reused = (cache_test ? addon::edit_reuse_reuses.load() - reuses_before : 0);
        const uint64_t refreshed = (cache_test ? addon::edit_reuse_refreshes.load() - refreshes_before : 0);
        const uint64_t fallback = (cache_test ? addon::edit_reuse_fallbacks.load() - fallbacks_before : 0);
        if (cache_test) {
          Check(reused <= 1 && (model_executed || reused == 1), "cache frame performs real NR or exactly one production reuse");
          Check(reused == 0 || !previous_frame_reused, "cache never reuses on consecutive base frames");
          Check(reused == 0 || (cache_state != nullptr && cache_state->reused && !model_executed),
                "cache reuse skips actual NR and borrows the anchor at most once");
          Check(condition != "baseline" || (model_executed && reused == 0), "baseline runs the real NR wrapper on every input");
          previous_frame_reused = reused != 0;
          if (cache_state != nullptr && cache_state->validity != nullptr && reused != 0) {
            const uint64_t validity_bytes = static_cast<uint64_t>(width) * height * sizeof(uint32_t);
            validity_readback = gpu.Buffer(validity_bytes, D3D12_HEAP_TYPE_READBACK);
            gpu.Transition(cache_state->validity.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gpu.list->CopyBufferRegion(validity_readback.Get(), 0, cache_state->validity.Get(), 0, validity_bytes);
            gpu.Transition(cache_state->validity.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          }
        }
        if (refresh || cache_test) {
          gpu.Transition(cache_color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          gpu.RecordTextureReadback(cache_color, final_readback);
          gpu.Transition(cache_color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        gpu.Submit();
        addon::DrainRetiredResources(true);
        const auto& look_history = addon::features.at(nr_owner).slots[0].look_history;
        Check(look_history.buffers[0] == nullptr && look_history.buffers[1] == nullptr,
              "history fixture never allocates temporal Look history");
        Check(gpu.fence->GetCompletedValue() >= gpu.serial && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
              "history frame completed on a healthy GPU");
        const std::string stem = "frame-" + std::to_string(1000 + frame).substr(1);
        const auto save = [&](const char* suffix, const auto& data, const char* format, const char* encoding,
                              UINT plane_width, UINT plane_height) {
          return save_packed_plane(raw_directory, stem + "-" + suffix, data, format, encoding, plane_width, plane_height);
        };
        const auto sr_bytes = gpu.ReadTexture(sr_readback);
        const auto final_bytes = ((refresh || cache_test) ? gpu.ReadTexture(final_readback) : std::vector<uint8_t>{});
        nlohmann::json planes{{"sr", save("sr.rgba16f", sr_bytes, "RGBA16F", "linear HDR", width, height)},
            {"proxy", nullptr}, {"neural", nullptr}, {"final", nullptr}};
        if (refresh && !cache_test) {
          planes["proxy"] = save("proxy.rgba16f", gpu.ReadTexture(proxy_readback), "RGBA16F", "v6 encoded sRGB float", width, height);
          planes["neural"] = save("neural.rgba16f", gpu.ReadTexture(neural_readback), "RGBA16F", "v6 encoded sRGB float", width, height);
        }
        if (refresh || cache_test) {
          planes["final"] = (pq_cache_test
              ? save("final.r10g10b10a2", final_bytes, "R10G10B10A2_UNORM", "pq_bt2020_10000nits", width, height)
              : save("final.rgba16f", final_bytes, "RGBA16F", "linear HDR; production resolve", width, height));
        }
        if (pq_cache_test) {
          planes["source"] = save("source.r10g10b10a2", pq_source_codes, "R10G10B10A2_UNORM", "pq_bt2020_10000nits", width, height);
          Check(final_bytes.size() == pq_source_codes.size() * sizeof(uint32_t), "PQ final matches actual packed source extent");
          for (size_t pixel = 0; pixel < pq_source_codes.size(); ++pixel) {
            uint32_t actual;
            std::memcpy(&actual, final_bytes.data() + pixel * sizeof(uint32_t), sizeof(actual));
            Check((actual >> 30) == (pq_source_codes[pixel] >> 30), "native PQ output preserves actual current-source alpha code");
          }
        }
        if (cache_test) {
          planes["validity"] = nullptr;
          if (validity_readback != nullptr) {
            const D3D12_RANGE range{0, static_cast<SIZE_T>(width) * height * sizeof(uint32_t)};
            uint32_t* mapped = nullptr;
            Check(SUCCEEDED(validity_readback->Map(0, &range, reinterpret_cast<void**>(&mapped))), "cache validity readback map");
            std::vector<uint32_t> values(mapped, mapped + static_cast<size_t>(width) * height);
            const D3D12_RANGE no_writes{0, 0};
            validity_readback->Unmap(0, &no_writes);
            if (pq_cache_test) {
              for (size_t pixel = 0; pixel < values.size(); ++pixel) {
                if (values[pixel] != 0) continue;
                uint32_t actual;
                std::memcpy(&actual, final_bytes.data() + pixel * sizeof(uint32_t), sizeof(actual));
                Check(actual == pq_source_codes[pixel], "native invalid PQ reuse returns exact current packed RGB and alpha");
              }
            }
            planes["validity"] = save("validity.r32u", values, "R32_UINT", "0 selects exact current NR source; 1 reuses anchor edit", width, height);
          }
        }
        const auto sr_timing = queries.Read(frame * intervals_per_frame);
        nlohmann::json nr{{"calls", native_runtime_results.size()}, {"executed", model_executed},
            {"result", nullptr}, {"reset", nullptr}, {"parameters", native_history_parameters},
            {"gpu_ms", nullptr}, {"gpu_ticks", nullptr}};
        if (model_executed) {
          const auto timing = queries.Read(frame * intervals_per_frame + 1);
          nr["result"] = static_cast<uint32_t>(native_runtime_results[0]);
          nr["reset"] = native_runtime_resets[0];
          nr["gpu_ms"] = timing.gpu_ms;
          nr["gpu_ticks"] = {timing.begin_tick, timing.end_tick};
        }
        if (cache_test) {
          const auto timing = queries.Read(frame * intervals_per_frame + 2);
          nr["whole_chain_gpu_ms"] = timing.gpu_ms;
          nr["whole_chain_gpu_ticks"] = {timing.begin_tick, timing.end_tick};
        }
        report["frames"].push_back({{"frame", frame}, {"scene", (frame < 6 ? "stationary" : (frame < 12 ? "translation" : (frame < 18 ? "occlusion" : "lighting")))},
            {"jitter", jitter}, {"raster_sign", raster_sign},
            {"foreground_edge_render", {foreground_edge_x, foreground_edge_y}},
            {"refresh", refresh}, {"camera_shift", camera_shift}, {"camera_delta", camera_delta[frame]},
            {"foreground_left", foreground_left}, {"illumination", illumination},
            {"composition_differs_from_double_valid_pixels", composition_differs_from_double},
            {"inputs", {{"color", save("input-color.rgba16f", color_data, "RGBA16F", "linear HDR", render_width, render_height)},
                {"depth", save("depth.r32f", depth_data, "R32F", "normal depth", render_width, render_height)},
                {"motion", save("motion.rg16f", motion_data, "RG16F", "render pixels current-to-previous", render_width, render_height)},
                {"nr_motion", save("nr-motion.rg16f", nr_motion_data, "RG16F", "render pixels; NR only", render_width, render_height)},
                {"surface", save("surface.r8", surface, "R8_UINT", "0 background 1 foreground", render_width, render_height)},
                {"valid", save("valid.r8", validity, "R8_UINT", "bit0 one-step bit1 two-step", render_width, render_height)}}},
            {"planes", planes}, {"sr", {{"calls", native_motion_sr_calls}, {"result", static_cast<uint32_t>(result)},
                {"reset", frame == 0 ? 1 : 0}, {"handle", reinterpret_cast<uintptr_t>(owner)},
                {"create_flags", addon::features.at(owner).create_flags}, {"has_jitter", true}, {"jitter", jitter},
                {"mv_jittered", false}, {"motion_scale", {1, 1}}, {"pre_exposure", 1.f}, {"exposure_scale", 1.f},
                {"gpu_ms", sr_timing.gpu_ms}, {"gpu_ticks", {sr_timing.begin_tick, sr_timing.end_tick}}}},
            {"nr", nr}, {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()}});
        if (cache_test) {
          auto& current = report["frames"].back();
          current["refresh"] = model_executed;
          current["frame_kind"] = (reused != 0 ? "reuse" : (fallback != 0 ? "fallback" : "refresh"));
          current["reuse"] = {{"refreshes", refreshed}, {"reuses", reused}, {"fallbacks", fallback},
              {"anchor_generation", (cache_state != nullptr ? cache_state->anchor_generation : 0)},
              {"reuse_count", (cache_state != nullptr ? cache_state->reuse_count : 0)},
              {"anchor_valid", cache_state != nullptr && cache_state->valid},
              {"allocated_bytes", (cache_state != nullptr ? cache_state->allocated_bytes : 0)}};
          current["inputs"].erase("nr_motion");
        }
        previous_motion = motion_data;
        previous_surface = surface;
        previous_validity = validity;
        std::ofstream report_file(raw_directory / (cache_test ? "nr-edit-reuse-cache.json" : "nr-edit-reuse-history.json"), std::ios::binary | std::ios::trunc);
        report_file << report.dump(2) << '\n';
        report_file.close();
        Check(!report_file.fail(), "history partial evidence saved after each completed frame");
      }
      report["sequence_host_sr_evaluates"] = frame_count;
      report["sequence_nr_evaluates"] = nr_calls;
      std::cout << "PASS NR " << (cache_test ? "cache" : "history") << " condition=" << condition << " frames=" << frame_count << " SR=" << frame_count
                << " NR=" << nr_calls << "; raw planes=" << raw_directory.string() << '\n';
    }
    if (cache_two_tiers) {
      const std::string condition = cache_cost_report.at("condition").get<std::string>();
      const bool reuse_enabled = condition == "cache";
      constexpr uint32_t total_frames = cache_cost_warmup_frames + cache_cost_sample_frames;
      constexpr uint32_t preparation_limit = addon::kContractDebounceFrames + 64;
      constexpr UINT intervals_per_frame = 3;
      Check(width == 3840 && height == 2160 && render_width == 2560 && render_height == 1440
                && !addon::direct_api.private_instance && !addon::direct_api.borrowed_core_context
                && addon::parameter_runtime_via_core.load() && !addon::control_mask_enabled.load(),
            "two-tier cost uses the ordinary public Core provider and one unmasked native Upscaled source");
      struct Frame {
        uint32_t sr_calls = 0, sr_result = 0, nr_calls = 0, nr_result = 0, active_calls = 0, preparation_calls = 0;
        uint32_t creates = 0, releases = 0, slot = 0;
        int reset = -1;
        uint64_t sequence_before = 0, sequence = 0, source_successes = 0, refreshes = 0, reuses = 0, fallbacks = 0;
        uint64_t model = 0, workset = 0, cache_bytes = 0, fence = 0, completed_fence = 0;
        HRESULT device_status = E_FAIL;
        double wrapper_ms = 0., sr_api_ms = 0., nr_api_ms = 0., nr_chain_record_ms = 0.;
        double submit_complete_ms = 0., source_complete_ms = 0., retire_ms = 0., interval_ms = 0., entry_ms = 0.;
        double create_ms = 0., release_ms = 0.;
      };
      uint32_t sequence_sr_calls = 0, sequence_nr_calls = 0, preparation_nr_calls = 0;
      const auto source_origin = std::chrono::steady_clock::now();
      auto previous_entry = std::chrono::steady_clock::time_point{};
      const auto source_frame = [&](int reset, d3d12_test::TimestampQueries* queries = nullptr, UINT interval = 0) {
        native_motion_sr_calls = 0;
        native_runtime_masks.clear();
        native_runtime_results.clear();
        native_runtime_resets.clear();
        native_resize_models.clear();
        native_resize_preparing.clear();
        native_resize_evaluate_cpu_ms.clear();
        native_resize_parameter_snapshots.clear();
        native_resize_create_calls.clear();
        native_resize_creates = native_resize_releases = 0;
        native_resize_create_ms = native_resize_release_ms = native_resize_host_ms = native_resize_evaluate_ms = 0.;
        native_motion_sr_readback = native_motion_proxy_readback = native_motion_neural_readback = nullptr;
        native_history_motion = nullptr;
        native_history_reset = -1;
        native_resize_fixed_input = native_resize_observe_parameters = false;
        native_history_queries = queries;
        native_history_interval = interval;
        native_cache_observe = true;
        const auto sequence_before = addon::features.at(owner).sr_sequence;
        const auto successes_before = addon::SourceSuccessfulEvaluations();
        const auto refreshes_before = addon::edit_reuse_refreshes.load();
        const auto reuses_before = addon::edit_reuse_reuses.load();
        const auto fallbacks_before = addon::edit_reuse_fallbacks.load();
        parameters.Set(NVSDK_NGX_Parameter_Reset, reset);
        gpu.list->ClearState(nullptr);
        addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(ObserveNativeHostEvaluate);
        const auto entry = std::chrono::steady_clock::now();
        NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
        try {
          result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
        } catch (...) {
          addon::ngx_slot_real[0].evaluate = nullptr;
          native_history_queries = nullptr;
          throw;
        }
        const auto recorded = std::chrono::steady_clock::now();
        addon::ngx_slot_real[0].evaluate = nullptr;
        if (queries != nullptr) {
          Check(native_runtime_results.size() <= 1, "steady source records at most one real NR Evaluate");
          queries->EndAndResolve(gpu.list.Get(), interval + 2);
          if (!native_runtime_results.empty()) {
            const UINT first = 2 * (interval + 1);
            gpu.list->ResolveQueryData(queries->heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2,
                queries->readback.Get(), static_cast<uint64_t>(first) * sizeof(uint64_t));
          }
        }
        native_history_queries = nullptr;
        const auto submit_start = std::chrono::steady_clock::now();
        submit_resize();
        const auto completed = std::chrono::steady_clock::now();
        {
          std::scoped_lock runtime_lock(addon::runtime_mutex);
          addon::DrainRetiredResources(true);
          renodx::addons::dlss5::gpu_timers::Poll(true, false);
          gpu.uploads.clear();
          gpu.retained_resources.clear();
        }
        const auto retired = std::chrono::steady_clock::now();
        auto& feature = addon::features.at(owner);
        const auto tiers = feature.adaptive_cache;
        if (tiers != nullptr) {
          addon::PollAdaptiveCache(tiers.get());
        }
        const auto slot = (tiers != nullptr ? static_cast<uint32_t>(tiers->active) : 0u);
        const auto workset = addon::worksets.find({owner, output.Get(), false, slot});
        const auto& model = feature.slots[0];
        Frame sample{.sr_calls = native_motion_sr_calls, .sr_result = static_cast<uint32_t>(result),
            .nr_calls = static_cast<uint32_t>(native_runtime_results.size()),
            .nr_result = (native_runtime_results.empty() ? 0u : static_cast<uint32_t>(native_runtime_results.front())),
            .active_calls = static_cast<uint32_t>(std::count(native_resize_preparing.begin(), native_resize_preparing.end(), false)),
            .preparation_calls = static_cast<uint32_t>(std::count(native_resize_preparing.begin(), native_resize_preparing.end(), true)),
            .creates = native_resize_creates, .releases = native_resize_releases, .slot = slot,
            .reset = (native_runtime_resets.size() == 1 ? native_runtime_resets.front() : -1),
            .sequence_before = sequence_before, .sequence = feature.sr_sequence,
            .source_successes = addon::SourceSuccessfulEvaluations() - successes_before,
            .refreshes = addon::edit_reuse_refreshes.load() - refreshes_before,
            .reuses = addon::edit_reuse_reuses.load() - reuses_before,
            .fallbacks = addon::edit_reuse_fallbacks.load() - fallbacks_before,
            .model = reinterpret_cast<uintptr_t>(model.handle),
            .workset = (workset != addon::worksets.end() ? workset->second.id : 0),
            .cache_bytes = (model.edit_reuse != nullptr ? model.edit_reuse->allocated_bytes : 0),
            .fence = gpu.serial, .completed_fence = gpu.fence->GetCompletedValue(),
            .device_status = gpu.device->GetDeviceRemovedReason(),
            .wrapper_ms = std::chrono::duration<double, std::milli>(recorded - entry).count(),
            .sr_api_ms = native_resize_host_ms, .nr_api_ms = native_resize_evaluate_ms,
            .nr_chain_record_ms = std::chrono::duration<double, std::milli>(recorded - native_cache_nr_cpu_begin).count(),
            .submit_complete_ms = std::chrono::duration<double, std::milli>(completed - submit_start).count(),
            .source_complete_ms = std::chrono::duration<double, std::milli>(completed - entry).count(),
            .retire_ms = std::chrono::duration<double, std::milli>(retired - completed).count(),
            .interval_ms = (previous_entry == std::chrono::steady_clock::time_point{} ? 0.
                : std::chrono::duration<double, std::milli>(entry - previous_entry).count()),
            .entry_ms = std::chrono::duration<double, std::milli>(entry - source_origin).count(),
            .create_ms = native_resize_create_ms, .release_ms = native_resize_release_ms};
        previous_entry = entry;
        Check(NVSDK_NGX_SUCCEED(result) && sample.sr_calls == 1 && sample.sequence == sample.sequence_before + 1
                  && sample.active_calls + sample.preparation_calls == sample.nr_calls
                  && native_runtime_resets.size() == sample.nr_calls
                  && std::all_of(native_runtime_results.begin(), native_runtime_results.end(),
                      [](auto value) { return NVSDK_NGX_SUCCEED(value); })
                  && std::all_of(native_runtime_masks.begin(), native_runtime_masks.end(),
                      [](const auto* value) { return value == nullptr; })
                  && sample.source_successes == sample.active_calls + sample.reuses,
              "real SR advances one source clock; preparation is separate from active NR and reused output success");
        Check(sample.completed_fence >= sample.fence && sample.completed_fence != UINT64_MAX
                  && SUCCEEDED(sample.device_status), "two-tier source finishes on a healthy completed Fence");
        return sample;
      };
      const auto history_frame = [&](const Frame& sample, const char* stage) {
        return nlohmann::json{{"stage", stage}, {"source_sequence_before", sample.sequence_before},
            {"source_sequence", sample.sequence}, {"sr_calls", sample.sr_calls}, {"sr_result", sample.sr_result},
            {"nr_calls", sample.nr_calls}, {"active_nr_calls", sample.active_calls}, {"preparation_nr_calls", sample.preparation_calls},
            {"nr_results", native_runtime_results}, {"nr_resets", native_runtime_resets}, {"nr_models", native_resize_models},
            {"nr_preparing", native_resize_preparing}, {"model_parameters", native_resize_parameter_snapshots},
            {"create_calls", native_resize_create_calls}, {"creates", sample.creates}, {"releases", sample.releases},
            {"slot", sample.slot}, {"active_model", sample.model}, {"workset", sample.workset},
            {"source_successes", sample.source_successes}, {"refreshes", sample.refreshes}, {"reuses", sample.reuses},
            {"cpu", {{"wrapper_ms", sample.wrapper_ms}, {"sr_api_ms", sample.sr_api_ms}, {"nr_api_ms", sample.nr_api_ms},
                {"nr_chain_record_ms", sample.nr_chain_record_ms}, {"create_ms", sample.create_ms}, {"release_ms", sample.release_ms},
                {"submit_complete_ms", sample.submit_complete_ms}, {"source_complete_ms", sample.source_complete_ms},
                {"retire_ms", sample.retire_ms}, {"source_interval_ms", sample.interval_ms}}},
            {"fence", sample.fence}, {"completed_fence", sample.completed_fence}, {"device_status", sample.device_status}};
      };
      cache_cost_report.update({{"variant", "two-ready-tiers"}, {"route", "inline-Upscaled-FG-Off"},
          {"process_id", GetCurrentProcessId()}, {"host_sr_handle", reinterpret_cast<uintptr_t>(owner)},
          {"source_format", "RGBA16F"}, {"source_encoding", "linear BT709 HDR SR output; exposure/pre-exposure 1"},
          {"runtime", addon::direct_runtime_requirements.file_version}, {"nr_sha256", addon::direct_runtime_sha256},
          {"driver_x100", addon::direct_runtime_requirements.driver_x100}, {"errors", 0},
          {"provider", "public SDK ProjectID initialized ordinary Core; no private Runtime or snippet identity bridge"},
          {"core_module", reinterpret_cast<uintptr_t>(addon::NgxLoaderView().core)},
          {"dimensions", {{"render", {render_width, render_height}}, {"output", {width, height}},
              {"upper", {width, height}}, {"lower", {width * 3 / 4, height * 3 / 4}}}},
          {"passes", 1}, {"colors", 1}, {"adaptive_enabled", true}, {"selected_levels", 2}, {"minimum_scale", .75},
          {"limited_residency", false}, {"feature_capacity_reuse", false}, {"jitter", {0, 0}}, {"motion", "zero"},
          {"control_mask_enabled", false}, {"prefix_edit_reuse_enabled", true},
          {"reset_schedule", "first cold source and first warmup source Reset=1; ready-tier changes send NR Reset with host Reset=0; otherwise Reset=0"},
          {"requested_model", "B"}, {"internal_model", "unknown"}, {"visual_validation", "not performed"},
          {"selection", "direct bounded Session requests; not the automatic output-FPS controller"},
          {"cold_preparation", {{"source_limit", preparation_limit}, {"frames", nlohmann::json::array()},
              {"codec_root_prepared_before_first_source", addon::codec_pipeline.root_signature != nullptr},
              {"control_mask_pso_prepared_before_first_source", addon::codec_pipeline.control_mask != nullptr},
              {"scope", "first NR source through both complete ready tiers; SDK Init, SR Create and fixture uploads precede this clock"}}},
          {"history", nlohmann::json::array()}, {"status", "preparing"}});
      cache_cost_report.update(runtime_identity());
      const auto cold_begin = std::chrono::steady_clock::now();
      bool prepared = false;
      for (uint32_t frame = 0; frame < preparation_limit; ++frame) {
        const auto sample = source_frame(frame == 0 ? 1 : 0);
        ++prefix_host_evaluates;
        prefix_nr_evaluates += sample.nr_calls;
        preparation_nr_calls += sample.preparation_calls;
        cache_cost_report["cold_preparation"]["frames"].push_back(history_frame(sample, "prepare"));
        auto& feature = addon::features.at(owner);
        Check(feature.adaptive_session == nullptr || (!feature.adaptive_session->failed
                  && feature.adaptive_session->preparation_reason != renodx::addons::dlss5::adaptive_resolution::Reason::BUDGET),
              "two-tier cold preparation stops on actual failure or budget rejection");
        const auto tiers = feature.adaptive_cache;
        prepared = tiers != nullptr && tiers->count == 2 && tiers->active == 0
            && tiers->tiers[0].ready && tiers->tiers[1].ready && feature.adaptive_session->prepared;
        Check(sample.reuses == 0, "cold preparation never consumes an edit cache before its first complete ready source");
        if (prepared) break;
      }
      cache_cost_report["cold_preparation"].update({{"wall_ms", std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - cold_begin).count()}, {"source_calls", prefix_host_evaluates},
          {"nr_calls", prefix_nr_evaluates}, {"standby_nr_calls", preparation_nr_calls}, {"complete", prepared}});
      save_cache_cost_report();
      Check(prepared && preparation_nr_calls > 0, "both actual independent NR Features become ready within the source bound");
      auto& feature = addon::features.at(owner);
      const auto tiers = feature.adaptive_cache;
      std::array<uintptr_t, 2> models{}, proxies{};
      std::array<uint64_t, 2> workset_ids{};
      nlohmann::json ready_tiers = nlohmann::json::array();
      for (size_t slot = 0; slot < 2; ++slot) {
        const auto& state = (slot == tiers->active ? feature : tiers->tiers[slot].state);
        const auto& workset = addon::worksets.at({owner, output.Get(), false, static_cast<uint32_t>(slot)});
        models[slot] = reinterpret_cast<uintptr_t>(state.slots[0].handle);
        proxies[slot] = reinterpret_cast<uintptr_t>(workset.proxy);
        workset_ids[slot] = workset.id;
        Check(models[slot] != 0 && state.slots[0].via_core && state.slots[0].recording != nullptr
                  && state.control_mask_stream != nullptr && workset.recording != nullptr,
              "ready model and workset retain the same ordered-history preparation contract");
        ready_tiers.push_back({{"slot", slot}, {"model", models[slot]},
            {"parameters", reinterpret_cast<uintptr_t>(state.slots[0].parameters)}, {"proxy", proxies[slot]},
            {"working_size", {workset.input_width, workset.input_height}}, {"output_size", {workset.width, workset.height}},
            {"source_units", {{"encoding", static_cast<uint32_t>(workset.units.encoding)},
                {"absolute", workset.units.absolute}, {"unit_nits", workset.units.unit_nits}, {"bt2020", workset.units.bt2020}}},
            {"workset", workset.id}, {"ready", tiers->tiers[slot].ready}});
      }
      Check(models[0] != models[1] && proxies[0] != proxies[1] && workset_ids[0] != workset_ids[1]
                && tiers->ladder.sizes[1][0].width == width * 3 / 4 && tiers->ladder.sizes[1][0].height == height * 3 / 4,
            "the ready collection contains distinct exact 100 and 75 percent model histories and working resources");
      cache_cost_report["ready_tiers"] = std::move(ready_tiers);
      const auto creates_at_ready = addon::nr_features_created;
      const auto worksets_at_ready = addon::worksets_created;
      addon::edit_reuse_enabled = reuse_enabled;
      const auto check_steady = [&](const Frame& sample, uint32_t slot, bool refresh, int reset) {
        Check(sample.slot == slot && sample.model == models[slot] && sample.workset == workset_ids[slot]
                  && sample.creates == 0 && sample.releases == 0 && sample.preparation_calls == 0
                  && sample.active_calls == (refresh ? 1u : 0u) && sample.nr_calls == (refresh ? 1u : 0u)
                  && (refresh ? sample.reset == reset : sample.reset == -1)
                  && sample.reuses == (refresh ? 0u : 1u) && sample.source_successes == 1 && sample.fallbacks == 0
                  && sample.refreshes == (reuse_enabled && refresh ? 1u : 0u)
                  && addon::nr_features_created == creates_at_ready && addon::worksets_created == worksets_at_ready,
              "a ready tier uses its original model/workset with exact real refresh/reuse calls and Reset, without cold work");
      };
      const auto history_source = [&](const char* stage, uint32_t slot, int reset, bool refresh, int model_reset) {
        const auto sample = source_frame(reset);
        ++sequence_sr_calls;
        sequence_nr_calls += sample.nr_calls;
        cache_cost_report["history"].push_back(history_frame(sample, stage));
        check_steady(sample, slot, refresh, model_reset);
      };
      history_source("upper-refresh", 0, 1, true, 1);
      std::weak_ptr<renodx::addons::dlss5::edit_reuse::State> upper_anchor = feature.slots[0].edit_reuse;
      history_source("upper-next", 0, 0, !reuse_enabled, 0);
      feature.adaptive_session->Request(1, addon::SteadyNowNs());
      history_source("lower-switch-reset", 1, 0, true, 1);
      Check(native_runtime_resets.front() == 1 && upper_anchor.expired()
                && tiers->tiers[0].state.slots[0].edit_reuse == nullptr,
            "switching to the ready lower model sends Reset and retires the departed edit anchor");
      history_source("lower-next", 1, 0, !reuse_enabled, 0);
      native_resize_selected_sequence = false;
      suppressed_control_mask_cost_logs = 0;
      suppress_control_mask_cost_logs = true;
      previous_entry = {};
      auto queries = d3d12_test::CreateTimestampQueries(gpu, total_frames * intervals_per_frame);
      std::array<Frame, total_frames> frames{};
      cache_cost_report["status"] = "measuring";
      for (uint32_t frame = 0; frame < total_frames; ++frame) {
        frames[frame] = source_frame(frame == 0 ? 1 : 0, &queries, frame * intervals_per_frame);
        ++sequence_sr_calls;
        sequence_nr_calls += frames[frame].nr_calls;
        check_steady(frames[frame], 1, !reuse_enabled || frame % 2 == 0, frame == 0 ? 1 : 0);
      }
      suppress_control_mask_cost_logs = false;
      double chain_sum = 0., sr_sum = 0., refresh_sum = 0., reuse_sum = 0., refresh_peak = 0.;
      double model_sum = 0., wrapper_sum = 0., nr_cpu_sum = 0., submit_sum = 0., interval_sum = 0.;
      uint32_t refresh_count = 0, reuse_count = 0;
      std::array<double, cache_cost_sample_frames> measured_chain{};
      cache_cost_report["frames"] = nlohmann::json::array();
      const auto timestamp_json = [](const d3d12_test::TimestampSample& sample) {
        return nlohmann::json{{"begin_tick", sample.begin_tick}, {"end_tick", sample.end_tick},
            {"ticks", sample.ticks}, {"gpu_ms", sample.gpu_ms}};
      };
      for (uint32_t frame = 0; frame < total_frames; ++frame) {
        const auto& sample = frames[frame];
        const auto sr = queries.Read(frame * intervals_per_frame);
        const auto chain = queries.Read(frame * intervals_per_frame + 2);
        Check(sr.begin_tick < sr.end_tick && sr.end_tick <= chain.begin_tick && chain.begin_tick < chain.end_tick,
              "two-tier timestamps separate real SR from the complete post-SR NR chain");
        nlohmann::json model = nullptr;
        double model_ms = 0.;
        if (sample.nr_calls != 0) {
          const auto timing = queries.Read(frame * intervals_per_frame + 1);
          Check(chain.begin_tick <= timing.begin_tick && timing.begin_tick < timing.end_tick && timing.end_tick <= chain.end_tick,
                "actual NR model timestamps are nested inside the complete chain");
          model_ms = timing.gpu_ms;
          model = timestamp_json(timing);
        }
        cache_cost_report["frames"].push_back({{"frame", frame}, {"warmup", frame < cache_cost_warmup_frames},
            {"frame_kind", sample.reuses != 0 ? "reuse" : "refresh"},
            {"source_sequence_before", sample.sequence_before}, {"source_sequence", sample.sequence}, {"slot", sample.slot},
            {"sr", {{"calls", sample.sr_calls}, {"result", sample.sr_result}, {"gpu", timestamp_json(sr)}}},
            {"nr", {{"calls", sample.nr_calls},
                {"result", sample.nr_calls != 0 ? nlohmann::json(sample.nr_result) : nlohmann::json(nullptr)},
                {"reset", sample.nr_calls != 0 ? nlohmann::json(sample.reset) : nlohmann::json(nullptr)},
                {"gpu", model}, {"whole_chain_gpu", timestamp_json(chain)},
                {"model", sample.model}, {"workset", sample.workset}, {"cache_bytes", sample.cache_bytes}}},
            {"reuse", {{"refreshes", sample.refreshes}, {"reuses", sample.reuses}, {"fallbacks", sample.fallbacks}}},
            {"cpu", {{"entry_ms", sample.entry_ms}, {"wrapper_ms", sample.wrapper_ms},
                {"interval_ms", frame == 0 ? nlohmann::json(nullptr) : nlohmann::json(sample.interval_ms)},
                {"record_ms", sample.nr_chain_record_ms}, {"sr_api_ms", sample.sr_api_ms}, {"nr_api_ms", sample.nr_api_ms},
                {"submit_complete_ms", sample.submit_complete_ms},
                {"source_complete_ms", sample.source_complete_ms}, {"retire_ms", sample.retire_ms},
                {"nr_chain_record_ms", sample.nr_chain_record_ms}}},
            {"source_successes", sample.source_successes}, {"fence", sample.fence}, {"completed_fence", sample.completed_fence},
            {"device_status", sample.device_status}});
        if (frame < cache_cost_warmup_frames) continue;
        measured_chain[frame - cache_cost_warmup_frames] = chain.gpu_ms;
        chain_sum += chain.gpu_ms;
        sr_sum += sr.gpu_ms;
        model_sum += model_ms;
        wrapper_sum += sample.wrapper_ms;
        nr_cpu_sum += sample.nr_chain_record_ms;
        submit_sum += sample.submit_complete_ms;
        interval_sum += sample.interval_ms;
        if (sample.reuses != 0) {
          reuse_sum += chain.gpu_ms;
          ++reuse_count;
        } else {
          refresh_sum += chain.gpu_ms;
          refresh_peak = std::max(refresh_peak, chain.gpu_ms);
          ++refresh_count;
        }
      }
      nlohmann::json cycles = nlohmann::json::array();
      for (size_t frame = 0; frame < measured_chain.size(); frame += 2) {
        cycles.push_back((measured_chain[frame] + measured_chain[frame + 1]) / 2.);
      }
      cache_cost_report.update({{"status", "measurement_completed"}, {"warmup_frames", cache_cost_warmup_frames},
          {"sample_frames", cache_cost_sample_frames}, {"measured_nr_evaluates", refresh_count},
          {"timestamp_frequency_hz", queries.frequency_hz}, {"suppressed_log_calls", suppressed_control_mask_cost_logs},
          {"complete_pair_average_gpu_ms", std::move(cycles)},
          {"steady_low_summary", {{"whole_chain_mean_gpu_ms", chain_sum / cache_cost_sample_frames},
              {"sr_mean_gpu_ms", sr_sum / cache_cost_sample_frames}, {"model_mean_gpu_ms_per_source", model_sum / cache_cost_sample_frames},
              {"refresh_frames", refresh_count}, {"refresh_chain_mean_gpu_ms", refresh_sum / refresh_count},
              {"refresh_chain_peak_gpu_ms", refresh_peak}, {"reuse_frames", reuse_count},
              {"reuse_chain_mean_gpu_ms", reuse_count != 0 ? nlohmann::json(reuse_sum / reuse_count) : nlohmann::json(nullptr)},
              {"wrapper_mean_cpu_ms", wrapper_sum / cache_cost_sample_frames}, {"nr_chain_record_mean_cpu_ms", nr_cpu_sum / cache_cost_sample_frames},
              {"submit_complete_mean_cpu_ms", submit_sum / cache_cost_sample_frames}, {"source_interval_mean_ms", interval_sum / cache_cost_sample_frames}}},
          {"timing_scope", {{"whole_nr_chain_gpu", "timestamp after real SR through native Upscaled encode/cache/Resolve/Commit and output writeback"},
              {"nr_chain_record_cpu", "after real SR observer returns through the production source wrapper return"},
              {"cold", "separate preparation frames; excluded from steady measurements"},
              {"source_interval", "serialized fixture entries including prior Fence completion and bookkeeping; not display pacing"}}},
          {"input", {{"schedule", "single deterministic color/depth upload; zero motion and jitter"},
              {"color_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(color_data.data()), color_data.size() * sizeof(uint16_t))},
              {"depth_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(depth_data.data()), depth_data.size() * sizeof(float))}}}});
      auto low_output = gpu.CreateTextureTransfer(output.Get());
      gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.RecordTextureReadback(output.Get(), low_output);
      gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      ComPtr<ID3D12Resource> validity_readback;
      uint64_t validity_pixels = 0;
      if (reuse_enabled) {
        const auto& edit = feature.slots[0].edit_reuse;
        Check(edit != nullptr && edit->reused && edit->width == width * 3 / 4 && edit->height == height * 3 / 4
                  && edit->output_width == width && edit->output_height == height,
              "last measured low source owns the actual low-grid reuse cache");
        validity_pixels = static_cast<uint64_t>(edit->width) * edit->height;
        const auto bytes = validity_pixels * sizeof(uint32_t);
        validity_readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
        gpu.Transition(edit->validity.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->CopyBufferRegion(validity_readback.Get(), 0, edit->validity.Get(), 0, bytes);
        gpu.Transition(edit->validity.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
      submit_resize();
      const auto pixels = gpu.ReadTexture(low_output);
      uint64_t nonfinite = 0;
      float maximum_rgb = 0.f;
      for (size_t pixel = 0; pixel < pixels.size() / 8; ++pixel) {
        std::array<uint16_t, 4> rgba;
        std::memcpy(rgba.data(), pixels.data() + pixel * 8, 8);
        for (size_t channel = 0; channel < 4; ++channel) {
          const float value = DirectX::PackedVector::XMConvertHalfToFloat(rgba[channel]);
          nonfinite += !std::isfinite(value);
          if (channel < 3) {
            maximum_rgb = std::max(maximum_rgb, std::abs(value));
          }
        }
      }
      Check(pixels.size() == pixel_count * 8 && nonfinite == 0 && maximum_rgb > 0.f,
            "final complete low-grid native output is finite and nonzero");
      cache_cost_report["output_readback"] = {{"count", 1}, {"after_measurement", true}, {"slot", 1},
          {"bytes", pixels.size()}, {"raw_retained", false}, {"nonfinite_channels", nonfinite}, {"maximum_absolute_rgb", maximum_rgb},
          {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()}};
      cache_cost_report["validity_readback"] = nullptr;
      if (validity_readback != nullptr) {
        const D3D12_RANGE range{0, static_cast<SIZE_T>(validity_pixels * sizeof(uint32_t))};
        uint32_t* mapped = nullptr;
        Check(SUCCEEDED(validity_readback->Map(0, &range, reinterpret_cast<void**>(&mapped))), "low-grid validity readback map");
        uint64_t valid = 0, invalid = 0, other = 0;
        for (uint64_t pixel = 0; pixel < validity_pixels; ++pixel) {
          if (mapped[pixel] == 0) {
            ++invalid;
          } else if (mapped[pixel] == 1) {
            ++valid;
          } else {
            ++other;
          }
        }
        const D3D12_RANGE no_writes{0, 0};
        validity_readback->Unmap(0, &no_writes);
        cache_cost_report["validity_readback"] = {{"count", 1}, {"slot", 1}, {"after_measurement", true},
            {"raw_retained", false}, {"working_pixels", validity_pixels}, {"zero_pixels", invalid}, {"one_pixels", valid}, {"other_pixels", other}};
        Check(other == 0 && valid > 0, "the actual final low-grid reuse has nonempty valid edits and only 0/1 validity");
      }
      native_resize_selected_sequence = true;
      std::weak_ptr<renodx::addons::dlss5::edit_reuse::State> low_anchor = feature.slots[0].edit_reuse;
      feature.adaptive_session->Request(0, addon::SteadyNowNs());
      history_source("upper-return-reset", 0, 0, true, 1);
      Check(native_runtime_resets.front() == 1 && low_anchor.expired()
                && tiers->tiers[1].state.slots[0].edit_reuse == nullptr,
            "returning to the ready upper model resets history and retires the old low-grid anchor");
      history_source("upper-return-next", 0, 0, !reuse_enabled, 0);
      cache_cost_report.update({{"sequence_host_sr_evaluates", sequence_sr_calls}, {"sequence_nr_evaluates", sequence_nr_calls},
          {"prefix_host_sr_evaluates", prefix_host_evaluates}, {"prefix_nr_evaluates", prefix_nr_evaluates},
          {"ready_switch_creates", addon::nr_features_created - creates_at_ready}, {"ready_switch_worksets", addon::worksets_created - worksets_at_ready},
          {"history_scope", "real API parameters, independent handles, Reset, source sequence and GPU completion; no internal-network or dynamic-quality assertion"}});
      addon::core_create_feature = native_resize_create;
      addon::core_release_feature = native_resize_release;
      native_resize_ready = native_resize_selected_sequence = false;
      native_cache_observe = native_cache_cost = false;
      native_cache_two_tiers = false;
      native_cache_two_tier_owner = nullptr;
      addon::edit_reuse_adaptive_experiment = false;
      save_cache_cost_report();
      std::cout << "PASS native two-ready-tier edit cost condition=" << condition
                << " cold=separate low=2880x1620 output=3840x2160 render=2560x1440 warmup=32 samples=128 pairs=64 route=inline-Upscaled-FG-Off\n";
    } else if (cache_cost) {
      const auto condition = cache_cost_report.at("condition").get<std::string>();
      const auto look = addon::LookSettingsSnapshot(true);
      Check(!addon::nr_adaptive_enabled.load() && !addon::look_mode.load() && !addon::control_mask_enabled.load()
                && addon::detail_stability.load() == 1u && look.stabilize == 0u
                && !look.stabilize_detail && look.stabilize_alpha == 0.f,
            "cache cost disables adaptive resolution, ControlMask and temporal Look");
      constexpr uint32_t total_frames = cache_cost_warmup_frames + cache_cost_sample_frames;
      constexpr UINT intervals_per_frame = 3;
      auto queries = d3d12_test::CreateTimestampQueries(gpu, total_frames * intervals_per_frame);
      struct CacheCostFrame {
        uint32_t sr_calls = 0, sr_result = 0, nr_calls = 0, nr_result = 0;
        int reset = -1;
        uint64_t refreshes = 0, reuses = 0, fallbacks = 0, source_successes = 0;
        uint64_t fence = 0, completed_fence = 0, model = 0, workset = 0, cache_bytes = 0;
        HRESULT device_status = E_FAIL;
        double entry_ms = 0., interval_ms = 0., wrapper_ms = 0., record_ms = 0.;
        double sr_api_ms = 0., nr_api_ms = 0., submit_complete_ms = 0., source_complete_ms = 0.;
      };
      std::array<CacheCostFrame, total_frames> frames{};
      const NVSDK_NGX_Handle* stable_model = nullptr;
      uint64_t stable_workset = 0;
      auto previous_entry = std::chrono::steady_clock::time_point{};
      const auto origin = std::chrono::steady_clock::now();
      addon::edit_reuse_enabled = condition == "cache";
      native_cache_cost = true;
      suppressed_control_mask_cost_logs = 0;
      suppress_control_mask_cost_logs = true;
      const auto clear_observers = [&] {
        native_history_queries = nullptr;
        native_cache_observe = false;
        native_cache_cost = false;
        suppress_control_mask_cost_logs = false;
      };
      uint32_t attempted_frame = 0, completed_frames = 0;
      try {
        for (uint32_t frame = 0; frame < total_frames; ++frame) {
          attempted_frame = frame;
          native_motion_sr_calls = 0;
          native_runtime_masks.clear();
          native_runtime_results.clear();
          native_runtime_resets.clear();
          native_resize_host_ms = native_resize_evaluate_ms = 0.;
          native_motion_sr_readback = nullptr;
          native_motion_proxy_readback = nullptr;
          native_motion_neural_readback = nullptr;
          native_history_queries = &queries;
          native_history_interval = frame * intervals_per_frame;
          native_cache_observe = true;
          const uint64_t refreshes_before = addon::edit_reuse_refreshes.load();
          const uint64_t reuses_before = addon::edit_reuse_reuses.load();
          const uint64_t fallbacks_before = addon::edit_reuse_fallbacks.load();
          const auto successes_before = addon::SourceSuccessfulEvaluations();
          parameters.Set(NVSDK_NGX_Parameter_Reset, frame == 0 ? 1 : 0);
          gpu.list->ClearState(nullptr);
          const auto entry = std::chrono::steady_clock::now();
          const auto result = ObserveNativeHostEvaluate(gpu.list.Get(), owner, &parameters, nullptr);
          const auto sr_recorded = std::chrono::steady_clock::now();
          const bool ready = record_xefg();
          const auto recorded = std::chrono::steady_clock::now();
          queries.EndAndResolve(gpu.list.Get(), native_history_interval + 2);
          if (!native_runtime_results.empty()) {
            const UINT query = 2 * (native_history_interval + 1);
            gpu.list->ResolveQueryData(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query, 2,
                queries.readback.Get(), static_cast<uint64_t>(query) * sizeof(uint64_t));
          }
          native_history_queries = nullptr;
          native_cache_observe = false;
          const auto submit_start = std::chrono::steady_clock::now();
          gpu.Submit();
          const auto completed = std::chrono::steady_clock::now();
          addon::DrainRetiredResources(true);
          const auto& model = addon::features.at(nr_owner).slots[0];
          const auto& workset = addon::worksets.at({nr_owner, nr_workset_key, false});
          auto& sample = frames[frame];
          sample = {.sr_calls = native_motion_sr_calls, .sr_result = static_cast<uint32_t>(result),
              .nr_calls = static_cast<uint32_t>(native_runtime_results.size()),
              .nr_result = (native_runtime_results.empty() ? 0u : static_cast<uint32_t>(native_runtime_results[0])),
              .reset = (native_runtime_resets.empty() ? -1 : native_runtime_resets[0]),
              .refreshes = addon::edit_reuse_refreshes.load() - refreshes_before,
              .reuses = addon::edit_reuse_reuses.load() - reuses_before,
              .fallbacks = addon::edit_reuse_fallbacks.load() - fallbacks_before,
              .source_successes = addon::SourceSuccessfulEvaluations() - successes_before,
              .fence = gpu.serial, .completed_fence = gpu.fence->GetCompletedValue(),
              .model = reinterpret_cast<uintptr_t>(model.handle), .workset = workset.id,
              .cache_bytes = (model.edit_reuse != nullptr ? model.edit_reuse->allocated_bytes : 0),
              .device_status = gpu.device->GetDeviceRemovedReason(),
              .entry_ms = std::chrono::duration<double, std::milli>(entry - origin).count(),
              .interval_ms = (frame == 0 ? 0. : std::chrono::duration<double, std::milli>(entry - previous_entry).count()),
              .wrapper_ms = std::chrono::duration<double, std::milli>(recorded - entry).count(),
              .record_ms = std::chrono::duration<double, std::milli>(recorded - sr_recorded).count(),
              .sr_api_ms = native_resize_host_ms, .nr_api_ms = native_resize_evaluate_ms,
              .submit_complete_ms = std::chrono::duration<double, std::milli>(completed - submit_start).count(),
              .source_complete_ms = std::chrono::duration<double, std::milli>(completed - entry).count()};
          ++completed_frames;
          previous_entry = entry;
          const bool refresh = condition == "baseline" || frame % 2 == 0;
          Check(NVSDK_NGX_SUCCEED(result) && sample.sr_calls == 1 && ready && sample.source_successes == 1
                    && sample.nr_calls == (refresh ? 1u : 0u) && native_runtime_resets.size() == sample.nr_calls
                    && (sample.nr_calls == 0 || (NVSDK_NGX_SUCCEED(native_runtime_results[0]) && sample.reset == (frame == 0 ? 1 : 0)))
                    && sample.reuses == (refresh ? 0u : 1u) && sample.fallbacks == 0
                    && sample.refreshes == (condition == "cache" && refresh ? 1u : 0u),
                "cache cost executes real SR each frame, exact NR 1/0 cadence and preserved refresh History");
          Check(sample.completed_fence >= sample.fence && sample.completed_fence != UINT64_MAX
                    && SUCCEEDED(sample.device_status) && model.handle != nullptr && model.via_core
                    && !model.control_mask_bound && workset.control_mask == nullptr,
                "cache cost completes every recording on a healthy GPU without mask bindings");
          if (frame == cache_cost_warmup_frames - 1) {
            stable_model = model.handle;
            stable_workset = workset.id;
          }
          if (frame >= cache_cost_warmup_frames) {
            Check(model.handle == stable_model && workset.id == stable_workset,
                  "measured cache cost frames retain the warmed model and workset");
          }
        }
      } catch (...) {
        clear_observers();
        const auto& sample = frames[attempted_frame];
        cache_cost_report["failure_phase"] = "warmup_or_measurement";
        cache_cost_report["attempted_frame"] = attempted_frame;
        cache_cost_report["completed_frames"] = completed_frames;
        cache_cost_report["last_attempt"] = {{"sr_calls", native_motion_sr_calls}, {"nr_results", native_runtime_results},
            {"nr_resets", native_runtime_resets}, {"fence", sample.fence}, {"completed_fence", sample.completed_fence},
            {"device_status", sample.device_status}};
        throw;
      }
      clear_observers();
      cache_cost_report = {{"schema", "nr-nexus-nr-edit-reuse-cost-v1"}, {"condition", condition},
          {"evidence", "NATIVE_GPU+NATIVE_NGX"}, {"status", "measurement_completed"},
          {"process_id", GetCurrentProcessId()}, {"host_sr_handle", reinterpret_cast<uintptr_t>(owner)},
          {"route", "xefg-record"}, {"source_format", "RGBA16F"}, {"source_encoding", "linear BT709 scRGB; 1=80 nits"},
          {"dimensions", {{"render", {render_width, render_height}}, {"output", {width, height}}}},
          {"passes", 1}, {"adaptive_enabled", false}, {"control_mask_enabled", false}, {"jitter", {0, 0}},
          {"requested_model", "B"}, {"internal_model", "unknown"}, {"present_white_nits", 200},
          {"runtime", addon::direct_runtime_requirements.file_version}, {"nr_sha256", addon::direct_runtime_sha256},
          {"driver_x100", addon::direct_runtime_requirements.driver_x100}, {"prefix", cost_prefix},
          {"prefix_host_sr_evaluates", prefix_host_evaluates}, {"prefix_nr_evaluates", prefix_nr_evaluates},
          {"prefix_edit_reuse_enabled", true}, {"prefix_force_reset", true},
          {"warmup_frames", cache_cost_warmup_frames}, {"sample_frames", cache_cost_sample_frames},
          {"sequence_host_sr_evaluates", total_frames}, {"sequence_nr_evaluates", condition == "baseline" ? total_frames : total_frames / 2},
          {"measured_host_sr_evaluates", cache_cost_sample_frames},
          {"measured_nr_evaluates", condition == "baseline" ? cache_cost_sample_frames : cache_cost_sample_frames / 2},
          {"errors", 0}, {"timestamp_frequency_hz", queries.frequency_hz},
          {"suppressed_log_calls", suppressed_control_mask_cost_logs},
          {"input", {{"schedule", "single deterministic upload; static color/depth, zero motion and jitter"},
              {"color_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(color_data.data()), color_data.size() * sizeof(uint16_t))},
              {"depth_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(depth_data.data()), depth_data.size() * sizeof(float))}}},
          {"reset_schedule", "identical Reset=1 prefix; warmup frame0 Reset=1, then host/NR refresh Reset=0"},
          {"timing_scope", {{"sr", "real SR Evaluate only"}, {"model", "real NR Evaluate only; null on reuse"},
              {"whole_nr_chain", "after SR timestamp resolve through production Record, original-color staging, cache/codec and final private-color writeback"},
              {"cpu_wrapper", "SR observer plus production Record/private-color writeback recording"},
              {"cpu_submit_complete", "test list Close/submission/Fence wait and allocator/list/tracker reset"},
              {"source_interval", "adjacent test call entries including prior completion and test bookkeeping; no display pacing"}}},
          {"synchronization", "one completed list submission per source frame; all GPU query reads after measurement"},
          {"xefg_scope", "production Record only; no XeFG SDK/Service admission, generated frames or display FPS"},
          {"visual_validation", "not performed; cost-only static input"},
          {"frames", nlohmann::json::array()}};
      cache_cost_report.update(runtime_identity());
      const auto timestamp_json = [](const d3d12_test::TimestampSample& sample) {
        return nlohmann::json{{"begin_tick", sample.begin_tick}, {"end_tick", sample.end_tick},
            {"ticks", sample.ticks}, {"gpu_ms", sample.gpu_ms}};
      };
      for (uint32_t frame = 0; frame < total_frames; ++frame) {
        const auto& sample = frames[frame];
        const auto sr = queries.Read(frame * intervals_per_frame);
        const auto chain = queries.Read(frame * intervals_per_frame + 2);
        Check(sr.begin_tick < sr.end_tick && sr.end_tick <= chain.begin_tick && chain.begin_tick < chain.end_tick,
              "cache cost SR and complete NR chain have ordered nonempty GPU intervals");
        nlohmann::json model = nullptr;
        if (sample.nr_calls != 0) {
          const auto timing = queries.Read(frame * intervals_per_frame + 1);
          Check(chain.begin_tick <= timing.begin_tick && timing.begin_tick < timing.end_tick && timing.end_tick <= chain.end_tick,
                "cache cost actual NR Evaluate is nested in the complete NR chain");
          model = timestamp_json(timing);
        }
        cache_cost_report["frames"].push_back({{"frame", frame}, {"warmup", frame < cache_cost_warmup_frames},
            {"frame_kind", sample.reuses != 0 ? "reuse" : "refresh"},
            {"sr", {{"calls", sample.sr_calls}, {"result", sample.sr_result}, {"gpu", timestamp_json(sr)}}},
            {"nr", {{"calls", sample.nr_calls}, {"result", sample.nr_calls != 0 ? nlohmann::json(sample.nr_result) : nlohmann::json(nullptr)},
                {"reset", sample.nr_calls != 0 ? nlohmann::json(sample.reset) : nlohmann::json(nullptr)}, {"gpu", model},
                {"whole_chain_gpu", timestamp_json(chain)}, {"model", sample.model}, {"workset", sample.workset}, {"cache_bytes", sample.cache_bytes}}},
            {"reuse", {{"refreshes", sample.refreshes}, {"reuses", sample.reuses}, {"fallbacks", sample.fallbacks}}},
            {"source_successes", sample.source_successes},
            {"cpu", {{"entry_ms", sample.entry_ms}, {"interval_ms", frame == 0 ? nlohmann::json(nullptr) : nlohmann::json(sample.interval_ms)},
                {"wrapper_ms", sample.wrapper_ms}, {"record_ms", sample.record_ms}, {"sr_api_ms", sample.sr_api_ms},
                {"nr_api_ms", sample.nr_api_ms}, {"submit_complete_ms", sample.submit_complete_ms}, {"source_complete_ms", sample.source_complete_ms}}},
            {"fence", sample.fence}, {"completed_fence", sample.completed_fence}, {"device_status", sample.device_status}});
      }
      save_cache_cost_report();
    }
    if (single_feature) {
      namespace serial = renodx::addons::dlss5::ngx_serial;
      constexpr UINT small_width = 960, small_height = 540;
      constexpr UINT reset_prefix = 2, measured_samples = 8, evaluations_per_stage = reset_prefix + measured_samples;
      constexpr std::array<const char*, 5> phase_names{
          "full-baseline", "small-standard-subrect", "small-private-width-height", "full-restored", "true-small-reference"};
      constexpr std::array<uint16_t, 4> sentinel{0xC800, 0xC400, 0xC000, 0xBC00};
      const auto create = addon::core_create_feature;
      const auto allocate = addon::core_allocate_parameters;
      const auto destroy = addon::core_destroy_parameters;
      const auto release = addon::core_release_feature;
      const auto evaluate = native_runtime_evaluate;
      const auto& production_feature = addon::features.at(owner);
      const uint32_t create_flags = production_feature.create_flags;
      const uint32_t model_preset = production_feature.slots[0].nr_preset;
      const uint8_t hdr_mode = production_feature.slots[0].nr_hdr_mode;
      const auto production_nr = production_feature.slots[0].handle;
      const auto production_workset = addon::worksets.at({owner, output.Get(), false}).id;
      Check(create != nullptr && allocate != nullptr && destroy != nullptr && release != nullptr && evaluate != nullptr,
            "single feature probe snapshots an initialized Core provider");
      auto& report = resize_report;
      report = {{"schema", "nr-nexus-native-single-feature-v1"}, {"evidence", "NATIVE_GPU+NATIVE_NGX"},
          {"mode", "single-feature-effective-size-probe"}, {"production_async_enabled", false}, {"p1_enabled", false},
          {"resource_layout_mode", single_small_physical ? "small-physical-resource" : "maximum-physical-backing"},
          {"source_version", NR_NEXUS_VERSION}, {"source_identity_scope", "exact frozen source identity is recorded by the test runner"},
          {"runtime", addon::direct_runtime_requirements.file_version}, {"nr_sha256", addon::direct_runtime_sha256},
          {"requested_model", model_preset}, {"internal_model", "unknown"}, {"format", "RGBA16F"},
          {"physical_maximum", {width, height}}, {"physical_reference", {small_width, small_height}},
          {"guide_size", {render_width, render_height}}, {"create_flags", create_flags}, {"hdr_mode", hdr_mode},
          {"scaling_ratio", 1.f}, {"reset_prefix", reset_prefix}, {"measured_samples", measured_samples},
          {"sentinel_rgba", {-8.f, -4.f, -2.f, -1.f}},
          {"pixel_scope", "RGB coverage is primary; alpha is reported separately; no subjective visual conclusion"},
          {"dispatch_scope", "direct Dispatch calls visible on the supplied list, not FLOPS or unobserved internal paths"},
          {"timing_scope", "GPU timestamps surround only Core Evaluate; sentinel upload, parameter logging and readback are outside"},
          {"teardown_status", "NotRun"}, {"frames", nlohmann::json::array()}, {"stages", nlohmann::json::array()},
          {"features", nlohmann::json::array()}};
      for (const auto* phase : phase_names) {
        report["stages"].push_back({{"phase", phase}, {"status", "NotRun"}, {"samples", nlohmann::json::array()}});
      }
      ComPtr<IDXGIAdapter3> adapter;
      Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))),
            "single feature probe adapter");
      DXGI_ADAPTER_DESC adapter_desc{};
      Check(SUCCEEDED(adapter->GetDesc(&adapter_desc)), "single feature adapter identity");
      report["adapter"] = std::filesystem::path(adapter_desc.Description).string();
      LARGE_INTEGER driver_version{};
      report["driver_status"] = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver_version);
      report["driver_version_raw"] = driver_version.QuadPart;
      auto queries = d3d12_test::CreateTimestampQueries(gpu, static_cast<UINT>(phase_names.size()) * evaluations_per_stage);
      const std::array<UINT, 2> physical_widths{width, small_width}, physical_heights{height, small_height};
      const std::array<size_t, phase_names.size()> phase_resource_indices{
          0u, single_small_physical ? 1u : 0u, single_small_physical ? 1u : 0u, 0u, 1u};
      std::array<addon::FeatureState, 2> private_features;
      std::array<ComPtr<ID3D12Resource>, 2> proxies, neural_outputs;
      std::array<d3d12_test::TextureTransfer, 2> proxy_transfers, output_transfers;
      std::array<std::vector<uint16_t>, 2> private_inputs, sentinel_inputs;
      std::array<std::shared_ptr<submission::RecordingStatus>, 2> initialization_receipts;
      std::array<std::vector<uint8_t>, phase_names.size()> stage_pixels;
      std::array<std::vector<std::array<uint32_t, 3>>, phase_names.size()> measured_dispatches;
      std::array<bool, phase_names.size()> dispatch_stable{};
      std::array<double, phase_names.size()> gpu_medians{};
      std::array<bool, 2> created{};
      ComPtr<ID3D12Resource> retained_depth = depth, retained_motion = motion;
      bool dispatch_attached = false, cleanup_attempted = false;
      const auto submit_healthy = [&](const std::shared_ptr<submission::RecordingStatus>& receipt) {
        gpu.Submit();
        const auto completed = gpu.fence->GetCompletedValue();
        Check(completed >= gpu.serial && completed != UINT64_MAX && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
              "single feature submission completes on a healthy device");
        if (receipt != nullptr) {
          std::lock_guard lock(submission::tracker_mutex);
          Check(receipt->closed.load() && receipt->submitted.load() && receipt->completed_healthy,
                "single feature use has a completed non-replayable recording receipt");
        }
      };
      const auto detach_dispatch = [&] {
        native_single_feature_dispatch_list = nullptr;
        if (!dispatch_attached) return true;
        if (DetourTransactionBegin() != NO_ERROR) return false;
        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
            || DetourDetach(reinterpret_cast<PVOID*>(&real_dispatch), reinterpret_cast<PVOID>(ObserveDispatch)) != NO_ERROR) {
          DetourTransactionAbort();
          return false;
        }
        if (DetourTransactionCommit() != NO_ERROR) return false;
        dispatch_attached = false;
        return true;
      };
      const auto cleanup_private = [&] {
        if (cleanup_attempted) return report["private_cleanup"].value("completed", false);
        cleanup_attempted = true;
        bool safe = !gpu.submission_pending && gpu.fence->GetCompletedValue() != UINT64_MAX
            && SUCCEEDED(gpu.device->GetDeviceRemovedReason());
        if (safe) {
          gpu.list->Close();
          safe = SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr));
          if (safe) submission::OnCommandListReset(gpu.list.Get());
          submission::PruneCompletedGenerations();
          for (size_t index = 0; index < private_features.size(); ++index) {
            safe &= submission::ResourceReleasable(private_features[index].slots[0].handle)
                && submission::ResourceReleasable(proxies[index].Get()) && submission::ResourceReleasable(neural_outputs[index].Get());
          }
        }
        report["private_cleanup"] = {{"recordings_safe", safe}, {"features", nlohmann::json::array()}};
        try {
          const serial::PresentTurn turn(true);
          safe &= turn.owns();
          if (safe) {
            renodx::addons::dlss5::DirectCallScope direct;
            for (auto& feature : private_features) {
              auto& slot = feature.slots[0];
              nlohmann::json result;
              if (slot.handle != nullptr) {
                const auto released = release(slot.handle);
                result["release_result"] = static_cast<uint32_t>(released);
                safe &= NVSDK_NGX_SUCCEED(released);
                if (NVSDK_NGX_SUCCEED(released)) slot.handle = nullptr;
              }
              if (slot.handle == nullptr && slot.parameters != nullptr) {
                const auto destroyed = destroy(slot.parameters);
                result["destroy_parameters_result"] = static_cast<uint32_t>(destroyed);
                safe &= NVSDK_NGX_SUCCEED(destroyed);
                if (NVSDK_NGX_SUCCEED(destroyed)) slot.parameters = nullptr;
              }
              report["private_cleanup"]["features"].push_back(std::move(result));
            }
          }
        } catch (...) {
          report["private_cleanup"]["sdk_exception"] = true;
          safe = false;
        }
        report["private_cleanup"]["completed"] = safe;
        if (!safe) {
          for (size_t index = 0; index < private_features.size(); ++index) {
            proxies[index].Detach();
            neural_outputs[index].Detach();
            proxy_transfers[index].upload.Detach();
            proxy_transfers[index].readback.Detach();
            output_transfers[index].upload.Detach();
            output_transfers[index].readback.Detach();
          }
          retained_depth.Detach();
          retained_motion.Detach();
        }
        return safe;
      };
      try {
        for (size_t index = 0; index < private_features.size(); ++index) {
          const UINT physical_width = physical_widths[index], physical_height = physical_heights[index];
          auto& feature = private_features[index];
          auto& slot = feature.slots[0];
          feature.create_flags = create_flags;
          private_inputs[index].resize(static_cast<size_t>(physical_width) * physical_height * 4);
          sentinel_inputs[index].resize(private_inputs[index].size());
          for (UINT y = 0; y < physical_height; ++y) {
            for (UINT x = 0; x < physical_width; ++x) {
              const size_t pixel = (static_cast<size_t>(y) * physical_width + x) * 4;
              if (index == 0) {
                const float u = static_cast<float>(x) / (width - 1);
                const float v = static_cast<float>(y) / (height - 1);
                private_inputs[index][pixel] = DirectX::PackedVector::XMConvertFloatToHalf(0.12f + 0.65f * u);
                private_inputs[index][pixel + 1] = DirectX::PackedVector::XMConvertFloatToHalf(0.1f + 0.65f * v);
                private_inputs[index][pixel + 2] = DirectX::PackedVector::XMConvertFloatToHalf(0.15f + 0.5f * (1.f - u * v));
                private_inputs[index][pixel + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
              } else {
                std::memcpy(private_inputs[index].data() + pixel,
                    private_inputs[0].data() + (static_cast<size_t>(y) * width + x) * 4, 4 * sizeof(uint16_t));
              }
              std::memcpy(sentinel_inputs[index].data() + pixel, sentinel.data(), 4 * sizeof(uint16_t));
            }
          }
          proxies[index] = gpu.Texture(physical_width, physical_height, DXGI_FORMAT_R16G16B16A16_FLOAT);
          auto desc = proxies[index]->GetDesc();
          desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
          Check(SUCCEEDED(gpu.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&neural_outputs[index]))), "single feature private output");
          proxy_transfers[index] = gpu.CreateTextureTransfer(proxies[index].Get());
          output_transfers[index] = gpu.CreateTextureTransfer(neural_outputs[index].Get());
          gpu.Transition(proxies[index].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
          gpu.UploadTexture(proxies[index].Get(), proxy_transfers[index], reinterpret_cast<const uint8_t*>(private_inputs[index].data()),
              private_inputs[index].size() * sizeof(uint16_t), physical_width * 8);
          gpu.Transition(proxies[index].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          submit_healthy(nullptr);
          report["features"].push_back({{"kind", index == 0 ? "maximum" : "true-small-reference"}, {"status", "Running"},
              {"physical_size", {physical_width, physical_height}}, {"create_size", {physical_width, physical_height}},
              {"proxy", reinterpret_cast<uintptr_t>(proxies[index].Get())},
              {"output", reinterpret_cast<uintptr_t>(neural_outputs[index].Get())},
              {"input_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(private_inputs[index].data()),
                  private_inputs[index].size() * sizeof(uint16_t))}});
          auto& identity = report["features"].back();
          const auto create_begin = addon::SteadyNowNs();
          {
            const serial::PresentTurn turn(true);
            Check(turn.owns(), "single feature Create obtains its serialized SDK turn");
            renodx::addons::dlss5::DirectCallScope direct;
            const auto allocated = allocate(&slot.parameters);
            identity["allocate_result"] = static_cast<uint32_t>(allocated);
            Check(NVSDK_NGX_SUCCEED(allocated) && slot.parameters != nullptr, "single feature private parameters");
            addon::SetNrCreateParameters(slot.parameters, physical_width, physical_height, physical_width, physical_height,
                create_flags, hdr_mode != 0, model_preset);
            submission::TrackUse(gpu.list.Get(), &feature);
            initialization_receipts[index] = submission::ObserveRecording(gpu.list.Get());
            const auto begin = addon::SteadyNowNs();
            const auto result = create(gpu.list.Get(), addon::kFeatureDlssNr, slot.parameters, &slot.handle);
            identity["cpu_create_ms"] = (addon::SteadyNowNs() - begin) / 1'000'000.;
            identity["create_result"] = static_cast<uint32_t>(result);
            identity["create_calls"] = 1;
            created[index] = NVSDK_NGX_SUCCEED(result) && slot.handle != nullptr;
            if (slot.handle != nullptr) submission::TrackUse(gpu.list.Get(), slot.handle);
          }
          submit_healthy(initialization_receipts[index]);
          identity["cpu_create_and_completion_ms"] = (addon::SteadyNowNs() - create_begin) / 1'000'000.;
          identity["handle"] = reinterpret_cast<uintptr_t>(slot.handle);
          identity["parameters"] = reinterpret_cast<uintptr_t>(slot.parameters);
          identity["status"] = created[index] ? "Passed" : "Failed";
          identity["initialization_receipt"] = {{"list", initialization_receipts[index]->list},
              {"generation", initialization_receipts[index]->generation}, {"closed", true}, {"submitted", true}, {"completed_healthy", true}};
          slot.via_core = true;
          slot.nr_preset = model_preset;
          slot.nr_hdr_mode = hdr_mode;
          slot.pending_reset = true;
          slot.pending_reset_reasons = addon::kResetCreated;
        }
        std::vector<uint16_t> cropped_input(static_cast<size_t>(small_width) * small_height * 4);
        for (UINT y = 0; y < small_height; ++y) {
          std::memcpy(cropped_input.data() + static_cast<size_t>(y) * small_width * 4,
              private_inputs[0].data() + static_cast<size_t>(y) * width * 4, small_width * 8);
        }
        Check(cropped_input == private_inputs[1], "true small feature receives exactly the maximum proxy's ROI bytes");
        report["input_contract"] = {{"roi_bytes_identical", true},
            {"roi_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(cropped_input.data()), cropped_input.size() * sizeof(uint16_t))},
            {"depth_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(depth_data.data()), depth_data.size() * sizeof(float))},
            {"motion_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(motion_data.data()), motion_data.size() * sizeof(uint16_t))},
            {"guides_shared_read_only", true}, {"motion_scale", {1.f, 1.f}}, {"jitter", "unset"}, {"origin", {0, 0}}};
        native_single_feature_dispatches.reserve(4096);
        real_dispatch = reinterpret_cast<DispatchFn>((*reinterpret_cast<void***>(gpu.list.Get()))[14]);
        Check(DetourTransactionBegin() == NO_ERROR, "single feature dispatch transaction begins");
        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR
            || DetourAttach(reinterpret_cast<PVOID*>(&real_dispatch), reinterpret_cast<PVOID>(ObserveDispatch)) != NO_ERROR) {
          DetourTransactionAbort();
          Check(false, "single feature dispatch observer attaches");
        }
        Check(DetourTransactionCommit() == NO_ERROR, "single feature dispatch observer commits");
        dispatch_attached = true;
        for (size_t phase = 0; phase < phase_names.size(); ++phase) {
          const size_t feature_index = (phase == 4 ? 1u : 0u);
          const size_t resource_index = phase_resource_indices[phase];
          auto& feature = private_features[feature_index];
          auto& slot = feature.slots[0];
          auto& stage = report["stages"][phase];
          if (!created[feature_index]) {
            stage["reason"] = "Feature Create failed; no Evaluate attempted";
            continue;
          }
          const bool use_small_dimensions = phase == 1 || phase == 2 || phase == 4;
          const UINT effective_width = use_small_dimensions ? small_width : width,
                     effective_height = use_small_dimensions ? small_height : height;
          const bool sync_private_dimensions = phase == 2 || phase == 3;
          stage["feature_index"] = feature_index;
          stage["resource_index"] = resource_index;
          stage["feature_handle"] = reinterpret_cast<uintptr_t>(slot.handle);
          stage["feature_create_size"] = {physical_widths[feature_index], physical_heights[feature_index]};
          stage["physical_size"] = {physical_widths[resource_index], physical_heights[resource_index]};
          stage["actualphysical_size"] = {physical_widths[resource_index], physical_heights[resource_index]};
          stage["proxy"] = reinterpret_cast<uintptr_t>(proxies[resource_index].Get());
          stage["output"] = reinterpret_cast<uintptr_t>(neural_outputs[resource_index].Get());
          stage["effective_size"] = {effective_width, effective_height};
          stage["private_width_height_request"] = sync_private_dimensions ? "explicitly synchronized"
              : "Create values reasserted before every Evaluate";
          stage["samples"] = nlohmann::json::array();
          stage["status"] = "Running";
          bool accepted_all = true;
          std::vector<double> measured_gpu;
          addon::FinalResources resources{.proxy = proxies[resource_index].Get(),
              .nr_output = neural_outputs[resource_index].Get(), .hdr_mode = hdr_mode};
          dispatch_stable[phase] = true;
          for (UINT iteration = 0; iteration < evaluations_per_stage; ++iteration) {
            const auto frame_begin = addon::SteadyNowNs();
            gpu.list->ClearState(nullptr);
            gpu.Transition(neural_outputs[resource_index].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            gpu.UploadTexture(neural_outputs[resource_index].Get(), output_transfers[resource_index],
                reinterpret_cast<const uint8_t*>(sentinel_inputs[resource_index].data()), sentinel_inputs[resource_index].size() * sizeof(uint16_t),
                physical_widths[resource_index] * 8);
            gpu.Transition(neural_outputs[resource_index].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const addon::EvaluationContract frame{.source_handle = owner, .motion = retained_motion.Get(), .depth = retained_depth.Get(),
                .input_width = render_width, .input_height = render_height, .output_width = effective_width, .output_height = effective_height,
                .motion_width = render_width, .motion_height = render_height, .create_flags = create_flags,
                .motion_scale_x = 1.f, .motion_scale_y = 1.f, .frame_reset = iteration < reset_prefix ? 1 : 0};
            NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
            std::shared_ptr<submission::RecordingStatus> receipt;
            const UINT interval = static_cast<UINT>(phase) * evaluations_per_stage + iteration;
            native_single_feature_dispatches.clear();
            native_single_feature_dispatch_overflow = 0;
            double cpu_evaluate_ms = 0.;
            {
              const serial::PresentTurn turn(true);
              Check(turn.owns(), "single feature Evaluate obtains its serialized SDK turn");
              renodx::addons::dlss5::DirectCallScope direct;
              addon::SetEvaluationParameters(feature, slot, 0, addon::history_reset_epoch.load(), resources, frame,
                  effective_width, effective_height, effective_width, effective_height);
              NVSDK_NGX_Parameter_SetUI(slot.parameters, "DLSSNR.Width",
                  sync_private_dimensions ? effective_width : physical_widths[feature_index]);
              NVSDK_NGX_Parameter_SetUI(slot.parameters, "DLSSNR.Height",
                  sync_private_dimensions ? effective_height : physical_heights[feature_index]);
              if (iteration == 0) {
                for (const auto* key : {NVSDK_NGX_Parameter_Width, NVSDK_NGX_Parameter_Height,
                         NVSDK_NGX_Parameter_OutWidth, NVSDK_NGX_Parameter_OutHeight, "DLSSNR.Width", "DLSSNR.Height",
                         "DLSSNR.InputWidth", "DLSSNR.InputHeight", "DLSSNR.OutputWidth", "DLSSNR.OutputHeight",
                         "DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight",
                         "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY", "DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight",
                         "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight", "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight"}) {
                  uint32_t value = 0;
                  const bool available = addon::GetUIntIfPresent(slot.parameters, key, &value);
                  stage["parameter_snapshot"][key] = {{"available", available}, {"value", value}};
                }
                stage["parameter_snapshot"]["DLSSNR.ScalingRatio"] = addon::GetFloat(slot.parameters, "DLSSNR.ScalingRatio", -1.f);
                stage["parameter_snapshot"]["DLSSNR.Scale"] = addon::GetFloat(slot.parameters, "DLSSNR.Scale", -1.f);
              }
              submission::TrackUse(gpu.list.Get(), slot.handle);
              submission::TrackUse(gpu.list.Get(), proxies[resource_index].Get());
              submission::TrackUse(gpu.list.Get(), neural_outputs[resource_index].Get());
              submission::TrackUse(gpu.list.Get(), retained_depth.Get());
              submission::TrackUse(gpu.list.Get(), retained_motion.Get());
              receipt = submission::ObserveRecording(gpu.list.Get());
              queries.Begin(gpu.list.Get(), interval);
              native_single_feature_dispatch_list = gpu.list.Get();
              const auto begin = addon::SteadyNowNs();
              result = evaluate(gpu.list.Get(), slot.handle, slot.parameters, nullptr);
              cpu_evaluate_ms = (addon::SteadyNowNs() - begin) / 1'000'000.;
              native_single_feature_dispatch_list = nullptr;
              queries.EndAndResolve(gpu.list.Get(), interval);
            }
            const auto recorded = addon::SteadyNowNs();
            stage["samples"].push_back({{"index", iteration}, {"scope", iteration < reset_prefix ? "reset-prefix" : "measurement"},
                {"result", static_cast<uint32_t>(result)}, {"api_accepted", NVSDK_NGX_SUCCEED(result)}, {"reset_sent", slot.model_reset},
                {"cpu_evaluate_ms", cpu_evaluate_ms}, {"cpu_frame_record_ms", (recorded - frame_begin) / 1'000'000.},
                {"gpu_completion", "NotRun"},
                {"dispatch_count", native_single_feature_dispatches.size()}, {"dispatch_dimensions", native_single_feature_dispatches},
                {"dispatch_overflow", native_single_feature_dispatch_overflow}});
            submit_healthy(receipt);
            const auto completed = addon::SteadyNowNs();
            stage["samples"].back()["gpu_completion"] = "Passed";
            const auto sample = queries.Read(interval);
            accepted_all &= NVSDK_NGX_SUCCEED(result);
            stage["samples"].back().update({{"cpu_submit_fence_ms", (completed - recorded) / 1'000'000.}, {"gpu_ms", sample.gpu_ms},
                {"begin_tick", sample.begin_tick}, {"end_tick", sample.end_tick},
                {"receipt", {{"list", receipt->list}, {"generation", receipt->generation}, {"closed", true}, {"submitted", true}, {"completed_healthy", true}}},
                {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()}});
            report["frames"].push_back({{"phase", phase_names[phase]}, {"sample", iteration}, {"result", static_cast<uint32_t>(result)},
                {"gpu_ms", sample.gpu_ms}});
            if (iteration >= reset_prefix && NVSDK_NGX_SUCCEED(result)) {
              if (measured_gpu.empty()) measured_dispatches[phase] = native_single_feature_dispatches;
              else dispatch_stable[phase] &= measured_dispatches[phase] == native_single_feature_dispatches;
              measured_gpu.push_back(sample.gpu_ms);
            }
            Check(native_single_feature_dispatch_overflow == 0, "bounded direct Dispatch observation retains every visible call");
            if (NVSDK_NGX_FAILED(result)) {
              stage["remaining_evaluations_not_run"] = evaluations_per_stage - iteration - 1;
              stage["stop_reason"] = "request rejected; do not repeat the failed condition; continue the next distinct request on the healthy device";
              break;
            }
          }
          if (measured_gpu.empty()) {
            stage["gpu_measurement"] = {{"status", "NotRun"}, {"samples", 0}};
          } else {
            std::sort(measured_gpu.begin(), measured_gpu.end());
            double sum = 0.;
            for (const auto value : measured_gpu) sum += value;
            gpu_medians[phase] = (measured_gpu[(measured_gpu.size() - 1) / 2] + measured_gpu[measured_gpu.size() / 2]) / 2.;
            stage["gpu_measurement"] = {{"status", "Measured"}, {"samples", measured_gpu.size()}, {"minimum_ms", measured_gpu.front()},
                {"median_ms", gpu_medians[phase]}, {"mean_ms", sum / measured_gpu.size()}, {"maximum_ms", measured_gpu.back()}};
          }
          stage["dispatch_signature_stable"] = dispatch_stable[phase];
          stage["dispatch_visibility"] = measured_dispatches[phase].empty() ? "Unknown" : "direct Dispatch observed";
          gpu.Transition(neural_outputs[resource_index].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          gpu.RecordTextureReadback(neural_outputs[resource_index].Get(), output_transfers[resource_index]);
          gpu.Transition(neural_outputs[resource_index].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          submit_healthy(nullptr);
          stage_pixels[phase] = gpu.ReadTexture(output_transfers[resource_index]);
          std::array<uint64_t, 4> roi_modified{}, outside_modified{}, roi_nonfinite{}, outside_nonfinite{};
          uint64_t roi_rgb_modified = 0, outside_rgb_modified = 0;
          UINT min_x = physical_widths[resource_index], min_y = physical_heights[resource_index], max_x = 0, max_y = 0;
          for (UINT y = 0; y < physical_heights[resource_index]; ++y) {
            for (UINT x = 0; x < physical_widths[resource_index]; ++x) {
              const bool roi = x < effective_width && y < effective_height;
              bool rgb_changed = false;
              for (size_t channel = 0; channel < 4; ++channel) {
                uint16_t bits = 0;
                std::memcpy(&bits, stage_pixels[phase].data()
                    + ((static_cast<size_t>(y) * physical_widths[resource_index] + x) * 4 + channel) * sizeof(uint16_t), sizeof(bits));
                const bool changed = bits != sentinel[channel];
                (roi ? roi_modified : outside_modified)[channel] += changed;
                (roi ? roi_nonfinite : outside_nonfinite)[channel] += !std::isfinite(DirectX::PackedVector::XMConvertHalfToFloat(bits));
                if (channel < 3) rgb_changed |= changed;
              }
              if (rgb_changed) {
                if (roi) ++roi_rgb_modified; else ++outside_rgb_modified;
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                max_x = std::max(max_x, x + 1);
                max_y = std::max(max_y, y + 1);
              }
            }
          }
          const bool finite_rgb = roi_nonfinite[0] + roi_nonfinite[1] + roi_nonfinite[2]
              + outside_nonfinite[0] + outside_nonfinite[1] + outside_nonfinite[2] == 0;
          stage["pixels"] = {{"channel_order", "RGBA"}, {"roi_modified_channels", roi_modified}, {"outside_modified_channels", outside_modified},
              {"roi_nonfinite_channels", roi_nonfinite}, {"outside_nonfinite_channels", outside_nonfinite},
              {"roi_rgb_modified_pixels", roi_rgb_modified}, {"outside_rgb_modified_pixels", outside_rgb_modified},
              {"roi_pixels", static_cast<uint64_t>(effective_width) * effective_height},
              {"outside_pixels", static_cast<uint64_t>(physical_widths[resource_index]) * physical_heights[resource_index]
                  - static_cast<uint64_t>(effective_width) * effective_height},
              {"crc32", renodx::utils::hash::ComputeCRC32(stage_pixels[phase].data(), stage_pixels[phase].size())},
              {"rgb_write_bbox", max_x == 0 ? nlohmann::json(nullptr) : nlohmann::json{min_x, min_y, max_x, max_y}},
              {"scope_status", !accepted_all ? "Inconclusive" : !finite_rgb ? "Failed" : outside_rgb_modified != 0 ? "Blocked"
                  : roi_rgb_modified == static_cast<uint64_t>(effective_width) * effective_height ? "Passed" : "Inconclusive"}};
          stage["status"] = accepted_all ? "Passed" : "Failed";
        }
        const auto compare_pixels = [&](size_t first, size_t second, UINT compare_width, UINT compare_height) {
          nlohmann::json comparison{{"status", "NotRun"}, {"channel_order", "RGBA"}};
          if (stage_pixels[first].empty() || stage_pixels[second].empty()) return comparison;
          const UINT first_stride = physical_widths[phase_resource_indices[first]],
                     second_stride = physical_widths[phase_resource_indices[second]];
          std::array<double, 4> absolute_sum{}, squared_sum{}, maximum{};
          std::array<uint64_t, 4> finite_pairs{};
          for (UINT y = 0; y < compare_height; ++y) {
            for (UINT x = 0; x < compare_width; ++x) {
              for (size_t channel = 0; channel < 4; ++channel) {
                uint16_t a = 0, b = 0;
                std::memcpy(&a, stage_pixels[first].data() + ((static_cast<size_t>(y) * first_stride + x) * 4 + channel) * sizeof(uint16_t), sizeof(a));
                std::memcpy(&b, stage_pixels[second].data() + ((static_cast<size_t>(y) * second_stride + x) * 4 + channel) * sizeof(uint16_t), sizeof(b));
                const float a_value = DirectX::PackedVector::XMConvertHalfToFloat(a), b_value = DirectX::PackedVector::XMConvertHalfToFloat(b);
                if (!std::isfinite(a_value) || !std::isfinite(b_value)) continue;
                const double difference = static_cast<double>(a_value) - b_value;
                absolute_sum[channel] += std::abs(difference);
                squared_sum[channel] += difference * difference;
                maximum[channel] = std::max(maximum[channel], std::abs(difference));
                ++finite_pairs[channel];
              }
            }
          }
          nlohmann::json mae = nlohmann::json::array(), rmse = nlohmann::json::array(), max_absolute = nlohmann::json::array();
          for (size_t channel = 0; channel < 4; ++channel) {
            mae.push_back(finite_pairs[channel] == 0 ? nlohmann::json(nullptr) : nlohmann::json(absolute_sum[channel] / finite_pairs[channel]));
            rmse.push_back(finite_pairs[channel] == 0 ? nlohmann::json(nullptr) : nlohmann::json(std::sqrt(squared_sum[channel] / finite_pairs[channel])));
            max_absolute.push_back(finite_pairs[channel] == 0 ? nlohmann::json(nullptr) : nlohmann::json(maximum[channel]));
          }
          comparison.update({{"status", report["stages"][first]["status"] == "Passed"
                  && report["stages"][second]["status"] == "Passed" ? "Measured" : "Inconclusive"},
              {"size", {compare_width, compare_height}}, {"finite_pairs", finite_pairs},
              {"mae", mae}, {"rmse", rmse}, {"maximum_absolute_difference", max_absolute},
              {"quality_threshold", "none; numerical comparison only"}});
          return comparison;
        };
        report["numeric_comparisons"] = {{"full_roi_vs_true_small", compare_pixels(0, 4, small_width, small_height)},
            {"standard_small_vs_true_small", compare_pixels(1, 4, small_width, small_height)},
            {"private_small_vs_true_small", compare_pixels(2, 4, small_width, small_height)},
            {"restored_full_vs_baseline", compare_pixels(3, 0, width, height)}};
        for (const size_t phase : {size_t{1}, size_t{2}}) {
          auto& stage = report["stages"][phase];
          if (stage["status"] == "NotRun") continue;
          const bool signatures_observed = !measured_dispatches[0].empty() && !measured_dispatches[phase].empty();
          stage["work_comparison"] = {{"dispatch_changed_from_full", signatures_observed && measured_dispatches[phase] != measured_dispatches[0]},
              {"dispatch_comparison_status", signatures_observed && dispatch_stable[0] && dispatch_stable[phase] ? "Measured" : "Inconclusive"},
              {"gpu_median_ratio_to_full", stage["gpu_measurement"].value("samples", size_t{0}) != 0
                      && report["stages"][0].value("gpu_measurement", nlohmann::json::object()).value("samples", size_t{0}) != 0
                      && gpu_medians[0] > 0.
                  ? nlohmann::json(gpu_medians[phase] / gpu_medians[0]) : nlohmann::json(nullptr)},
              {"gpu_median_ratio_to_true_small", stage["gpu_measurement"].value("samples", size_t{0}) != 0
                      && report["stages"][4].value("gpu_measurement", nlohmann::json::object()).value("samples", size_t{0}) != 0
                      && gpu_medians[4] > 0.
                  ? nlohmann::json(gpu_medians[phase] / gpu_medians[4]) : nlohmann::json(nullptr)}};
          stage["variable_work_resolution"] = stage["pixels"]["scope_status"] == "Failed" ? "Failed"
              : stage["status"] == "Failed" || stage["pixels"]["scope_status"] == "Blocked" ? "Blocked" : "Inconclusive";
          stage["conclusion_scope"] = "API, RGB write coverage, numeric error, visible Dispatch and GPU cost are independent evidence; internal dimensions remain unknown";
        }
        report["full_restore"] = {{"feature_handle_unchanged", private_features[0].slots[0].handle
                == reinterpret_cast<NVSDK_NGX_Handle*>(report["features"][0]["handle"].get<uintptr_t>())},
            {"status", report["stages"][3]["status"]}, {"pixel_scope", report["stages"][3].value("pixels", nlohmann::json::object()).value("scope_status", "NotRun")}};
        report["dispatch_observer_detached"] = detach_dispatch();
        Check(report["dispatch_observer_detached"].get<bool>(), "single feature dispatch observer detaches");
        native_runtime_results.clear();
        native_runtime_masks.clear();
        native_runtime_resets.clear();
        gpu.list->ClearState(nullptr);
        addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(ObserveNativeHostEvaluate);
        const auto resumed = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
        addon::ngx_slot_real[0].evaluate = nullptr;
        report["production_chain_resumed"] = {{"sr_result", static_cast<uint32_t>(resumed)}, {"nr_results", native_runtime_results},
            {"gpu_completion", "NotRun"}};
        submit_healthy(nullptr);
        report["production_chain_resumed"].update({{"gpu_completion", "Passed"},
            {"nr_handle_unchanged", addon::features.at(owner).slots[0].handle == production_nr},
            {"workset_unchanged", addon::worksets.at({owner, output.Get(), false}).id == production_workset}});
        Check(NVSDK_NGX_SUCCEED(resumed) && native_runtime_results.size() == 1 && NVSDK_NGX_SUCCEED(native_runtime_results[0])
                  && addon::features.at(owner).slots[0].handle == production_nr
                  && addon::worksets.at({owner, output.Get(), false}).id == production_workset,
              "production SR and original NR remain available after the private size probe");
        Check(cleanup_private(), "single feature probe releases completed private features and parameters");
        report["probe_execution"] = "Passed";
        report["contract_execution_scope"] = "API acceptance and healthy submitted execution; pixel and work findings are separate";
        report["contract_execution"] = std::all_of(report["stages"].begin(), report["stages"].end(),
            [](const auto& stage) { return stage["status"] == "Passed"; }) ? "Passed" : "Failed";
      } catch (...) {
        const auto failure = std::current_exception();
        native_single_feature_dispatch_list = nullptr;
        report["dispatch_observer_detached"] = detach_dispatch();
        addon::ngx_slot_real[0].evaluate = nullptr;
        cleanup_private();
        report["probe_execution"] = "Failed";
        for (auto& stage : report["stages"]) {
          if (stage["status"] == "Running") stage["status"] = "Failed";
        }
        for (auto& feature : report["features"]) {
          if (feature["status"] == "Running") feature["status"] = "Failed";
        }
        try {
          std::rethrow_exception(failure);
        } catch (const std::exception& error) {
          report["failure"] = error.what();
        } catch (...) {
          report["failure"] = "non-standard exception";
        }
        std::ofstream file("adaptive-native-single-feature.json", std::ios::binary | std::ios::trunc);
        file << report.dump(2) << '\n';
        file.close();
        std::rethrow_exception(failure);
      }
      std::ofstream file("adaptive-native-single-feature.json", std::ios::binary | std::ios::trunc);
      file << report.dump(2) << '\n';
      file.close();
      Check(!file.fail(), "single feature probe checkpoint saved before source teardown");
    } else if (cold_prepare) {
      namespace serial = renodx::addons::dlss5::ngx_serial;
      constexpr UINT candidate_width = 960, candidate_height = 540;
      const auto create = addon::core_create_feature;
      const auto allocate = addon::core_allocate_parameters;
      const auto destroy = addon::core_destroy_parameters;
      const auto release = addon::core_release_feature;
      const auto evaluate = native_runtime_evaluate;
      const auto& original_feature = addon::features.at(owner);
      const auto original_nr = original_feature.slots[0].handle;
      const auto original_parameters = original_feature.slots[0].parameters;
      const auto original_workset_id = addon::worksets.at({owner, output.Get(), false}).id;
      const auto original_proxy = addon::worksets.at({owner, output.Get(), false}).proxy;
      const uint32_t create_flags = original_feature.create_flags;
      const uint32_t model_preset = original_feature.slots[0].nr_preset;
      const uint8_t hdr_mode = original_feature.slots[0].nr_hdr_mode;
      Check(create != nullptr && allocate != nullptr && destroy != nullptr && release != nullptr && evaluate != nullptr,
            "cold prepare snapshots a complete initialized Core provider");
      auto& report = resize_report;
      report = {{"schema", "nr-nexus-native-cold-prepare-v1"}, {"evidence", "NATIVE_GPU+NATIVE_NGX"},
          {"mode", "serialized-worker-probe"}, {"production_async_enabled", false}, {"p1_enabled", false},
          {"purpose", "private worker Create without runtime_mutex; independent D3D progress and same-Core SR wait"},
          {"host_sr", "NATIVE_NGX 1280x720 to 1920x1080"}, {"stage", "Upscaled"}, {"passes", 1}, {"colors", 1},
          {"runtime", addon::direct_runtime_requirements.file_version}, {"nr_sha256", addon::direct_runtime_sha256},
          {"requested_model", model_preset}, {"internal_model", "unknown"}, {"format", "RGBA16F"},
          {"candidate_size", {candidate_width, candidate_height}}, {"source_size", {width, height}},
          {"guide_size", {render_width, render_height}}, {"create_flags", create_flags}, {"hdr_mode", hdr_mode},
          {"timing_clock", "SteadyNowNs; CPU nanoseconds with a common steady-clock epoch"},
          {"timing_scope", "source starts are unpaced headless calls, not game/display FPS; explicit serialized SDK gate"},
          {"main_thread_id", GetCurrentThreadId()}, {"teardown_status", "NotRun"},
          {"frames", nlohmann::json::array()}};
      ComPtr<IDXGIAdapter3> adapter;
      Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))),
            "cold prepare adapter");
      DXGI_ADAPTER_DESC adapter_desc{};
      Check(SUCCEEDED(adapter->GetDesc(&adapter_desc)), "cold prepare adapter identity");
      report["adapter"] = std::filesystem::path(adapter_desc.Description).string();
      LARGE_INTEGER driver_version{};
      report["driver_status"] = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver_version);
      report["driver_version_raw"] = driver_version.QuadPart;
      report["input_crc32"] = {{"color", renodx::utils::hash::ComputeCRC32(
          reinterpret_cast<const uint8_t*>(color_data.data()), color_data.size() * sizeof(uint16_t))},
          {"depth", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(depth_data.data()), depth_data.size() * sizeof(float))},
          {"motion", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(motion_data.data()), motion_data.size() * sizeof(uint16_t))}};
      auto queries = d3d12_test::CreateTimestampQueries(gpu, 17);
      ComPtr<ID3D12CommandAllocator> worker_allocator;
      ComPtr<ID3D12GraphicsCommandList> worker_list;
      ComPtr<ID3D12Fence> worker_fence;
      ComPtr<ID3D12Resource> candidate_proxy, candidate_output;
      ComPtr<ID3D12Resource> retained_depth = depth, retained_motion = motion;
      addon::FeatureState candidate_state;
      candidate_state.create_flags = create_flags;
      auto& candidate_slot = candidate_state.slots[0];
      std::shared_ptr<submission::RecordingStatus> create_receipt, evaluate_receipt;
      std::atomic_bool worker_sdk_active{false}, worker_finished{false};
      std::atomic_int64_t create_entered_ns{0};
      std::exception_ptr worker_error;
      nlohmann::json worker_report;
      bool worker_submitted = false, worker_completed = false;
      bool cleanup_attempted = false;
      std::jthread worker;
      native_cold_worker_sdk_active = &worker_sdk_active;
      native_cold_serial_timeouts_before = serial::serial_timeouts.load();
      native_cold_sdk_entry_refused = false;
      const auto overlap_before = serial::overlap.load();
      const auto runtime_contentions_before = renodx::addons::dlss5::runtime_lock_contended.load();
      const auto submit_contentions_before = renodx::addons::dlss5::submit_lock_blocked.load();
      const auto evaluate_source = [&](const char* phase) {
        native_runtime_masks.clear();
        native_runtime_results.clear();
        native_runtime_resets.clear();
        native_resize_host_ms = native_resize_evaluate_ms = 0.;
        native_cold_sr_enter_ns = 0;
        native_cold_sr_runtime_locked = false;
        native_cold_sr_ngx_depth = 0;
        gpu.list->ClearState(nullptr);
        native_cost_interval = static_cast<UINT>(report["frames"].size()) * 3;
        native_cost_queries = &queries;
        native_cost_list = gpu.list.Get();
        queries.Begin(gpu.list.Get(), native_cost_interval);
        const auto source_start = addon::SteadyNowNs();
        int64_t turn_acquired = 0, recorded = 0;
        const auto sr_calls_before = native_motion_sr_calls;
        const auto source_successes_before = addon::SourceSuccessfulEvaluations();
        NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
        {
          // The gate wait is outside runtime_mutex and remains separately
          // visible from CPU recording in the existing production wrapper.
          const serial::Call turn;
          turn_acquired = addon::SteadyNowNs();
          if (worker_sdk_active.load(std::memory_order_acquire)
              || serial::serial_timeouts.load() != native_cold_serial_timeouts_before) {
            native_cold_sdk_entry_refused = true;
          } else {
            parameters.Set(NVSDK_NGX_Parameter_Reset, 0);
            addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(ObserveNativeHostEvaluate);
            result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
            addon::ngx_slot_real[0].evaluate = nullptr;
          }
          recorded = addon::SteadyNowNs();
        }
        native_cost_queries = nullptr;
        gpu.list->EndQuery(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * native_cost_interval + 1);
        gpu.Retain(queries.readback.Get());
        gpu.list->ResolveQueryData(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            2 * native_cost_interval, 2, queries.readback.Get(), 2ull * native_cost_interval * sizeof(uint64_t));
        if (!native_runtime_results.empty()) {
          gpu.list->ResolveQueryData(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
              2 * (native_cost_interval + 1), 2, queries.readback.Get(), 2ull * (native_cost_interval + 1) * sizeof(uint64_t));
        }
        if (native_motion_sr_calls != sr_calls_before) {
          gpu.list->ResolveQueryData(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
              2 * (native_cost_interval + 2), 2, queries.readback.Get(), 2ull * (native_cost_interval + 2) * sizeof(uint64_t));
        }
        const auto submitted = addon::SteadyNowNs();
        gpu.Submit();
        const auto completed = addon::SteadyNowNs();
        Check(gpu.fence->GetCompletedValue() >= gpu.serial && gpu.fence->GetCompletedValue() != UINT64_MAX
                  && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
              "cold source submission completes on a healthy device");
        addon::DrainRetiredResources(true);
        const auto& active_feature = addon::features.at(owner);
        const auto& workset = addon::worksets.at({owner, output.Get(), false});
        const bool identities_preserved = active_feature.slots[0].handle == original_nr
            && active_feature.slots[0].parameters == original_parameters && workset.id == original_workset_id
            && workset.proxy == original_proxy;
        const auto previous_start = (report["frames"].empty() ? source_start
            : report["frames"].back()["source_start_ns"].get<int64_t>());
        report["frames"].push_back({{"phase", phase}, {"source_start_ns", source_start}, {"turn_acquired_ns", turn_acquired},
            {"sr_enter_ns", native_cold_sr_enter_ns}, {"recorded_ns", recorded}, {"completed_ns", completed},
            {"sr_runtime_mutex_held", native_cold_sr_runtime_locked}, {"sr_ngx_call_depth", native_cold_sr_ngx_depth},
            {"source_start_interval_ms", (source_start - previous_start) / 1'000'000.},
            {"cpu_turn_wait_ms", (turn_acquired - source_start) / 1'000'000.},
            {"cpu_wrapper_after_turn_ms", (recorded - turn_acquired) / 1'000'000.},
            {"cpu_wrapper_including_turn_ms", (recorded - source_start) / 1'000'000.},
            {"cpu_host_sr_ms", native_resize_host_ms}, {"cpu_nr_evaluate_ms", native_resize_evaluate_ms},
            {"cpu_submit_fence_ms", (completed - submitted) / 1'000'000.},
            {"cpu_complete_from_start_ms", (completed - source_start) / 1'000'000.},
            {"host_result", static_cast<uint32_t>(result)}, {"nr_results", native_runtime_results},
            {"nr_reset_parameters", native_runtime_resets},
            {"actual_sr_calls", native_motion_sr_calls - sr_calls_before}, {"actual_nr_calls", native_runtime_results.size()},
            {"source_successes_delta", addon::SourceSuccessfulEvaluations() - source_successes_before},
            {"old_chain_preserved", identities_preserved}, {"sr_handle", reinterpret_cast<uintptr_t>(owner)},
            {"nr_handle", reinterpret_cast<uintptr_t>(original_nr)}, {"workset_id", workset.id},
            {"nr_acknowledged_reset_epoch", active_feature.slots[0].acknowledged_reset_epoch},
            {"global_reset_epoch", addon::history_reset_epoch.load()},
            {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()},
            {"sdk_entry_refused", native_cold_sdk_entry_refused}});
        Check(!native_cold_sdk_entry_refused && NVSDK_NGX_SUCCEED(result) && native_motion_sr_calls == sr_calls_before + 1
                  && native_runtime_results.size() == 1 && NVSDK_NGX_SUCCEED(native_runtime_results[0])
                  && addon::SourceSuccessfulEvaluations() == source_successes_before + 1 && identities_preserved,
              "cold prepare keeps one real SR and the original active NR successful without SDK overlap");
        report["frames"].back()["gpu_wrapper_ms"] = queries.Read(native_cost_interval).gpu_ms;
        report["frames"].back()["gpu_nr_ms"] = queries.Read(native_cost_interval + 1).gpu_ms;
        report["frames"].back()["gpu_sr_ms"] = queries.Read(native_cost_interval + 2).gpu_ms;
      };
      const auto cleanup_candidate = [&] {
        if (cleanup_attempted) return report["private_cleanup"].value("completed", false);
        cleanup_attempted = true;
        // On failure, discard only unsubmitted recordings. Unknown submitted
        // work keeps its handle, parameters and GPU references until process exit.
        bool safe = !gpu.submission_pending;
        if (worker_submitted && !worker_completed) {
          safe &= worker_fence != nullptr && worker_fence->GetCompletedValue() >= 1
              && worker_fence->GetCompletedValue() != UINT64_MAX && SUCCEEDED(gpu.device->GetDeviceRemovedReason());
        }
        if (safe) {
          if (worker_list != nullptr) {
            worker_list->Close();
            const auto allocator_reset = worker_allocator->Reset();
            const auto list_reset = worker_list->Reset(worker_allocator.Get(), nullptr);
            safe &= SUCCEEDED(allocator_reset) && SUCCEEDED(list_reset);
            if (safe) submission::OnCommandListReset(worker_list.Get());
          }
          gpu.list->Close();
          const auto allocator_reset = gpu.allocator->Reset();
          const auto list_reset = gpu.list->Reset(gpu.allocator.Get(), nullptr);
          safe &= SUCCEEDED(allocator_reset) && SUCCEEDED(list_reset);
          if (safe) submission::OnCommandListReset(gpu.list.Get());
          submission::PruneCompletedGenerations();
          safe &= submission::ResourceReleasable(candidate_slot.handle)
              && submission::ResourceReleasable(candidate_proxy.Get())
              && submission::ResourceReleasable(candidate_output.Get());
        }
        auto& cleanup = report["private_cleanup"];
        cleanup["recordings_safe"] = safe;
        try {
          if (safe) {
            const serial::PresentTurn turn(true);
            safe = turn.owns();
            cleanup["serialized_turn"] = safe;
            if (safe) {
              renodx::addons::dlss5::DirectCallScope direct;
              if (candidate_slot.handle != nullptr) {
                const auto result = release(candidate_slot.handle);
                cleanup["release_result"] = static_cast<uint32_t>(result);
                safe &= NVSDK_NGX_SUCCEED(result);
                if (NVSDK_NGX_SUCCEED(result)) candidate_slot.handle = nullptr;
              }
              if (safe && candidate_slot.parameters != nullptr) {
                const auto result = destroy(candidate_slot.parameters);
                cleanup["destroy_parameters_result"] = static_cast<uint32_t>(result);
                safe &= NVSDK_NGX_SUCCEED(result);
                if (NVSDK_NGX_SUCCEED(result)) candidate_slot.parameters = nullptr;
              }
            }
          }
        } catch (...) {
          cleanup["sdk_exception"] = true;
          safe = false;
        }
        cleanup["completed"] = safe;
        if (!safe) {
          candidate_proxy.Detach();
          candidate_output.Detach();
          retained_depth.Detach();
          retained_motion.Detach();
          worker_allocator.Detach();
          worker_list.Detach();
          worker_fence.Detach();
        }
        return safe;
      };
      try {
        for (uint32_t frame = 0; frame < 3; ++frame) evaluate_source("baseline");
        const auto resources_start = addon::SteadyNowNs();
        candidate_proxy = gpu.Texture(candidate_width, candidate_height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto candidate_desc = candidate_proxy->GetDesc();
        candidate_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Check(SUCCEEDED(gpu.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &candidate_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&candidate_output))), "cold prepare private output");
        auto candidate_transfer = gpu.CreateTextureTransfer(candidate_proxy.Get());
        std::vector<uint16_t> candidate_data(static_cast<size_t>(candidate_width) * candidate_height * 4);
        for (UINT y = 0; y < candidate_height; ++y) {
          for (UINT x = 0; x < candidate_width; ++x) {
            const size_t source = (static_cast<size_t>(y) * render_height / candidate_height * render_width
                + static_cast<size_t>(x) * render_width / candidate_width) * 4;
            std::memcpy(candidate_data.data() + (static_cast<size_t>(y) * candidate_width + x) * 4,
                color_data.data() + source, 4 * sizeof(uint16_t));
          }
        }
        gpu.Transition(candidate_proxy.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.UploadTexture(candidate_proxy.Get(), candidate_transfer, reinterpret_cast<const uint8_t*>(candidate_data.data()),
            candidate_data.size() * sizeof(uint16_t), candidate_width * 8);
        gpu.Transition(candidate_proxy.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gpu.Submit();
        report["private_resources"] = {{"cpu_prepare_and_upload_ms", (addon::SteadyNowNs() - resources_start) / 1'000'000.},
            {"scope", "main-thread probe setup before worker starts; not worker preparation savings"},
            {"proxy", reinterpret_cast<uintptr_t>(candidate_proxy.Get())}, {"output", reinterpret_cast<uintptr_t>(candidate_output.Get())},
            {"depth", reinterpret_cast<uintptr_t>(retained_depth.Get())}, {"motion", reinterpret_cast<uintptr_t>(retained_motion.Get())},
            {"input_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(candidate_data.data()), candidate_data.size() * sizeof(uint16_t))},
            {"production_workset_added", false}};
        auto marker_upload = gpu.Buffer(sizeof(uint32_t), D3D12_HEAP_TYPE_UPLOAD);
        auto marker_readback = gpu.Buffer(sizeof(uint32_t), D3D12_HEAP_TYPE_READBACK);
        uint32_t* mapped = nullptr;
        Check(SUCCEEDED(marker_upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "cold prepare marker upload map");
        *mapped = 0x4E525750;
        marker_upload->Unmap(0, nullptr);
        worker = std::jthread([&] {
          try {
            worker_report["thread_id"] = GetCurrentThreadId();
            const auto resources_begin = addon::SteadyNowNs();
            const auto allocator_result = gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&worker_allocator));
            worker_report["allocator_result"] = allocator_result;
            Check(SUCCEEDED(allocator_result), "cold worker allocator");
            const auto list_result = gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                worker_allocator.Get(), nullptr, IID_PPV_ARGS(&worker_list));
            worker_report["list_result"] = list_result;
            Check(SUCCEEDED(list_result), "cold worker list");
            const auto fence_result = gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&worker_fence));
            worker_report["fence_result"] = fence_result;
            Check(SUCCEEDED(fence_result), "cold worker fence");
            worker_report["cpu_d3d_recording_resources_ms"] = (addon::SteadyNowNs() - resources_begin) / 1'000'000.;
            {
              const auto turn_begin = addon::SteadyNowNs();
              const serial::PresentTurn turn(true);
              worker_report["cpu_turn_acquire_ms"] = (addon::SteadyNowNs() - turn_begin) / 1'000'000.;
              worker_report["turn_acquired"] = turn.owns();
              Check(turn.owns(), "cold worker obtains its exclusive SDK turn");
              renodx::addons::dlss5::DirectCallScope direct;
              worker_sdk_active.store(true, std::memory_order_release);
              const auto allocate_begin = addon::SteadyNowNs();
              const auto allocated = allocate(&candidate_slot.parameters);
              worker_report["allocate_result"] = static_cast<uint32_t>(allocated);
              Check(NVSDK_NGX_SUCCEED(allocated) && candidate_slot.parameters != nullptr, "cold worker private parameters");
              addon::SetNrCreateParameters(candidate_slot.parameters, candidate_width, candidate_height,
                  candidate_width, candidate_height, create_flags, hdr_mode != 0, model_preset);
              worker_report["cpu_parameters_ms"] = (addon::SteadyNowNs() - allocate_begin) / 1'000'000.;
              worker_report["runtime_mutex_held"] = renodx::addons::dlss5::HoldingRuntimeLock();
              worker_report["ngx_call_depth"] = serial::depth;
              worker_report["create_calls"] = 1;
              const auto begin = addon::SteadyNowNs();
              create_entered_ns.store(begin, std::memory_order_release);
              const auto created = create(worker_list.Get(), addon::kFeatureDlssNr, candidate_slot.parameters, &candidate_slot.handle);
              const auto end = addon::SteadyNowNs();
              worker_sdk_active.store(false, std::memory_order_release);
              worker_report["create_begin_ns"] = begin;
              worker_report["create_end_ns"] = end;
              worker_report["cpu_create_ms"] = (end - begin) / 1'000'000.;
              worker_report["create_result"] = static_cast<uint32_t>(created);
              if (candidate_slot.handle != nullptr) {
                submission::TrackUse(worker_list.Get(), candidate_slot.handle);
                create_receipt = submission::ObserveRecording(worker_list.Get());
              }
              Check(NVSDK_NGX_SUCCEED(created) && candidate_slot.handle != nullptr, "cold worker Feature 18 Create");
              const auto closed = worker_list->Close();
              worker_report["close_result"] = closed;
              Check(SUCCEEDED(closed), "cold worker closes initialization list");
              ID3D12CommandList* lists[]{worker_list.Get()};
              const auto pending = submission::BeginSubmission(1, lists);
              worker_submitted = true;
              gpu.queue->ExecuteCommandLists(1, lists);
              const auto signaled = gpu.queue->Signal(worker_fence.Get(), 1);
              worker_report["signal_result"] = signaled;
              submission::CompleteSubmission(pending, SUCCEEDED(signaled) ? worker_fence.Get() : nullptr, 1);
              Check(SUCCEEDED(signaled), "cold worker signals initialization proof");
            }
            worker_report["turn_released_ns"] = addon::SteadyNowNs();
            const auto wait_begin = addon::SteadyNowNs();
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            Check(event != nullptr, "cold worker completion event");
            const auto armed = worker_fence->SetEventOnCompletion(1, event);
            const auto waited = SUCCEEDED(armed) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
            CloseHandle(event);
            worker_report["set_event_result"] = armed;
            worker_report["wait_result"] = waited;
            worker_report["cpu_fence_wait_ms"] = (addon::SteadyNowNs() - wait_begin) / 1'000'000.;
            worker_completed = waited == WAIT_OBJECT_0 && worker_fence->GetCompletedValue() >= 1
                && worker_fence->GetCompletedValue() != UINT64_MAX && SUCCEEDED(gpu.device->GetDeviceRemovedReason());
            worker_report["completed_fence"] = worker_fence->GetCompletedValue();
            worker_report["device_removed_reason"] = gpu.device->GetDeviceRemovedReason();
            worker_report["initialization_completed_healthy"] = worker_completed;
            worker_report["gpu_completed_ns"] = addon::SteadyNowNs();
            worker_report["production_ready_state_changed"] = false;
            Check(worker_completed, "cold worker initialization GPU completes on a healthy device");
            const auto allocator_reset = worker_allocator->Reset();
            const auto list_reset = worker_list->Reset(worker_allocator.Get(), nullptr);
            worker_report["allocator_reset_result"] = allocator_reset;
            worker_report["list_reset_result"] = list_reset;
            Check(SUCCEEDED(allocator_reset) && SUCCEEDED(list_reset), "cold worker closes replay lifetime");
            submission::OnCommandListReset(worker_list.Get());
            submission::PruneCompletedGenerations();
          } catch (...) {
            worker_error = std::current_exception();
            worker_sdk_active.store(false, std::memory_order_release);
          }
          worker_finished.store(true, std::memory_order_release);
        });
        const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (create_entered_ns.load(std::memory_order_acquire) == 0 && !worker_finished.load(std::memory_order_acquire)
            && std::chrono::steady_clock::now() < ready_deadline) std::this_thread::yield();
        Check(create_entered_ns.load(std::memory_order_acquire) != 0, "cold worker reaches its bounded Create observation point");
        const auto marker_begin = addon::SteadyNowNs();
        queries.Begin(gpu.list.Get(), 15);
        gpu.Retain(marker_upload.Get());
        gpu.Retain(marker_readback.Get());
        gpu.list->CopyBufferRegion(marker_readback.Get(), 0, marker_upload.Get(), 0, sizeof(uint32_t));
        queries.EndAndResolve(gpu.list.Get(), 15);
        const auto marker_recorded = addon::SteadyNowNs();
        gpu.Submit();
        const auto marker_complete = addon::SteadyNowNs();
        Check(gpu.fence->GetCompletedValue() >= gpu.serial && gpu.fence->GetCompletedValue() != UINT64_MAX
                  && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
              "cold marker submission completes on a healthy device");
        Check(SUCCEEDED(marker_readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "cold marker completed readback");
        const auto marker_value = *mapped;
        marker_readback->Unmap(0, nullptr);
        report["marker"] = {{"begin_ns", marker_begin}, {"complete_ns", marker_complete},
            {"recorded_ns", marker_recorded}, {"cpu_record_ms", (marker_recorded - marker_begin) / 1'000'000.},
            {"cpu_submit_fence_ms", (marker_complete - marker_recorded) / 1'000'000.},
            {"cpu_record_submit_fence_ms", (marker_complete - marker_begin) / 1'000'000.},
            {"gpu_ms", queries.Read(15).gpu_ms}, {"value", marker_value}, {"expected_value", 0x4E525750}};
        Check(marker_value == 0x4E525750, "independent D3D marker copy completes correctly");
        evaluate_source("worker-create");
        worker.join();
        report["worker"] = worker_report;
        if (worker_error != nullptr) std::rethrow_exception(worker_error);
        {
          std::lock_guard lock(submission::tracker_mutex);
          Check(create_receipt != nullptr && create_receipt->closed.load() && create_receipt->submitted.load()
                    && create_receipt->completed_healthy, "cold candidate has exact completed initialization proof");
          report["worker"]["receipt"] = {{"list", create_receipt->list}, {"generation", create_receipt->generation},
              {"closed", create_receipt->closed.load()}, {"submitted", create_receipt->submitted.load()},
              {"completed_healthy", create_receipt->completed_healthy}};
        }
        report["worker"]["handle"] = reinterpret_cast<uintptr_t>(candidate_slot.handle);
        report["worker"]["parameters"] = reinterpret_cast<uintptr_t>(candidate_slot.parameters);
        report["worker"]["list"] = reinterpret_cast<uintptr_t>(worker_list.Get());
        report["worker"]["allocator"] = reinterpret_cast<uintptr_t>(worker_allocator.Get());
        report["worker"]["queue"] = reinterpret_cast<uintptr_t>(gpu.queue.Get());
        Check(candidate_slot.handle != original_nr && candidate_slot.parameters != original_parameters
                  && candidate_proxy.Get() != original_proxy, "cold candidate owns independent feature, parameters and resources");
        const auto create_begin = worker_report["create_begin_ns"].get<int64_t>();
        const auto create_end = worker_report["create_end_ns"].get<int64_t>();
        const auto foreground_begin = report["frames"].back()["source_start_ns"].get<int64_t>();
        const bool marker_overlapped = marker_begin >= create_begin && marker_complete < create_end;
        const bool foreground_overlapped = foreground_begin >= create_begin && foreground_begin < create_end;
        report["marker"]["completed_during_create"] = marker_overlapped;
        report["critical_path"] = {{"status", foreground_overlapped ? "Blocked" : "Inconclusive"},
            {"foreground_attempted_during_create", foreground_overlapped},
            {"actual_sr_entered_after_create", report["frames"].back()["sr_enter_ns"].get<int64_t>() >= create_end},
            {"meaning", "one serialized Core gate also delays source SR; this probe does not enable concurrent Create/Evaluate"}};
        candidate_slot.via_core = true;
        candidate_slot.nr_preset = model_preset;
        candidate_slot.nr_hdr_mode = hdr_mode;
        candidate_slot.pending_reset = true;
        candidate_slot.pending_reset_reasons = addon::kResetCreated;
        addon::FinalResources candidate_resources{.proxy = candidate_proxy.Get(), .nr_output = candidate_output.Get(), .hdr_mode = hdr_mode};
        const addon::EvaluationContract candidate_frame{.source_handle = owner, .motion = retained_motion.Get(), .depth = retained_depth.Get(),
            .input_width = render_width, .input_height = render_height, .output_width = candidate_width, .output_height = candidate_height,
            .motion_width = render_width, .motion_height = render_height, .create_flags = create_flags,
            .motion_scale_x = 1.f, .motion_scale_y = 1.f, .frame_reset = 1};
        gpu.list->ClearState(nullptr);
        gpu.Retain(candidate_proxy.Get());
        gpu.Retain(candidate_output.Get());
        gpu.Retain(retained_depth.Get());
        gpu.Retain(retained_motion.Get());
        NVSDK_NGX_Result first_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
        const auto first_begin = addon::SteadyNowNs();
        {
          const serial::PresentTurn turn(true);
          Check(turn.owns(), "cold candidate first Evaluate serialized turn");
          renodx::addons::dlss5::DirectCallScope direct;
          addon::SetEvaluationParameters(candidate_state, candidate_slot, 0, addon::history_reset_epoch.load(),
              candidate_resources, candidate_frame, candidate_width, candidate_height, candidate_width, candidate_height);
          submission::TrackUse(gpu.list.Get(), candidate_slot.handle);
          submission::TrackUse(gpu.list.Get(), candidate_proxy.Get());
          submission::TrackUse(gpu.list.Get(), candidate_output.Get());
          evaluate_receipt = submission::ObserveRecording(gpu.list.Get());
          queries.Begin(gpu.list.Get(), 16);
          const auto evaluate_begin = addon::SteadyNowNs();
          first_result = evaluate(gpu.list.Get(), candidate_slot.handle, candidate_slot.parameters, nullptr);
          report["first_evaluate"]["cpu_sdk_ms"] = (addon::SteadyNowNs() - evaluate_begin) / 1'000'000.;
          queries.EndAndResolve(gpu.list.Get(), 16);
        }
        const auto first_recorded = addon::SteadyNowNs();
        gpu.Submit();
        const auto first_completed = addon::SteadyNowNs();
        report["first_evaluate"].update({{"result", static_cast<uint32_t>(first_result)}, {"reset_sent", candidate_slot.model_reset},
            {"begin_ns", first_begin}, {"recorded_ns", first_recorded}, {"completed_ns", first_completed},
            {"cpu_record_ms", (first_recorded - first_begin) / 1'000'000.}, {"gpu_ms", queries.Read(16).gpu_ms},
            {"cpu_submit_fence_ms", (first_completed - first_recorded) / 1'000'000.},
            {"cpu_complete_from_start_ms", (first_completed - first_begin) / 1'000'000.},
            {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()}});
        {
          std::lock_guard lock(submission::tracker_mutex);
          report["first_evaluate"]["receipt"] = {{"list", evaluate_receipt->list}, {"generation", evaluate_receipt->generation},
              {"closed", evaluate_receipt->closed.load()}, {"submitted", evaluate_receipt->submitted.load()},
              {"completed_healthy", evaluate_receipt->completed_healthy}};
          Check(NVSDK_NGX_SUCCEED(first_result) && candidate_slot.model_reset && evaluate_receipt->closed.load()
                    && evaluate_receipt->submitted.load() && evaluate_receipt->completed_healthy,
                "the privately created new-size feature accepts its first Evaluate and completes on GPU");
        }
        evaluate_source("after-candidate");
        report["serialization"] = {{"sdk_entry_refused", native_cold_sdk_entry_refused},
            {"serial_timeouts_delta", serial::serial_timeouts.load() - native_cold_serial_timeouts_before},
            {"overlap_counter_delta", serial::overlap.load() - overlap_before},
            {"runtime_lock_contentions_delta", renodx::addons::dlss5::runtime_lock_contended.load() - runtime_contentions_before},
            {"submit_lock_contentions_delta", renodx::addons::dlss5::submit_lock_blocked.load() - submit_contentions_before}};
        Check(!worker_report["runtime_mutex_held"].get<bool>() && !native_cold_sdk_entry_refused
                  && serial::serial_timeouts.load() == native_cold_serial_timeouts_before && serial::overlap.load() == overlap_before,
              "cold worker never holds runtime_mutex or overlaps actual source SDK work");
        report["contract_execution"] = "Passed";
        report["independent_d3d_progress"] = marker_overlapped ? "Passed" : "Inconclusive";
        report["coverage"] = marker_overlapped && foreground_overlapped ? "Passed" : "Inconclusive";
        Check(cleanup_candidate(), "cold candidate releases its completed private feature and parameters");
      } catch (...) {
        const auto failure = std::current_exception();
        if (worker.joinable()) worker.join();
        native_cost_queries = nullptr;
        native_cost_list = nullptr;
        addon::ngx_slot_real[0].evaluate = nullptr;
        report["worker"] = worker_report;
        report["contract_execution"] = "Failed";
        report["sdk_entry_refused"] = native_cold_sdk_entry_refused;
        report["serialization"] = {{"serial_timeouts_delta", serial::serial_timeouts.load() - native_cold_serial_timeouts_before},
            {"overlap_counter_delta", serial::overlap.load() - overlap_before}, {"worker_joined", true}};
        try {
          std::rethrow_exception(failure);
        } catch (const std::exception& error) {
          report["failure"] = error.what();
        } catch (...) {
          report["failure"] = "non-standard exception";
        }
        cleanup_candidate();
        native_cold_worker_sdk_active = nullptr;
        std::ofstream file("adaptive-native-cold-prepare.json", std::ios::binary | std::ios::trunc);
        file << report.dump(2) << '\n';
        file.close();
        std::rethrow_exception(failure);
      }
      native_cold_worker_sdk_active = nullptr;
      native_cost_list = nullptr;
      std::ofstream file("adaptive-native-cold-prepare.json", std::ios::binary | std::ios::trunc);
      file << report.dump(2) << '\n';
      file.close();
      Check(!file.fail(), "cold prepare probe checkpoint saved before source teardown");
    } else if (resize_test && !cold_components) {
      const auto* ready_environment = std::getenv("NR_NEXUS_TEST_RESIZE_READY");
      native_resize_ready = ready_environment == nullptr || std::strcmp(ready_environment, "0") != 0;
      const auto* settings_environment = std::getenv("NR_NEXUS_TEST_RESIZE_SETTINGS");
      const bool settings_requested = native_resize_ready && settings_environment != nullptr
          && std::strcmp(settings_environment, "1") == 0;
      const bool settings_reuse = settings_requested && !demand_prepare;
      const auto* expect_reuse_environment = std::getenv("NR_NEXUS_TEST_RESIZE_EXPECT_REUSE");
      const bool expect_requested = settings_requested && expect_reuse_environment != nullptr
          && std::strcmp(expect_reuse_environment, "1") == 0;
      const bool expect_reuse = expect_requested && !demand_prepare;
      const auto* reuse_feature_environment = std::getenv("NR_NEXUS_TEST_RESIZE_REUSE_FEATURE");
      auto feature_mode = addon::AdaptiveFeatureMode::OFF;
      const char* feature_mode_name = "OFF";
      if (reuse_feature_environment != nullptr) {
        if (std::strcmp(reuse_feature_environment, "1") == 0) {
          feature_mode = addon::AdaptiveFeatureMode::RETAIN;
          feature_mode_name = "RETAIN";
        } else if (std::strcmp(reuse_feature_environment, "2") == 0) {
          feature_mode = addon::AdaptiveFeatureMode::UPPER_CAPACITY;
          feature_mode_name = "UPPER_CAPACITY";
        } else if (std::strcmp(reuse_feature_environment, "3") == 0) {
          Check(demand_prepare, "mode 3 is only supported by the Session-driven demand fixture");
          feature_mode = addon::AdaptiveFeatureMode::UPPER_CAPACITY_ON_DEMAND;
          feature_mode_name = "UPPER_CAPACITY_ON_DEMAND";
        } else if (std::strcmp(reuse_feature_environment, "4") == 0) {
          Check(demand_adjacent, "mode 4 is only supported by the consecutive demand fixture");
          feature_mode = addon::AdaptiveFeatureMode::UPPER_CAPACITY_ADJACENT;
          feature_mode_name = "UPPER_CAPACITY_ADJACENT";
        }
      }
      const bool reuse_feature = feature_mode != addon::AdaptiveFeatureMode::OFF;
      const bool on_demand = feature_mode == addon::AdaptiveFeatureMode::UPPER_CAPACITY_ON_DEMAND;
      const bool adjacent_ready = feature_mode == addon::AdaptiveFeatureMode::UPPER_CAPACITY_ADJACENT;
      const bool upper_capacity = feature_mode == addon::AdaptiveFeatureMode::UPPER_CAPACITY || on_demand || adjacent_ready;
      const bool demand_mode_valid = on_demand
          || (demand_adjacent ? adjacent_ready : feature_mode == addon::AdaptiveFeatureMode::UPPER_CAPACITY);
      Check(!demand_prepare || (native_resize_ready && demand_mode_valid && !settings_requested),
            "demand comparison requires ready mode 2/3 for variant 1 or mode 3/4 for variant 2, with settings edits disabled");
      const auto* fixed_input_environment = std::getenv("NR_NEXUS_TEST_RESIZE_FIXED_INPUT");
      const bool fixed_input = fixed_input_environment != nullptr && std::strcmp(fixed_input_environment, "1") == 0;
      Check(!fixed_input || settings_reuse, "fixed-input attribution requires the existing settings sequence");
      Check(native_resize_fixed_inputs.empty(), "fixed-input upload storage starts empty");
      native_resize_fixed_input = fixed_input;
      native_resize_observe_parameters = settings_reuse;
      native_resize_frame = UINT32_MAX;
      native_resize_fixed_input_copies = 0;
      Check(!reuse_feature || expect_requested || demand_prepare,
            "Feature capacity experiment requires either the existing settings assertions or the demand fixture");
      Check(addon::adaptive_feature_mode.load() == addon::AdaptiveFeatureMode::OFF, "Feature capacity reuse is disabled by default");
      addon::adaptive_feature_mode = feature_mode;
      native_resize_demand_prepare = demand_prepare;
      native_resize_create_gpu = demand_adjacent;
      const uint32_t frame_count = (automatic_limited ? selected_source_limit : (demand_prepare ? 900u : (settings_reuse ? 360u : 180u)));
      const UINT intervals_per_frame = (demand_adjacent ? 4u : (native_resize_ready ? 3u : 2u));
      auto queries = d3d12_test::CreateTimestampQueries(gpu, frame_count * intervals_per_frame);
      ComPtr<IDXGIAdapter3> adapter;
      Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))),
            "resize memory adapter");
      DXGI_ADAPTER_DESC adapter_desc{};
      Check(SUCCEEDED(adapter->GetDesc(&adapter_desc)), "resize adapter identity");
      LARGE_INTEGER driver_version{};
      const auto driver_status = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver_version);
      if (!selected_sequence) {
        native_resize_create = addon::core_create_feature;
        native_resize_release = addon::core_release_feature;
        addon::core_create_feature = ObserveNativeResizeCreate;
        addon::core_release_feature = ObserveNativeResizeRelease;
      }
      auto& report = resize_report;
      auto selected_report = (selected_sequence ? std::move(report["selected_collection"]) : nlohmann::json{});
      auto pipeline_report = (pipeline_steps ? std::move(report["pipeline_steps"]) : nlohmann::json{});
      auto trace_report = (pipeline_steps ? std::move(report["cold_cpu_trace"]) : nlohmann::json{});
      report = {{"schema", "nr-nexus-native-resize-v1"}, {"evidence", "NATIVE_GPU+NATIVE_NGX"},
          {"backend", "production owned NR; public SDK ProjectID preinitialization"},
          {"host_sr", "NATIVE_NGX fixed 1280x720 to 1920x1080"}, {"stage", "Upscaled"},
          {"passes", 1}, {"colors", 1}, {"mfg", false}, {"requested_model", "B"}, {"internal_model", "unknown"},
          {"format", "RGBA16F"}, {"depth", "R32F fixed gradient"}, {"motion", "RG16F zero"},
          {"runtime", addon::direct_runtime_requirements.file_version}, {"nr_sha256", addon::direct_runtime_sha256},
          {"driver_x100", addon::direct_runtime_requirements.driver_x100},
          {"driver_version_raw", driver_version.QuadPart}, {"driver_status", driver_status},
          {"vendor_id", adapter_desc.VendorId}, {"device_id", adapter_desc.DeviceId},
          {"timestamp_frequency_hz", queries.frequency_hz},
          {"input_hash_algorithm", "CRC32 of uploaded tightly packed bytes"},
          {"input_color_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(color_data.data()), color_data.size() * sizeof(uint16_t))},
          {"input_depth_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(depth_data.data()), depth_data.size() * sizeof(float))},
          {"input_motion_crc32", renodx::utils::hash::ComputeCRC32(reinterpret_cast<const uint8_t*>(motion_data.data()), motion_data.size() * sizeof(uint16_t))},
          {"mode", (native_resize_ready ? "ready-cache" : "cold")},
          {"settings_reuse_sequence", settings_reuse},
          {"expect_settings_reuse", expect_reuse},
          {"demand_prepare_sequence", demand_prepare},
          {"demand_prepare_variant", (demand_prepare ? (demand_adjacent ? 2 : 1) : 0)},
          {"feature_capacity_reuse_experiment", reuse_feature},
          {"feature_mode", feature_mode_name},
          {"fixed_input_probe", fixed_input},
          {"performance_comparison_valid", !fixed_input},
          {"parameter_snapshot_scope", "actual Core entry at frames 285-299; scalar availability recorded by type; identities are separate"},
          {"input_replacement_timing", fixed_input
              ? "wrapper and NR GPU intervals include the additional fixed-input copy; CPU SDK time excludes replacement; attribution probe, no performance claim"
              : "original production proxy input"},
          {"selection", (demand_prepare ? "production Session::Prepare; MOCK output FPS windows, no direct Request; GPU cost is measured"
                                        : "MOCK direct Session::Request; output FPS source unavailable, no FPS accuracy claim")},
          {"timing_scope", "CPU resource/wrapper residual includes resource preparation, binding, dispatch recording, logging and retirement outside SDK calls; GPU wrapper includes real SR and NR"},
          {"synchronization", "one submitted list, fence completed before reuse; 60Hz paced starts; raw actual start intervals"},
          {"frames", nlohmann::json::array()}};
      if (selected_sequence) {
        report["selected_collection"] = std::move(selected_report);
        report["probe_execution"] = "Running";
        report["teardown_status"] = "NotRun";
        report["mode"] = "selected-complete-collection";
        report["selection"] = "MOCK direct ready-only Session::Request; real SR/NR; no output FPS accuracy claim";
        report["selected_collection"]["memory_scope"] =
            "sampled process local usage before/after source recording and after drain; includes SR, NR and retirement, not instantaneous hardware peak";
        auto& selected = report["selected_collection"];
        for (const auto* key : {"cpu_wrapper_ms", "cpu_create_ms", "cpu_release_ms", "cpu_host_sr_ms", "cpu_nr_evaluate_ms"}) {
          selected[std::string("full_preparation_") + key] = selected.at(std::string("prefix_") + key);
        }
        selected["full_preparation_create_count"] = selected.at("prefix_create_count");
        selected["full_preparation_release_count"] = selected.at("prefix_release_count");
      }
      if (pipeline_steps) {
        report["pipeline_steps"] = std::move(pipeline_report);
        report["cold_cpu_trace"] = std::move(trace_report);
        report["mode"] = "cold-adaptive-pipeline-steps";
        report["render_size"] = {render_width, render_height};
        report["output_size"] = {width, height};
      }
      if (automatic_limited) {
        report["mode"] = "automatic-player-limited-residency";
        report["selection"] = "production Controller with MOCK FPS; actual GPU timing, real NGX and monotonic time; no direct requests";
      }
      report["loaded_ngx_modules"] = nlohmann::json::array();
      std::array<HMODULE, 1024> modules{};
      DWORD module_bytes = 0;
      Check(EnumProcessModules(GetCurrentProcess(), modules.data(), sizeof(modules), &module_bytes)
                && module_bytes <= sizeof(modules), "resize enumerate actual loaded modules");
      for (size_t index = 0; index < module_bytes / sizeof(HMODULE); ++index) {
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(modules[index], path.data(), static_cast<DWORD>(path.size()));
        const std::filesystem::path module_path(std::wstring(path.data(), length));
        const auto filename = module_path.filename().string();
        if (filename.find("nvngx") != std::string::npos) {
          report["loaded_ngx_modules"].push_back(module_path.string());
        }
      }
      const auto origin = std::chrono::steady_clock::now();
      auto previous = origin;
      auto deadline = origin;
      std::array<uintptr_t, renodx::addons::dlss5::adaptive_resolution::MAX_LEVELS> stable_models{}, stable_proxies{};
      std::array<uint64_t, renodx::addons::dlss5::adaptive_resolution::MAX_LEVELS> stable_worksets{};
      bool recorded_ready = false;
      uint32_t selected_ready_frame = UINT32_MAX;
      size_t selected_request_index = 0;
      uint32_t selected_ready_features = 0;
      uint32_t selected_ready_creates = 0, selected_ready_releases = 0;
      uint64_t selected_ready_worksets = 0;
      auto selected_prepare_begin = std::chrono::steady_clock::time_point{};
      std::vector<size_t> selected_requests(pipeline_steps ? 3u : 6u);
      uint32_t limited_phase = 0;
      bool limited_request_issued = false;
      ComPtr<ID3D12CommandAllocator> limited_retained_allocator;
      ComPtr<ID3D12GraphicsCommandList> limited_retained_list;
      const NVSDK_NGX_Handle* limited_retained_model = nullptr;
      uint32_t limited_wait_creates = 0, limited_reverse_creates = 0;
      size_t preparing_evaluates = 0;
      size_t endpoint_slot = 0;
      uintptr_t endpoint_model = 0, endpoint_parameters = 0;
      std::shared_ptr<submission::RecordingStatus> endpoint_old_completion;
      bool endpoint_reset_seen = false, endpoint_dimension_reset_seen = false, endpoint_ready_seen = false;
      uint32_t initial_creates = 0, initial_releases = 0;
      double initial_create_ms = 0.;
      std::array<uintptr_t, renodx::addons::dlss5::adaptive_resolution::MAX_LEVELS> initial_models{}, initial_parameters{};
      std::array<uint32_t, renodx::addons::dlss5::adaptive_resolution::MAX_LEVELS> demand_slot_creates{};
      std::array<bool, renodx::addons::dlss5::adaptive_resolution::MAX_LEVELS> demand_requested_slots{};
      uint32_t demand_startup_creates = 0, demand_total_creates = 0, demand_wait_frames = 0;
      double demand_startup_create_ms = 0., demand_total_create_ms = 0.;
      uint64_t demand_peak_bytes = 0;
      size_t demand_previous_active = 0;
      size_t demand_previous_requested = 0;
      bool demand_previous_trial = false;
      std::array<bool, 3> demand_trial_accepted{}, demand_applied_slots{}, demand_standby_evaluated{};
      bool demand_initial_neighbor_ready = false;
      bool demand_second_prefetch_observed = false;
      bool demand_lower_applied = false, demand_returned_upper = false;
      namespace output_rate = renodx::addons::dlss5::output_framerate;
      namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
      uint32_t automatic_phase = 0, automatic_pending_frames = 0, automatic_peak_features = 0;
      std::array<uint32_t, 3> automatic_slot_creates{};
      const NVSDK_NGX_Handle* automatic_anchor = nullptr;
      int64_t automatic_cooldown_deadline = 0;
      std::string automatic_failure;
      if (automatic_limited) {
        report["selected_collection"]["limited_residency"]["automatic"] = {
            {"input_evidence", "MOCK FPS by actual Applied size: upper 80; first 75% 84; 50% 60; returned 75% 60"},
            {"gpu_evidence", "NATIVE_GPU production timestamps and actual Session median/MAD; no injected GPU samples"},
            {"clock", "real SteadyNowNs/steady_clock; no time, level, Request or blocked-state edits"},
            {"cooldown_scope", "30-second expiry NotRun; synthetic 84-to-60 FPS workload change reopens production blocked gate"},
            {"feature_count_scope", "sampled after each source submit/Reset/drain; live plus pending retired NR Features, not an instantaneous SDK allocation peak"},
            {"frame_limit", frame_count}, {"wall_limit_ms_including_prefix", 20000}, {"source_cadence_hz", 60},
            {"target_fps", 100}, {"mock_fps_mad", 0}, {"logical_levels", 3}, {"minimum_scale", .5},
            {"events", nlohmann::json::array()}, {"states", nlohmann::json::array()}};
      }
      if (demand_prepare) {
        const auto& upper = addon::features.at(owner).slots[0];
        initial_models[0] = reinterpret_cast<uintptr_t>(upper.handle);
        initial_parameters[0] = reinterpret_cast<uintptr_t>(upper.parameters);
        Check(upper.handle != nullptr && upper.parameters != nullptr && upper.create_input_width == width
                  && upper.create_input_height == height, "demand fixture starts with the existing real upper Feature");
        report["probe_execution"] = "Running";
        report["teardown_status"] = "NotRun";
        report["demand_preparation"] = {{"fps_evidence", "MOCK: synthetic windows through the existing output_framerate test state"},
            {"gpu_evidence", "NATIVE_GPU: production adaptive timestamps, no injected GPU samples"},
            {"target_fps", 100}, {"mock_fps_before_frame_360", 120}, {"mock_fps_from_frame_360", 80},
            {"mock_fps_mad", 0}, {"frame_count", frame_count}, {"direct_session_requests", false},
            {"upper_model", initial_models[0]}, {"upper_parameters", initial_parameters[0]},
            {"submission_path", "queue hook registers once; healthy fixture fence, Reset, production receipt preservation and timing Poll"},
            {"cost_scope", "each frame retains actual Create calls, wrapper time and next successful source-start gap; deferred Create remains foreground cold cost"}};
        if (demand_adjacent) {
          report["demand_preparation"]["mock_fps_from_frame_360"] = nullptr;
          report["demand_preparation"]["mock_fps_by_frame_start_active_slot"] = {80, 90, 100};
          report["demand_preparation"]["create_gpu_scope"] = "timestamps bracket commands recorded by Create on the provided list; internal GPU/model preparation remains unobserved";
        }
      }
      const auto observe_pixels = [&](ID3D12Resource* surface, D3D12_RESOURCE_STATES state,
                                      uint32_t frame, const char* role) {
        const auto desc = surface->GetDesc();
        Check(desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, "resize pixel observation uses the actual FP16 surface");
        auto transfer = gpu.CreateTextureTransfer(surface);
        gpu.Retain(surface);
        submission::TrackUse(gpu.list.Get(), surface);
        gpu.Transition(surface, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(surface, transfer);
        gpu.Transition(surface, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        gpu.Submit();
        const auto completed = gpu.fence->GetCompletedValue();
        Check(completed >= gpu.serial && completed != UINT64_MAX && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
              "resize pixel observation completes on a healthy device");
        const auto bytes = gpu.ReadTexture(transfer);
        const bool keep_raw = std::getenv("NR_NEXUS_TEST_KEEP_RAW") != nullptr
            && std::strcmp(std::getenv("NR_NEXUS_TEST_KEEP_RAW"), "1") == 0;
        if (keep_raw) {
          const auto filename = "adaptive-pixels-" + std::to_string(frame) + "-" + role + ".rgba16f";
          std::ofstream raw(filename, std::ios::binary | std::ios::trunc);
          raw.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
          Check(raw.good(), "resize diagnostic pixel bytes are saved");
        }
        std::array<uint64_t, 4> nonfinite{}, finite{};
        std::array<float, 4> minimum{}, maximum{};
        minimum.fill(std::numeric_limits<float>::max());
        maximum.fill(std::numeric_limits<float>::lowest());
        for (size_t channel = 0; channel < bytes.size() / sizeof(uint16_t); ++channel) {
          uint16_t bits = 0;
          std::memcpy(&bits, bytes.data() + channel * sizeof(uint16_t), sizeof(bits));
          const float value = DirectX::PackedVector::XMConvertHalfToFloat(bits);
          const size_t component = channel % 4;
          if (std::isfinite(value)) {
            ++finite[component];
            minimum[component] = std::min(minimum[component], value);
            maximum[component] = std::max(maximum[component], value);
          } else {
            ++nonfinite[component];
          }
        }
        nlohmann::json minima = nlohmann::json::array(), maxima = nlohmann::json::array();
        for (size_t component = 0; component < 4; ++component) {
          minima.push_back(finite[component] != 0 ? nlohmann::json(minimum[component]) : nlohmann::json(nullptr));
          maxima.push_back(finite[component] != 0 ? nlohmann::json(maximum[component]) : nlohmann::json(nullptr));
        }
        return nlohmann::json{{"size", {desc.Width, desc.Height}}, {"format", static_cast<uint32_t>(desc.Format)},
            {"crc32", renodx::utils::hash::ComputeCRC32(bytes.data(), bytes.size())}, {"channel_order", "RGBA"},
            {"nonfinite_channels", nonfinite}, {"finite_channels", finite}, {"minimum", minima}, {"maximum", maxima},
            {"raw_retained", keep_raw},
            {"state_restored", true}, {"fence", gpu.serial}, {"completed_fence", completed}};
      };
      if (native_resize_ready) {
        renodx::addons::dlss5::gpu_timers::comparison_settings = addon::comparison_ui::CaptureSettings;
        addon::adaptive_configuration_settings = addon::comparison_ui::CaptureAdaptiveSettings;
        addon::kSetAdaptiveEnabled.Store(1.f);
        if (demand_prepare) {
          addon::kSetAdaptiveTargetFps.Store(100.f);
        }
      }
      for (uint32_t frame = 0; frame < frame_count; ++frame) {
        if (automatic_limited && std::chrono::steady_clock::now() - selected_prefix_begin >= std::chrono::seconds(20)) {
          automatic_failure = "20-second real wall bound reached before the automatic decision sequence completed";
          break;
        }
        native_resize_frame = frame;
        const uint32_t phase = (selected_sequence ? (recorded_ready ? 1u : 0u)
            : (demand_prepare ? (frame < 360 ? 0u : 1u) : frame / 60));
        bool changed = !selected_sequence && !demand_prepare && (frame == 60 || frame == 120);
        const bool selected_request = selected_sequence && !limited_sequence && selected_ready_frame != UINT32_MAX;
        size_t selected_requested_slot = 0;
        if (selected_request) {
          auto& source = addon::features.at(owner);
          selected_requested_slot = selected_requests[selected_request_index++];
          changed = source.adaptive_cache->active != selected_requested_slot;
          source.adaptive_session->Request(selected_requested_slot, addon::SteadyNowNs());
        }
        if (limited_sequence && !automatic_limited && limited_phase != 0 && !limited_request_issued) {
          auto& session = *addon::features.at(owner).adaptive_session;
          const size_t target = (limited_phase == 1 ? 6u : (limited_phase == 3 || limited_phase == 5 ? 10u : 0u));
          session.Request(target, addon::SteadyNowNs());
          limited_request_issued = true;
        }
        const char* setting_event = "none";
        std::array<bool, renodx::addons::dlss5::adaptive_resolution::MAX_LEVELS> demand_ready_before{};
        double mock_output_fps = (frame < 360 ? 120. : 80.);
        size_t demand_active_before = 0;
        int64_t mock_window_end_ns = 0;
        if (demand_prepare || automatic_limited) {
          mock_window_end_ns = addon::SteadyNowNs();
          const auto& feature = addon::features.at(owner);
          if (feature.adaptive_cache != nullptr) {
            demand_active_before = feature.adaptive_cache->active;
            for (size_t slot = 0; slot < feature.adaptive_cache->count; ++slot) {
              const auto& tier = feature.adaptive_cache->tiers[slot];
              demand_ready_before[slot] = tier.ready && tier.completion != nullptr
                  && tier.completion->closed.load() && tier.completion->submitted.load() && tier.completion->completed_healthy;
            }
          }
          if (demand_adjacent && frame >= 360) {
            Check(demand_active_before < 3, "consecutive MOCK output uses the actual three-slot active state");
            mock_output_fps = 80. + 10. * demand_active_before;
          }
          if (automatic_limited) {
            const auto& session = *feature.adaptive_session;
            Check(session.ladder.count == 3 && session.settings.limited_residency
                && !session.settings.legacy_preparation && !addon::async_autoscale_preparation.load(),
                "automatic player mode keeps three logical tiers with legacy and private PSO experiments off");
            const auto applied = session.status.applied[0];
            mock_output_fps = (applied == session.ladder.sizes[1][0] ? (automatic_phase >= 6 ? 60. : 84.)
                : (applied == session.ladder.sizes[2][0] ? 60. : 80.));
          }
          std::unique_lock lock(output_rate::internal::state.mutex);
          auto& estimate = output_rate::internal::state.stream;
          estimate.previous_ns = mock_window_end_ns;
          estimate.snapshot = {.available = true, .reason = output_rate::Reason::kREADY, .source = output_rate::Source::PRESENT,
              .source_generation = 1, .configuration_generation = addon::configuration_generation.load(),
              .window_serial = frame + 1, .window_begin_ns = mock_window_end_ns - 500'000'000,
              .window_end_ns = mock_window_end_ns, .fps = mock_output_fps, .fps_mad = 0., .fg_off = true};
        }
        const auto automatic_session_before = (automatic_limited ? addon::features.at(owner).adaptive_session : nullptr);
        const auto automatic_applied_before = (automatic_limited ? automatic_session_before->status.applied : adaptive::Sizes{});
        const bool automatic_retiring_before = automatic_limited && addon::AdaptiveRetirementPending(owner);
        const auto automatic_requested_before = (automatic_limited ? automatic_session_before->level : 0u);
        const auto automatic_generation_before = (automatic_limited ? automatic_session_before->status.generation : 0u);
        const bool automatic_trial_before = automatic_limited && automatic_session_before->trial;
        const auto automatic_previous_before = (automatic_limited ? automatic_session_before->previous_level : 0u);
        const double automatic_baseline_fps_before = (automatic_limited ? automatic_session_before->baseline_fps : 0.);
        const auto automatic_applied_ns_before = (automatic_limited ? automatic_session_before->applied_ns : 0);
        const double automatic_baseline_before = (automatic_limited ? automatic_session_before->baseline_gpu_us : 0.);
        double automatic_gpu_before = 0., automatic_gpu_mad_before = 0.;
        const bool automatic_gpu_valid_before = automatic_limited
            && automatic_session_before->GpuStatistics(mock_window_end_ns, &automatic_gpu_before, &automatic_gpu_mad_before);
        if (settings_reuse) {
          switch (frame) {
            case 180:
              addon::kSetStyle.Store(1.f);
              addon::ApplySettingEffect(addon::kSetStyle.effect);
              setting_event = "style";
              break;
            case 210:
              addon::kSetTransferStrength.Store(0.5f);
              setting_event = "transfer-strength";
              break;
            case 240:
              addon::kSetAdaptiveMinScale.Store(0.80f);
              setting_event = "minimum-80";
              break;
            case 270:
              addon::kSetAdaptiveMinScale.Store(0.50f);
              setting_event = "minimum-50";
              break;
            case 285:
              if (expect_reuse) {
                Check(addon::features.at(owner).adaptive_session->RequestMinimum(addon::SteadyNowNs()),
                      "native minimum request has an idle non-active slot");
                const auto& source = addon::features.at(owner);
                endpoint_slot = source.adaptive_session->level;
                const auto& tier = source.adaptive_cache->tiers[endpoint_slot];
                const auto& model = tier.state.slots[0];
                endpoint_model = reinterpret_cast<uintptr_t>(model.handle);
                endpoint_parameters = reinterpret_cast<uintptr_t>(model.parameters);
                endpoint_old_completion = tier.completion;
                report["endpoint_before_retarget"] = {{"slot", endpoint_slot}, {"model", endpoint_model}, {"parameters", endpoint_parameters},
                    {"create_input", {model.create_input_width, model.create_input_height}},
                    {"create_output", {model.create_output_width, model.create_output_height}},
                    {"logical_size", {tier.state.input_width, tier.state.input_height}},
                    {"active_model", reinterpret_cast<uintptr_t>(source.slots[0].handle)},
                    {"old_receipt_list", endpoint_old_completion != nullptr ? endpoint_old_completion->list : 0},
                    {"old_receipt_generation", endpoint_old_completion != nullptr ? endpoint_old_completion->generation : 0}};
                setting_event = "request-minimum-50";
              }
              break;
            case 300:
              addon::kSetAdaptiveMinScale.Store(0.67f);
              setting_event = "minimum-67";
              break;
            default:
              break;
          }
          if (std::strcmp(setting_event, "none") != 0) recorded_ready = false;
        }
        if (native_resize_ready && !selected_sequence && !demand_prepare && frame == 150) addon::kSetAdaptiveTargetFps.Store(120.f);
        if (changed && !selected_sequence) {
          if (native_resize_ready) {
            auto& feature = addon::features.at(owner);
            Check(recorded_ready && feature.adaptive_session != nullptr && feature.adaptive_cache != nullptr,
                  "all real NR tiers are ready before the mock selection changes");
            feature.adaptive_session->Request((phase == 1 ? feature.adaptive_cache->count - 1 : 0), addon::SteadyNowNs());
          } else {
            addon::nr_resolution_scale = (phase == 1 ? 2.f / 3.f : 1.f);
            ++addon::nr_resolution_commit_epochs[0];
          }
        }
        native_runtime_masks.clear();
        native_runtime_results.clear();
        native_runtime_resets.clear();
        native_resize_models.clear();
        native_resize_preparing.clear();
        native_resize_evaluate_cpu_ms.clear();
        native_resize_parameter_snapshots.clear();
        native_resize_create_calls.clear();
        native_resize_fixed_input_cpu_ms = 0.;
        const auto fixed_copies_before = native_resize_fixed_input_copies;
        native_resize_create_ms = native_resize_release_ms = native_resize_host_ms = native_resize_evaluate_ms = 0;
        native_resize_creates = native_resize_releases = 0;
        native_resize_create_runtime_locked = native_resize_create_ngx_call = false;
        gpu.list->ClearState(nullptr);
        parameters.Set(NVSDK_NGX_Parameter_Reset, 0);
        native_cost_interval = frame * intervals_per_frame;
        native_cost_list = gpu.list.Get();
        native_cost_queries = &queries;
        queries.Begin(gpu.list.Get(), native_cost_interval);
        const auto start = std::chrono::steady_clock::now();
        if (selected_sequence && frame == 0) {
          selected_prepare_begin = start;
          report["selected_collection"]["fixture_setup_gap_ms"] = std::chrono::duration<double, std::milli>(
              selected_prepare_begin - selected_prefix_completed).count();
          report["selected_collection"]["prefix_to_selected_first_source_interval_ms"] = std::chrono::duration<double, std::milli>(
              start - selected_previous_start).count();
          const double prefix_tail_ms = std::chrono::duration<double, std::milli>(selected_prefix_completed - selected_previous_start).count();
          report["selected_collection"]["prefix_to_selected_first_source_interval_ms_excluding_setup"] = prefix_tail_ms;
          selected_max_source_interval_ms = std::max(selected_max_source_interval_ms, prefix_tail_ms);
        }
        const auto runtime_contentions_before = renodx::addons::dlss5::runtime_lock_contended.load();
        const auto submit_contentions_before = renodx::addons::dlss5::submit_lock_blocked.load();
        addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(ObserveNativeHostEvaluate);
        const auto source_successes_before = addon::SourceSuccessfulEvaluations();
        NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
        {
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
          const addon::ColdCpuTraceScope source_trace("selected.source_call", nullptr,
              pipeline_steps ? &cold_cpu_traces[prefix_host_evaluates + frame] : nullptr);
#endif
          result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
        }
        const auto source_successes_after = addon::SourceSuccessfulEvaluations();
        addon::ngx_slot_real[0].evaluate = nullptr;
        const auto recorded = std::chrono::steady_clock::now();
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
        if (pipeline_steps) {
          Check(cold_cpu_traces[prefix_host_evaluates + frame].count != 0,
              "every selected source has its own bounded cold CPU trace");
          export_cold_cpu_trace(false);
          const auto& trace = report["cold_cpu_trace"]["frames"].back();
          Check(!cold_components_failed && trace.at("pso_create_api_calls").get<uint32_t>() == 0,
              "a complete prefix pipeline cache needs no further PSO creation during selected preparation or ready switching");
        }
#endif
        const double wrapper_ms = std::chrono::duration<double, std::milli>(recorded - start).count();
        if (selected_sequence) {
          selected_max_wrapper_ms = std::max(selected_max_wrapper_ms, wrapper_ms);
          if (frame != 0) {
            selected_max_source_interval_ms = std::max(selected_max_source_interval_ms,
                std::chrono::duration<double, std::milli>(start - selected_previous_start).count());
          }
          selected_previous_start = start;
          report["selected_collection"]["last_recording"] = {{"stage", "selected-collection"}, {"frame", frame},
              {"cpu_wrapper_ms", wrapper_ms}, {"cpu_create_count", native_resize_creates},
              {"cpu_create_ms", native_resize_create_ms}, {"cpu_release_count", native_resize_releases},
              {"nr_results", native_runtime_results}, {"create_calls", native_resize_create_calls},
              {"core_parameters", native_resize_parameter_snapshots},
              {"process_local_after_record", sample_selected_memory().CurrentUsage}};
          if (selected_ready_frame == UINT32_MAX) {
            auto& selected = report["selected_collection"];
            for (const auto& [key, value] : {std::pair{"cpu_wrapper_ms", wrapper_ms},
                     std::pair{"cpu_create_ms", native_resize_create_ms}, std::pair{"cpu_release_ms", native_resize_release_ms},
                     std::pair{"cpu_host_sr_ms", native_resize_host_ms}, std::pair{"cpu_nr_evaluate_ms", native_resize_evaluate_ms}}) {
              const std::string total_key = std::string("full_preparation_") + key;
              selected[total_key] = selected.at(total_key).get<double>() + value;
            }
            selected["full_preparation_create_count"] = selected.at("full_preparation_create_count").get<uint32_t>() + native_resize_creates;
            selected["full_preparation_release_count"] = selected.at("full_preparation_release_count").get<uint32_t>() + native_resize_releases;
          }
        }
        gpu.list->EndQuery(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * native_cost_interval + 1);
        native_cost_queries = nullptr;
        const bool evaluated = !native_runtime_results.empty();
        Check(NVSDK_NGX_SUCCEED(result) && native_runtime_results.size() <= (native_resize_ready ? 2u : 1u)
                  && std::all_of(native_runtime_results.begin(), native_runtime_results.end(),
                      [](const auto value) { return NVSDK_NGX_SUCCEED(value); })
                  && (!selected_sequence || std::all_of(native_resize_create_calls.begin(), native_resize_create_calls.end(),
                      [](const auto& call) { return NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(call.at("result").template get<uint32_t>())); })),
            "resize wrapper and all real NR Creates and Evaluates succeed");
        gpu.Retain(queries.readback.Get());
        gpu.list->ResolveQueryData(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            2 * native_cost_interval, 2u * (1u + static_cast<UINT>(native_runtime_results.size())),
            queries.readback.Get(), 2ull * native_cost_interval * sizeof(uint64_t));
        if (demand_adjacent && native_resize_creates != 0) {
          gpu.list->ResolveQueryData(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
              2 * (native_cost_interval + 3), 2, queries.readback.Get(),
              2ull * (native_cost_interval + 3) * sizeof(uint64_t));
        }
        nlohmann::json tiers_before = nlohmann::json::array();
        if (native_resize_ready) {
          Check(native_resize_preparing.size() == native_runtime_results.size()
                    && native_resize_models.size() == native_runtime_results.size()
                    && (!demand_prepare || native_resize_evaluate_cpu_ms.size() == native_runtime_results.size()),
                "every real resize Evaluate records its model and preparation context");
          const auto preparation_count = static_cast<size_t>(std::count(native_resize_preparing.begin(), native_resize_preparing.end(), true));
          const auto active_count = native_resize_preparing.size() - preparation_count;
          Check((active_count == 1 || (settings_reuse && frame >= 180 && active_count == 0 && native_resize_creates != 0))
                    && source_successes_after == source_successes_before + active_count,
                "real active outputs count once; setting cold-create and preparation-only frames add no source success");
          preparing_evaluates += preparation_count;
          const auto cache = addon::features.at(owner).adaptive_cache;
          if (selected_sequence && cache != nullptr) {
            report["selected_collection"]["last_recording"].update({{"cache_failed", cache->failed},
                {"cache_budget_blocked", cache->budget_blocked},
                {"reason", renodx::addons::dlss5::adaptive_resolution::ReasonText(addon::features.at(owner).adaptive_session->status.reason)}});
          }
          if (selected_sequence && cache != nullptr && cache->budget_blocked) {
            report["probe_execution"] = "Blocked";
            report["selected_collection"]["stop_reason"] = "production workset/VRAM budget rejected the full requested collection";
          }
          Check(cache != nullptr && !cache->failed && !cache->budget_blocked, "real NR preparation cache remains healthy");
          if (selected_sequence) {
            Check(cache->count == (selected_levels == 0 ? 4u : selected_levels),
                "selected native comparison preserves the requested full tuple count");
          }
          for (size_t slot = 0; slot < cache->count; ++slot) {
            const auto& tier = cache->tiers[slot];
            if (tier.completion != nullptr && !tier.completion->submitted.load()) {
              Check(!tier.ready, "recorded but unsubmitted real NR preparation is never ready");
            }
            tiers_before.push_back({{"slot", slot}, {"ready", tier.ready},
                {"has_receipt", tier.completion != nullptr},
                {"submitted", tier.completion != nullptr && tier.completion->submitted.load()},
                {"closed", tier.completion != nullptr && tier.completion->closed.load()}});
          }
        }
        const size_t retired_nr_before = addon::retired_nr_features.size();
        const size_t retired_worksets_before = addon::retired_final_resources.size();
        DXGI_QUERY_VIDEO_MEMORY_INFO memory_before{};
        Check(SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory_before)), "resize memory before submit");
        const auto submitted = std::chrono::steady_clock::now();
        const auto before_submit_cache = addon::features.at(owner).adaptive_cache;
        const bool retain_limited_source = limited_sequence && !automatic_limited && limited_phase == 1
            && limited_retained_list == nullptr && before_submit_cache != nullptr && before_submit_cache->active == 6;
        submit_resize(!retain_limited_source);
        const auto completed = std::chrono::steady_clock::now();
        double limited_list_replacement_ms = 0.;
        if (retain_limited_source) {
          const auto replacing = std::chrono::steady_clock::now();
          limited_retained_allocator = gpu.allocator;
          limited_retained_list = gpu.list;
          limited_retained_model = addon::features.at(owner).slots[0].handle;
          Check(!submission::ResourceReleasable(limited_retained_model),
              "the real lower NR source remains protected by its completed replayable recording");
          gpu.allocator.Reset();
          gpu.list.Reset();
          Check(SUCCEEDED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator)))
              && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                  gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list)))
              && addon::InstallCommandListStateHooks(gpu.list.Get()), "limited native replacement recording");
          limited_list_replacement_ms = std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - replacing).count();
        }
        if (demand_prepare || selected_sequence) {
          std::scoped_lock runtime_lock(addon::runtime_mutex);
          addon::DrainRetiredResources(true);
          renodx::addons::dlss5::gpu_timers::Poll(true, false);
          gpu.uploads.clear();
          gpu.retained_resources.clear();
        } else {
          addon::DrainRetiredResources(true);
        }
        const auto retired = std::chrono::steady_clock::now();
        DXGI_QUERY_VIDEO_MEMORY_INFO memory_after{};
        Check(SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory_after)), "resize memory after retirement");
        if (selected_sequence) {
          selected_peak_local_bytes = std::max({selected_peak_local_bytes, memory_before.CurrentUsage, memory_after.CurrentUsage});
          report["selected_collection"]["sampled_peak_process_local_bytes"] = selected_peak_local_bytes;
          report["selected_collection"]["last_recording"].update({{"process_local_after_drain", memory_after.CurrentUsage},
              {"budget_bytes", memory_after.Budget}, {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()}});
        }
        const auto wrapper_gpu = queries.Read(native_cost_interval);
        const auto nr_gpu = (evaluated ? queries.Read(native_cost_interval + 1) : d3d12_test::TimestampSample{});
        if (demand_adjacent && native_resize_creates != 0) {
          const auto create_gpu = queries.Read(native_cost_interval + 3);
          for (auto& call : native_resize_create_calls) {
            call["gpu_recording_ms"] = create_gpu.gpu_ms;
            call["gpu_begin_tick"] = create_gpu.begin_tick;
            call["gpu_end_tick"] = create_gpu.end_tick;
          }
        }
        const auto cache = addon::features.at(owner).adaptive_cache;
        if (native_resize_ready) addon::PollAdaptiveCache(cache.get());
        const auto active = (native_resize_ready ? static_cast<uint32_t>(cache->active) : 0u);
        const auto& workset = addon::worksets.at({owner, output.Get(), false, active});
        nlohmann::json tier_states = nlohmann::json::array();
        nlohmann::json evaluations = nlohmann::json::array();
        uint32_t actual_resident_features = 0, new_demand_models = 0, first_standby_evaluates = 0;
        if (native_resize_ready) {
          bool all_ready = true;
          std::set<uintptr_t> models, proxies;
          std::set<uint64_t> workset_ids;
          for (size_t slot = 0; slot < cache->count; ++slot) {
            const auto& tier = cache->tiers[slot];
            all_ready &= tier.ready;
            const auto& state = (slot == cache->active ? addon::features.at(owner) : tier.state);
            const auto found = addon::worksets.find({owner, output.Get(), false, static_cast<uint32_t>(slot)});
            const auto model = reinterpret_cast<uintptr_t>(state.slots[0].handle);
            const auto model_parameters = reinterpret_cast<uintptr_t>(state.slots[0].parameters);
            const auto proxy = (found != addon::worksets.end() ? reinterpret_cast<uintptr_t>(found->second.proxy) : 0);
            const auto workset_id = (found != addon::worksets.end() ? found->second.id : 0);
            actual_resident_features += model != 0;
            if (demand_prepare) {
              if (model != 0 && initial_models[slot] == 0) {
                ++new_demand_models;
                ++demand_slot_creates[slot];
                initial_models[slot] = model;
                initial_parameters[slot] = model_parameters;
                Check(!on_demand || ((slot == 1 || (demand_adjacent && slot == 2))
                          && addon::features.at(owner).adaptive_session->level == slot),
                      "on-demand creates only a tier actually requested by Session");
                bool observed_create = false;
                for (auto& call : native_resize_create_calls) {
                  if (call.at("model").get<uintptr_t>() == model) {
                    call["slot"] = slot;
                    observed_create = true;
                    if (adjacent_ready && call.at("session").at("requested_slot").get<size_t>() != slot) {
                      const auto& context = call.at("session");
                      const auto requested = context.at("requested_slot").get<size_t>();
                      Check(slot == requested + 1 && context.at("source_active_slot").get<size_t>() == requested
                                && !context.at("trial").get<bool>() && !context.at("blocked").get<bool>()
                                && !context.at("defer_preparation").get<bool>(),
                            "unrequested adjacent Create occurs only beside the accepted legal active tier outside a trial");
                      if (slot == 2) {
                        demand_second_prefetch_observed = true;
                      }
                    }
                  }
                }
                Check(observed_create, "each newly resident demand Feature has an actual observed Create call");
              }
              Check(model == initial_models[slot] && model_parameters == initial_parameters[slot],
                    "demand decision and activation retain each slot's original Feature and parameters");
              if (stable_proxies[slot] != 0) {
                Check(proxy == stable_proxies[slot] && workset_id == stable_worksets[slot],
                      "demand decisions preserve each previously allocated workset and proxy");
              }
              if (on_demand && !demand_adjacent && slot == 2) {
                Check(model == 0 && proxy == 0 && tier.completion == nullptr && !tier.ready,
                      "the unrequested second standby owns no Feature, workset or readiness receipt");
              }
            }
            if (upper_capacity && state.slots[0].handle != nullptr) {
              Check(cache->count == 3 && state.slots[0].create_input_width == width
                        && state.slots[0].create_input_height == height
                        && state.slots[0].create_output_width == width && state.slots[0].create_output_height == height,
                    "upper-capacity mode owns three slots whose actual Feature Create dimensions remain the manual upper size");
            }
            if (tier.ready) {
              Check(tier.completion != nullptr && tier.completion->closed.load()
                        && tier.completion->submitted.load() && tier.completion->completed_healthy,
                    "real NR ready tier requires complete healthy GPU submission proof");
              Check(model != 0 && proxy != 0 && workset_id != 0 && models.insert(model).second
                        && proxies.insert(proxy).second && workset_ids.insert(workset_id).second,
                    "ready NR tiers own distinct Feature handles, worksets and color proxies");
              if (selected_sequence) {
                const auto& size = cache->ladder.sizes[slot][0];
                Check(state.slots[0].create_input_width == size.width && state.slots[0].create_input_height == size.height
                          && state.slots[0].create_output_width == size.width && state.slots[0].create_output_height == size.height
                          && found->second.input_width == size.width && found->second.input_height == size.height
                          && found->second.proxy->GetDesc().Width == size.width && found->second.proxy->GetDesc().Height == size.height
                          && found->second.nr_output->GetDesc().Width == size.width && found->second.nr_output->GetDesc().Height == size.height,
                    "each ready selected tier owns an actual correctly sized Feature and both working surfaces");
              }
            }
            if (recorded_ready && !limited_sequence) {
              Check(stable_models[slot] == model && stable_proxies[slot] == proxy && stable_worksets[slot] == workset_id,
                    "ready switching preserves every tier Feature, workset and resource identity");
            } else {
              stable_models[slot] = model;
              stable_proxies[slot] = proxy;
              stable_worksets[slot] = workset_id;
            }
            tier_states.push_back({{"slot", slot}, {"active", slot == cache->active}, {"ready", tier.ready},
                {"model", model}, {"proxy", proxy}, {"workset_id", workset_id},
                {"parameters", reinterpret_cast<uintptr_t>(state.slots[0].parameters)},
                {"create_input", {state.slots[0].create_input_width, state.slots[0].create_input_height}},
                {"create_output", {state.slots[0].create_output_width, state.slots[0].create_output_height}},
                {"last_reset_reasons", state.slots[0].model_reset_reasons},
                {"logical_size", {state.input_width, state.input_height}},
                {"preparation_cpu_ms", tier.preparation_cpu_ms},
                {"receipt_closed", tier.completion != nullptr && tier.completion->closed.load()},
                {"receipt_submitted", tier.completion != nullptr && tier.completion->submitted.load()},
                {"receipt_completed_healthy", tier.completion != nullptr && tier.completion->completed_healthy}});
          }
          if (selected_sequence && !limited_sequence) {
            all_ready &= addon::features.at(owner).adaptive_session->prepared;
          }
          if (automatic_limited) {
            auto& session = *addon::features.at(owner).adaptive_session;
            auto& automatic = report["selected_collection"]["limited_residency"]["automatic"];
            const auto now_ns = addon::SteadyNowNs();
            const bool applied_target = session.TargetReady() && cache->active == session.level
                && session.status.applied == cache->ladder.sizes[cache->active] && session.applied_ns != 0;
            Check(cache->count == 3 && session.settings.selected_levels == 3 && session.settings.limited_residency
                && !session.settings.legacy_preparation && !addon::async_autoscale_preparation.load()
                && addon::adaptive_feature_mode.load() == addon::AdaptiveFeatureMode::OFF,
                "automatic player limited mode retains its selected logical count and ordinary independent Features");
            for (const auto& call : native_resize_create_calls) {
              Check(call.contains("session") && call.at("session").at("preparing_slot").get<size_t>() < 3,
                  "each automatic Create belongs to an observed production tier");
              const auto slot = call.at("session").at("preparing_slot").get<size_t>();
              ++automatic_slot_creates[slot];
              Check(call.at("input")[0] == session.ladder.sizes[slot][0].width
                  && call.at("input")[1] == session.ladder.sizes[slot][0].height,
                  "automatic Create uses its actual selected dimensions rather than the old capacity-reuse experiment");
            }
            automatic_peak_features = std::max(automatic_peak_features,
                actual_resident_features + static_cast<uint32_t>(addon::retired_nr_features.size()));
            Check(addon::nr_features_created - selected_features_before <= 4,
                "the bounded automatic sequence cannot create beyond upper, 75 percent and two 50 percent candidates");
            if (session.level != active) {
              ++automatic_pending_frames;
              Check(session.status.applied == automatic_applied_before
                  && session.applied_ns == 0 && session.gpu_count == 0,
                  "cold waiting preserves the real Applied output and does not measure fallback GPU work for the target");
            }
            if (active != demand_active_before) {
              Check(demand_ready_before[active] && native_runtime_resets[0] == 1 && applied_target,
                  "automatic activation uses an earlier complete healthy receipt and a real Reset before committing Applied");
            }
            if (addon::AdaptiveRetirementPending(owner)) {
              Check(native_resize_creates == 0, "an outstanding ordinary retirement cannot admit another automatic cold model");
            }
            const auto phase_before = automatic_phase;
            if (automatic_phase == 0 && applied_target && active == 0) {
              recorded_ready = true;
              selected_ready_frame = frame;
              automatic_slot_creates[0] = report["selected_collection"].at("prefix_create_count").get<uint32_t>();
              Check(automatic_slot_creates[0] == 1 && actual_resident_features == 1,
                  "player on-demand startup has only the actual complete upper");
              ++automatic_phase;
            } else if (automatic_phase == 1 && session.level == 1 && session.trial && session.previous_level == 0) {
              Check(session.baseline_fps == 80. && !demand_ready_before[1],
                  "the real Controller requests its first cold adjacent tier from the injected 80 FPS upper window");
              ++automatic_phase;
            } else if (automatic_phase == 2 && applied_target && active == 1) {
              automatic_anchor = addon::features.at(owner).slots[0].handle;
              Check(automatic_slot_creates[1] == 1 && actual_resident_features == 2,
                  "first automatic cold preparation actually applies the 75-percent model while retaining its upper anchor");
              ++automatic_phase;
            } else if (automatic_phase == 3 && session.level == 2 && session.trial && session.previous_level == 1) {
              Check(session.baseline_fps == 84. && !demand_ready_before[2],
                  "the Controller accepts the real cheaper 75-percent trial before requesting a second cold descent from its 84 FPS anchor");
              ++automatic_phase;
            } else if (automatic_phase == 4 && applied_target && active == 2) {
              Check(automatic_slot_creates[2] == 1 && addon::nr_features_created - selected_features_before == 3,
                  "the second automatically requested target reaches real 50-percent Applied dimensions");
              ++automatic_phase;
            } else if (automatic_phase == 5 && automatic_trial_before && automatic_requested_before == 2
                && automatic_previous_before == 1 && automatic_baseline_fps_before == 84.
                && session.status.generation > automatic_generation_before && session.blocked && !session.trial
                && session.level == 1) {
              // A ready rollback may already have replaced the display reason
              // with WARMING in this source. Observe the decision transition.
              Check(session.baseline_fps == 84. && session.status.requested == session.ladder.sizes[1],
                  "the worse 60 FPS result returns through the actual no-benefit branch to the accepted 75-percent anchor");
              automatic_cooldown_deadline = session.blocked_until_ns;
              ++automatic_phase;
            } else if (automatic_phase == 6 && applied_target && active == 1) {
              Check(addon::features.at(owner).slots[0].handle == automatic_anchor
                  && automatic_slot_creates[1] == 1 && session.blocked,
                  "the observed rollback Applied uses the original accepted 75-percent Feature without recreating it");
              ++automatic_phase;
            } else if (automatic_phase == 7 && session.level == 2 && session.trial && session.previous_level == 1) {
              Check(!session.blocked && now_ns < automatic_cooldown_deadline && session.baseline_fps == 60.
                  && !demand_ready_before[2],
                  "a synthetic 84-to-60 workload change reopens the real controller before cooldown expiry after the old 50-percent target retired");
              ++automatic_phase;
            } else if (automatic_phase == 8 && applied_target && active == 2) {
              Check(automatic_slot_creates == std::array<uint32_t, 3>{1, 1, 2}
                  && addon::nr_features_created - selected_features_before == 4
                  && cache->tiers[1].state.slots[0].handle == automatic_anchor,
                  "automatic re-exploration cold-creates a new 50-percent target while preserving the accepted rollback Feature");
              ++automatic_phase;
            }
            double gpu_median = 0., gpu_mad = 0.;
            const bool gpu_valid = session.GpuStatistics(now_ns, &gpu_median, &gpu_mad);
            automatic["states"].push_back({{"frame", frame}, {"phase", automatic_phase}, {"now_ns", now_ns},
                {"requested_slot", session.level}, {"active_slot", active}, {"generation", session.status.generation},
                {"target_ready", session.TargetReady()}, {"full_prepared", session.prepared}, {"applied_ns", session.applied_ns},
                {"applied_size", {session.status.applied[0].width, session.status.applied[0].height}},
                {"mock_fps", mock_output_fps}, {"reason", adaptive::ReasonText(session.status.reason)},
                {"trial", session.trial}, {"previous_level", session.previous_level}, {"blocked", session.blocked},
                {"blocked_until_ns", session.blocked_until_ns}, {"baseline_fps", session.baseline_fps},
                {"baseline_gpu_us", session.baseline_gpu_us}, {"baseline_gpu_mad", session.baseline_gpu_mad},
                {"gpu_sample_count", session.gpu_count}, {"gpu_statistics_available", gpu_valid},
                {"gpu_median_us", gpu_valid ? nlohmann::json(gpu_median) : nlohmann::json(nullptr)},
                {"gpu_mad_us", gpu_valid ? nlohmann::json(gpu_mad) : nlohmann::json(nullptr)},
                {"before_gpu_statistics_available", automatic_gpu_valid_before},
                {"before_gpu_median_us", automatic_gpu_valid_before ? nlohmann::json(automatic_gpu_before) : nlohmann::json(nullptr)},
                {"before_gpu_mad_us", automatic_gpu_valid_before ? nlohmann::json(automatic_gpu_mad_before) : nlohmann::json(nullptr)},
                {"before_baseline_gpu_us", automatic_baseline_before}, {"before_requested_slot", automatic_requested_before},
                {"before_trial", automatic_trial_before}, {"before_previous_level", automatic_previous_before},
                {"before_baseline_fps", automatic_baseline_fps_before},
                {"before_generation", automatic_generation_before}, {"before_applied_ns", automatic_applied_ns_before},
                {"retirement_pending_before", automatic_retiring_before}, {"retirement_pending_after", addon::AdaptiveRetirementPending(owner)},
                {"resident_features", actual_resident_features}, {"retiring_features", addon::retired_nr_features.size()},
                {"slot_create_counts", automatic_slot_creates}});
            if (automatic_phase != phase_before) {
              automatic["events"].push_back(automatic["states"].back());
              auto& event = automatic["events"].back();
              event["event"] = (phase_before == 5 ? "no_benefit_rollback_decision"
                  : (phase_before == 6 ? "accepted_anchor_applied_observed" : "controller_progress_observed"));
              if (phase_before == 5) {
                event["rollback_applied_in_same_source"] = applied_target && active == 1
                    && session.status.applied != automatic_applied_before;
              }
            }
            if (session.blocked && automatic_phase >= 2 && automatic_phase <= 4) {
              automatic_failure = "the genuine GPU/FPS acceptance gate rejected an earlier trial; later automatic stages were not reached";
            }
          } else if (limited_sequence) {
            auto& session = *addon::features.at(owner).adaptive_session;
            Check(cache->count == 15 && session.settings.limited_residency && !session.prepared,
                "limited native mode preserves fifteen logical tiers without claiming full-ladder readiness");
            const bool applied_target = session.TargetReady() && cache->active == session.level
                && session.status.applied == cache->ladder.sizes[cache->active];
            auto& finite = report["selected_collection"]["limited_residency"];
            if (limited_phase == 0 && applied_target && active == 0) {
              recorded_ready = true;
              selected_ready_frame = frame;
              selected_ready_features = addon::nr_features_created;
              selected_ready_worksets = addon::worksets_created;
              finite["initial_target"] = {{"ready_frame", frame}, {"target_ready", true}, {"applied_slot", active},
                  {"measured_source_calls", prefix_host_evaluates + frame + 1},
                  {"session_prepared", session.prepared}, {"logical_levels", cache->count},
                  {"resident_features", actual_resident_features},
                  {"actual_create_count", addon::nr_features_created - selected_features_before},
                  {"actual_worksets", addon::worksets_created - selected_worksets_before},
                  {"ready_wall_ms_excluding_setup", report["selected_collection"].at("prefix_wall_ms").get<double>()
                      + std::chrono::duration<double, std::milli>(retired - selected_prepare_begin).count()},
                  {"end_to_end_ready_wall_ms", std::chrono::duration<double, std::milli>(retired - selected_prefix_begin).count()}};
              Check(actual_resident_features == 1, "limited native startup creates only the complete healthy upper");
              ++limited_phase;
              limited_request_issued = false;
            } else if (limited_phase == 1 && applied_target && active == 6 && limited_retained_list != nullptr) {
              Check(actual_resident_features == 2 && !submission::ResourceReleasable(limited_retained_model),
                  "a requested cold target applies only after its complete real GPU receipt while retaining the upper");
              ++limited_phase;
              limited_request_issued = false;
            } else if (limited_phase == 2 && applied_target && active == 0) {
              Check(addon::AdaptiveRetirementPending(owner) && !submission::ResourceReleasable(limited_retained_model),
                  "return to the protected upper preserves the old completed-but-replayable lower Feature");
              limited_wait_creates = addon::nr_features_created;
              ++limited_phase;
              limited_request_issued = false;
            } else if (limited_phase == 3) {
              Check(active == 0 && !session.TargetReady() && session.status.applied == cache->ladder.sizes[0]
                  && addon::nr_features_created == limited_wait_creates && addon::AdaptiveRetirementPending(owner)
                  && !submission::ResourceReleasable(limited_retained_model),
                  "an unretired real lower recording blocks cold admission while the healthy upper still renders");
              limited_reverse_creates = addon::nr_features_created;
              ++limited_phase;
              limited_request_issued = false;
            } else if (limited_phase == 4 && applied_target && active == 0) {
              Check(addon::nr_features_created == limited_reverse_creates && addon::AdaptiveRetirementPending(owner),
                  "a reverse request cancels the unavailable cold target without adding a model");
              Check(SUCCEEDED(limited_retained_allocator->Reset())
                  && SUCCEEDED(limited_retained_list->Reset(limited_retained_allocator.Get(), nullptr))
                  && SUCCEEDED(limited_retained_list->Close()), "close the old recording only after its actual native Fence");
              addon::DrainRetiredResources(true);
              Check(!addon::AdaptiveRetirementPending(owner) && submission::ResourceReleasable(limited_retained_model),
                  "real retirement completes before a new cold target is admitted");
              ++limited_phase;
              limited_request_issued = false;
            } else if (limited_phase == 5 && applied_target && active == 10) {
              Check(addon::nr_features_created == limited_reverse_creates + 1 && actual_resident_features == 2,
                  "exact retirement permits only the newly requested complete target");
              ++limited_phase;
              limited_request_issued = false;
            } else if (limited_phase == 6 && applied_target && active == 0) {
              ++limited_phase;
            }
            finite["phase_states"].push_back({{"frame", frame}, {"next_phase", limited_phase},
                {"requested_slot", session.level}, {"active_slot", active}, {"target_ready", session.TargetReady()},
                {"session_prepared", session.prepared}, {"applied_size", {session.status.applied[0].width, session.status.applied[0].height}},
                {"resident_features", actual_resident_features}, {"retiring_nr_features", addon::retired_nr_features.size()},
                {"retiring_worksets", addon::retired_final_resources.size()},
                {"owner_retirement_pending", addon::AdaptiveRetirementPending(owner)}});
          } else if (all_ready) {
            if (!recorded_ready) {
              if (!report.contains("all_ready_frame")) report["all_ready_frame"] = frame;
              report["ready_frames"].push_back(frame);
            }
            recorded_ready = true;
            if (!demand_prepare && (selected_sequence || frame < 60) && !report.contains("initial_preparation")) {
              for (size_t slot = 0; slot < cache->count; ++slot) {
                const auto& state = (slot == cache->active ? addon::features.at(owner) : cache->tiers[slot].state);
                initial_models[slot] = reinterpret_cast<uintptr_t>(state.slots[0].handle);
                initial_parameters[slot] = reinterpret_cast<uintptr_t>(state.slots[0].parameters);
              }
              report["initial_preparation"] = {{"resident_slots", cache->count}, {"all_ready_frame", frame},
                  {"logical_coefficients", std::vector<float>(cache->ladder.coefficients.begin(), cache->ladder.coefficients.begin() + cache->count)},
                  {"tiers", tier_states}};
            }
            if (selected_sequence && selected_ready_frame == UINT32_MAX) {
              selected_ready_frame = frame;
              selected_ready_features = addon::nr_features_created;
              selected_ready_worksets = addon::worksets_created;
              selected_requests = (pipeline_steps ? std::vector<size_t>{0, cache->count - 1, 0}
                  : std::vector<size_t>{0, cache->count - 1, 0, cache->count / 2, cache->count - 1, 0});
              auto& selected = report["selected_collection"];
              selected["ready_frame"] = frame;
              selected["actual_levels"] = cache->count;
              selected["request_sequence"] = selected_requests;
              selected["selected_preparation_wall_ms"] = std::chrono::duration<double, std::milli>(retired - selected_prepare_begin).count();
              selected["full_preparation_wall_ms_excluding_setup"] = selected.at("prefix_wall_ms").get<double>()
                  + selected.at("selected_preparation_wall_ms").get<double>();
              selected["end_to_end_ready_wall_ms"] = std::chrono::duration<double, std::milli>(retired - selected_prefix_begin).count();
              selected["full_preparation_actual_features"] = addon::nr_features_created - selected_features_before;
              selected["full_preparation_actual_worksets"] = addon::worksets_created - selected_worksets_before;
              selected["sampled_peak_process_local_bytes_at_ready"] = selected_peak_local_bytes;
              selected["max_preparation_source_call_ms"] = selected_max_wrapper_ms;
              selected["max_preparation_source_start_interval_ms_excluding_setup"] = selected_max_source_interval_ms;
              Check(selected.at("full_preparation_create_count").get<uint32_t>() == cache->count
                        && selected.at("full_preparation_release_count").get<uint32_t>() == 0
                        && addon::nr_features_created - selected_features_before == cache->count
                        && addon::worksets_created - selected_worksets_before == cache->count,
                  "full selected readiness accounts for the upper and every standby without hidden rebuilding");
            }
          }
          if (selected_request) {
            selected_ready_creates += native_resize_creates;
            selected_ready_releases += native_resize_releases;
            Check(all_ready && active == selected_requested_slot && native_runtime_results.size() == 1
                      && native_resize_preparing.size() == 1 && !native_resize_preparing[0]
                      && native_resize_creates == 0 && native_resize_releases == 0
                      && addon::nr_features_created == selected_ready_features && addon::worksets_created == selected_ready_worksets
                      && addon::retired_nr_features.empty() && addon::retired_final_resources.empty()
                      && (!changed || native_runtime_resets[0] == 1),
                "consecutive ready-only requests apply this source frame with Reset and zero Feature or workset churn");
          }
          if (expect_reuse && frame >= 180) {
            Check(addon::features.at(owner).adaptive_session->IsLegal(
                      addon::features.at(owner).adaptive_session->status.applied),
                  "settings changes keep the actually applied dimensions within the current minimum");
            if (frame < 285 || frame >= 300) {
              Check(native_resize_creates == 0 && native_resize_releases == 0,
                    "compatible settings and minimum edits preserve prepared allocations");
            }
            if (frame == 180) {
              Check(!native_runtime_resets.empty() && native_runtime_resets[0] == 1,
                    "compatible style edit resets real NR history while retaining the Feature");
            }
            if (frame == 299) {
              Check(workset.input_width == width / 2 && workset.input_height == height / 2,
                    "requested new minimum reaches a completed real 50 percent NR tier");
            }
            if (reuse_feature && frame >= 285 && frame < 300) {
              const auto& candidate = (cache->active == endpoint_slot ? addon::features.at(owner) : cache->tiers[endpoint_slot].state);
              const auto& model = candidate.slots[0];
              const auto& tier = cache->tiers[endpoint_slot];
              endpoint_dimension_reset_seen |= (model.model_reset_reasons & addon::kResetDimensions) != 0;
              Check(native_resize_creates == 0 && native_resize_releases == 0
                        && reinterpret_cast<uintptr_t>(model.handle) == endpoint_model
                        && reinterpret_cast<uintptr_t>(model.parameters) == endpoint_parameters,
                    "capacity experiment retains only the completed candidate's own Feature and parameters without Create/Release");
              if (upper_capacity) {
                Check(endpoint_model == initial_models[endpoint_slot] && endpoint_parameters == initial_parameters[endpoint_slot]
                          && model.create_input_width == width && model.create_input_height == height,
                      "the new endpoint keeps the same initial upper-capacity Feature, parameter block and actual Create capacity");
              }
              for (size_t index = 0; index < native_resize_models.size(); ++index) {
                if (native_resize_models[index] == endpoint_model && native_runtime_resets[index] == 1) endpoint_reset_seen = true;
              }
              if (tier.ready) {
                endpoint_ready_seen = true;
                Check(tier.completion != nullptr && tier.completion != endpoint_old_completion
                          && tier.completion->closed.load() && tier.completion->submitted.load() && tier.completion->completed_healthy,
                      "retargeted candidate readiness comes from a new healthy GPU-complete recording");
              }
              if (frame == 299) {
                Check(endpoint_reset_seen && endpoint_dimension_reset_seen && endpoint_ready_seen && cache->active == endpoint_slot
                          && model.create_input_width >= width / 2 && model.create_input_height >= height / 2
                          && workset.proxy->GetDesc().Width == width / 2 && workset.proxy->GetDesc().Height == height / 2
                          && workset.nr_output->GetDesc().Width == width / 2 && workset.nr_output->GetDesc().Height == height / 2,
                      "the retained-capacity Feature resets and applies a real completed smaller physical workset");
              }
            }
          }
          if (changed) {
            Check(cache->active == (selected_sequence ? selected_requested_slot : (phase == 1 ? cache->count - 1 : 0)) && native_resize_creates == 0
                      && native_resize_releases == 0 && native_runtime_results.size() == 1
                      && native_runtime_resets.size() == 1 && native_runtime_resets[0] == 1
                      && addon::retired_nr_features.empty() && addon::retired_final_resources.empty(),
                  "ready selection switches immediately with reset, one real NR Evaluate and no Create/release/retirement");
          }
          if (!selected_sequence && !demand_prepare && frame == 150) {
            Check(native_resize_creates == 0 && native_resize_releases == 0 && recorded_ready,
                  "real Setting target-FPS edit preserves prepared GPU tiers without Create or release");
          }
          for (size_t index = 0; index < native_runtime_results.size(); ++index) {
            const auto sample = queries.Read(native_cost_interval + 1 + static_cast<UINT>(index));
            evaluations.push_back({{"model", native_resize_models[index]}, {"result", static_cast<uint32_t>(native_runtime_results[index])},
                {"preparing", static_cast<bool>(native_resize_preparing[index])},
                {"reset", native_runtime_resets[index]}, {"gpu_ms", sample.gpu_ms},
                {"begin_tick", sample.begin_tick}, {"end_tick", sample.end_tick}});
            if (demand_prepare) {
              evaluations.back()["cpu_ms"] = native_resize_evaluate_cpu_ms[index];
            }
            if (demand_adjacent) {
              size_t slot = 0;
              while (slot < cache->count && initial_models[slot] != native_resize_models[index]) {
                ++slot;
              }
              Check(slot < cache->count, "consecutive Evaluate belongs to an observed retained Feature slot");
              const bool first = slot != 0 && !demand_standby_evaluated[slot];
              demand_standby_evaluated[slot] = true;
              first_standby_evaluates += first;
              evaluations.back()["slot"] = slot;
              evaluations.back()["first_standby_evaluate"] = first;
            }
          }
        }
        if (native_resize_ready && !selected_sequence && !demand_prepare && frame < 60) {
          initial_creates += native_resize_creates;
          initial_releases += native_resize_releases;
          initial_create_ms += native_resize_create_ms;
          if (frame == 59) {
            Check(recorded_ready && initial_creates == cache->count - 1 && initial_releases == 0,
                  "initial preparation creates exactly the non-active resident Features and releases none");
            report["initial_preparation"]["standby_create_count"] = initial_creates;
            report["initial_preparation"]["standby_release_count"] = initial_releases;
            report["initial_preparation"]["cpu_create_total_ms"] = initial_create_ms;
            if (upper_capacity) Check(initial_creates == 2, "three-slot upper-capacity mode prepares exactly two standby Features");
          }
        }
        if (demand_prepare) {
          auto& session = *addon::features.at(owner).adaptive_session;
          auto& demand = report["demand_preparation"];
          Check(cache->count == 3 && session.level < (demand_adjacent ? 3u : 2u) && active < (demand_adjacent ? 3u : 2u)
                    && new_demand_models == native_resize_creates
                    && native_resize_releases == 0 && source_successes_after == source_successes_before + 1,
                "Session demand run preserves the active source chain and stays within its tested logical tiers");
          demand_requested_slots[session.level] = true;
          demand_total_creates += native_resize_creates;
          demand_total_create_ms += native_resize_create_ms;
          demand_peak_bytes = std::max({demand_peak_bytes, memory_before.CurrentUsage, memory_after.CurrentUsage});
          if (frame < 360) {
            demand_startup_creates += native_resize_creates;
            demand_startup_create_ms += native_resize_create_ms;
            Check(session.level == 0 && active == 0 && (!on_demand || native_resize_creates == 0),
                  "MOCK above-target output keeps the real upper tier and on-demand startup creates no standby");
          }
          if (cache->tiers[0].ready && !demand.contains("upper_ready_frame")) {
            demand["upper_ready_frame"] = frame;
          }
          if (session.level == 1 && !demand.contains("first_request_frame")) {
            demand["first_request_frame"] = frame;
            demand["first_request_baseline_gpu_ms"] = session.baseline_gpu_us / 1000.;
            demand["first_request_baseline_fps"] = session.baseline_fps;
          }
          if (demand_adjacent && session.level == 2 && !demand.contains("second_request_frame")) {
            demand["second_request_frame"] = frame;
            demand["second_request_baseline_gpu_ms"] = session.baseline_gpu_us / 1000.;
            demand["second_request_baseline_fps"] = session.baseline_fps;
          }
          if (session.level != active) {
            ++demand_wait_frames;
            Check((demand_adjacent ? session.level == active + 1 : session.level == 1 && active == 0)
                      && native_resize_models[0] == initial_models[active]
                      && !native_resize_preparing[0],
                  "while demand preparation is pending, actual successful NR continues on the legal current Feature");
          }
          if (active != demand_previous_active) {
            Check(demand_ready_before[active] && native_resize_models[0] == initial_models[active]
                      && !native_resize_preparing[0] && native_runtime_resets[0] == 1,
                  "demand activation evaluates the retained Feature with Reset after an already healthy preparation receipt");
            if (demand_adjacent && active > demand_previous_active) {
              Check(active == demand_previous_active + 1, "consecutive demand descends through the adjacent healthy tiers in order");
            }
          }
          if (active == 1 || (demand_adjacent && active == 2)) {
            demand_lower_applied = true;
            Check(cache->tiers[active].ready && session.status.applied == session.ladder.sizes[active]
                      && workset.input_width == session.ladder.sizes[active][0].width
                      && workset.input_height == session.ladder.sizes[active][0].height,
                  "the requested smaller physical NR workset actually applies after healthy GPU preparation");
            if (active == 1 && !demand.contains("first_lower_applied_frame")) {
              demand["first_lower_applied_frame"] = frame;
            }
            if (demand_adjacent && active == 2 && !demand.contains("second_lower_applied_frame")) {
              demand["second_lower_applied_frame"] = frame;
            }
          }
          if (demand_adjacent) {
            demand_applied_slots[active] = true;
            if (demand_previous_trial && demand_previous_requested == 1 && session.level == 2 && !demand_trial_accepted[1]) {
              // Prepare can accept trial 1 and request tier 2 in the same call.
              demand_trial_accepted[1] = true;
              demand["first_trial_accepted_frame"] = frame;
              demand["first_trial_acceptance_observation"] = "inferred from Session accepting trial 1 before requesting tier 2 in the same Prepare";
            }
            if (active != 0 && session.level == active && demand_previous_requested == active && demand_previous_trial
                && !session.trial && !session.blocked && session.status.reason == adaptive::Reason::OBSERVING
                && !demand_trial_accepted[active]) {
              demand_trial_accepted[active] = true;
              const char* key = (active == 1 ? "first_trial_accepted_frame" : "second_trial_accepted_frame");
              demand[key] = frame;
              if (active == 1) {
                demand["first_trial_acceptance_observation"] = "observed trial completion while tier 1 remained active and requested";
              }
            }
          }
          if (demand_lower_applied && active == 0 && session.level == 0 && session.status.reason == adaptive::Reason::NO_BENEFIT) {
            demand_returned_upper = true;
            Check(reinterpret_cast<uintptr_t>(addon::features.at(owner).slots[0].handle) == initial_models[0]
                      && reinterpret_cast<uintptr_t>(addon::features.at(owner).slots[0].parameters) == initial_parameters[0],
                  "Session no-benefit recovery returns to the same upper Feature and parameters");
            if (!demand.contains("first_no_benefit_upper_frame")) {
              demand["first_no_benefit_upper_frame"] = frame;
            }
          }
          if (frame == 359) {
            demand["startup"] = {{"configured_slots", cache->count}, {"actual_resident_features", actual_resident_features},
                {"standby_create_count", demand_startup_creates}, {"cpu_create_total_ms", demand_startup_create_ms},
                {"peak_local_bytes", demand_peak_bytes}, {"tiers", tier_states}};
            if (demand_adjacent) {
              demand_initial_neighbor_ready = cache->tiers[1].ready;
              demand["startup"]["first_neighbor_ready"] = demand_initial_neighbor_ready;
            }
          }
          demand_previous_active = active;
          demand_previous_requested = session.level;
          demand_previous_trial = session.trial;
        }
        Check(gpu.fence->GetCompletedValue() >= gpu.serial && gpu.fence->GetCompletedValue() != UINT64_MAX
                  && SUCCEEDED(gpu.device->GetDeviceRemovedReason()), "resize GPU fence completes on healthy device");
        report["frames"].push_back({{"frame", frame}, {"phase", phase}, {"switch_id", (changed ? phase : 0)},
            {"setting_event", setting_event},
            {"request_scale", (native_resize_ready
                ? addon::features.at(owner).adaptive_session->ladder.coefficients[active] : (phase == 1 ? 2.f / 3.f : 1.f))},
            {"nr_width", workset.input_width}, {"nr_height", workset.input_height}, {"workset_id", workset.id},
            {"workset_bytes", workset.allocated_bytes}, {"cpu_create_count", native_resize_creates},
            {"cpu_create_ms", native_resize_create_ms}, {"cpu_release_count", native_resize_releases},
            {"create_runtime_lock_held", native_resize_create_runtime_locked},
            {"create_ngx_call_active", native_resize_create_ngx_call},
            {"cpu_release_ms", native_resize_release_ms}, {"cpu_host_sr_ms", native_resize_host_ms},
            {"cpu_nr_evaluate_ms", native_resize_evaluate_ms}, {"cpu_wrapper_ms", wrapper_ms},
            {"fixed_input_probe", fixed_input}, {"fixed_input_copy_count", native_resize_fixed_input_copies - fixed_copies_before},
            {"fixed_input_cpu_ms", native_resize_fixed_input_cpu_ms},
            {"cpu_resource_wrapper_residual_ms", wrapper_ms - native_resize_create_ms - native_resize_host_ms - native_resize_evaluate_ms},
            {"start_ms", std::chrono::duration<double, std::milli>(start - origin).count()},
            {"raw_start_interval_ms", std::chrono::duration<double, std::milli>(start - previous).count()},
            {"source_interval_includes_previous_readback", settings_reuse && (frame == 91 || frame == 300 || (upper_capacity && frame == 299))},
            {"nr_evaluated", evaluated}, {"gpu_wrapper_ms", wrapper_gpu.gpu_ms},
            {"gpu_nr_ms", (evaluated ? nlohmann::json(nr_gpu.gpu_ms) : nlohmann::json(nullptr))},
            {"gpu_wrapper_begin_tick", wrapper_gpu.begin_tick}, {"gpu_wrapper_end_tick", wrapper_gpu.end_tick},
            {"gpu_nr_begin_tick", nr_gpu.begin_tick}, {"gpu_nr_end_tick", nr_gpu.end_tick},
            {"cpu_submit_wait_ms", std::chrono::duration<double, std::milli>(completed - submitted).count()},
            {"runtime_lock_contentions", renodx::addons::dlss5::runtime_lock_contended.load() - runtime_contentions_before},
            {"runtime_lock_wait_max_ns_cumulative", renodx::addons::dlss5::runtime_lock_wait_max_ns.load()},
            {"submit_lock_contentions", renodx::addons::dlss5::submit_lock_blocked.load() - submit_contentions_before},
            {"submit_lock_wait_max_ns_cumulative", renodx::addons::dlss5::submit_lock_wait_max_ns.load()},
            {"cpu_retirement_ms", std::chrono::duration<double, std::milli>(retired - completed).count()},
            {"completion_from_start_ms", std::chrono::duration<double, std::milli>(completed - start).count()},
            {"retired_nr_before_submit", retired_nr_before}, {"retired_worksets_before_submit", retired_worksets_before},
            {"retired_nr_after_drain", addon::retired_nr_features.size()},
            {"retired_worksets_after_drain", addon::retired_final_resources.size()},
            {"local_memory_before_submit", memory_before.CurrentUsage}, {"local_memory_after_drain", memory_after.CurrentUsage},
            {"local_budget", memory_after.Budget}, {"host_result", static_cast<uint32_t>(result)},
            {"nr_result", (evaluated ? nlohmann::json(static_cast<uint32_t>(native_runtime_results[0])) : nlohmann::json(nullptr))},
            {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()}});
        report["frames"].back()["active_slot"] = active;
        if (limited_sequence) {
          report["frames"].back()["limited_phase_after_source"] = (automatic_limited ? automatic_phase : limited_phase);
          report["frames"].back()["fixture_recording_replacement_ms"] = limited_list_replacement_ms;
          report["frames"].back()["fixture_recording_replacement_scope"] = "after the measured submit/Fence completion, before retirement polling; excluded from Hook and GPU timestamps";
        }
        report["frames"].back()["tiers_before_submit"] = std::move(tiers_before);
        report["frames"].back()["tiers"] = std::move(tier_states);
        report["frames"].back()["nr_evaluations"] = std::move(evaluations);
        if (settings_reuse && frame >= 285 && frame <= 299) {
          report["frames"].back()["core_parameter_snapshots"] = native_resize_parameter_snapshots;
        }
        if (selected_sequence) {
          report["frames"].back()["create_calls"] = native_resize_create_calls;
          report["frames"].back()["core_parameter_snapshots"] = native_resize_parameter_snapshots;
          report["frames"].back()["selected_request"] = selected_request;
          report["frames"].back()["selected_request_index"] = (selected_request ? nlohmann::json(selected_request_index - 1) : nlohmann::json(nullptr));
          report["frames"].back()["worksets_created_total"] = addon::worksets_created;
          if (frame != 0 && report["frames"][frame - 1].at("cpu_create_count").get<uint32_t>() != 0) {
            report["frames"][frame - 1]["next_successful_source_start_gap_ms"] = report["frames"].back().at("start_ms").get<double>()
                - report["frames"][frame - 1].at("start_ms").get<double>();
          }
        }
        if (native_resize_ready) {
          report["frames"].back()["source_successes_before"] = source_successes_before;
          report["frames"].back()["source_successes_after"] = source_successes_after;
          report["frames"].back()["source_successes_delta"] = source_successes_after - source_successes_before;
          report["frames"].back()["configuration_epoch"] = addon::present_restore_configuration.epoch;
          report["frames"].back()["cache_layout_identity"] = cache->identity;
          report["frames"].back()["session_identity"] = addon::features.at(owner).adaptive_session->identity;
          report["frames"].back()["target_fps_setting"] = addon::nr_adaptive_target_fps.load();
          report["frames"].back()["minimum_scale_setting"] = addon::nr_adaptive_min_scale.load();
          report["frames"].back()["style_setting"] = addon::style.load();
          report["frames"].back()["transfer_strength_setting"] = addon::transfer_strength.load();
          report["frames"].back()["requested_slot"] = addon::features.at(owner).adaptive_session->level;
        }
        if (demand_prepare || automatic_limited) {
          const auto& session = *addon::features.at(owner).adaptive_session;
          report["frames"].back()["create_calls"] = native_resize_create_calls;
          report["frames"].back()["demand"] = {{"fps_evidence", "MOCK"}, {"mock_output_fps", mock_output_fps},
              {"window_serial", frame + 1}, {"window_begin_ns", mock_window_end_ns - 500'000'000}, {"window_end_ns", mock_window_end_ns},
              {"reason", adaptive::ReasonText(session.status.reason)}, {"trial", session.trial}, {"blocked", session.blocked},
              {"gpu_sample_count", session.gpu_count}, {"production_gpu_ms", session.status.gpu_ms},
              {"production_timer_state", renodx::addons::dlss5::gpu_timers::StateText(renodx::addons::dlss5::gpu_timers::stage_stats[1].state)},
              {"production_timer_sequence", renodx::addons::dlss5::gpu_timers::stage_stats[1].sequence},
              {"production_timer_published", renodx::addons::dlss5::gpu_timers::stage_published[1]},
              {"baseline_gpu_ms", session.baseline_gpu_us / 1000.}, {"baseline_output_fps", session.baseline_fps},
              {"requested_size", {session.status.requested[0].width, session.status.requested[0].height}},
              {"applied_size", {session.status.applied[0].width, session.status.applied[0].height}},
              {"requested_slot_ready_before_recording", demand_ready_before[session.level]},
              {"active_slot_ready_before_recording", demand_ready_before[active]},
              {"actual_resident_features", actual_resident_features}, {"configured_slots", cache->count}};
          if (demand_adjacent) {
            report["frames"].back()["first_standby_evaluate_count"] = first_standby_evaluates;
            report["frames"].back()["demand"]["frame_start_active_slot"] = demand_active_before;
            report["frames"].back()["demand"]["defer_preparation"] = session.defer_preparation;
          }
          if (frame != 0) {
            auto& previous_frame = report["frames"][frame - 1];
            if (previous_frame.at("cpu_create_count").get<uint32_t>() != 0
                || (demand_adjacent && previous_frame.value("first_standby_evaluate_count", 0u) != 0)) {
              previous_frame["next_successful_source_frame"] = frame;
              previous_frame["next_successful_source_start_gap_ms"] = report["frames"].back().at("start_ms").get<double>()
                  - previous_frame.at("start_ms").get<double>();
            }
          }
        }
        // Upper-capacity retargeting activates one frame earlier; 298 pairs with
        // baseline 299 at the same number of evaluations after the final Reset.
        if (settings_reuse && (frame == 90 || frame == 299 || (upper_capacity && frame == 298))) {
          const auto observation_started = std::chrono::steady_clock::now();
          report["frames"].back()["pixel_observation"] = {
              {"proxy", observe_pixels(workset.proxy, workset.proxy_state, frame, "proxy")},
              {"neural", observe_pixels(workset.nr_output, workset.nr_output_state, frame, "neural")},
              {"output", observe_pixels(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, frame, "output")}};
          report["frames"].back()["pixel_observation_cpu_ms"] = std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - observation_started).count();
          report["frames"].back()["pixel_observation_after_measurement"] = true;
          report["frames"].back()["next_source_interval_includes_readback"] = true;
        }
        if (changed || native_resize_creates != 0) {
          std::cout << "RESIZE switch=" << phase << " create_ms=" << native_resize_create_ms
                    << " wrapper_ms=" << wrapper_ms << " first_nr_gpu_ms=" << nr_gpu.gpu_ms
                    << " completion_ms=" << std::chrono::duration<double, std::milli>(completed - start).count() << std::endl;
        }
        previous = start;
        if ((automatic_limited && (automatic_phase == 9 || !automatic_failure.empty()))
            || (limited_sequence && !automatic_limited && limited_phase == 7)
            || (selected_sequence && !limited_sequence && selected_request_index == selected_requests.size())) break;
        deadline = std::max(deadline + std::chrono::microseconds(16667), std::chrono::steady_clock::now());
        std::this_thread::sleep_until(deadline);
      }
      if (automatic_limited) {
        auto& finite = report["selected_collection"]["limited_residency"];
        const double wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - selected_prefix_begin).count();
        if (wall_ms > 20000. && automatic_failure.empty()) {
          automatic_failure = "automatic sequence exceeded the fixed 20-second real wall bound including prefix";
        }
        const bool passed = automatic_phase == 9 && automatic_failure.empty() && recorded_ready
            && automatic_slot_creates == std::array<uint32_t, 3>{1, 1, 2} && automatic_pending_frames != 0;
        if (!passed && automatic_failure.empty()) {
          automatic_failure = "900-source bound reached without all automatic stages; inspect real GPU median/MAD and controller reasons";
        }
        finite["automatic"]["result"] = {{"status", passed ? "Passed" : "Failed"}, {"completed_phase", automatic_phase},
            {"failure", passed ? nlohmann::json(nullptr) : nlohmann::json(automatic_failure)},
            {"actual_source_frames", report["frames"].size()}, {"prefix_source_calls", prefix_host_evaluates},
            {"wall_ms_including_prefix", wall_ms},
            {"slot_create_counts", automatic_slot_creates}, {"waiting_target_source_frames", automatic_pending_frames},
            {"peak_features_including_retirement", automatic_peak_features}, {"sampled_peak_process_local_bytes", selected_peak_local_bytes},
            {"direct_session_requests", false}, {"injected_gpu_samples", false}, {"synthetic_time", false},
            {"async_autoscale", false}, {"cooldown_expiry_tested", false},
            {"not_tested", "game output-FPS accuracy, dynamic visual quality, full 15-level automatic exploration and allocation failure"}};
        report["probe_execution"] = (passed ? "Passed" : "Failed");
        demand_validation_failed |= !passed;
      } else if (limited_sequence) {
        Check(limited_phase == 7 && recorded_ready && selected_ready_frame != UINT32_MAX,
            "the limited target-ready roundtrip and reverse request finish within the existing source bound");
        auto& finite = report["selected_collection"]["limited_residency"];
        finite["result"] = {{"completed_phases", limited_phase}, {"actual_source_frames", report["frames"].size()},
            {"prefix_source_calls", prefix_host_evaluates}, {"total_measured_source_calls", prefix_host_evaluates + report["frames"].size()},
            {"actual_create_count", addon::nr_features_created - selected_features_before},
            {"actual_worksets", addon::worksets_created - selected_worksets_before},
            {"max_source_call_ms", selected_max_wrapper_ms}, {"sampled_peak_process_local_bytes", selected_peak_local_bytes},
            {"memory_scope", "QueryVideoMemoryInfo LOCAL CurrentUsage; complete process including SR, NR, inputs/outputs, working surfaces and retirement; sampled, not incremental tier-only or instantaneous peak"},
            {"not_tested", "dynamic visual quality, automatic output-FPS decisions and resource-failure injection"}};
        report["mode"] = (limited_async_pso ? "cold-limited-target-ready-async-psos" : "cold-limited-target-ready-sync-psos");
        report["selection"] = "MOCK direct target requests; real SR/NR and exact retirement; no full-ladder readiness claim";
        report["probe_execution"] = "Passed";
      } else if (selected_sequence) {
        Check(selected_request_index == selected_requests.size() && selected_ready_frame != UINT32_MAX,
            "the full selected collection and requested roundtrip complete within the original 180-source bound");
        auto& selected = report["selected_collection"];
        selected["actual_source_frames"] = report["frames"].size();
        selected["ready_request_count"] = selected_request_index;
        selected["ready_create_count"] = selected_ready_creates;
        selected["ready_release_count"] = selected_ready_releases;
        selected["ready_worksets_created"] = addon::worksets_created - selected_ready_worksets;
        selected["sampled_peak_process_local_bytes"] = selected_peak_local_bytes;
        selected["max_source_call_ms"] = selected_max_wrapper_ms;
        selected["max_source_start_interval_ms_excluding_setup"] = selected_max_source_interval_ms;
        selected["max_source_start_interval_ms_including_setup"] = std::max(selected_max_source_interval_ms,
            selected.at("prefix_to_selected_first_source_interval_ms").get<double>());
        selected["cpu_scope"] = "HookedEvaluateFeature CPU sums include real host SR and NR wrapper work; SDK Create and Evaluate are separate; fences, pacing and probe setup remain wall time";
        if (pipeline_steps) {
          const auto& trace_frames = report["cold_cpu_trace"]["frames"];
          Check(trace_frames.size() == prefix_host_evaluates + report["frames"].size() && !cold_components_failed,
              "every prefix and selected source has exactly one valid complete trace");
          uint32_t pso_calls = 0, max_pso_calls = 0, ngx_creates = 0, sr_calls = 0, nr_calls = 0;
          int64_t pso_ns = 0, ngx_ns = 0, sr_ns = 0, nr_ns = 0;
          for (const auto& source : trace_frames) {
            const auto pso = source.at("pso_create_api_calls").get<uint32_t>();
            pso_calls += pso;
            max_pso_calls = std::max(max_pso_calls, pso);
            ngx_creates += source.at("ngx_create_calls").get<uint32_t>();
            sr_calls += source.at("sr_evaluate_calls").get<uint32_t>();
            nr_calls += source.at("nr_evaluate_calls").get<uint32_t>();
            pso_ns += source.at("pso_create_cpu_ns").get<int64_t>();
            ngx_ns += source.at("ngx_create_cpu_ns").get<int64_t>();
            sr_ns += source.at("sr_evaluate_cpu_ns").get<int64_t>();
            nr_ns += source.at("nr_evaluate_cpu_ns").get<int64_t>();
          }
          report["pipeline_steps"].update({{"prefix_sources", prefix_host_evaluates},
              {"selected_sources", report["frames"].size()}, {"pso_create_api_calls", pso_calls},
              {"max_pso_create_api_calls_per_source", max_pso_calls}, {"pso_create_cpu_ns", pso_ns},
              {"ngx_create_calls", ngx_creates}, {"ngx_create_cpu_ns", ngx_ns},
              {"sr_evaluate_calls", sr_calls}, {"sr_evaluate_cpu_ns", sr_ns},
              {"nr_evaluate_calls", nr_calls}, {"nr_evaluate_cpu_ns", nr_ns},
              {"max_source_call_ms", selected_max_wrapper_ms}, {"ready_pso_create_calls", 0},
              {"full_preparation_wall_ms_excluding_setup", selected.at("full_preparation_wall_ms_excluding_setup")},
              {"end_to_end_ready_wall_ms", selected.at("end_to_end_ready_wall_ms")},
              {"wall_scope", "includes measured-source reporting/export and pacing; fixture setup gap is separately reported; no total-time or per-source latency guarantee"}});
        }
        report["probe_execution"] = "Passed";
      }
      if (native_resize_ready) {
        if (!demand_prepare && !automatic_limited) {
          Check(preparing_evaluates != 0, "real ready-cache run includes extra preparation Evaluate calls");
        }
        report["preparing_nr_evaluate_count"] = preparing_evaluates;
        if (demand_prepare) {
          report["demand_preparation"]["result"] = {{"startup_standby_create_count", demand_startup_creates},
              {"total_standby_create_count", demand_total_creates}, {"cpu_create_total_ms", demand_total_create_ms},
              {"slot_create_counts", demand_slot_creates}, {"requested_slots", demand_requested_slots},
              {"pending_upper_source_frames", demand_wait_frames}, {"lower_actually_applied", demand_lower_applied},
              {"no_benefit_returned_original_upper", demand_returned_upper}, {"peak_local_bytes", demand_peak_bytes}};
          bool passed = false;
          if (demand_adjacent) {
            auto& result = report["demand_preparation"]["result"];
            result.erase("pending_upper_source_frames");
            result.erase("no_benefit_returned_original_upper");
            result["pending_legal_source_frames"] = demand_wait_frames;
            result["applied_slots"] = demand_applied_slots;
            result["trial_accepted"] = demand_trial_accepted;
            result["second_standby_unrequested_create"] = demand_second_prefetch_observed;
            result["final_active_slot"] = addon::features.at(owner).adaptive_cache->active;
            passed = preparing_evaluates != 0 && demand_startup_creates == (adjacent_ready ? 1u : 0u)
                && demand_total_creates == 2 && demand_slot_creates[1] == 1 && demand_slot_creates[2] == 1
                && demand_requested_slots[1] && demand_requested_slots[2] && demand_applied_slots[1] && demand_applied_slots[2]
                && demand_trial_accepted[1] && demand_trial_accepted[2] && (!adjacent_ready || demand_initial_neighbor_ready)
                && addon::features.at(owner).adaptive_cache->active == 2 && addon::nr_resolution_scale.load() == 1.f;
          } else {
            passed = preparing_evaluates != 0 && demand_startup_creates == (on_demand ? 0u : 2u)
                    && demand_total_creates == (on_demand ? 1u : 2u)
                    && demand_slot_creates[1] == 1 && demand_slot_creates[2] == (on_demand ? 0u : 1u)
                    && !demand_requested_slots[2] && demand_requested_slots[1]
                    && demand_lower_applied && demand_returned_upper && (!on_demand || demand_wait_frames != 0)
                    && addon::nr_resolution_scale.load() == 1.f;
          }
          report["probe_execution"] = (passed ? "Passed" : "Failed");
          if (!passed) {
            demand_validation_failed = true;
            report["failure"] = (demand_adjacent
                ? "real Session did not accept both consecutive healthy lower tiers with the expected standby Create counts and initial adjacent readiness"
                : "real Session demand trial did not reach its completed lower tier and return to the original upper with the expected standby Create counts");
          }
        } else {
          Check(recorded_ready && addon::nr_resolution_scale.load() == 1.f,
                "real ready-cache roundtrip preserves the manual upper resolution");
        }
        auto& before_off = addon::features.at(owner);
        size_t low_slot = 0;
        if (!demand_prepare && !pipeline_steps && !automatic_limited) {
          for (size_t slot = 1; slot < before_off.adaptive_cache->count; ++slot) {
            const auto& cache = *before_off.adaptive_cache;
            if (cache.tiers[slot].ready && before_off.adaptive_session->IsLegal(cache.ladder.sizes[slot])
                && cache.ladder.sizes[slot] == before_off.adaptive_session->ladder.sizes[slot]
                && cache.ladder.coefficients[slot] < cache.ladder.coefficients[low_slot]) {
              low_slot = slot;
            }
          }
          Check(low_slot != 0, "native Off test starts from a legal completed low tier");
          before_off.adaptive_session->Request(low_slot, addon::SteadyNowNs());
        }
        report["setting_transactions"] = nlohmann::json::array();
        const auto evaluate_transaction = [&](const char* name, bool disabled) {
          if (disabled) addon::kSetAdaptiveEnabled.Store(0.f);
          native_runtime_masks.clear();
          native_runtime_results.clear();
          native_runtime_resets.clear();
          native_resize_models.clear();
          native_resize_preparing.clear();
          native_resize_evaluate_cpu_ms.clear();
          native_resize_create_ms = native_resize_release_ms = native_resize_host_ms = native_resize_evaluate_ms = 0;
          native_resize_creates = native_resize_releases = 0;
          gpu.list->ClearState(nullptr);
          const auto start = std::chrono::steady_clock::now();
          addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(ObserveNativeHostEvaluate);
          const auto source_successes_before = addon::SourceSuccessfulEvaluations();
          const auto result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
          const auto source_successes_after = addon::SourceSuccessfulEvaluations();
          addon::ngx_slot_real[0].evaluate = nullptr;
          const auto recorded = std::chrono::steady_clock::now();
          submit_resize();
          if (demand_prepare || selected_sequence) {
            std::scoped_lock runtime_lock(addon::runtime_mutex);
            addon::DrainRetiredResources(true);
            renodx::addons::dlss5::gpu_timers::Poll(true, false);
            gpu.uploads.clear();
            gpu.retained_resources.clear();
          } else {
            addon::DrainRetiredResources(true);
          }
          auto& feature = addon::features.at(owner);
          const size_t slot = (feature.adaptive_cache != nullptr ? feature.adaptive_cache->active : 0);
          const auto& workset = addon::worksets.at({owner, output.Get(), false, static_cast<uint32_t>(slot)});
          std::cout << "RESIZE setting=" << name << " creates=" << native_resize_creates
                    << " releases=" << native_resize_releases << " active=" << slot
                    << " wrapper_ms=" << std::chrono::duration<double, std::milli>(recorded - start).count() << std::endl;
          Check(NVSDK_NGX_SUCCEED(result) && native_runtime_results.size() == 1
                    && NVSDK_NGX_SUCCEED(native_runtime_results[0]) && native_resize_creates == 0
                    && reinterpret_cast<uintptr_t>(feature.slots[0].handle) == stable_models[slot]
                    && workset.id == stable_worksets[slot]
                    && gpu.fence->GetCompletedValue() >= gpu.serial
                    && gpu.fence->GetCompletedValue() != UINT64_MAX && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
                "real setting transaction uses the existing correct Feature/workset and completes on the GPU without Create");
          Check(native_resize_preparing.size() == 1 && !native_resize_preparing[0]
                    && source_successes_after == source_successes_before + 1,
                "real setting transaction counts its active NR output as one successful source frame");
          if (disabled) {
            Check(feature.adaptive_cache == nullptr && slot == 0 && workset.input_width == width
                      && workset.input_height == height && addon::retired_nr_features.empty()
                      && addon::retired_final_resources.empty(),
                  "turning adaptive Off restores the prepared manual upper tier and retires all standby tiers");
            if (demand_prepare) {
              Check(reinterpret_cast<uintptr_t>(feature.slots[0].handle) == initial_models[0]
                        && reinterpret_cast<uintptr_t>(feature.slots[0].parameters) == initial_parameters[0],
                    "demand probe Off restores the same original upper Feature and parameters");
            }
          } else {
            Check(feature.adaptive_cache != nullptr && slot == low_slot,
                  "Off restoration starts from a real ready low tier");
          }
          report["setting_transactions"].push_back({{"name", name}, {"disabled", disabled},
              {"active_slot", slot}, {"nr_width", workset.input_width}, {"nr_height", workset.input_height},
              {"create_count", native_resize_creates}, {"release_count", native_resize_releases},
              {"wrapper_ms", std::chrono::duration<double, std::milli>(recorded - start).count()},
              {"model", reinterpret_cast<uintptr_t>(feature.slots[0].handle)}, {"workset_id", workset.id},
              {"host_result", static_cast<uint32_t>(result)}, {"nr_result", static_cast<uint32_t>(native_runtime_results[0])},
              {"source_successes_before", source_successes_before}, {"source_successes_after", source_successes_after},
              {"source_successes_delta", source_successes_after - source_successes_before},
              {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()}});
        };
        if (!demand_prepare && !pipeline_steps && !automatic_limited) {
          evaluate_transaction("ready-low-before-off", false);
        }
        evaluate_transaction("adaptive-off-real-setting", true);
      }
      addon::core_create_feature = native_resize_create;
      addon::core_release_feature = native_resize_release;
      addon::adaptive_feature_mode = addon::AdaptiveFeatureMode::OFF;
      native_resize_demand_prepare = false;
      native_resize_selected_sequence = false;
      native_resize_create_gpu = false;
      native_resize_create_calls.clear();
      native_resize_evaluate_cpu_ms.clear();
      if (demand_prepare || automatic_limited) {
        std::unique_lock lock(output_rate::internal::state.mutex);
        output_rate::internal::state.stream = {};
      }
      report["fixed_input_patterns"] = nlohmann::json::array();
      for (const auto& [size, input] : native_resize_fixed_inputs) {
        report["fixed_input_patterns"].push_back({{"size", {size.first, size.second}}, {"crc32", input.crc32}});
      }
      report["fixed_input_total_copies"] = native_resize_fixed_input_copies;
      native_resize_fixed_input = false;
      native_resize_observe_parameters = false;
      native_resize_frame = UINT32_MAX;
      native_resize_parameter_snapshots.clear();
      native_resize_fixed_inputs.clear();
      if (!demand_prepare) {
        report["feature_capacity_reuse_result"] = {{"requested", reuse_feature}, {"candidate_reset_observed", endpoint_reset_seen},
            {"candidate_dimension_reset_observed", endpoint_dimension_reset_seen},
            {"candidate_new_completion_observed", endpoint_ready_seen}, {"experiment_disabled_after_sequence", true}};
      }
      native_cost_list = nullptr;
      Check(addon::get_module_filename_iat == nullptr && addon::original_get_module_filename == nullptr,
            "resize uses no snippet caller identity bridge");
      report["identity_bridge_installed"] = false;
      std::ofstream file("adaptive-native-resize.json", std::ios::binary | std::ios::trunc);
      file << report.dump(2) << '\n';
      file.close();
      Check(!file.fail(), "resize evidence JSON saved");
      native_resize_ready = false;
    }
    if (motion_test && !resize_test && !sequence_test && !jitter_test) {
      constexpr std::array<float, 8> speeds{0.f, 0.f, 2.f, 8.f, 16.f, 0.f, 0.f, 0.f};
      std::array<std::vector<uint8_t>, speeds.size()> reference;
      std::array<std::vector<uint8_t>, speeds.size()> reference_sr;
      std::array<std::vector<uint8_t>, speeds.size()> reference_proxy;
      std::array<std::vector<uint8_t>, speeds.size()> reference_neural;
      auto motion_queries = d3d12_test::CreateTimestampQueries(gpu, 2 * static_cast<UINT>(speeds.size()));
      auto& report = motion_report;
      report = {{"evidence", "NATIVE_GPU+NATIVE_NGX"}, {"host_sr", "NATIVE_NGX"},
          {"backend", "production-owned-features"}, {"provider_init", "public SDK ProjectID; preinitialized device"},
          {"production_loader_init_tested", false}, {"render_width", render_width}, {"render_height", render_height},
          {"runtime", addon::direct_runtime_requirements.file_version},
          {"requested_model", "B"}, {"internal_model", "unknown"},
          {"format", "RGBA16_FLOAT"}, {"motion_format", "RG16_FLOAT"},
          {"width", width}, {"height", height}, {"start", 2}, {"end", 16},
          {"prefix_host_sr_evaluates", prefix_host_evaluates}, {"prefix_nr_evaluates", prefix_nr_evaluates},
          {"motion_scale", {-2, 1}}, {"motion_base", {0, 0}}, {"jitter", {0, 0}},
          {"feed_mode", addon::feed_mode.load()}, {"norm_governor", addon::norm_governor.load()},
          {"detail_stability", addon::detail_stability.load()},
          {"threshold_units", "NR input pixels per frame"}, {"frames", nlohmann::json::array()}};
      ComPtr<IDXGIAdapter1> native_adapter;
      Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&native_adapter))),
            "resolve native motion adapter from the actual device LUID");
      DXGI_ADAPTER_DESC1 adapter{};
      Check(SUCCEEDED(native_adapter->GetDesc1(&adapter)), "read native motion adapter identity");
      report["adapter"] = {{"vendor_id", adapter.VendorId}, {"device_id", adapter.DeviceId},
          {"luid_low", adapter.AdapterLuid.LowPart}, {"luid_high", adapter.AdapterLuid.HighPart}};
      report["driver_x100"] = addon::direct_runtime_requirements.driver_x100;
      report["nr_sha256"] = addon::direct_runtime_sha256;
      report["loaded_ngx_modules"] = nlohmann::json::array();
      std::array<HMODULE, 1024> modules{};
      DWORD module_bytes = 0;
      Check(EnumProcessModules(GetCurrentProcess(), modules.data(), sizeof(modules), &module_bytes)
                && module_bytes <= sizeof(modules), "enumerate actual native NGX backing modules");
      for (size_t index = 0; index < module_bytes / sizeof(HMODULE); ++index) {
        std::array<wchar_t, 4096> path{};
        const DWORD length = GetModuleFileNameW(modules[index], path.data(), static_cast<DWORD>(path.size()));
        Check(length != 0 && length < path.size(), "read loaded native module path");
        const std::filesystem::path module_path(path.data());
        if (module_path.filename().wstring().find(L"ngx") == std::wstring::npos) continue;
        report["loaded_ngx_modules"].push_back({{"path", module_path.string()},
            {"sha256", addon::ComputeRuntimeSha256(module_path)},
            {"version", addon::ReadNrRuntimeRequirements(module_path, gpu.device.Get()).file_version}});
      }
      double moving_difference = 0;
      for (const bool attenuate : {false, true}) {
        float shift = 0.f;
        addon::control_mask_motion_enabled = attenuate;
        for (size_t frame_index = 0; frame_index < speeds.size(); ++frame_index) {
          shift += speeds[frame_index];
          for (UINT y = 0; y < render_height; ++y) {
            for (UINT x = 0; x < render_width; ++x) {
              const size_t pixel = static_cast<size_t>(y) * render_width + x;
              const float u = static_cast<float>(x) / render_width - shift / width;
              const float v = static_cast<float>(y) / render_height;
              color_data[pixel * 4] = DirectX::PackedVector::XMConvertFloatToHalf(
                  0.35f + 0.2f * std::sin(u * 40.f) + 0.1f * v);
              color_data[pixel * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(0.1f + 0.65f * v);
              color_data[pixel * 4 + 2] = DirectX::PackedVector::XMConvertFloatToHalf(
                  0.4f + 0.2f * std::cos(u * 23.f + v * 11.f));
              motion_data[pixel * 2] = DirectX::PackedVector::XMConvertFloatToHalf(
                  speeds[frame_index] * static_cast<float>(render_width) / width / 2.f);
            }
          }
          auto dynamic_color = gpu.CreateTextureTransfer(color.Get());
          auto dynamic_motion = gpu.CreateTextureTransfer(motion.Get());
          for (auto* input : {color.Get(), motion.Get()}) {
            gpu.Transition(input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
          }
          gpu.UploadTexture(color.Get(), dynamic_color, reinterpret_cast<const uint8_t*>(color_data.data()),
              color_data.size() * sizeof(uint16_t), render_width * 8);
          gpu.UploadTexture(motion.Get(), dynamic_motion, reinterpret_cast<const uint8_t*>(motion_data.data()),
              motion_data.size() * sizeof(uint16_t), render_width * 4);
          gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          gpu.Transition(motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
          parameters.Set(NVSDK_NGX_Parameter_Reset, (frame_index == 0 ? 1 : 0));
          parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, -2.f);
          auto sr_readback = gpu.CreateTextureTransfer(output.Get());
          const auto& workset = addon::worksets.at({owner, output.Get(), false});
          auto proxy_readback = gpu.CreateTextureTransfer(workset.proxy);
          auto neural_readback = gpu.CreateTextureTransfer(workset.nr_output);
          native_motion_sr_readback = &sr_readback;
          native_motion_proxy_readback = &proxy_readback;
          native_motion_neural_readback = &neural_readback;
          native_motion_sr_calls = 0;
          native_runtime_masks.clear();
          native_runtime_results.clear();
          native_runtime_resets.clear();
          const UINT interval = static_cast<UINT>(frame_index + (attenuate ? speeds.size() : 0));
          motion_queries.Begin(gpu.list.Get(), interval);
          addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(ObserveNativeHostEvaluate);
          const auto result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
          addon::ngx_slot_real[0].evaluate = nullptr;
          native_motion_sr_readback = nullptr;
          native_motion_proxy_readback = nullptr;
          native_motion_neural_readback = nullptr;
          motion_queries.EndAndResolve(gpu.list.Get(), interval);
          Check(NVSDK_NGX_SUCCEED(result) && native_motion_sr_calls == 1 && native_runtime_results.size() == 1
                    && NVSDK_NGX_SUCCEED(native_runtime_results[0]) && native_runtime_masks[0] != nullptr,
                "dynamic motion frame records one real SR and one real NR Evaluate with a generated mask");
          Check(native_runtime_resets.size() == 1 && native_runtime_resets[0] == (frame_index == 0 ? 1 : 0),
                "paired motion sequences have the same explicit Reset prefix");
          auto* mask = native_runtime_masks[0];
          auto mask_readback = gpu.CreateTextureTransfer(mask);
          auto final_readback = gpu.CreateTextureTransfer(output.Get());
          for (auto* resource : {mask, output.Get()}) {
            gpu.Transition(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
          }
          gpu.RecordTextureReadback(mask, mask_readback);
          gpu.RecordTextureReadback(output.Get(), final_readback);
          for (auto* resource : {mask, output.Get()}) {
            gpu.Transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          }
          gpu.Submit();
          addon::DrainRetiredResources(true);
          const auto mask_bytes = gpu.ReadTexture(mask_readback);
          const auto final_bytes = gpu.ReadTexture(final_readback);
          const auto sr_bytes = gpu.ReadTexture(sr_readback);
          const auto proxy_bytes = gpu.ReadTexture(proxy_readback);
          const auto neural_bytes = gpu.ReadTexture(neural_readback);
          Check(sr_bytes.size() == pixel_count * 8 && proxy_bytes.size() == pixel_count * 8
                    && neural_bytes.size() == pixel_count * 8 && final_bytes.size() == pixel_count * 8,
                "all native pairing planes use the same packed RGBA16F extent");
          if (attenuate) {
            std::cout << "MOTION paired frame=" << frame_index
                      << " sr_identical=" << (sr_bytes == reference_sr[frame_index])
                      << " proxy_identical=" << (proxy_bytes == reference_proxy[frame_index]) << std::endl;
            Check(sr_bytes == reference_sr[frame_index] && proxy_bytes == reference_proxy[frame_index],
                  "paired conditions consume byte-identical real SR output and production NR proxy");
          } else {
            reference_sr[frame_index] = sr_bytes;
            reference_proxy[frame_index] = proxy_bytes;
            reference_neural[frame_index] = neural_bytes;
          }
          const float t = std::clamp((speeds[frame_index] - 2.f) / 14.f, 0.f, 1.f);
          const int expected_red = static_cast<int>(std::lround(255.f * (attenuate ? 1.f - t * t * (3.f - 2.f * t) : 1.f)));
          for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
            Check(std::abs(static_cast<int>(mask_bytes[pixel * 4]) - expected_red) <= 1
                      && mask_bytes[pixel * 4 + 1] == 255 && mask_bytes[pixel * 4 + 2] == 255
                      && mask_bytes[pixel * 4 + 3] == 255,
                  "native consumer's completed mask matches the motion fade and preserves G/B/A");
          }
          double difference = 0;
          double neural_difference = 0;
          for (size_t component = 0; component < pixel_count * 4; ++component) {
            uint16_t value;
            std::memcpy(&value, final_bytes.data() + component * 2, 2);
            const float actual = DirectX::PackedVector::XMConvertHalfToFloat(value);
            Check(std::isfinite(actual), "dynamic native output is finite");
            for (const auto* plane : {&sr_bytes, &proxy_bytes, &neural_bytes}) {
              std::memcpy(&value, plane->data() + component * 2, 2);
              Check(std::isfinite(DirectX::PackedVector::XMConvertHalfToFloat(value)),
                    "SR, production proxy and raw neural output are finite");
            }
            if (attenuate) {
              uint16_t original;
              std::memcpy(&original, reference[frame_index].data() + component * 2, 2);
              difference += std::abs(actual - DirectX::PackedVector::XMConvertHalfToFloat(original));
              uint16_t neural_value;
              std::memcpy(&neural_value, neural_bytes.data() + component * 2, 2);
              std::memcpy(&original, reference_neural[frame_index].data() + component * 2, 2);
              neural_difference += std::abs(DirectX::PackedVector::XMConvertHalfToFloat(neural_value)
                  - DirectX::PackedVector::XMConvertHalfToFloat(original));
            }
          }
          difference /= pixel_count * 4;
          neural_difference /= pixel_count * 4;
          if (!attenuate) {
            reference[frame_index] = final_bytes;
          }
          if (attenuate && speeds[frame_index] >= 16.f) {
            moving_difference = difference;
            Check(neural_difference > 1e-5, "motion mask changes real neural output before the commit codec");
            for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
              Check(std::memcmp(neural_bytes.data() + pixel * 8, proxy_bytes.data() + pixel * 8, 6) == 0,
                    "full motion attenuation returns the actual NR input RGB at the native output boundary");
            }
          }
          if (attenuate && frame_index < 2) {
            Check(difference < 1e-5, "common zero-motion Reset prefix reproduces the paired native output");
          }
          report["frames"].push_back({{"attenuate", attenuate}, {"frame", frame_index},
              {"speed", speeds[frame_index]}, {"shift", shift}, {"expected_red", expected_red},
              {"observed_red", mask_bytes[0]}, {"output_mae_vs_off", difference},
              {"neural_mae_vs_off", neural_difference},
              {"sr_and_proxy_match_off", (attenuate ? nlohmann::json(true) : nlohmann::json(nullptr))},
              {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()},
              {"host_sr_evaluate_count", native_motion_sr_calls}, {"host_sr_result", static_cast<uint32_t>(result)},
              {"evaluate_count", native_runtime_results.size()}, {"nr_result", static_cast<uint32_t>(native_runtime_results[0])},
              {"reset", native_runtime_resets[0]}});
        }
      }
      Check(moving_difference > 1e-5, "motion attenuation changes real native output on the paired moving frame");
      const auto motion_timings = motion_queries.ReadAll();
      report["timestamp_frequency_hz"] = motion_queries.frequency_hz;
      report["timing_scope"] = "SR+NR wrapper GPU work including SR/proxy/neural readbacks; diagnostic only";
      for (size_t index = 0; index < motion_timings.size(); ++index) {
        report["frames"][index]["wrapper_gpu_ms"] = motion_timings[index].gpu_ms;
        report["frames"][index]["wrapper_gpu_ticks"] = motion_timings[index].ticks;
      }
      Check(!addon::features.at(owner).slots[0].control_mask_bound
                && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
            "motion sequence clears transient bindings and leaves a healthy device");
      std::cout << "PASS motion dynamic pair frames=16 real_sr_evaluates=16 native_evaluates=16 moving_output_mae="
                << moving_difference << " zero_prefix=reproduced stopped_mask=restored\n";
    }
    if (cost) {
      constexpr uint32_t warmup_frames = 16;
      constexpr uint32_t sample_frames = 64;
      constexpr uint32_t total_frames = warmup_frames + sample_frames;
      auto queries = d3d12_test::CreateTimestampQueries(gpu, total_frames * 3);
      struct CostFrame {
        uint32_t host_result = 0;
        uint32_t evaluate_result = 0;
        size_t evaluate_count = 0;
        int reset = -1;
        uint32_t dispatch_count = 0;
        uint64_t fence = 0;
        uint64_t completed_fence = 0;
        uint64_t workset_id = 0;
        uint64_t allocated_bytes = 0;
        bool mask_bound = false;
        bool bindings_cleared = false;
        HRESULT device_status = E_FAIL;
      };
      std::array<CostFrame, total_frames> frames{};
      const auto& initial_workset = addon::worksets.at({owner, output.Get(), false});
      const uint64_t stable_workset = initial_workset.id;
      const uint64_t stable_bytes = initial_workset.allocated_bytes;
      auto* const stable_mask = initial_workset.control_mask;
      auto* const stable_history = addon::features.at(owner).slots[0].handle;
      Check((stable_mask != nullptr) == mask_enabled, "cost prefix establishes the requested initial mask state");
      model_gpu = &gpu;
      observed_pipeline = nullptr;
      real_dispatch = reinterpret_cast<DispatchFn>((*reinterpret_cast<void***>(gpu.list.Get()))[14]);
      real_pipeline = reinterpret_cast<PipelineFn>((*reinterpret_cast<void***>(gpu.list.Get()))[25]);
      Check(DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
                && DetourAttach(reinterpret_cast<PVOID*>(&real_pipeline), reinterpret_cast<PVOID>(ObservePipeline)) == NO_ERROR
                && DetourAttach(reinterpret_cast<PVOID*>(&real_dispatch), reinterpret_cast<PVOID>(ObserveDispatch)) == NO_ERROR
                && DetourTransactionCommit() == NO_ERROR,
            "cost installs existing production PSO/Dispatch observers");
      const auto remove_cost_hooks = [&] {
        native_cost_queries = nullptr;
        native_cost_list = nullptr;
        native_cost_observe = false;
        suppress_control_mask_cost_logs = false;
        model_gpu = nullptr;
        return DetourTransactionBegin() == NO_ERROR && DetourUpdateThread(GetCurrentThread()) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_dispatch), reinterpret_cast<PVOID>(ObserveDispatch)) == NO_ERROR
            && DetourDetach(reinterpret_cast<PVOID*>(&real_pipeline), reinterpret_cast<PVOID>(ObservePipeline)) == NO_ERROR
            && DetourTransactionCommit() == NO_ERROR;
      };
      suppressed_control_mask_cost_logs = 0;
      suppress_control_mask_cost_logs = true;
      try {
        for (uint32_t frame = 0; frame < total_frames; ++frame) {
          native_runtime_masks.clear();
          native_runtime_results.clear();
          native_runtime_resets.clear();
          native_cost_dispatches = 0;
          observed_pipeline = nullptr;
          gpu.list->ClearState(nullptr);
          restore_cost_inputs();
          parameters.Set(NVSDK_NGX_Parameter_Reset, 0);
          native_cost_interval = frame * 3;
          native_cost_list = gpu.list.Get();
          native_cost_queries = &queries;
          queries.Begin(gpu.list.Get(), native_cost_interval);
          addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(SuccessfulHostEvaluate);
          const auto result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
          addon::ngx_slot_real[0].evaluate = nullptr;
          gpu.list->EndQuery(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * native_cost_interval + 1);
          native_cost_queries = nullptr;
          Check(native_runtime_results.size() == 1 && native_runtime_resets.size() == 1
                    && native_cost_dispatches == (mask_enabled ? 1u : 0u),
                "cost frame records exactly one Core Evaluate and the expected production mask Dispatch");
          // All nested EndQuery markers precede any Resolve. Off has no Dispatch
          // query: do not read or resolve unwritten slots or invent an empty cost.
          gpu.Retain(queries.readback.Get());
          gpu.list->ResolveQueryData(queries.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
              2 * native_cost_interval, (mask_enabled ? 6u : 4u), queries.readback.Get(),
              2ull * native_cost_interval * sizeof(uint64_t));
          gpu.Submit();
          addon::DrainRetiredResources(true);
          const auto& workset = addon::worksets.at({owner, output.Get(), false});
          const auto& slot = addon::features.at(owner).slots[0];
          frames[frame] = {.host_result = static_cast<uint32_t>(result),
              .evaluate_result = static_cast<uint32_t>(native_runtime_results[0]),
              .evaluate_count = native_runtime_results.size(), .reset = native_runtime_resets[0],
              .dispatch_count = native_cost_dispatches, .fence = gpu.serial,
              .completed_fence = gpu.fence->GetCompletedValue(), .workset_id = workset.id,
              .allocated_bytes = workset.allocated_bytes, .mask_bound = native_runtime_masks[0] != nullptr,
              .bindings_cleared = !slot.control_mask_bound,
              .device_status = gpu.device->GetDeviceRemovedReason()};
          Check(NVSDK_NGX_SUCCEED(result) && NVSDK_NGX_SUCCEED(native_runtime_results[0])
                    && native_runtime_masks[0] == stable_mask && workset.control_mask == stable_mask
                    && workset.id == stable_workset && workset.allocated_bytes == stable_bytes
                    && slot.handle == stable_history && slot.via_core && !slot.control_mask_bound
                    && frames[frame].reset == 0 && frames[frame].completed_fence >= gpu.serial
                    && frames[frame].completed_fence != UINT64_MAX && SUCCEEDED(frames[frame].device_status),
                "cost frame preserves binding, History, workset and healthy GPU completion");
        }
      } catch (...) {
        if (!remove_cost_hooks()) {
          std::cerr << "FAIL ControlMask cost observer cleanup\n";
        }
        throw;
      }
      Check(remove_cost_hooks(), "cost removes its PSO/Dispatch observers");
      ComPtr<IDXGIAdapter1> adapter;
      const LUID adapter_luid = gpu.device->GetAdapterLuid();
      Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(adapter_luid, IID_PPV_ARGS(&adapter))), "cost adapter identity");
      DXGI_ADAPTER_DESC1 adapter_desc{};
      Check(SUCCEEDED(adapter->GetDesc1(&adapter_desc)), "cost adapter description");
      char adapter_name[512]{};
      Check(WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1,
                adapter_name, sizeof(adapter_name), nullptr, nullptr) > 0, "cost adapter UTF-8 name");
      LARGE_INTEGER driver_version{};
      const HRESULT driver_status = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver_version);
      cost_report = {{"schema", "nr-nexus-controlmask-cost-v1"}, {"evidence", "NATIVE_GPU+NATIVE_NGX"},
          {"host_sr", "MOCK no GPU work"}, {"backend", "borrowed-Core"}, {"stage", "Upscaled"},
          {"preset_request", "B"}, {"internal_model", "unknown"}, {"mode", cost_mode},
          {"environment", {{"NR_NEXUS_TEST_CONTROL_MASK_COST_MODE", cost_mode},
              {"mode_defaulted", cost_mode_environment == nullptr}}},
          {"width", width}, {"height", height}, {"color_output_format", "RGBA16F"},
          {"depth_format", "R32_FLOAT"}, {"motion_format", "RG16F zero"},
          {"mask_enabled", mask_enabled}, {"mask_rgb", {1.f, 1.f, 1.f}},
          {"depth_near", 0.7f}, {"depth_far", 0.9f}, {"auto_mask", false}, {"blend_mode", 0},
          {"intensity", 1.f}, {"codec_mode", 0}, {"passes", 1},
          {"warmup_frames", warmup_frames}, {"sample_frames", sample_frames},
          {"prefix_frames", cost_prefix_frames}, {"prefix", std::move(cost_prefix)},
          {"runtime_file_version", addon::NgxLoaderView().nr_requirements.file_version},
          {"gpu", {{"name", adapter_name}, {"vendor_id", adapter_desc.VendorId},
              {"device_id", adapter_desc.DeviceId}, {"driver_status", driver_status},
              {"driver_version_raw", driver_version.QuadPart}}},
          {"timestamp_frequency_hz", queries.frequency_hz},
          {"cost_meter_enabled", false}, {"gpu_timers_enabled", false},
          {"suppressed_log_calls", suppressed_control_mask_cost_logs},
          {"input_schedule", "fixed upload restores Color and Output before wrapper; fixed Depth and zero Motion"},
          {"reset_schedule", "host Reset=1 only at prefix frame 0, then 0; observed Core resets recorded"},
          {"synchronization", "one list submission per frame, Fence complete before allocator/query reuse"},
          {"timing_scope", {{"wrapper", "entire HookedEvaluateFeatureSlot including mock host SR"},
              {"core", "real Core Feature 18 Evaluate only"},
              {"dispatch", "production ControlMask PSO Dispatch only; excludes bindings and barriers; off is null"}}},
          {"frames", nlohmann::json::array()}};
      const auto timestamp_json = [](const d3d12_test::TimestampSample& sample) {
        return nlohmann::json{{"begin_tick", sample.begin_tick}, {"end_tick", sample.end_tick},
            {"ticks", sample.ticks}, {"gpu_ms", sample.gpu_ms}};
      };
      for (uint32_t frame = 0; frame < total_frames; ++frame) {
        const auto wrapper = queries.Read(frame * 3);
        const auto core = queries.Read(frame * 3 + 1);
        Check(wrapper.begin_tick <= core.begin_tick && core.end_tick <= wrapper.end_tick,
              "cost Core timestamps are nested inside wrapper");
        nlohmann::json dispatch = nullptr;
        if (mask_enabled) {
          const auto sample = queries.Read(frame * 3 + 2);
          Check(wrapper.begin_tick <= sample.begin_tick && sample.end_tick <= core.begin_tick,
                "cost production mask Dispatch precedes the real Core Evaluate");
          dispatch = timestamp_json(sample);
        }
        const auto& sample = frames[frame];
        cost_report["frames"].push_back({{"frame", frame}, {"warmup", frame < warmup_frames},
            {"wrapper", timestamp_json(wrapper)}, {"core", timestamp_json(core)}, {"dispatch", std::move(dispatch)},
            {"host_result", sample.host_result}, {"evaluate_result", sample.evaluate_result},
            {"evaluate_count", sample.evaluate_count}, {"reset", sample.reset},
            {"dispatch_count", sample.dispatch_count}, {"fence", sample.fence},
            {"completed_fence", sample.completed_fence}, {"workset_id", sample.workset_id},
            {"allocated_bytes", sample.allocated_bytes}, {"mask_bound", sample.mask_bound},
            {"bindings_cleared", sample.bindings_cleared}, {"device_status", sample.device_status}});
      }
    }
    if (hot_toggle) {
      ComPtr<ID3D12CommandAllocator> probe_allocator;
      ComPtr<ID3D12GraphicsCommandList> probe_list;
      ComPtr<ID3D12Fence> gate;
      Check(SUCCEEDED(gpu.device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&probe_allocator)))
                && SUCCEEDED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    probe_allocator.Get(), nullptr, IID_PPV_ARGS(&probe_list)))
                && SUCCEEDED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))),
            "native hot-toggle pending fixture allocation");
      native_runtime_results.clear();
      addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(SuccessfulHostEvaluate);
      addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
      addon::ngx_slot_real[0].evaluate = nullptr;
      Check(native_runtime_results.size() == 1 && NVSDK_NGX_SUCCEED(native_runtime_results[0]),
            "record native masked work before delaying its GPU execution");
      const auto* pending_history = addon::features.at(owner).slots[0].handle;
      auto* pending_mask = addon::worksets.at({owner, output.Get(), false}).control_mask;
      Check(SUCCEEDED(gpu.list->Close()), "close native pending model recording");
      const auto finish_pending = [&] {
        Check(SUCCEEDED(gate->Signal(1)), "release native pending GPU gate");
        Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)),
              "native pending completion signal");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "native pending completion event");
        const auto armed = gpu.fence->SetEventOnCompletion(gpu.serial, event);
        const auto wait = (SUCCEEDED(armed) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
        CloseHandle(event);
        Check(wait == WAIT_OBJECT_0, "native pending work completes after releasing its gate");
        gpu.submission_pending = false;
      };
      try {
        Check(SUCCEEDED(gpu.queue->Wait(gate.Get(), 1)), "delay native model GPU execution");
        ID3D12CommandList* lists[]{gpu.list.Get()};
        const auto pending = submission::BeginSubmission(1, lists);
        gpu.queue->ExecuteCommandLists(1, lists);
        gpu.submission_pending = true;
        Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "signal native queued work");
        submission::CompleteSubmission(pending, gpu.fence.Get(), gpu.serial);
        for (const bool enabled : {false, true}) {
          addon::control_mask_enabled = enabled;
          native_runtime_masks.clear();
          native_runtime_results.clear();
          std::cout << "HOT_TOGGLE pending probe enabled=" << enabled << std::endl;
          addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(SuccessfulHostEvaluate);
          addon::HookedEvaluateFeatureSlot<0>(probe_list.Get(), owner, &parameters, nullptr);
          addon::ngx_slot_real[0].evaluate = nullptr;
          Check(native_runtime_results.size() == 1 && NVSDK_NGX_SUCCEED(native_runtime_results[0])
                    && (native_runtime_masks.back() != nullptr) == enabled
                    && gate->GetCompletedValue() == 0
                    && addon::features.at(owner).slots[0].handle == pending_history
                    && !submission::ResourceReleasable(pending_mask)
                    && !submission::ResourceReleasable(pending_history),
                "ordered borrowed Core records one native OFF/ON Evaluate while retaining pending mask and History");
          std::cout << "HOT_TOGGLE pending probe returned enabled=" << enabled << std::endl;
        }
        Check(SUCCEEDED(probe_list->Close()), "close ordered native OFF/ON recording");
        ID3D12CommandList* probes[]{probe_list.Get()};
        const auto probe_submission = submission::BeginSubmission(1, probes);
        gpu.queue->ExecuteCommandLists(1, probes);
        Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "signal ordered native OFF/ON work");
        submission::CompleteSubmission(probe_submission, gpu.fence.Get(), gpu.serial);
      } catch (...) {
        finish_pending();
        throw;
      }
      finish_pending();
      Check(SUCCEEDED(probe_allocator->Reset())
                && SUCCEEDED(probe_list->Reset(probe_allocator.Get(), nullptr))
                && SUCCEEDED(gpu.allocator->Reset())
                && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
            "reset completed native pending recording");
      submission::OnCommandListReset(probe_list.Get());
      submission::OnCommandListReset(gpu.list.Get());
      addon::DrainRetiredResources(true);
      constexpr uint32_t transitions = 16;
      for (uint32_t transition = 0; transition < transitions; ++transition) {
        const bool enabled = (transition % 2) != 0;
        addon::control_mask_enabled = enabled;
        uint64_t stable_workset = 0;
        uint32_t completed_frames = 0;
        std::cout << "HOT_TOGGLE begin transition=" << transition << " enabled=" << enabled << std::endl;
        for (uint32_t attempt = 0; attempt < addon::kContractDebounceFrames + 32; ++attempt) {
          native_runtime_masks.clear();
          native_runtime_results.clear();
          addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(SuccessfulHostEvaluate);
          const auto result = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
          addon::ngx_slot_real[0].evaluate = nullptr;
          Check(NVSDK_NGX_SUCCEED(result), "hot-toggle host SR returns success");
          gpu.Submit();
          addon::DrainRetiredResources(true);
          if (native_runtime_results.empty()) continue;
          Check(native_runtime_results.size() == 1 && NVSDK_NGX_SUCCEED(native_runtime_results[0]),
                "hot-toggle records exactly one accepted native NGX Evaluate");
          const auto& workset = addon::worksets.at({owner, output.Get(), false});
          Check((native_runtime_masks[0] != nullptr) == enabled
                    && (workset.control_mask != nullptr) == enabled,
                "hot-toggle binds only the requested mask state");
          if (completed_frames == 0) {
            stable_workset = workset.id;
          }
          Check(workset.id == stable_workset, "stable hot-toggle frames retain the same workset");
          Check(!addon::features.at(owner).slots[0].control_mask_bound
                    && gpu.fence->GetCompletedValue() >= gpu.serial
                    && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
                "hot-toggle clears CPU bindings after native GPU completion");
          if (++completed_frames == 3) break;
        }
        Check(completed_frames == 3, "hot-toggle reaches three completed frames without reallocation");
        std::cout << "HOT_TOGGLE complete transition=" << transition << " frames=" << completed_frames
                  << " workset=" << stable_workset << std::endl;
      }
      std::cout << "PASS ControlMask hot-toggle evidence=NATIVE_GPU+NATIVE_NGX host_sr=MOCK"
                   " backend=borrowed-Core model_request=B pending_OFF_ON=ordered_native_evaluations"
                   " transitions=16 completed_frames=48 size=960x540 mask_mode_request="
                << mask_mode_name << '\n';
    }
    std::vector<uint8_t> output_bytes;
    ComPtr<ID3D12Resource> output_readback;
    bool keep_raw_output = false;
    if (!cold_components && !pipeline_steps && !cache_two_tiers && !render_cache_test) {
      const D3D12_RESOURCE_DESC final_output_desc = output->GetDesc();
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT output_footprint{};
      UINT64 output_readback_size = 0;
      gpu.device->GetCopyableFootprints(
          &final_output_desc, 0, 1, 0, &output_footprint, nullptr, nullptr,
          &output_readback_size);
      output_readback = gpu.Buffer(output_readback_size, D3D12_HEAP_TYPE_READBACK);
      gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
      const D3D12_TEXTURE_COPY_LOCATION output_source{
          .pResource = output.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      const D3D12_TEXTURE_COPY_LOCATION output_destination{
          .pResource = output_readback.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
          .PlacedFootprint = output_footprint};
      gpu.list->CopyTextureRegion(&output_destination, 0, 0, 0, &output_source, nullptr);
      gpu.Transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      ComPtr<ID3D12Resource> cache_cost_validity_readback;
      if (cache_cost && cache_cost_report.at("condition") == "cache") {
        const auto state = addon::features.at(nr_owner).slots[0].edit_reuse;
        Check(state != nullptr && state->reused && state->validity != nullptr,
              "last cache cost frame owns a produced reuse validity plane");
        const uint64_t bytes = static_cast<uint64_t>(width) * height * sizeof(uint32_t);
        cache_cost_validity_readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
        gpu.Transition(state->validity.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->CopyBufferRegion(cache_cost_validity_readback.Get(), 0, state->validity.Get(), 0, bytes);
        gpu.Transition(state->validity.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
      gpu.Submit();
      uint8_t* output_mapped = nullptr;
      Check(SUCCEEDED(output_readback->Map(
                0, nullptr, reinterpret_cast<void**>(&output_mapped))),
            "production output readback map");
      const size_t output_row_bytes = static_cast<size_t>(final_output_desc.Width) * 8;
      output_bytes.resize(output_row_bytes * final_output_desc.Height);
      for (UINT y = 0; y < final_output_desc.Height; ++y) {
        std::memcpy(output_bytes.data() + static_cast<size_t>(y) * output_row_bytes,
                    output_mapped + output_footprint.Offset
                        + static_cast<size_t>(y) * output_footprint.Footprint.RowPitch,
                    output_row_bytes);
      }
      output_readback->Unmap(0, nullptr);
      keep_raw_output = std::getenv("NR_NEXUS_TEST_KEEP_RAW") != nullptr
          && std::strcmp(std::getenv("NR_NEXUS_TEST_KEEP_RAW"), "1") == 0;
      if (keep_raw_output && !jitter_test && !cache_cost) {
        std::ofstream output_file("controlmask-production-output.rgba16f",
                                  std::ios::binary | std::ios::trunc);
        Check(output_file.is_open(), "production output file opens");
        output_file.write(reinterpret_cast<const char*>(output_bytes.data()),
                          static_cast<std::streamsize>(output_bytes.size()));
        Check(output_file.good(), "production output bytes are saved");
      }
      if (cache_cost) {
        uint64_t nonfinite = 0;
        float maximum_rgb = 0.f;
        for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
          std::array<uint16_t, 4> rgba;
          std::memcpy(rgba.data(), output_bytes.data() + pixel * sizeof(rgba), sizeof(rgba));
          for (size_t channel = 0; channel < rgba.size(); ++channel) {
            const float value = DirectX::PackedVector::XMConvertHalfToFloat(rgba[channel]);
            nonfinite += !std::isfinite(value);
            if (channel < 3) {
              maximum_rgb = (std::max)(maximum_rgb, std::abs(value));
            }
          }
        }
        cache_cost_report["output_readback"] = {{"count", 1}, {"after_measurement", true},
            {"bytes", output_bytes.size()}, {"raw_retained", false}, {"nonfinite_channels", nonfinite},
            {"maximum_absolute_rgb", maximum_rgb}, {"fence", gpu.serial}, {"completed_fence", gpu.fence->GetCompletedValue()}};
        Check(nonfinite == 0 && maximum_rgb > 0.f && gpu.fence->GetCompletedValue() >= gpu.serial
                  && gpu.fence->GetCompletedValue() != UINT64_MAX && SUCCEEDED(gpu.device->GetDeviceRemovedReason()),
              "post-measurement cache cost output is finite, nonzero and GPU-completed");
        cache_cost_report["validity_readback"] = nullptr;
        if (cache_cost_validity_readback != nullptr) {
          const D3D12_RANGE range{0, static_cast<SIZE_T>(pixel_count * sizeof(uint32_t))};
          uint32_t* mapped = nullptr;
          Check(SUCCEEDED(cache_cost_validity_readback->Map(0, &range, reinterpret_cast<void**>(&mapped))),
                "final cost reuse validity readback map");
          uint64_t invalid = 0, valid = 0, other = 0;
          for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
            if (mapped[pixel] == 0) {
              ++invalid;
            } else if (mapped[pixel] == 1) {
              ++valid;
            } else {
              ++other;
            }
          }
          const D3D12_RANGE no_writes{0, 0};
          cache_cost_validity_readback->Unmap(0, &no_writes);
          cache_cost_report["validity_readback"] = {{"count", 1}, {"after_measurement", true}, {"raw_retained", false},
              {"frame", cache_cost_warmup_frames + cache_cost_sample_frames - 1},
              {"zero_pixels", invalid}, {"one_pixels", valid}, {"other_pixels", other}};
          Check(other == 0 && valid > 0, "final cost reuse has valid cached pixels and only 0/1 validity values");
        }
        std::cout << "PASS NR edit reuse cost condition=" << cache_cost_report.at("condition").get<std::string>()
                  << " warmup=" << cache_cost_warmup_frames << " samples=" << cache_cost_sample_frames
                  << " size=" << width << 'x' << height << " real_SR=per_frame route=xefg-record; XeFG SDK/Service=not_run\n";
      } else if (jitter_test) {
        std::cout << "PASS SR jitter calibration samples=9 real_sr_evaluates=9 nr_evaluates=0 size="
                  << width << 'x' << height << " raw=jitter-calibration; raster sign pending CPU analysis\n";
      } else {
        std::cout << (demand_validation_failed ? "FAIL" : "PASS") << " ControlMask production chain stages=" << (single_stage ? "Upscaled" : "Render+Upscaled")
                  << " evidence=NATIVE_GPU+NATIVE_NGX host_sr=" << (motion_test ? "NATIVE_NGX" : "MOCK") << " frames="
                  << (sequence_test ? 24 : (resize_test ? resize_report["frames"].size() : (cost ? 80 : (motion_test ? 16 : (hot_toggle ? 52 : 1)))))
                  << (demand_prepare ? " frame_scope=Session-demand-prepare" : single_feature ? " frame_scope=single-feature-effective-size" : cold_prepare ? " frame_scope=serialized-worker-cold-prepare" : resize_test ? (resize_report.value("settings_reuse_sequence", false)
                          ? " frame_scope=high-low-high-and-settings" : " frame_scope=high-low-high")
                      : (motion_test ? " frame_scope=paired_only" : ""))
                  << " size=" << width << 'x' << height << " mask="
                  << (mask_enabled ? mask_mode_name : "off")
                  << " output_readback=rgba16f raw="
                  << (keep_raw_output ? "retained" : "discarded") << "\n";
      }
    }

    if (motion_test) {
      if (cold_components || pipeline_steps) {
        cold_teardown_started = true;
      }
      addon::enabled = false;
      native_runtime_results.clear();
      native_motion_sr_calls = 0;
      if (!jitter_test) {
        std::optional<d3d12_test::TextureTransfer> render_control_readback;
        if (render_cache_test) {
          // Match the measured Reset frame with the original Color bound to real SR.
          parameters.Set(NVSDK_NGX_Parameter_Reset, 1);
          render_control_readback.emplace(gpu.CreateTextureTransfer(output.Get()));
          native_motion_sr_readback = &*render_control_readback;
        }
        addon::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(ObserveNativeHostEvaluate);
        const auto resumed = addon::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), owner, &parameters, nullptr);
        addon::ngx_slot_real[0].evaluate = nullptr;
        native_motion_sr_readback = nullptr;
        gpu.Submit();
        if (diagnostic_probe || demand_prepare || selected_sequence) {
          resize_report["teardown_apis"]["disabled_host_result"] = static_cast<uint32_t>(resumed);
          resize_report["teardown_apis"]["disabled_actual_sr_calls"] = native_motion_sr_calls;
          resize_report["teardown_apis"]["disabled_actual_nr_calls"] = native_runtime_results.size();
        }
        Check(NVSDK_NGX_SUCCEED(resumed) && native_motion_sr_calls == 1 && native_runtime_results.empty(),
              "real SR continues once with no NR Evaluate after the add-on is disabled");
        if (render_control_readback) {
          const auto completed = gpu.fence->GetCompletedValue();
          const auto device_status = gpu.device->GetDeviceRemovedReason();
          Check(completed >= gpu.serial && completed != UINT64_MAX && SUCCEEDED(device_status),
                "Render x2 original-Color control completes on a healthy GPU");
          const auto original_sr_output = gpu.ReadTexture(*render_control_readback);
          const bool changed = original_sr_output != render_reset_output;
          render_report["original_color_sr_control"] = {{"reset", 1}, {"sr_calls", native_motion_sr_calls},
              {"sr_result", static_cast<uint32_t>(resumed)}, {"fence", gpu.serial}, {"completed_fence", completed},
              {"device_status", device_status}, {"output_differs_from_render_reset", changed},
              {"sr_output_crc32", renodx::utils::hash::ComputeCRC32(original_sr_output.data(), original_sr_output.size())}};
          Check(render_report["reset_stand_in_differs_from_original"] == true && changed,
                "real SR produces different Reset output with the edited stand-in than with the original Color");
        }
      }
      addon::ngx_slot_real[0].release = reinterpret_cast<void*>(NVSDK_NGX_D3D12_ReleaseFeature);
      const auto released = addon::HookedReleaseFeatureSlot<0>(sr_handle);
      addon::ngx_slot_real[0].release = nullptr;
      if (diagnostic_probe || demand_prepare || selected_sequence) {
        resize_report["teardown_apis"]["source_release_result"] = static_cast<uint32_t>(released);
      }
      Check(NVSDK_NGX_SUCCEED(released) && !addon::features.contains(owner),
            "production Release wrapper releases the real SR owner and retires its NR slots");
      Check(addon::get_module_filename_iat == nullptr && addon::original_get_module_filename == nullptr,
            "public SDK chain never installs a snippet caller identity bridge");
    }
    {
      std::scoped_lock runtime_lock(addon::runtime_mutex);
      if (xefg_cache_test) {
        addon::fg_service::Release(&cache_stream);
      }
      addon::ReleaseFeatureStates();
      addon::RetireAllWorksets();
      addon::FlushRetiredResources();
      if (addon::direct_device != nullptr) {
        renodx::addons::dlss5::DirectCallScope direct_call;
        if (addon::direct_api.shutdown != nullptr) {
          const auto shutdown = addon::direct_api.shutdown(addon::direct_device);
          if (diagnostic_probe || demand_prepare || selected_sequence) {
            resize_report["teardown_apis"]["production_shutdown_result"] = static_cast<uint32_t>(shutdown);
          }
          Check(NVSDK_NGX_SUCCEED(shutdown), "production Runtime shutdown");
        }
        addon::direct_device->Release();
        addon::direct_device = nullptr;
      }
    }
    if (motion_test) {
      const auto destroyed = NVSDK_NGX_D3D12_DestroyParameters(sr_parameters);
      if (diagnostic_probe || demand_prepare || selected_sequence) {
        resize_report["teardown_apis"]["sr_destroy_parameters_result"] = static_cast<uint32_t>(destroyed);
      }
      Check(NVSDK_NGX_SUCCEED(destroyed), "release real SR parameters");
      const auto shutdown = NVSDK_NGX_D3D12_Shutdown1(gpu.device.Get());
      if (diagnostic_probe || demand_prepare || selected_sequence) {
        resize_report["teardown_apis"]["public_shutdown_result"] = static_cast<uint32_t>(shutdown);
      }
      Check(NVSDK_NGX_SUCCEED(shutdown), "public SDK device shutdown");
      native_motion_gpu = nullptr;
      if (jitter_test) {
        jitter_report["teardown"] = {{"sr_release", true}, {"sdk_shutdown", true},
            {"identity_bridge_installed", false}, {"nr_evaluates", 0}};
        jitter_report["total_host_sr_evaluates"] = 9;
        jitter_report["total_nr_evaluates"] = 0;
        std::ofstream file(std::filesystem::path("jitter-calibration") / "sr-jitter-calibration.json",
                           std::ios::binary | std::ios::trunc);
        file << jitter_report.dump(2) << '\n';
        file.close();
        Check(!file.fail(), "jitter calibration completed teardown evidence saved");
      } else if (cache_cost) {
        Check(addon::retired_nr_features.empty() && addon::retired_final_resources.empty(),
              "cache cost teardown drains completed model and workset retirement");
        cache_cost_report["teardown"] = {{"sr_continues_without_nr", true}, {"sr_release", true}, {"sdk_shutdown", true},
            {"identity_bridge_installed", false}, {"retirement_drained", true},
            {"disabled_host_sr_evaluates", native_motion_sr_calls}, {"fence", gpu.serial},
            {"completed_fence", gpu.fence->GetCompletedValue()}, {"device_status", gpu.device->GetDeviceRemovedReason()}};
        cache_cost_report["total_host_sr_evaluates"] = prefix_host_evaluates
            + (cache_two_tiers ? cache_cost_report.at("sequence_host_sr_evaluates").get<uint32_t>()
                : cache_cost_warmup_frames + cache_cost_sample_frames) + native_motion_sr_calls;
        cache_cost_report["total_nr_evaluates"] = prefix_nr_evaluates
            + cache_cost_report.at("sequence_nr_evaluates").get<uint32_t>();
        cache_cost_report["status"] = "passed";
        save_cache_cost_report();
      } else if (render_cache_test) {
        render_report["status"] = "passed";
        render_report["teardown"] = {{"sr_release", true}, {"sdk_shutdown", true}, {"identity_bridge_installed", false}};
        std::ofstream file("nr-edit-reuse-render.json", std::ios::binary | std::ios::trunc);
        file << render_report.dump(2) << '\n';
        file.close();
        Check(!file.fail(), "completed Render x2 evidence saved");
        std::cout << "PASS Render x2 NATIVE_GPU+NATIVE_NGX: 5 SR frames, 3 NR refreshes, 2 reuses, 1 original-Color SR control\n";
      } else if (sequence_test) {
        history_report["teardown"] = {{"sr_continues_without_nr", true}, {"sr_release", true},
            {"sdk_shutdown", true}, {"identity_bridge_installed", false},
            {"disabled_host_sr_evaluates", native_motion_sr_calls}};
        history_report["total_host_sr_evaluates"] = prefix_host_evaluates + 24 + native_motion_sr_calls;
        history_report["total_nr_evaluates"] = prefix_nr_evaluates
            + history_report["sequence_nr_evaluates"].get<uint32_t>();
        const auto condition = history_report["condition"].get<std::string>();
        std::ofstream file(std::filesystem::path(cache_test && condition == "baseline" ? "cache-baseline" : condition)
            / (cache_test ? "nr-edit-reuse-cache.json" : "nr-edit-reuse-history.json"), std::ios::binary | std::ios::trunc);
        file << history_report.dump(2) << '\n';
        file.close();
        Check(!file.fail(), "history completed teardown evidence saved");
      } else if (resize_test) {
        Check(std::none_of(addon::worksets.begin(), addon::worksets.end(),
                  [owner](const auto& entry) { return entry.first.handle == owner; })
                  && addon::retired_nr_features.empty() && addon::retired_final_resources.empty(),
              "ready-cache source release and completed teardown retain no owner worksets or retired NR resources");
        resize_report["teardown"] = {{"sr_continues_without_nr", true}, {"source_release", true},
            {"sdk_shutdown", true}, {"identity_bridge_installed", false}, {"retirement_drained", true}};
        if (diagnostic_probe || demand_prepare || selected_sequence) {
          resize_report["teardown_status"] = "Passed";
        }
        std::ofstream file(single_feature ? "adaptive-native-single-feature.json" : cold_prepare ? "adaptive-native-cold-prepare.json" : "adaptive-native-resize.json",
                           std::ios::binary | std::ios::trunc);
        file << resize_report.dump(2) << '\n';
        file.close();
        Check(!file.fail(), "completed ready-cache teardown evidence JSON saved");
        std::cout << "PASS resize real SR disable/resume, source Release and public SDK shutdown; identity_bridge=unused\n";
      } else {
        motion_report["teardown"] = {{"sr_continues_without_nr", true}, {"sr_release", true},
          {"sdk_shutdown", true}, {"identity_bridge_installed", false}, {"disabled_host_sr_evaluates", native_motion_sr_calls}};
        motion_report["total_host_sr_evaluates"] = prefix_host_evaluates + 16 + native_motion_sr_calls;
        motion_report["total_nr_evaluates"] = prefix_nr_evaluates + 16;
        std::ofstream motion_file("controlmask-motion.json", std::ios::binary | std::ios::trunc);
        motion_file << motion_report.dump(2) << '\n';
        motion_file.close();
        Check(!motion_file.fail(), "completed motion chain evidence JSON is saved");
        std::cout << "PASS real SR disable/resume, source Release and public SDK shutdown; identity_bridge=unused\n";
      }
    }
    if (cost) {
      cost_report["output_readback"] = {{"count", 1}, {"after_measurement", true},
          {"bytes", output_bytes.size()}, {"raw_retained", keep_raw_output}};
      cost_report["production_teardown_completed"] = true;
      std::ofstream cost_file("controlmask-cost.json", std::ios::binary | std::ios::trunc);
      Check(cost_file.is_open(), "cost JSON opens");
      cost_file << cost_report.dump(2) << '\n';
      cost_file.close();
      Check(!cost_file.fail(), "cost JSON is saved");
      std::cout << "PASS ControlMask cost mode=" << cost_mode
                << " warmup=16 samples=64 output=controlmask-cost.json\n";
    }
    if (demand_validation_failed) {
      std::cerr << "FAIL demand behavior validation; healthy Off and SDK teardown completed\n";
      return 1;
    }
    if (cold_components_failed) {
      std::cerr << "FAIL cold CPU components; bounded prefix stopped and healthy Off/SDK teardown completed\n";
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    save_cache_cost_failure(error.what());
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
    addon::cold_cpu_trace_active = nullptr;
    export_cold_cpu_trace(true);
#endif
    save_limited_worker_failure();
    addon::adaptive_feature_mode = addon::AdaptiveFeatureMode::OFF;
    native_resize_demand_prepare = false;
    native_resize_selected_sequence = false;
    if ((selected_sequence || cache_two_tiers) && native_resize_create != nullptr) {
      addon::core_create_feature = native_resize_create;
      addon::core_release_feature = native_resize_release;
    }
    native_resize_create_gpu = false;
    if (demand_prepare || selected_sequence) {
      resize_report["failed_create_calls"] = native_resize_create_calls;
    }
    if (demand_prepare || automatic_limited) {
      std::unique_lock lock(renodx::addons::dlss5::output_framerate::internal::state.mutex);
      renodx::addons::dlss5::output_framerate::internal::state.stream = {};
    }
    native_resize_create_calls.clear();
    native_resize_evaluate_cpu_ms.clear();
    native_resize_fixed_input = false;
    native_resize_observe_parameters = false;
    native_resize_frame = UINT32_MAX;
    native_resize_parameter_snapshots.clear();
    native_resize_fixed_inputs.clear();
    if ((diagnostic_probe || demand_prepare || selected_sequence) && !resize_report.is_null()) {
      resize_report["failure"] = error.what();
      if ((demand_prepare || selected_sequence) && resize_report.value("probe_execution", "") == "Running") {
        resize_report["probe_execution"] = "Failed";
      }
      if (((cold_components || pipeline_steps) && cold_teardown_started)
          || resize_report.value("contract_execution", "") == "Passed" || resize_report.value("probe_execution", "") == "Passed") {
        resize_report["teardown_status"] = "Failed";
      }
      std::ofstream file(single_feature ? "adaptive-native-single-feature.json" : (cold_prepare ? "adaptive-native-cold-prepare.json" : "adaptive-native-resize.json"),
                         std::ios::binary | std::ios::trunc);
      file << resize_report.dump(2) << '\n';
    }
    std::cerr << "FAIL ControlMask production NATIVE_NGX: " << error.what() << '\n';
    return 1;
  } catch (...) {
    save_cache_cost_failure("non-standard exception");
#if defined(NR_NEXUS_TEST_COLD_CPU_TRACE)
    addon::cold_cpu_trace_active = nullptr;
    export_cold_cpu_trace(true);
#endif
    save_limited_worker_failure();
    addon::adaptive_feature_mode = addon::AdaptiveFeatureMode::OFF;
    native_resize_demand_prepare = false;
    native_resize_selected_sequence = false;
    if ((selected_sequence || cache_two_tiers) && native_resize_create != nullptr) {
      addon::core_create_feature = native_resize_create;
      addon::core_release_feature = native_resize_release;
    }
    native_resize_create_gpu = false;
    if (demand_prepare || selected_sequence) {
      resize_report["failed_create_calls"] = native_resize_create_calls;
    }
    if (demand_prepare || automatic_limited) {
      std::unique_lock lock(renodx::addons::dlss5::output_framerate::internal::state.mutex);
      renodx::addons::dlss5::output_framerate::internal::state.stream = {};
    }
    native_resize_create_calls.clear();
    native_resize_evaluate_cpu_ms.clear();
    native_resize_fixed_input = false;
    native_resize_observe_parameters = false;
    native_resize_frame = UINT32_MAX;
    native_resize_parameter_snapshots.clear();
    native_resize_fixed_inputs.clear();
    if (!diagnostic_probe && !demand_prepare && !selected_sequence && !cache_cost) throw;
    if (!resize_report.is_null()) {
      resize_report["failure"] = "non-standard exception";
      if ((demand_prepare || selected_sequence) && resize_report.value("probe_execution", "") == "Running") {
        resize_report["probe_execution"] = "Failed";
      }
      if (((cold_components || pipeline_steps) && cold_teardown_started)
          || resize_report.value("contract_execution", "") == "Passed" || resize_report.value("probe_execution", "") == "Passed") {
        resize_report["teardown_status"] = "Failed";
      }
      std::ofstream file(single_feature ? "adaptive-native-single-feature.json" : (cold_prepare ? "adaptive-native-cold-prepare.json" : "adaptive-native-resize.json"),
                         std::ios::binary | std::ios::trunc);
      file << resize_report.dump(2) << '\n';
    }
    std::cerr << "FAIL ControlMask production NATIVE_NGX: non-standard exception\n";
    return 1;
  }
}

#endif  // NR_NEXUS_TEST_NATIVE_NGX

int Run() { return RunSafety(false, false); }
}  // namespace control_mask_gpu_cases
