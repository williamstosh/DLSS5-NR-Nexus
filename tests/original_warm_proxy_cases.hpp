#pragma once

#if defined(NR_NEXUS_TEST_NATIVE_NGX)
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(NR_NEXUS_TEST_INTEL_SDK)
#include "original_intel_sdk_cases.hpp"
#endif

namespace original_warm_proxy_cases {
namespace guard = renodx::addons::dlss5::detour_guard;

static native::DirectCreateFn real_create = nullptr;
static native::DirectEvaluateFn real_evaluate = nullptr;
static ID3D12Device* expected_proxy = nullptr;
static uint64_t proxy_evaluate_lists = 0;
static thread_local bool inside_nr = false;
static uint64_t real_creates = 0;
static uint64_t proxy_create_lists = 0;
static uint64_t wrong_face_attempts = 0;
static uint64_t nr_native_heap_bindings = 0;
static uint64_t nr_bad_heap_bindings = 0;
static std::vector<std::shared_ptr<submission::RecordingStatus>> signed_evaluation_recordings;
using SetHeapsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
static SetHeapsFn real_set_heaps = nullptr;

static bool IsProxyRecording(ID3D12GraphicsCommandList* list) {
  ComPtr<ID3D12Device> face;
  auto* native_list = list;
  return list != nullptr && SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&face)))
      && face.Get() == expected_proxy
      && renodx::utils::directx::NativeFromReShadeProxy(&native_list) && native_list != list;
}

static NVSDK_NGX_Result NVSDK_CONV ObserveCreate(ID3D12GraphicsCommandList* list,
    NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* block, NVSDK_NGX_Handle** handle) {
  if (!IsProxyRecording(list)) {
    // Report a failure without actually exposing the known incompatible list/heap pair to the SDK.
    ++wrong_face_attempts;
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  ++real_creates;
  ++proxy_create_lists;
  const bool previous = inside_nr;
  inside_nr = true;
  const auto result = real_create(list, feature, block, handle);
  inside_nr = previous;
  return result;
}

static NVSDK_NGX_Result NVSDK_CONV ObserveEvaluate(ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* block, PFN_NVSDK_NGX_ProgressCallback callback) {
  if (!IsProxyRecording(list)) {
    ++wrong_face_attempts;
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  ++proxy_evaluate_lists;
  signed_evaluation_recordings.push_back(submission::ObserveRecording(list));
  native_runtime_evaluations.push_back({
      .model = handle,
      .list = list,
      .depth = native::GetD3D12Resource(block, "DLSSNR.Depth"),
      .motion = native::GetD3D12Resource(block, "DLSSNR.MVec"),
      .mask = native::GetD3D12Resource(block, "DLSSNR.ControlMask")});
  const bool previous = inside_nr;
  inside_nr = true;
  const auto result = real_evaluate(list, handle, block, callback);
  inside_nr = previous;
  native_runtime_evaluation_results.push_back(result);
  return result;
}

static void STDMETHODCALLTYPE ObserveSetHeaps(ID3D12GraphicsCommandList* list, UINT count,
                                              ID3D12DescriptorHeap* const* heaps) {
  if (inside_nr) {
    for (UINT i = 0; i < count; ++i) {
      auto* face = heaps[i];
      if (renodx::utils::directx::NativeFromReShadeProxy(&face)) {
        ++nr_bad_heap_bindings;
      } else {
        ++nr_native_heap_bindings;
      }
    }
  }
  real_set_heaps(list, count, heaps);
}

inline int Run(const char* project_id_path, bool completion_only) {
  try {
    constexpr UINT width = 960, height = 540;
    constexpr size_t passes = 2;
    const bool fg_first = GetEnvironmentVariableW(L"NR_NEXUS_TEST_PROXY_FG_FIRST", nullptr, 0) != 0;
    Check(!fg_first || !completion_only, "completion-only starts with the ordinary proxy path");
    wchar_t host_path[32768]{};
    const DWORD host_path_length = GetEnvironmentVariableW(L"NR_NEXUS_TEST_RESHADES_DLL", host_path, 32768);
    Check(host_path_length > 0 && host_path_length < 32768,
          "real proxy fixture requires the explicit prepared ReShade DLL");
    SetEnvironmentVariableW(L"RESHADE_DISABLE_LOADING_CHECK", L"1");
    SetEnvironmentVariableW(L"RESHADE_DISABLE_INPUT_HOOK", L"1");
    SetEnvironmentVariableW(L"RESHADE_DISABLE_NETWORK_HOOK", L"1");
    SetEnvironmentVariableW(L"RESHADE_DISABLE_GRAPHICS_HOOK", nullptr);
    Check(LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr,
          "ReShade D3D12 trampoline has a real system module before explicit export entry");
    const auto host = LoadLibraryExW(host_path, nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    Check(host != nullptr, "actual full Add-on ReShade loaded in the controlled test process");
    const auto create_device = reinterpret_cast<decltype(&D3D12CreateDevice)>(GetProcAddress(host, "D3D12CreateDevice"));
    Check(create_device != nullptr, "actual ReShade exports D3D12CreateDevice");
    Gpu gpu(true, width, height, create_device);
    expected_proxy = gpu.device.Get();
    ComPtr<ID3D12Device> native_device;
    Check(SUCCEEDED(gpu.device->QueryInterface(__uuidof(renodx::utils::directx::ReShadeRetrieveBaseInterface), &native_device))
              && native_device.Get() != gpu.device.Get(),
          "real ReShade device is not the native interface");

    auto* native_list = gpu.list.Get();
    Check(renodx::utils::directx::NativeFromReShadeProxy(&native_list) && native_list != gpu.list.Get(),
          "real ReShade list owns a different native list");
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ComPtr<ID3D12DescriptorHeap> wrapped_heap;
    Check(SUCCEEDED(gpu.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&wrapped_heap))), "actual host descriptor heap");
    auto* native_heap = wrapped_heap.Get();
    Check(renodx::utils::directx::NativeFromReShadeProxy(&native_heap) && native_heap != wrapped_heap.Get(),
          "full Add-on host really wraps descriptor heaps");
    ComPtr<ID3D12Device> list_face, heap_face;
    Check(SUCCEEDED(gpu.list->GetDevice(IID_PPV_ARGS(&list_face))) && list_face.Get() == gpu.device.Get()
              && SUCCEEDED(wrapped_heap->GetDevice(IID_PPV_ARGS(&heap_face))) && heap_face.Get() == gpu.device.Get(),
          "list and heap GetDevice return the same exact real proxy face");
    ComPtr<ID3D12Device> proxy_queue_face;
    Check(SUCCEEDED(gpu.queue->GetDevice(IID_PPV_ARGS(&proxy_queue_face)))
              && proxy_queue_face.Get() == expected_proxy,
          "the real application's COM queue carries the exact proxy recording face");
    auto* queue = gpu.queue.Get();
    Check(renodx::utils::directx::NativeFromReShadeProxy(&queue) && queue != gpu.queue.Get(),
          "SDK submission queue has its genuine underlying native interface");
    ComPtr<ID3D12Device> submit_face;
    Check(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&submit_face))) && submit_face.Get() == native_device.Get(),
          "the native DIRECT submission interface does not identify the recording face");
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    native::addon_module = GetModuleHandleW(nullptr);
    native::enabled = true;
    native::hooks_enabled = true;
    native::hook_point = native::kHookPresent;
    native::present_pre_fg = 1;
    native::present_guides = native::kPresentGuidesOptional;
    native::stack_passes = passes;
    native::nr_resolution_scale = 1.f;
    native::nr_follow_input_res = false;
    native::codec_mode = 0;
    native::transfer_strength = native::color_strength = 1.f;
    native::control_mask_enabled = false;
    native::nr_adaptive_enabled = false;
    native::edit_reuse_enabled = false;
    native::codec_pipeline.incremental_preparation = false;
    Check(native::InstallQueueCompletionHooks(queue), "real native queue completion hooks");
    Check(native::InstallCommandListStateHooks(gpu.list.Get()), "real proxy/native recording hooks");
    const auto wait_fence = [](ID3D12Fence* fence, uint64_t value) {
      if (fence == nullptr || value == 0) return;
      if (fence->GetCompletedValue() < value) {
        const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event != nullptr, "proxy fixture completion event");
        const auto hr = fence->SetEventOnCompletion(value, event);
        const auto wait = (SUCCEEDED(hr) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
        CloseHandle(event);
        Check(wait == WAIT_OBJECT_0, "proxy fixture bounded completion");
      }
      Check(fence->GetCompletedValue() >= value && fence->GetCompletedValue() != UINT64_MAX,
            "actual fence completion is not device removal");
    };
    const auto submit = [&] {
      Check(SUCCEEDED(gpu.list->Close()), "proxy fixture closes its owned upload/consumer list");
      ID3D12CommandList* list = gpu.list.Get();
      Check(renodx::utils::directx::NativeFromReShadeProxy(&list),
            "fixture unwraps its own closed list only at the native queue boundary");
      queue->ExecuteCommandLists(1, &list);
      gpu.submission_pending = true;
      Check(SUCCEEDED(queue->Signal(gpu.fence.Get(), ++gpu.serial)), "fixture queue Signal");
      wait_fence(gpu.fence.Get(), gpu.serial);
      gpu.submission_pending = false;
      Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
            "fixture reuses only its completed owned list");
      submission::OnCommandListReset(gpu.list.Get());
      native::PruneCompletedSubmissionUses();
      gpu.uploads.clear();
      gpu.retained_resources.clear();
    };
    const auto read_counts = [] {
      // The direct Init boundary tells its own real arguments and return; don't interpose
      // Init/Shutdown or change which adapter owns the vendor call.
      const auto count_log = [](const char* marker) {
        return static_cast<uint64_t>(std::ranges::count_if(captured_logs, [&](const auto& line) {
          return line.second.find(marker) != std::string::npos;
        }));
      };
      return std::array<uint64_t, 4>{count_log("NR Init starting:"), count_log("NR Init finished:"),
                                     real_creates, native_runtime_evaluations.size()};
    };
    const auto module_path = [](HMODULE module) {
      wchar_t path[32768]{};
      const auto length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
      Check(length > 0 && length < std::size(path), "read actual mapped module path");
      return std::filesystem::path(path);
    };
    const auto selected_nr_images = [] {
      const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
      Check(snapshot != INVALID_HANDLE_VALUE, "enumerate actual process module mappings");
      MODULEENTRY32W entry{.dwSize = sizeof(MODULEENTRY32W)};
      std::vector<HMODULE> images;
      if (Module32FirstW(snapshot, &entry)) {
        do {
          if (_wcsicmp(entry.szModule, L"nvngx_dlssnr.dll") == 0) {
            images.push_back(entry.hModule);
          }
        } while (Module32NextW(snapshot, &entry));
      }
      CloseHandle(snapshot);
      return images;
    };
    Check(selected_nr_images().empty(), "the process starts without the signed NR image mapped");
    const auto private_roots = std::array{
        native::AddonDirectory() / L"nr-nexus-runtime",
        native::ReShadeBaseDirectory() / L"nr-nexus-runtime"};
    const auto no_private_artifacts = [&] {
      for (const auto& root : private_roots) {
        std::error_code error;
        const bool exists = std::filesystem::exists(root, error);
        Check(!error && !exists, "this controlled fixture has no private runtime directory");
      }
    };
    no_private_artifacts();
    const auto expected_original = native::FindNrRuntimePath();
    std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> preload(nullptr, &FreeLibrary);
    if (fg_first) {
      // A process may have only mapped the original file; do not call any NGX entry here.
      preload.reset(LoadLibraryExW(expected_original.c_str(), nullptr,
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
      Check(preload != nullptr && selected_nr_images() == std::vector{preload.get()},
            "pure preload maps the authorized original DLL before product discovery");
      Check(native::NgxLoaderView().nr_module == nullptr && native::direct_device == nullptr
                && read_counts() == std::array<uint64_t, 4>{},
            "preloading does not initialize or select an NR context");
    }
    native::PrimeNgxLoaderSymbols(gpu.device.Get());
    const auto& loader = native::NgxLoaderView();
    const auto original_module = loader.nr_module;
    Check(original_module != nullptr && loader.nr_ready && !loader.core_ready,
          "product discovery resolves the original image without an external Core client");
    Check(!fg_first || original_module == preload.get(), "product discovery references the same already mapped DLL");
    const auto original_path = module_path(original_module);
    Check(std::filesystem::equivalent(expected_original, original_path) && native::direct_device == nullptr
              && read_counts() == std::array<uint64_t, 4>{},
          "mapping the genuine original exports does not initialize NR");
    no_private_artifacts();
    Check(selected_nr_images() == std::vector{original_module}, "exactly one signed original NR mapping");

    const auto signed_create = reinterpret_cast<native::DirectCreateFn>(
        GetProcAddress(original_module, "NVSDK_NGX_D3D12_CreateFeature"));
    const auto signed_evaluate = reinterpret_cast<native::DirectEvaluateFn>(
        GetProcAddress(original_module, "NVSDK_NGX_D3D12_EvaluateFeature"));
    const auto native_set_heaps = reinterpret_cast<SetHeapsFn>((*reinterpret_cast<void***>(native_list))[28]);
    std::array<guard::Record, 3> records;
    const std::array hooks{
        guard::Hook{"actual original Create", reinterpret_cast<void**>(&real_create), reinterpret_cast<void*>(&ObserveCreate), &records[0]},
        guard::Hook{"actual original Evaluate", reinterpret_cast<void**>(&real_evaluate), reinterpret_cast<void*>(&ObserveEvaluate), &records[1]},
        guard::Hook{"native SetDescriptorHeaps", reinterpret_cast<void**>(&real_set_heaps), reinterpret_cast<void*>(&ObserveSetHeaps), &records[2]}};
    const auto attach_observers = [&] {
      real_create = signed_create;
      real_evaluate = signed_evaluate;
      real_set_heaps = native_set_heaps;
      size_t failed_hooks = 0;
      Check(guard::AttachAll(hooks, &failed_hooks) == hooks.size(),
            "narrow real signed and heap observers do not replace production state");
    };
    const auto detach_observers = [&] {
      for (const auto& hook : hooks) {
        Check(guard::Detach(hook) == guard::Removal::kDetached,
              "the fixture restores the original SDK entry before invoking independent Core");
      }
    };
    int observer_key = 0;
    const auto cleanup = [&](int*) {
      for (const auto& hook : hooks) {
        if (*hook.real != nullptr) {
          guard::Detach(hook);
        }
      }
    };
    const std::unique_ptr<int, decltype(cleanup)> observers(&observer_key, cleanup);
    attach_observers();

    const auto counters_before_ordinary = read_counts();

    adapter::modules[0].complete = true;
    adapter::modules[0].real[0] = reinterpret_cast<void*>(&FakeInit);
    adapter::modules[0].real[3] = reinterpret_cast<void*>(&FakeResource);
    adapter::modules[0].real[4] = reinterpret_cast<void*>(&FakeConstants);
    adapter::modules[0].real[5] = reinterpret_cast<void*>(&FakeId);
    adapter::modules[0].real[6] = reinterpret_cast<void*>(+[](void*, uint32_t) -> int32_t { return 0; });
    adapter::modules[0].real[8] = reinterpret_cast<void*>(+[](void*) -> int32_t { return 0; });
    adapter::proxies[0].real[0] = reinterpret_cast<void*>(&nr_edit_reuse_xefg_cases::ConsumeSdkPresent);
    adapter::proxies[0].real[1] = reinterpret_cast<void*>(&FakePresent1);
    xefg::Configure(&native::fg_service::Service, &native::fg_service::PrepareCapture,
                    &native::fg_service::Retire, &native::fg_service::Requested,
                    false, true, &native::fg_service::PrepareRuntime);
    Session session(&gpu);
    session.context->ui_composition = false;
    session.extent_left = session.extent_top = 0;
    session.extent_width = width;
    session.extent_height = height;
    Check(Hooks::Enable(session.handle, 0) == 0, "ordinary proxy path starts with XeFG genuinely Off");
    ComPtr<ID3D12Resource> color;
    Check(SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color))),
          "actual current SDK-proxy backbuffer");
    auto motion = gpu.Texture(width, height, DXGI_FORMAT_R16G16_FLOAT);
    auto depth = gpu.Texture(width, height, DXGI_FORMAT_R32_FLOAT);
    gpu.Write(motion.Get(), 0);
    gpu.Write(depth.Get(), 0);
    submit();
    const auto transfer = gpu.CreateTextureTransfer(color.Get());
    std::vector<uint8_t> consumed;
    uint64_t consumer_reads = 0;
    size_t frame_evaluation_begin = 0;
    nlohmann::json first_delivery;
    nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* output) {
      Check(output == session.proxy.Get(), "first SDK consumer receives the same current output object");
      if (!completion_only && native_runtime_evaluations.size() == frame_evaluation_begin + passes) {
        const auto& first = signed_evaluation_recordings[frame_evaluation_begin];
        const auto& last = signed_evaluation_recordings.back();
        Check(first != nullptr && first == last && first->commands_closed.load() && first->submitted.load(),
              "both signed calls belong to the exact same closed, submitted production recording");
        auto* const proxy_list = native_runtime_evaluations.back().list;
        auto* submitted_list = proxy_list;
        Check(renodx::utils::directx::NativeFromReShadeProxy(&submitted_list) && submitted_list != proxy_list,
              "a genuine proxy records both passes before its native list is executed");
        Check(submission::RecordingIdentity(submitted_list) == std::pair{first->list, first->generation},
              "the executed native alias and the proxy recording have one actual identity and generation");
        FenceRef tracked_submit_fence;
        {
          const std::shared_lock lock(renodx::addons::dlss5::queue_completion_mutex);
          tracked_submit_fence = FenceRef(renodx::addons::dlss5::queue_completions.at(queue).fence);
        }
        const auto native_color = renodx::utils::directx::NativeIdentity(color.Get());
        {
          const std::lock_guard lock(submission::tracker_mutex);
          const auto& generation = submission::tracked_lists.at(first->list).generations.at(first->generation);
          const auto color_use = std::ranges::find_if(generation.classified_uses, [&](const auto& use) {
            return use.kind == submission::UseKind::kOutputColor
                && use.output.resource == native_color && use.output.subresource == 0;
          });
          Check(generation.first_execute != 0 && generation.unreserved_submissions == 0
                    && generation.submissions.size() == 1 && generation.first_submission_index == 0
                    && color_use != generation.classified_uses.end(),
                "one actual Execute carries the same current backbuffer subresource before this first consumer");
          const auto first_signal = submission::ReadSubmission(generation.submissions.front());
          Check(first_signal.fence.fence == tracked_submit_fence.fence && first_signal.value != 0
                    && (first_signal.state == submission::SubmissionProofState::kInFlight
                        || first_signal.state == submission::SubmissionProofState::kComplete),
                "the first Signal belongs to the actual native submission queue, without pre-waiting");
          first_delivery = {{"recording", {first->list, first->generation}},
                            {"proxy_list", reinterpret_cast<uintptr_t>(proxy_list)},
                            {"submitted_list", reinterpret_cast<uintptr_t>(submitted_list)},
                            {"first_execute", generation.first_execute},
                            {"submit_queue", reinterpret_cast<uintptr_t>(queue)},
                            {"first_fence", reinterpret_cast<uintptr_t>(first_signal.fence.fence)},
                            {"first_fence_value", first_signal.value},
                            {"consumer_resource", reinterpret_cast<uintptr_t>(native_color)},
                            {"subresource", color_use->output.subresource}, {"tracked_version", color_use->output.version}};
        }
      }
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.RecordTextureReadback(color.Get(), transfer);
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
      submit();
      consumed = gpu.ReadTexture(transfer);
      ++consumer_reads;
    };
    uint32_t frame_id = 70000;
    native::PresentStream ordinary;
    native::FeatureState contract{.input_width = width, .input_height = height};
    native::PresentTarget target{.output_state = D3D12_RESOURCE_STATE_COMMON,
                                 .workset_key = reinterpret_cast<const ID3D12Resource*>(&ordinary)};
    native::SnapshotPresentRestoreConfiguration(&target);
    target.domain = renodx::addons::dlss5::present_restore_scope::Domain::FINAL_PRESENT;
    target.swapchain_identity = adapter::Identity(session.proxy.Get());
    target.diagnostic.bypass_host_look = true;
    target.pass_transfer_strength.fill(1.f);
    target.pass_color_strength.fill(1.f);
    const auto prepare_seed = [&](uint8_t value) {
      gpu.Write(color.Get(), value);
      submit();
      consumed.clear();
      first_delivery = nullptr;
      frame_evaluation_begin = native_runtime_evaluations.size();
      ++frame_id;
    };
#if defined(NR_NEXUS_TEST_INTEL_SDK)
    std::unique_ptr<original_intel_sdk_cases::Consumer> intel;
    if (!completion_only) {
      intel = std::make_unique<original_intel_sdk_cases::Consumer>(queue, width, height, "intel-warm-proxy.json");
    }
#endif
    const auto joined_pixels = [&] {
      Check(consumer_reads != 0, "a consumer has actually read the current output");
      uint64_t hash = 14695981039346656037ull;
      for (const auto value : consumed) {
        hash = (hash ^ value) * 1099511628211ull;
      }
      return hash;
    };
    const auto run_ordinary = [&](uint8_t seed, native::PresentSlot* replacement_slot = nullptr,
                                  native::PresentStream* replacement_stream = nullptr) {
      prepare_seed(seed);
      auto& output_stream = (replacement_stream != nullptr ? *replacement_stream : ordinary);
      target.workset_key = reinterpret_cast<const ID3D12Resource*>(&output_stream);
      const auto before = native_runtime_evaluations.size();
      const auto reads = consumer_reads;
      bool delivered = false;
      {
        const renodx::addons::dlss5::ngx_serial::PresentTurn turn;
        Check(turn.owns(), "ordinary proxy NGX turn");
        const renodx::addons::dlss5::RuntimeLock lock(native::runtime_mutex);
        // This is the actual game's device face of the real command list,
        // not a face manufactured from the bottom-level SDK queue.
        Check(native::EnsurePresentRing(list_face.Get()), "ordinary proxy ring on the real recording face");
        ++native::present_generation;
        auto& block = native::FillPresentBlock(&output_stream, color.Get(), motion.Get(), depth.Get(),
                                               contract, width, height, 1.f, 0);
        auto* slot = (replacement_slot != nullptr
            ? replacement_slot
            : &native::present_ring.slots[(native::present_ring.submitted + 1) % native::kPresentRing]);
        delivered = native::SubmitPresentGroup(queue, nullptr, slot, output_stream, block, nullptr, target, false, {});
      }
      session.Present(frame_id);
      wait_fence(native::present_ring.fence, native::present_ring.submitted);
      Check(consumer_reads == reads + 1, "ordinary frame has one first SDK-consumer GPU read");
      if (!delivered) return false;
      Check(native_runtime_evaluations.size() == before + passes,
            "qualified ordinary frame completes exactly two signed Evaluate calls");
      for (size_t i = before; i < before + passes; ++i) {
        Check(NVSDK_NGX_SUCCEED(native_runtime_evaluation_results[i]), "real ordinary signed Evaluate succeeds");
      }
      return true;
    };
    const auto stream_key = std::pair{false, reinterpret_cast<uintptr_t>(session.handle)};
    constexpr uint32_t full_mask = (1u << passes) - 1;
    nlohmann::json xefg_cold = nlohmann::json::array();
    const auto run_xefg = [&](uint8_t seed) {
      prepare_seed(seed);
      const auto prior = native::fg_service::streams.find(stream_key);
      const uint32_t ready_before = (prior != native::fg_service::streams.end()
          ? LiveWarmFixturePassMask(prior->second.roles[0].Handle()) : 0);
      const auto before = native_runtime_evaluations.size();
      const auto create_before = real_creates;
      const auto reads = consumer_reads;
      session.Guides(frame_id, depth.Get(), motion.Get());
      session.Present(frame_id);
      const auto it = native::fg_service::streams.find(stream_key);
      if (it != native::fg_service::streams.end()) {
        wait_fence(it->second.fence, it->second.submitted);
      }
      Check(consumer_reads == reads + 1, "XeFG frame has one first SDK-consumer GPU read");
      const auto result = xefg::CurrentResult();
      const auto evaluates = native_runtime_evaluations.size() - before;
      const auto expected_size = static_cast<size_t>(width) * height * 4;
      Check(consumed.size() == expected_size, "the first read covers the actual current XeFG frame");
      for (size_t i = 3; i < consumed.size(); i += 4) {
        Check(consumed[i] == seed, "XeFG first-consumer frame marker matches");
      }
      for (size_t i = before; i < before + evaluates; ++i) {
        Check(NVSDK_NGX_SUCCEED(native_runtime_evaluation_results[i]), "real XeFG signed Evaluate succeeds");
      }
      if (result != fg::Result::SUBMITTED || evaluates != passes) {
        const auto creates = real_creates - create_before;
        if (result == fg::Result::NOT_SERVED) {
          Check(creates == 0 && evaluates == 0, "declined service does not invoke NR");
          return false;
        }
        const bool wrote_color = result == fg::Result::SUBMITTED;
        Check(ready_before != full_mask && creates > 0 && evaluates < passes
                  && ((wrote_color && evaluates > 0) || (result == fg::Result::PREPARED && evaluates == 0)),
              "only a real cold model contract can expose preparation or partial color");
        if (!wrote_color) {
          Check(std::ranges::all_of(consumed, [&](uint8_t channel) { return channel == seed; }),
                "preparation is not a current color write");
        }
        Check(it != native::fg_service::streams.end(), "cold preparation came from this live SDK epoch");
        xefg_cold.push_back({{"frame", frame_id}, {"marker", seed}, {"ready_before", ready_before},
                            {"ready_after", LiveWarmFixturePassMask(it->second.roles[0].Handle())},
                            {"creates", creates}, {"evaluates", evaluates}, {"submitted_color", wrote_color},
                            {"result", static_cast<uint32_t>(result)}, {"first_rgba", {consumed[0], consumed[1],
                             consumed[2], consumed[3]}}});
        return false;
      }
      Check(ready_before == full_mask, "the already-mature XeFG contract gives exactly two real Evaluate calls");
#if defined(NR_NEXUS_TEST_INTEL_SDK)
      if (intel != nullptr) {
        intel->Accept(color.Get(), depth.Get(), motion.Get(), consumed,
                      session.context->epoch, frame_id, "xefg-warm-proxy-backbuffer");
      }
#endif
      return true;
    };

    const auto edited = [&](uint8_t seed) {
      Check(consumed.size() == static_cast<size_t>(width) * height * 4, "first SDK entry reads a complete current frame");
      size_t changed = 0;
      for (size_t index = 0; index < consumed.size(); index += 4) {
        Check(consumed[index + 3] == seed, "consumer alpha belongs to this seeded current frame");
        changed += consumed[index] != seed || consumed[index + 1] != seed || consumed[index + 2] != seed;
      }
      return changed;
    };
    nlohmann::json receipt{{"mode", (fg_first ? "preloaded_proxy_fg_first" : "cold_ordinary_proxy_xefg_ordinary")},
                           {"scope", "NATIVE_GPU+NATIVE_NGX+MOCK_XeFG"},
                           {"original_module", reinterpret_cast<uintptr_t>(original_module)},
                           {"original_mapped_path", original_path.string()},
                           {"host_module", reinterpret_cast<uintptr_t>(host)},
                           {"host_mapped_path", module_path(host).string()},
                           {"proxy_device", reinterpret_cast<uintptr_t>(gpu.device.Get())},
                           {"native_device", reinterpret_cast<uintptr_t>(native_device.Get())},
                           {"proxy_application_queue", reinterpret_cast<uintptr_t>(gpu.queue.Get())},
                           {"native_queue", reinterpret_cast<uintptr_t>(queue)},
                           {"counter_fields", {"direct_init_started", "direct_init_finished", "signed_create", "signed_evaluate"}},
                           {"before_business_entry", counters_before_ordinary},
                           {"mapped_without_init", fg_first},
                           {"rounds", nlohmann::json::array()}};
    const auto save_frame = [&](const char* route, uint8_t seed, ID3D12Fence* fence, uint64_t value) {
      Check(first_delivery.contains("first_execute") && fence != nullptr && value != 0
                && fence->GetCompletedValue() != UINT64_MAX && fence->GetCompletedValue() >= value,
            "same-resource first reader and actual producer completion are both proven");
      receipt["rounds"].push_back({{"route", route}, {"frame", frame_id}, {"seed", seed},
                                   {"application_frame", session.context->application_frame},
                                   {"rgba_fingerprint", joined_pixels()}, {"changed_rgb_pixels", edited(seed)},
                                   {"first_rgba", {consumed[0], consumed[1], consumed[2], consumed[3]}},
                                   {"delivery", first_delivery}, {"fence", reinterpret_cast<uintptr_t>(fence)},
                                   {"fence_value", value}, {"sdk_epoch", session.context->epoch},
                                   {"call_counts", read_counts()}});
    };
    const auto save_xefg = [&](const char* route, uint8_t seed) {
      const auto& stream = native::fg_service::streams.at(stream_key);
      Check(stream.queue == queue && stream.device == expected_proxy
                && renodx::utils::directx::SameNativeObject(stream.device, native_device.Get()),
            "production SDK service records with the actual proxy face and submits on the actual native DIRECT queue");
      save_frame(route, seed, stream.fence, stream.submitted);
    };
    const auto first_init = [&] {
      const auto found = std::ranges::find_if(captured_logs, [](const auto& line) {
        return line.second.find("NR Init finished:") != std::string::npos;
      });
      Check(found != captured_logs.end() && found->second.ends_with("result=0x1")
                && native::direct_device == expected_proxy && native::direct_api.module == original_module
                && !native::parameter_runtime_via_core.load(),
            "the normal product Init succeeds once with the legal proxy objects on the known signed original");
      receipt["first_init_result_log"] = found->second;
    };
    const auto mature_xefg = [&](uint8_t marker) {
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8; ++attempt) {
        const auto seed = static_cast<uint8_t>(marker + attempt);
        if (run_xefg(seed)) return seed;
      }
      throw std::runtime_error("no complete mature two-pass XeFG color reached the original consumer");
    };
    const auto write_receipt = [&] {
      no_private_artifacts();
      Check(selected_nr_images() == std::vector{original_module}
                && proxy_create_lists == real_creates && proxy_create_lists != 0
                && proxy_evaluate_lists == native_runtime_evaluations.size() && proxy_evaluate_lists != 0
                && wrong_face_attempts == 0 && nr_native_heap_bindings > 0 && nr_bad_heap_bindings == 0,
            "one original image; all real signed calls record on proxy lists and bind only native heaps in the driver");
      receipt["first_sdk_gpu_reads"] = consumer_reads;
      receipt["signed_create_proxy_lists"] = proxy_create_lists;
      receipt["signed_evaluate_proxy_lists"] = proxy_evaluate_lists;
      receipt["wrong_face_attempts_at_signed_sdk"] = wrong_face_attempts;
      receipt["native_heap_bindings"] = nr_native_heap_bindings;
      receipt["bad_heap_bindings"] = nr_bad_heap_bindings;
      receipt["mapped_original_nr_images"] = selected_nr_images().size();
      receipt["private_artifact_scope"] = "mapping and endpoint snapshot; process-wide file-create proof is separate";
      receipt["true_intel_sdk_internal_first_read"] = "not measured; mock first GPU read and real Intel Tag/Present are separate";
      const char* name = (fg_first ? "proxy-preloaded-fg-first.json" : "warm-proxy-runtime.json");
      std::ofstream output(name);
      output << receipt.dump(2) << '\n';
      output.close();
      Check(!output.fail(), "write exact real-proxy receipt");
      nr_edit_reuse_xefg_cases::consume_sdk_present = {};
    };
    if (fg_first) {
      Check(Hooks::Enable(session.handle, 1) == 0 && native::direct_device == nullptr
                && native::present_ring.device == nullptr && read_counts() == counters_before_ordinary,
            "production SDK enable hook does not warm a device or call vendor Init");
      Check(session.context->queue.Get() == gpu.queue.Get(),
            "XeFG gets the real COM proxy queue from the existing application Init boundary");
      const uint8_t first_marker = mature_xefg(136);
      Check(edited(first_marker) > 0 && native::present_ring.device == nullptr
                && read_counts()[0] == 1 && read_counts()[1] == 1,
            "the first actual NR business path is XeFG, without a preparation pass through ordinary");
      first_init();
      save_xefg("preloaded-fg-first", first_marker);
      const auto first_counts = read_counts();
      Check(run_xefg(162) && edited(162) > 0 && read_counts()[2] == first_counts[2],
            "next complete XeFG frame uses the same mature model pair");
      save_xefg("preloaded-fg-next", 162);
      Check(Hooks::Enable(session.handle, 0) == 0, "preloaded FG-first path transitions to ordinary");
      bool ordinary_mature = false;
      for (uint32_t step = 0; step < native::kContractDebounceFrames + 8 && !ordinary_mature; ++step) {
        ordinary_mature = run_ordinary(117);
      }
      Check(ordinary_mature && edited(117) > 0 && read_counts()[0] == first_counts[0]
                && read_counts()[1] == first_counts[1] && native::direct_device == expected_proxy,
            "ordinary after FG-first shares the initialized proxy face and delivers current color");
      save_frame("preloaded-ordinary-after", 117, native::present_ring.fence, native::present_ring.submitted);
      receipt["xefg_model_preparation"] = std::move(xefg_cold);
      session.CloseSdk();
      write_receipt();
      std::cout << "PASS real proxy preloaded original, FG-first two current passes, no extra Init on ordinary return\n";
      return 0;
    }

    bool ordinary_ready = false;
    for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !ordinary_ready; ++attempt) {
      ordinary_ready = run_ordinary(72);
    }
    const auto first_ordinary_counts = read_counts();
    Check(ordinary_ready && edited(72) > 0 && first_ordinary_counts[0] == 1
              && first_ordinary_counts[1] == 1 && first_ordinary_counts[2] >= passes
              && first_ordinary_counts[3] >= passes,
          "cold ordinary SubmitPresentGroup itself initializes and delivers two signed passes on the real proxy");
    first_init();
    if (!completion_only) {
      save_frame("ordinary-first", 72, native::present_ring.fence, native::present_ring.submitted);
    }
    const auto ordinary_handles = [&] {
      std::array<uintptr_t, passes> handles{};
      const auto& feature = native::features.at(ordinary.Handle());
      for (size_t pass = 0; pass < passes; ++pass) {
        handles[pass] = reinterpret_cast<uintptr_t>(feature.slots[pass].handle);
      }
      return handles;
    }();
    receipt["first_ordinary_counts"] = first_ordinary_counts;
    receipt["first_ordinary_frame"] = frame_id;
    receipt["ordinary_handles"] = ordinary_handles;

    if (completion_only) {
      // Exercise completion through the same production SDK epoch and Service entry.
      namespace timers = renodx::addons::dlss5::gpu_timers;
      Check(Hooks::Enable(session.handle, 1) == 0, "enable the real Service owner on the warm proxy face");
      bool mature = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !mature; ++attempt) {
        mature = run_xefg(149);
      }
      Check(mature && edited(149) != 0, "actual Service prepares and first-delivers its mature warm group");
      native::gpu_timers_enabled = false;
      native::cost_meter_enabled = false;
      timers::comparison_settings = native::comparison_ui::CaptureSettings;
      timers::Poll(true, false);
      const auto pending_before = timers::internal::next_sequence + 1;
      const auto prior_counts = read_counts();
      const auto sdk_epoch = session.context->epoch;
      uint64_t first_slot_value = 0;
      uint64_t first_completed_value = 0;
      std::shared_ptr<submission::RecordingStatus> first_recording;
      std::shared_ptr<submission::RecordingStatus> second_recording;
      bool completed_before_reuse = false;
      auto* const active_list = [&]() {
        const auto& stream = native::fg_service::streams.at(stream_key);
        const auto slot = std::ranges::find(stream.slots, stream.submitted, &native::fg_service::Slot::fence_value);
        Check(slot != stream.slots.end() && slot->list != nullptr, "warm submitted Service slot exists");
        return slot->list;
      }();
      for (const uint8_t seed : {uint8_t{161}, uint8_t{173}}) {
        prepare_seed(seed);
        // This callback reports SDK return, not proof that its GPU read took
        // place. Readback of the two frames below occurs after the test gate.
        const auto sdk_consumer = std::exchange(nr_edit_reuse_xefg_cases::consume_sdk_present,
                                                [](IDXGISwapChain*) {});
        ComPtr<ID3D12Fence> gate;
        const auto unblock = [](ID3D12Fence* fence) { fence->Signal(1); };
        std::unique_ptr<ID3D12Fence, decltype(unblock)> gate_release(nullptr, unblock);
        if (seed == 161) {
          Check(SUCCEEDED(native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))),
                "completion proof test gate");
          gate_release.reset(gate.Get());
          Check(SUCCEEDED(queue->Wait(gate.Get(), 1)), "hold first true Service operation in flight");
        }
        const auto before = read_counts();
        native::stage_cost_sample_due_ns[2] = 0;
        session.Guides(frame_id, depth.Get(), motion.Get());
        session.Present(frame_id);
        const auto after = read_counts();
        Check(xefg::CurrentResult() == fg::Result::SUBMITTED && after[3] == before[3] + passes
                  && after[2] == before[2] && before[0] == after[0] && before[1] == after[1],
              "completion-only fixture reaches actual Service submission, two real Evaluate and no rebuild");
        const auto& stream = native::fg_service::streams.at(stream_key);
        const auto slot = std::ranges::find(stream.slots, stream.submitted, &native::fg_service::Slot::fence_value);
        Check(stream.submitted != 0 && slot != stream.slots.end() && slot->list == active_list
                  && stream.epoch == sdk_epoch && SUCCEEDED(native_device->GetDeviceRemovedReason()),
              "the actual queue reuses the same XeFG slot in the same SDK epoch");
        auto status = submission::ObserveRecording(slot->list);
        Check(status != nullptr && status->submitted.load() && status->commands_closed.load()
                  && submission::FirstHealthyUse(*status, slot->list),
              "the exact Service generation owns a genuine single tracked submission");
        if (seed == 161) {
          first_recording = std::move(status);
          first_slot_value = stream.submitted;
          const auto in_flight = stream.fence->GetCompletedValue();
          Check(in_flight != UINT64_MAX && in_flight < first_slot_value, "GPU really has not completed that Service frame");
          timers::Poll(true, false);
          Check(!timers::execution_stats[2].valid || timers::execution_stats[2].sequence < pending_before,
                "submitting a real but incomplete Service recording cannot publish a completed sample");
          Check(SUCCEEDED(gate->Signal(1)), "release test-only obstruction after the incomplete proof check");
          gate_release.release();
        } else {
          second_recording = std::move(status);
        }
        wait_fence(stream.fence, stream.submitted);
        if (seed == 161) {
          first_completed_value = stream.fence->GetCompletedValue();
          timers::Poll(true, false);
          completed_before_reuse = timers::execution_stats[2].valid
                                   && timers::execution_stats[2].sequence >= pending_before;
          if (completed_before_reuse) {
            Check(first_recording->closed.load()
                      && first_recording->close_reason.load() == submission::CloseReason::kOwnerDiscarded,
                  "GPU completion alone cannot publish a still-replayable Service recording");
          }
        }
        nr_edit_reuse_xefg_cases::consume_sdk_present = sdk_consumer;
        sdk_consumer(session.proxy.Get());
        Check(edited(seed) != 0 && consumed[3] == seed, "completion fixture output belongs to the same current real NR frame");
      }
      timers::Poll(true, false);
      const auto& completed = timers::execution_stats[2];
      Check(first_recording != nullptr && second_recording != nullptr && first_recording->closed.load()
                && first_recording->list == second_recording->list
                && first_recording->generation != second_recording->generation,
            "the next actual Service records a new generation on the completed slot");
      Check(completed.valid && completed.state == timers::SampleState::VALID && completed.sequence >= pending_before
                && completed.width == width && completed.height == height
                && completed.total_us == 0 && native::CurrentGpuSample(completed, timers::NowNs()),
            "completion-only Service slot reuse preserves real stage dimensions without timestamps");
      for (uint32_t pass = 0; pass < passes; ++pass) {
        Check(completed.passes[pass].width == width && completed.passes[pass].height == height
                  && completed.passes[pass].total_us == 0,
              "completion-only sample preserves both actual NR pass dimensions");
      }
      const auto counts = read_counts();
      Check(counts[0] == prior_counts[0] && counts[1] == prior_counts[1]
                && counts[2] == prior_counts[2] && counts[3] == prior_counts[3] + 2 * passes,
            "reusing the genuine Service slot neither rebuilds nor reinitializes NR");
      const auto evidence = nlohmann::json{
          {"scope", "NATIVE_GPU+NATIVE_NGX+MOCK_XeFG"}, {"mapped_path", loader.nr_loaded_path.string()}, {"original_module", reinterpret_cast<uintptr_t>(original_module)}, {"sdk_epoch", sdk_epoch}, {"recording_list", first_recording->list}, {"first_generation", first_recording->generation}, {"second_generation", second_recording->generation}, {"first_fence_value", first_slot_value}, {"first_completed_value", first_completed_value}, {"sample_before_reuse", completed_before_reuse}, {"sample_sequence", completed.sequence}, {"stage_dimensions", {completed.width, completed.height}}, {"pass_dimensions", {{completed.passes[0].width, completed.passes[0].height}, {completed.passes[1].width, completed.passes[1].height}}}, {"timestamps", false}, {"real_evaluate_delta", counts[3] - prior_counts[3]}, {"sdk_internal_first_read", "not measured"}};
      session.CloseSdk();
      {
        const renodx::addons::dlss5::ngx_serial::PresentTurn turn;
        Check(turn.owns(), "completion fixture retires its local ordinary owner");
        const renodx::addons::dlss5::RuntimeLock lock(native::runtime_mutex);
        native::ForgetSourceHandleLocked(ordinary.Handle());
        Check(native::ReleasePresentRing(), "release the already completed ordinary fixture ring");
        native::DrainRetiredResources(true);
      }
      no_private_artifacts();
      std::ofstream output("present-guide-service-completion.json");
      output << evidence.dump(2) << '\n';
      output.close();
      Check(!output.fail(), "write the real Service completion receipt");
      std::cout << "PASS present-guide-service-completion evidence=NATIVE_GPU+NATIVE_NGX+MOCK_XeFG"
                   " real_evaluate=2,2 same_slot=1 new_generation=1 true_fence=1"
                   " incomplete_sample=blocked stage=960x540 passes=960x540,960x540 timestamps=off\n";
      return 0;
    }

    // The separate real public Core client starts only after the ordinary
    // production path has established its own real, working signed context.
    detach_observers();
    std::ifstream project_file(project_id_path);
    std::string project_id;
    Check(project_file && std::getline(project_file, project_id) && project_id.size() == 36,
          "read only the existing project registration");
    const auto directory = native::AddonDirectory().wstring();
    const wchar_t* paths[]{directory.c_str()};
    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo = {paths, 1};
    common.LoggingInfo = {control_mask_gpu_cases::NativeNgxSdkLog, NVSDK_NGX_LOGGING_LEVEL_ON, true};
    const auto core_init = NVSDK_NGX_D3D12_Init_with_ProjectID(project_id.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                               "NR-Nexus-original-warm-proxy", directory.c_str(),
                                                               gpu.device.Get(), &common);
    Check(NVSDK_NGX_SUCCEED(core_init), "public Core initializes independently after the first ordinary frame");
    NVSDK_NGX_Parameter* core_parameters = nullptr;
    Check(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_AllocateParameters(&core_parameters)) && core_parameters != nullptr,
          "public Core owns its paired parameter allocation");
    native::SetNrCreateParameters(core_parameters, width, height, width, height, 0, false, 0);
    NVSDK_NGX_Handle* core_model = nullptr;
    const auto core_create = NVSDK_NGX_D3D12_CreateFeature(gpu.list.Get(), native::kFeatureDlssNr,
                                                          core_parameters, &core_model);
    Check(NVSDK_NGX_SUCCEED(core_create) && core_model != nullptr
              && reinterpret_cast<uintptr_t>(core_model) != ordinary_handles[0]
              && reinterpret_cast<uintptr_t>(core_model) != ordinary_handles[1],
          "real public Core creates its own independent original NR feature");
    submit();
    receipt["core_init"] = static_cast<uint32_t>(core_init);
    receipt["core_create"] = static_cast<uint32_t>(core_create);
    Check(selected_nr_images() == std::vector{original_module}, "public Core has no second NR image");
    attach_observers();
    Check(run_ordinary(82) && edited(82) > 0 && native::direct_device == expected_proxy,
          "already-selected ordinary path and its exact face still work with the external Core client live");
    const auto measured_counts = read_counts();
    const auto off_fingerprint = joined_pixels();
    save_frame("ordinary-before", 82, native::present_ring.fence, native::present_ring.submitted);
    no_private_artifacts();
    Check(Hooks::Enable(session.handle, 1) == 0, "actual Off to On transition");
    const uint8_t first_xefg_marker = mature_xefg(149);
    Check(edited(first_xefg_marker) > 0 && joined_pixels() != off_fingerprint,
          "first mature XeFG double Pass reaches the same-frame consumer with distinct current pixels");
    const auto on_fingerprint = joined_pixels();
    save_xefg("xefg-first", first_xefg_marker);
    receipt["xefg_distinct_model_creation_window"] = std::move(xefg_cold);
    const auto warm_creates = real_creates;
    Check(run_xefg(162) && edited(162) > 0 && joined_pixels() != on_fingerprint
              && real_creates == warm_creates,
          "same-epoch hot double Pass uses current pixels without recreating the models");
    save_xefg("xefg-hot", 162);
    const auto hot_fingerprint = joined_pixels();
    Check(Hooks::Enable(session.handle, 0) == 0 && run_ordinary(117)
              && edited(117) > 0 && joined_pixels() != hot_fingerprint,
          "On to Off restores the actual ordinary double Pass path with a new current frame");
    save_frame("ordinary-after", 117, native::present_ring.fence, native::present_ring.submitted);
    for (size_t pass = 0; pass < passes; ++pass) {
      const auto& feature = native::features.at(ordinary.Handle());
      Check(reinterpret_cast<uintptr_t>(feature.slots[pass].handle) == ordinary_handles[pass],
            "ordinary model identity is not retired by a healthy provider handoff");
    }
    const auto after_handoff = read_counts();
    Check(after_handoff[0] == measured_counts[0] && after_handoff[1] == measured_counts[1]
              && native::direct_device == expected_proxy && native::direct_api.module == original_module,
          "the switch preserves the real proxy binding without another product Init");
    receipt["before_switch"] = measured_counts;
    receipt["after_switch"] = after_handoff;

    {
      // The same native submission device really can create a distinct native list.
      // It is not an alternate recording face for this live proxy runtime.
      ComPtr<ID3D12CommandAllocator> allocator;
      ComPtr<ID3D12GraphicsCommandList> list;
      Check(SUCCEEDED(native_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
                && SUCCEEDED(native_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    allocator.Get(), nullptr, IID_PPV_ARGS(&list))),
            "construct the actual same-device native list mismatch");
      ComPtr<ID3D12Device> wrong_face;
      Check(SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&wrong_face))) && wrong_face.Get() == native_device.Get()
                && wrong_face.Get() != expected_proxy && SUCCEEDED(list->Close()),
            "the mismatch is exact COM face, not a foreign adapter or fabricated pointer");
      native::PresentSlot incompatible{.allocator = allocator.Get(), .list = list.Get()};
      native::PresentStream incompatible_stream;
      const auto before = read_counts();
      const auto previous_fence = native::present_ring.submitted;
      constexpr uint8_t invalid_marker = 103;
      Check(!run_ordinary(invalid_marker, &incompatible, &incompatible_stream)
                && edited(invalid_marker) == 0 && read_counts() == before && wrong_face_attempts == 0
                && native::present_ring.submitted == previous_fence && native::direct_device == expected_proxy,
            "production declines the genuinely incompatible list before Init/Create/Evaluate or any new output write");
      receipt["exact_face_negative"] = {{"frame", frame_id}, {"native_list", reinterpret_cast<uintptr_t>(list.Get())},
                                        {"native_face", reinterpret_cast<uintptr_t>(wrong_face.Get())},
                                        {"active_proxy_face", reinterpret_cast<uintptr_t>(expected_proxy)},
                                        {"first_current_original", joined_pixels()}, {"submitted", false},
                                        {"call_count_delta", {0, 0, 0, 0}}};
      Check(run_ordinary(91) && edited(91) > 0 && read_counts()[2] == before[2],
            "the genuine ordinary proxy path remains usable without recreating either model");
      for (size_t pass = 0; pass < passes; ++pass) {
        Check(reinterpret_cast<uintptr_t>(native::features.at(ordinary.Handle()).slots[pass].handle) == ordinary_handles[pass],
              "the incompatible request did not retire the genuine ordinary model pair");
      }
      save_frame("ordinary-after-mismatch", 91, native::present_ring.fence, native::present_ring.submitted);
    }

    const auto before_rearm = read_counts();
    const auto released_before = native::nr_features_released;
    Check(native::Shutdown(false) && native::direct_device == nullptr
              && !native::features.contains(ordinary.Handle()) && native::nr_features_released > released_before,
          "actual production device teardown retires its own models and clears its recording binding");
    Check(selected_nr_images() == std::vector{original_module} && read_counts() == before_rearm,
          "local device shutdown does not remap the signed original or attempt another Init");
    native::PrimeNgxLoaderSymbols(gpu.device.Get());
    Check(native::NgxLoaderView().nr_module == original_module && read_counts() == before_rearm,
          "after teardown, production discovery still does not pre-initialize the original image");
    native::PresentStream rearmed_stream;
    bool rearmed = false;
    for (uint32_t step = 0; step < native::kContractDebounceFrames + 8 && !rearmed; ++step) {
      rearmed = run_ordinary(136, nullptr, &rearmed_stream);
    }
    const auto rearm_counts = read_counts();
    Check(rearmed && edited(136) > 0 && rearm_counts[0] == before_rearm[0] + 1
              && rearm_counts[1] == before_rearm[1] + 1 && rearm_counts[2] >= before_rearm[2] + passes
              && native::direct_device == expected_proxy && native::direct_api.module == original_module,
          "the next legitimate proxy entry rebuilds local models without a private image or stale recording");
    save_frame("ordinary-after-device-teardown", 136, native::present_ring.fence, native::present_ring.submitted);
    receipt["local_rearm"] = {{"retired_owned_features", native::nr_features_released - released_before},
                               {"nexus_dll_unload_reload", false}, {"before", before_rearm}, {"after", rearm_counts}};
    detach_observers();

    // Keep the independent Core feature live during the measured switch. It
    // consumes its own FP16 surfaces and does not write to the Nexus output.
    auto core_input = gpu.Texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto core_depth = gpu.Texture(width, height, DXGI_FORMAT_R32_FLOAT);
    auto core_motion = gpu.Texture(width, height, DXGI_FORMAT_R16G16_FLOAT);
    gpu.Write(core_input.Get(), 0);
    gpu.Write(core_depth.Get(), 0);
    gpu.Write(core_motion.Get(), 0);
    for (auto* resource : {core_input.Get(), core_depth.Get(), core_motion.Get()}) {
      gpu.Transition(resource, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
    }
    auto output_desc = core_input->GetDesc();
    output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> core_output;
    Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &output_desc,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&core_output))),
          "independent Core output");
    native::NrFeatureSlot core_slot;
    core_slot.parameters = core_parameters;
    native::FinalResources resources;
    resources.proxy = core_input.Get();
    resources.nr_output = core_output.Get();
    const native::EvaluationContract frame{.motion = core_motion.Get(), .depth = core_depth.Get(), .input_width = width, .input_height = height, .output_width = width, .output_height = height, .motion_width = width, .motion_height = height, .motion_scale_x = 1.f, .motion_scale_y = 1.f, .frame_reset = 1};
    native::SetEvaluationParameters(contract, core_slot, 0, 1, resources, frame, width, height, width, height);
    const auto final_core_evaluate = NVSDK_NGX_D3D12_EvaluateFeature(gpu.list.Get(), core_model, core_parameters, nullptr);
    Check(NVSDK_NGX_SUCCEED(final_core_evaluate), "the actual original Core feature still evaluates after Nexus switches");
    submit();
    Check(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_ReleaseFeature(core_model)), "the original Core releases its own feature");
    Check(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_DestroyParameters(core_parameters)), "the original allocator destroys its own parameters");
    receipt["core_after_switch_evaluate"] = static_cast<uint32_t>(final_core_evaluate);
    receipt["core_calls_unwrapped"] = true;
    receipt["shared_core_still_evaluates_after_local_teardown"] = true;
    session.CloseSdk();
    write_receipt();
    std::cout << "PASS real proxy cold ordinary, XeFG and ordinary, exact-face rejection and local device re-arm\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL original-recording-face-proxy: " << error.what() << '\n';
    return 1;
  }
}
}  // namespace original_warm_proxy_cases
#endif
