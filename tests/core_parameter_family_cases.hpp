/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include <nlohmann/json.hpp>

namespace core_parameter_family_cases {
namespace nr = renodx::addons::dlss5::internal;
namespace bridge = renodx::addons::dlss5::bridge;
namespace submission = renodx::addons::dlss5::submission;

struct Calls {
  std::array<uint32_t, 2> allocated{};
  std::array<uint32_t, 2> destroyed{};
  std::array<uint32_t, 2> created{};
  std::array<uint32_t, 2> evaluated{};
  std::array<uint32_t, 2> released{};
  uint32_t init = 0;
  uint32_t shutdown = 0;
  uint32_t wrong_family = 0;
  uint32_t next_model = 0;
  NVSDK_NGX_Result core_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  ID3D12QueryHeap* query = nullptr;
  std::map<const NVSDK_NGX_Parameter*, uint32_t> parameters;
  std::map<const NVSDK_NGX_Handle*, uint32_t> models;
};
static Calls* active = nullptr;

// Two deliberately distinct allocator/API families share only the public
// parameter interface. Successful calls still record real WARP GPU work.
template <uint32_t Family>
NVSDK_NGX_Result NVSDK_CONV Allocate(NVSDK_NGX_Parameter** output) {
  const auto result = bridge::AllocateOwnedParameters(output);
  if (NVSDK_NGX_SUCCEED(result) && *output != nullptr) {
    (*output)->Set("NR.Fixture.ParameterFamily", Family);
    active->parameters.emplace(*output, Family);
    ++active->allocated[Family];
  }
  return result;
}

template <uint32_t Family>
NVSDK_NGX_Result NVSDK_CONV Destroy(NVSDK_NGX_Parameter* parameters) {
  Check(active->parameters.contains(parameters) && active->parameters.at(parameters) == Family,
        "parameter retirement calls the actual allocator's paired destructor");
  active->parameters.erase(parameters);
  ++active->destroyed[Family];
  return bridge::DestroyOwnedParameters(parameters);
}

template <uint32_t Family>
NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature,
                                   const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output) {
  ++active->created[Family];
  unsigned int marker = 99;
  const bool matched = NVSDK_NGX_SUCCEED(parameters->Get("NR.Fixture.ParameterFamily", &marker)) && marker == Family
                       && active->parameters.contains(parameters) && active->parameters.at(parameters) == Family;
  if (!matched) {
    ++active->wrong_family;
  }
  Check(matched, "each Create sees only its own newly allocated parameter family");
  list->EndQuery(active->query, D3D12_QUERY_TYPE_TIMESTAMP, 0);
  *output = nullptr;
  if constexpr (Family == 0) {
    return active->core_result;
  }
  *output = reinterpret_cast<NVSDK_NGX_Handle*>(new uint32_t{++active->next_model});
  active->models.emplace(*output, Family);
  return NVSDK_NGX_Result_Success;
}

template <uint32_t Family>
NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                     const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  Check(active->models.contains(handle) && active->models.at(handle) == Family
            && active->parameters.contains(parameters) && active->parameters.at(parameters) == Family,
        "successful handle and parameters use their captured Evaluate family");
  ++active->evaluated[Family];
  list->EndQuery(active->query, D3D12_QUERY_TYPE_TIMESTAMP, 0);
  return NVSDK_NGX_Result_Success;
}

template <uint32_t Family>
NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle* handle) {
  Check(active->models.contains(handle) && active->models.at(handle) == Family,
        "successful handle uses its captured Release family");
  active->models.erase(handle);
  ++active->released[Family];
  delete reinterpret_cast<uint32_t*>(handle);
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV WrongDestroy(NVSDK_NGX_Parameter*) {
  ++active->wrong_family;
  return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result NVSDK_CONV WrongRelease(NVSDK_NGX_Handle*) {
  ++active->wrong_family;
  return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result NVSDK_CONV WrongEvaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                          const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
  ++active->wrong_family;
  return NVSDK_NGX_Result_Fail;
}
NVSDK_NGX_Result NVSDK_CONV Init(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                 const NVSDK_NGX_Parameter*) {
  ++active->init;
  return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV Shutdown(ID3D12Device*) {
  ++active->shutdown;
  return NVSDK_NGX_Result_Success;
}

inline int Run() {
  try {
    Check(std::filesystem::current_path().native().starts_with(L"D:"), "Core family fixture output stays on D");
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    Gpu gpu(false);
    Calls calls;
    active = &calls;
    ComPtr<ID3D12QueryHeap> query;
    const D3D12_QUERY_HEAP_DESC query_desc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 1};
    Check(SUCCEEDED(gpu.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&query))), "real WARP SDK-work timestamp");
    calls.query = query.Get();
    const auto readback = gpu.Buffer(sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK);
    gpu.Retain(query.Get());
    gpu.Retain(readback.Get());
    Check(nr::direct_device == nullptr && nr::retired_nr_features.empty() && nr::features.empty(),
          "Core family test starts with its own isolated mock runtime state");
    nr::NgxLoaderSymbols symbols;
    symbols.nr_ready = symbols.core_ready = true;
    symbols.nr_module = symbols.core = GetModuleHandleW(nullptr);
    symbols.init = Init;
    symbols.shutdown = Shutdown;
    symbols.core_allocate = Allocate<0>;
    symbols.core_destroy = Destroy<0>;
    symbols.core_create = Create<0>;
    symbols.core_evaluate = Evaluate<0>;
    symbols.core_release = Release<0>;
    symbols.nr_requirements.file_version = "310.9.16.0";
    std::vector<std::function<void()>> restore;
    const auto save = [&](auto* value) {
      if constexpr (requires { value->load(); }) {
        const auto previous = value->load();
        restore.emplace_back([value, previous] { value->store(previous); });
      } else {
        const auto previous = *value;
        restore.emplace_back([value, previous] { *value = previous; });
      }
    };
    for (auto* value : {&nr::direct_device, &nr::direct_device_native, &nr::last_nr_device_native}) save(value);
    save(&nr::direct_api);
    save(&nr::direct_load_state);
    save(&nr::direct_runtime_failed);
    save(&nr::parameter_runtime_ready);
    save(&nr::parameter_runtime_via_core);
    save(&nr::core_allocate_parameters);
    save(&nr::core_destroy_parameters);
    save(&nr::core_create_feature);
    save(&nr::core_evaluate_feature);
    save(&nr::core_release_feature);
    save(&nr::control_mask_use_core_context);
    save(&nr::control_mask_enabled);
    save(&nr::hook_point);
    save(&nr::stack_passes);
    save(&nr::ngx_loader_published);
    const auto restore_values = [&](int*) {
      for (auto item = restore.rbegin(); item != restore.rend(); ++item) (*item)();
      active = nullptr;
    };
    int restore_key = 0;
    const std::unique_ptr<int, decltype(restore_values)> restore_scope(&restore_key, restore_values);
    nr::ngx_loader_published.store(&symbols);
    nr::direct_api = {.module = symbols.nr_module, .allocate = Allocate<1>, .destroy = Destroy<1>, .init = Init, .create = Create<1>, .evaluate = Evaluate<1>, .release = Release<1>, .shutdown = Shutdown};
    nr::direct_load_state = nr::DirectLoadState::Ready;
    nr::direct_runtime_failed = false;
    nr::direct_device = gpu.device.Get();
    nr::direct_device->AddRef();
    restore.emplace_back([] { nr::ReleaseCom(nr::direct_device); });
    nr::direct_device_native = nr::last_nr_device_native = gpu.device.Get();
    nr::parameter_runtime_ready = true;
    nr::parameter_runtime_via_core = true;
    nr::core_allocate_parameters = Allocate<0>;
    nr::core_destroy_parameters = Destroy<0>;
    nr::core_create_feature = Create<0>;
    nr::core_evaluate_feature = Evaluate<0>;
    nr::core_release_feature = Release<0>;
    nr::control_mask_use_core_context = false;
    nr::control_mask_enabled = false;
    Check(nr::InstallQueueCompletionHooks(gpu.queue.Get()) && nr::InstallCommandListStateHooks(gpu.list.Get()),
          "production recording and queue hooks observe the real WARP list");
    const auto with_runtime = [&](const auto& operation) {
      const renodx::addons::dlss5::ngx_serial::PresentTurn turn;
      Check(turn.owns(), "Core-family mock SDK turn");
      const renodx::addons::dlss5::RuntimeLock lock(nr::runtime_mutex);
      return operation();
    };
    const auto drain = [&] { with_runtime([&] { nr::DrainRetiredResources(true); }); };
    const auto complete = [&](bool reset) {
      gpu.list->ResolveQueryData(query.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, readback.Get(), 0);
      Check(SUCCEEDED(gpu.list->Close()), "real failed-Create recording Close");
      ID3D12CommandList* command = gpu.list.Get();
      gpu.queue->ExecuteCommandLists(1, &command);
      gpu.submission_pending = true;
      Check(SUCCEEDED(gpu.queue->Signal(gpu.fence.Get(), ++gpu.serial)), "real failed-Create recording Fence Signal");
      const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      Check(event != nullptr && SUCCEEDED(gpu.fence->SetEventOnCompletion(gpu.serial, event)), "real WARP completion event");
      const auto waited = WaitForSingleObject(event, 10000);
      CloseHandle(event);
      Check(waited == WAIT_OBJECT_0 && gpu.fence->GetCompletedValue() >= gpu.serial
                && gpu.fence->GetCompletedValue() != UINT64_MAX,
            "actual opaque WARP Create work completes");
      gpu.submission_pending = false;
      uint64_t* timestamp = nullptr;
      Check(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&timestamp))), "read failed-Create GPU timestamp");
      const uint64_t value = *timestamp;
      readback->Unmap(0, nullptr);
      Check(value != 0, "failed Core Create really recorded GPU work before returning failure");
      if (reset) {
        Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
              "real owned recording Reset removes replay eligibility");
      }
      nr::PruneCompletedSubmissionUses();
      return value;
    };
    nr::FinalResources resources;
    nr::FeatureState state;
    const auto ensure = [&](nr::FeatureState* feature) {
      return with_runtime([&] { return nr::EnsureNrFeature(gpu.list.Get(), *feature, 0, resources, 8, 8, 8, 8); });
    };
    Check(!ensure(&state) && state.slots[0].direct_fallback && state.slots[0].fallback_recording != nullptr,
          "a true Core Create failure keeps the next signed-family attempt pending");
    const auto failed_recording = state.slots[0].fallback_recording;
    const auto* failed_parameters = state.slots[0].fallback_parameters;
    drain();
    Check(!ensure(&state) && calls.allocated == std::array<uint32_t, 2>{1, 0}
              && calls.destroyed == std::array<uint32_t, 2>{0, 0}
              && calls.created == std::array<uint32_t, 2>{1, 0}
              && active->parameters.contains(static_cast<const NVSDK_NGX_Parameter*>(failed_parameters)),
          "same open failed recording neither destroys Core parameters nor calls signed Create");
    const auto failed_timestamp = complete(false);
    drain();
    Check(!ensure(&state) && calls.destroyed[0] == 0 && calls.created[1] == 0
              && failed_recording->commands_closed.load() && !failed_recording->closed.load(),
          "GPU completion alone cannot reuse a still replayable failed Core recording");
    Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
          "real owned failed recording is Reset before switching API families");
    nr::PruneCompletedSubmissionUses();
    Check(failed_recording->closed.load(), "production tracker observes that failed recording can no longer replay");
    nr::core_destroy_parameters = WrongDestroy;
    drain();
    Check(calls.destroyed[0] == 1 && calls.wrong_family == 0, "retired failed Core parameters retain their allocator even if global API changes");
    nr::core_destroy_parameters = Destroy<0>;
    Check(ensure(&state) && state.slots[0].direct_fallback && !state.slots[0].via_core
              && state.slots[0].fallback_recording == nullptr
              && state.slots[0].destroy_parameters == Destroy<1>
              && state.slots[0].evaluate_feature == Evaluate<1> && state.slots[0].release_feature == Release<1>
              && calls.created == std::array<uint32_t, 2>{1, 1}
              && calls.allocated == std::array<uint32_t, 2>{1, 1},
          "the same pending slot recovers on the legal new recording with signed-family parameters");
    complete(true);
    nr::direct_api.evaluate = WrongEvaluate;
    nr::direct_api.release = WrongRelease;
    nr::direct_api.destroy = nr::core_destroy_parameters = WrongDestroy;
    Check(with_runtime([&] { return NVSDK_NGX_SUCCEED(state.slots[0].evaluate_feature(gpu.list.Get(),
                                                                                      state.slots[0].handle, state.slots[0].parameters, nullptr)); }),
          "the captured successful Evaluate family remains paired after global function changes");
    const auto evaluate_timestamp = complete(true);
    with_runtime([&] { nr::ReleaseNrSlot(state, 0, true); });
    drain();
    Check(calls.destroyed == std::array<uint32_t, 2>{1, 1}
              && calls.released == std::array<uint32_t, 2>{0, 1}
              && calls.evaluated == std::array<uint32_t, 2>{0, 1} && calls.wrong_family == 0,
          "actual retirement uses captured signed Release and destructor, not changed global API entries");
    nr::direct_api.evaluate = Evaluate<1>;
    nr::direct_api.release = Release<1>;
    nr::direct_api.destroy = Destroy<1>;
    nr::core_destroy_parameters = Destroy<0>;
    const auto direct_before = calls.created[1];
    const auto core_before_oom = calls.created[0];
    calls.core_result = NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
    nr::FeatureState oom;
    Check(!ensure(&oom) && oom.slots[0].failed && oom.slots[0].memory_failure && !oom.slots[0].direct_fallback
              && oom.slots[0].fallback_recording == nullptr && calls.created[1] == direct_before
              && calls.created[0] == core_before_oom + 1,
          "Core GPU-memory failure retains the existing no-fallback behavior");
    const bool oom_memory_flag = oom.slots[0].memory_failure;
    complete(true);
    drain();
    calls.core_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    nr::control_mask_use_core_context = true;
    nr::hook_point = nr::kHookUpscaled;
    nr::stack_passes = 1;
    nr::direct_api.borrowed_core_context = true;
    nr::FeatureState borrowed;
    const auto core_before_borrow = calls.created[0];
    {
      const nr::SuccessfulSrCoreScope successful_sr(gpu.list.Get());
      Check(!ensure(&borrowed) && borrowed.slots[0].failed && !borrowed.slots[0].direct_fallback
                && borrowed.slots[0].fallback_recording == nullptr && calls.created[1] == direct_before
                && calls.created[0] == core_before_borrow + 1,
            "borrowed successful SR Core failure never escapes to the signed-family backend");
    }
    complete(true);
    drain();
    Check(calls.parameters.empty() && calls.models.empty() && nr::retired_nr_features.empty()
              && calls.init == 0 && calls.shutdown == 0 && calls.wrong_family == 0,
          "all mock objects retired without Init/Shutdown, family mismatch or retained leftovers");
    const nlohmann::json receipt{{"evidence", "MOCK+WARP"},
                                 {"runtime_state", "test-injected mock state, not original NR context evidence"},
                                 {"core_allocations", calls.allocated[0]},
                                 {"core_destroys", calls.destroyed[0]},
                                 {"signed_allocations", calls.allocated[1]},
                                 {"signed_destroys", calls.destroyed[1]},
                                 {"core_creates", calls.created[0]},
                                 {"signed_creates", calls.created[1]},
                                 {"signed_evaluates", calls.evaluated[1]},
                                 {"signed_releases", calls.released[1]},
                                 {"wrong_family_calls", calls.wrong_family},
                                 {"init_calls", calls.init},
                                 {"shutdown_calls", calls.shutdown},
                                 {"failed_core_recording", {{"list", failed_recording->list}, {"generation", failed_recording->generation}, {"closed_nonreplayable", failed_recording->closed.load()}, {"gpu_timestamp", failed_timestamp}}},
                                 {"signed_evaluate_gpu_timestamp", evaluate_timestamp},
                                 {"oom_no_fallback", true},
                                 {"oom_memory_failure_flag", oom_memory_flag},
                                 {"borrowed_core_no_fallback", true}};
    std::ofstream output("core-parameter-family.json");
    output << receipt.dump(2) << '\n';
    Check(!output.fail(), "write Core parameter-family regression receipt");
    std::cout << "PASS Core-to-signed failure retirement, exact new-recording retry, captured function families, "
                 "OOM and borrowed-Core no-fallback; evidence=MOCK+WARP\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL Core parameter-family regression: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace core_parameter_family_cases
