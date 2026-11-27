#pragma once

#if defined(NR_NEXUS_TEST_NATIVE_NGX)
namespace present_original_handoff_cases {

namespace guard = renodx::addons::dlss5::detour_guard;

static native::DirectInitFn real_init = nullptr;
static native::DirectShutdownFn real_shutdown = nullptr;
static uint64_t real_inits = 0, real_shutdowns = 0;
static native::DirectCreateFn real_create = nullptr;
static native::DirectEvaluateFn real_evaluate = nullptr;
static ID3D12Device* expected_recording_face = nullptr;
static thread_local bool inside_nr = false;
static uint64_t real_creates = 0;
static uint64_t exact_create_lists = 0;
static uint64_t nr_native_heap_bindings = 0;
static uint64_t nr_bad_heap_bindings = 0;
static std::vector<int> model_resets;
using SetHeapsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
static SetHeapsFn real_set_heaps = nullptr;

using QueueWaitFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
static QueueWaitFn real_queue_wait = nullptr;
struct HeldQueueWait {
  ID3D12CommandQueue* queue = nullptr;
  ID3D12Fence* fence = nullptr;
  UINT64 value = 0;
  const std::atomic_uint64_t* assigned = nullptr;
  uint32_t observed = 0;
  bool before_submit_with_gate_held = false;
};
static HeldQueueWait held_queue_wait;

static HRESULT STDMETHODCALLTYPE ObserveHeldQueueWait(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
  const auto hr = real_queue_wait(queue, fence, value);
  auto& expected = held_queue_wait;
  if (SUCCEEDED(hr) && queue == expected.queue && fence == expected.fence && value == expected.value) {
    ++expected.observed;
    expected.before_submit_with_gate_held = expected.assigned->load() == 0 && fence->GetCompletedValue() < value;
  }
  return hr;
}

static NVSDK_NGX_Result NVSDK_CONV ObserveInit(unsigned long long app, const wchar_t* path, ID3D12Device* device,
    NVSDK_NGX_Version version, const NVSDK_NGX_Parameter* block) {
  ++real_inits;
  Check(device == expected_recording_face, "signed original Init receives the actual ReShade device face");
  return real_init(app, path, device, version, block);
}

static NVSDK_NGX_Result NVSDK_CONV ObserveShutdown(ID3D12Device* device) {
  ++real_shutdowns;
  return real_shutdown(device);
}

static NVSDK_NGX_Result NVSDK_CONV ObserveCreate(ID3D12GraphicsCommandList* list,
                                                 NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* block, NVSDK_NGX_Handle** handle) {
  ++real_creates;
  ComPtr<ID3D12Device> face;
  if (list != nullptr && SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&face))) && face.Get() == expected_recording_face) {
    ++exact_create_lists;
  }
  const bool previous = inside_nr;
  inside_nr = true;
  const auto result = real_create(list, feature, block, handle);
  inside_nr = previous;
  return result;
}

static NVSDK_NGX_Result NVSDK_CONV ObserveEvaluate(ID3D12GraphicsCommandList* list,
                                                   const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* block, PFN_NVSDK_NGX_ProgressCallback callback) {
  ComPtr<ID3D12Device> face;
  Check(SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&face))) && face.Get() == expected_recording_face,
        "actual Evaluate uses the initialized face");
  model_resets.push_back(native::GetInt(block, "DLSSNR.Reset", 0));
  native_runtime_evaluations.push_back({.model = handle, .list = list, .depth = native::GetD3D12Resource(block, "DLSSNR.Depth"), .motion = native::GetD3D12Resource(block, "DLSSNR.MVec"), .mask = native::GetD3D12Resource(block, "DLSSNR.ControlMask")});
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

inline int Run() {
  try {
    constexpr UINT width = 960, height = 540;
    constexpr UINT guide_width = 640, guide_height = 360;
    constexpr size_t passes = 2;
    const bool cold_epoch = GetEnvironmentVariableW(L"NR_NEXUS_TEST_LAD_COLD_EPOCH", nullptr, 0) != 0;
    wchar_t host_path[32768]{};
    Check(GetEnvironmentVariableW(L"NR_NEXUS_TEST_RESHADES_DLL", host_path, 32768) != 0,
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
    model_gpu = &gpu;
    ComPtr<ID3D12Device> native_device;
    Check(SUCCEEDED(gpu.device->QueryInterface(__uuidof(renodx::utils::directx::ReShadeRetrieveBaseInterface), &native_device))
              && native_device.Get() != gpu.device.Get(),
          "real ReShade device is not the native interface");
    expected_recording_face = gpu.device.Get();
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
    auto* queue = gpu.queue.Get();
    Check(renodx::utils::directx::NativeFromReShadeProxy(&queue) && queue != gpu.queue.Get(),
          "SDK submission queue has its genuine underlying native interface");
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
      Check(SUCCEEDED(gpu.list->Close()), "proxy fixture closes owned upload/consumer list");
      ID3D12CommandList* list = gpu.list.Get();
      renodx::utils::directx::NativeFromReShadeProxy(&list);
      queue->ExecuteCommandLists(1, &list);
      Check(SUCCEEDED(queue->Signal(gpu.fence.Get(), ++gpu.serial)), "proxy fixture native queue Signal");
      wait_fence(gpu.fence.Get(), gpu.serial);
      Check(SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr)),
            "proxy fixture only reuses a completed owned list");
      submission::OnCommandListReset(gpu.list.Get());
      native::PruneCompletedSubmissionUses();
      gpu.uploads.clear();
      gpu.retained_resources.clear();
    };
    // The production loader alone discovers the signed image. Never seed a
    // device/context or initialize an extra Core client in this fixture.
    native::PrimeNgxLoaderSymbols(gpu.device.Get());
    const auto& symbols = native::NgxLoaderView();
    const auto original_module = symbols.nr_module;
    Check(symbols.nr_ready && symbols.original_nr_exports && original_module != nullptr
              && !symbols.native_ready && native::direct_device == nullptr && native::original_nr_init_generation == 0,
          "discovery selects only the original image without activating an SDK context");
    real_init = symbols.init;
    real_shutdown = symbols.shutdown;
    real_create = symbols.create;
    real_evaluate = symbols.evaluate;
    real_set_heaps = reinterpret_cast<SetHeapsFn>((*reinterpret_cast<void***>(native_list))[28]);
    std::array<guard::Record, 5> records;
    const std::array hooks{
        guard::Hook{"signed original Init", reinterpret_cast<void**>(&real_init),
            reinterpret_cast<void*>(&ObserveInit), &records[0]},
        guard::Hook{"signed original Create", reinterpret_cast<void**>(&real_create),
            reinterpret_cast<void*>(&ObserveCreate), &records[1]},
        guard::Hook{"signed original Evaluate", reinterpret_cast<void**>(&real_evaluate),
            reinterpret_cast<void*>(&ObserveEvaluate), &records[2]},
        guard::Hook{"signed original Shutdown", reinterpret_cast<void**>(&real_shutdown),
            reinterpret_cast<void*>(&ObserveShutdown), &records[3]},
        guard::Hook{"native SetDescriptorHeaps", reinterpret_cast<void**>(&real_set_heaps),
            reinterpret_cast<void*>(&ObserveSetHeaps), &records[4]}};
    size_t failed_hooks = 0;
    Check(guard::AttachAll(hooks, &failed_hooks) == hooks.size(), "observers are attached to production calls");
    int observer_key = 0;
    const auto unhook = [&](int*) { for (const auto& hook : hooks) guard::Detach(hook); };
    const std::unique_ptr<int, decltype(unhook)> observers(&observer_key, unhook);
    const auto context_counts = [] {
      return std::array{real_inits, real_shutdowns, native::original_nr_init_generation};
    };
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
    Session session(&gpu, 0, 1);
    session.context->ui_composition = false;
    session.extent_left = session.extent_top = 0;
    session.extent_width = width;
    session.extent_height = height;
    Check(Hooks::Enable(session.handle, 0) == 0, "ordinary proxy path starts with XeFG genuinely Off");
    ComPtr<ID3D12Resource> color;
    Check(SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color))),
          "actual current SDK-proxy backbuffer");
    auto motion = gpu.Texture(guide_width, guide_height, DXGI_FORMAT_R16G16_FLOAT);
    auto depth = gpu.Texture(guide_width, guide_height, DXGI_FORMAT_R32_FLOAT);
    auto changed_motion = gpu.Texture(width, height, DXGI_FORMAT_R16G16_FLOAT);
    auto changed_depth = gpu.Texture(width, height, DXGI_FORMAT_R32_FLOAT);
    gpu.Write(motion.Get(), 0);
    gpu.Write(depth.Get(), 0);
    gpu.Write(changed_motion.Get(), 0);
    gpu.Write(changed_depth.Get(), 0);
    submit();
    const auto transfer = gpu.CreateTextureTransfer(color.Get());
    std::vector<uint8_t> consumed;
    uint64_t consumer_reads = 0;
    bool hold_old_service = false;
    nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* output) {
      Check(output == session.proxy.Get(), "first SDK consumer receives the same current output object");
      if (hold_old_service) return;  // Fixture only: leave the prior valid SDK service submission in flight.
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.RecordTextureReadback(color.Get(), transfer);
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
      submit();
      consumed = gpu.ReadTexture(transfer);
      ++consumer_reads;
    };
    uint32_t frame_id = 70000;
    uint8_t ordinary_output_key = 0;
    const auto ordinary_owner = reinterpret_cast<uintptr_t>(&ordinary_output_key);
    // The one-queue DXGI swapchain remains real for the ordinary/XeFG
    // roundtrip. The optional second queue exercises an equivalent
    // registered application-color surface, not an illegal write to a DXGI
    // buffer whose Present queue is the first one.
    auto next_color = gpu.Texture(width, height, color->GetDesc().Format);
    const auto color_identity = [](ID3D12Resource* buffer) {
      return reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(buffer));
    };
    const std::array<uintptr_t, 2> registered_buffers{{color_identity(color.Get()), color_identity(next_color.Get())}};
    renodx::addons::dlss5::output_framerate::RegisterPresentOutput(
        ordinary_owner, reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(gpu.device.Get())),
        adapter::Identity(session.proxy.Get()), registered_buffers);
    auto& ordinary = native::present_streams[&ordinary_output_key];
    ordinary.output = renodx::addons::dlss5::output_framerate::FindPresentOwner(0, registered_buffers[0]);
    Check(ordinary.output && ordinary.output.view == 0, "ordinary output has an exact registered backbuffer owner");
    native::FeatureState contract{.input_width = guide_width, .input_height = guide_height};
    contract.create_flags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    contract.has_motion_scale_x = contract.has_motion_scale_y = true;
    contract.motion_scale_x = 8.f;
    contract.motion_scale_y = -8.f;
    native::PresentTarget target{.output_state = D3D12_RESOURCE_STATE_COMMON,
                                 .workset_key = reinterpret_cast<const ID3D12Resource*>(&ordinary)};
    native::SnapshotPresentRestoreConfiguration(&target);
    target.domain = renodx::addons::dlss5::present_restore_scope::Domain::FINAL_PRESENT;
    target.swapchain_identity = ordinary_owner;
    target.swapchain_format = color->GetDesc().Format;
    target.guide_mode = native::kPresentGuidesOptional;
    native::PresentGuideSet ordinary_guides;
    target.diagnostic.bypass_host_look = true;
    target.pass_transfer_strength.fill(1.f);
    target.pass_color_strength.fill(1.f);
    const auto prepare_seed = [&](uint8_t value) {
      gpu.Write(color.Get(), value);
      submit();
      consumed.clear();
      ++frame_id;
    };
    const auto run_ordinary = [&](uint8_t value, bool tagged = true, ID3D12CommandQueue* secondary = nullptr,
                                  const std::function<void()>& queued = {}) {
      if (secondary == nullptr) prepare_seed(value);
      const auto before = native_runtime_evaluations.size();
      bool delivered = false;
      {
        const renodx::addons::dlss5::ngx_serial::PresentTurn turn;
        Check(turn.owns(), "ordinary proxy NGX turn");
        const renodx::addons::dlss5::RuntimeLock lock(native::runtime_mutex);
        ++native::present_generation;
        target.adaptive_output = renodx::addons::dlss5::output_framerate::MatchPresentOutput(ordinary_owner, color_identity(color.Get()));
        Check(native::EnsureOrdinaryPresentRing(color.Get(), gpu.device.Get())
                  && native::present_ring.device == gpu.device.Get(),
              "the ordinary Ring uses the live signed runtime's actual proxy face");
        ComPtr<ID3D12Device> ordinary_face;
        Check(SUCCEEDED(native::present_ring.slots[0].list->GetDevice(IID_PPV_ARGS(&ordinary_face)))
                  && ordinary_face.Get() == gpu.device.Get(),
              "the ordinary list was created on the same initialized recording face");
        const auto neutral = (!tagged ? native::SelectWarmPresentNeutralGuides(&ordinary, target,
            color_identity(color.Get()), ordinary_owner, width, height) : std::nullopt);
        Check(tagged || neutral.has_value(), "missing ordinary guides use the warmed low-resolution contract");
        target.warm_neutral_guides = neutral.has_value();
        if (tagged != ordinary.real_guides) {
          ordinary.real_guides = tagged;
          native::ResetPresentStreamHistory(&ordinary, native::kResetSourceChange);
        }
        auto& block = native::FillPresentBlock(&ordinary, color.Get(),
            tagged ? motion.Get() : ordinary.warm_neutral_guides.motion,
            tagged ? depth.Get() : ordinary.warm_neutral_guides.depth,
            tagged ? contract : *neutral, width, height, 1.f, 0);
        auto* slot = &native::present_ring.slots[(native::present_ring.submitted + 1) % native::kPresentRing];
        delivered = native::SubmitPresentGroup(secondary != nullptr ? secondary : queue, nullptr, slot, ordinary, block,
            tagged ? &ordinary_guides : nullptr, target, false, {});
        if (delivered) {
          Check(target.original_signed_runtime && native::direct_api.module == original_module
                    && native::WarmPresentBindingLive(gpu.device.Get()),
                "actual ordinary execution retained the same live original API");
        }
        target.warm_neutral_guides = false;
      }
      if (queued) queued();
      session.Present(frame_id);
      wait_fence(native::present_ring.fence, native::present_ring.submitted);
      const bool complete = delivered && native_runtime_evaluations.size() == before + passes;
      return complete;
    };
    const auto stream_key = std::pair{false, reinterpret_cast<uintptr_t>(session.handle)};
    const auto run_xefg = [&](uint8_t value, bool tagged = true, bool changed = false,
                               ID3D12Fence* hold = nullptr, bool keep_seed = false) {
      if (keep_seed) {
        ++frame_id;
        consumed.clear();
      } else {
        prepare_seed(value);
      }
      const auto before = native_runtime_evaluations.size();
      if (tagged) {
        session.extent_width = changed ? width : guide_width;
        session.extent_height = changed ? height : guide_height;
        session.Guides(frame_id, changed ? changed_depth.Get() : depth.Get(),
                       changed ? changed_motion.Get() : motion.Get());
        session.extent_width = width;
        session.extent_height = height;
      } else {
        const xefg::abi::Constants constants{.motion_scale_x = 8.f, .motion_scale_y = -8.f, .frame_time_ms = 16.5f};
        Check(Hooks::Constants(session.handle, frame_id, &constants) == sdk_status,
            "Optional missing guides retain this application's current constants");
      }
      if (hold != nullptr && !keep_seed) {
        Check(SUCCEEDED(queue->Wait(hold, 1)), "hold the outgoing exact XeFG submission at the GPU");
      }
      hold_old_service = hold != nullptr;
      session.Present(frame_id);
      hold_old_service = false;
      const auto it = native::fg_service::streams.find(stream_key);
      if (hold == nullptr && it != native::fg_service::streams.end()) wait_fence(it->second.fence, it->second.submitted);
      const bool complete = xefg::CurrentResult() == fg::Result::SUBMITTED && native_runtime_evaluations.size() == before + passes;
      return complete;
    };
    const auto edited = [&](uint8_t seed) {
      Check(consumed.size() == static_cast<size_t>(width) * height * 4, "first SDK entry reads a complete current frame");
      size_t changed = 0;
      for (size_t index = 0; index < consumed.size(); index += 4) {
        changed += consumed[index] != seed || consumed[index + 1] != seed || consumed[index + 2] != seed;
      }
      return changed;
    };
    const auto both_reset = [&] {
      return model_resets.size() >= 2 && model_resets.back() == 1 && model_resets[model_resets.size() - 2] == 1;
    };
    const auto stream_contract = [&] {
      return native::direct_api.module == original_module && native::WarmPresentBindingLive(gpu.device.Get())
          && native::direct_device == gpu.device.Get();
    };
    nlohmann::json receipt{{"scope", "NATIVE_GPU+NATIVE_NGX+MOCK_XeFG"},
                          {"native_face", reinterpret_cast<uintptr_t>(native_device.Get())},
                          {"proxy_face", reinterpret_cast<uintptr_t>(gpu.device.Get())},
                          {"original_module", reinterpret_cast<uintptr_t>(original_module)},
                          {"original_image_generation", symbols.nr_image_generation}};
    if (cold_epoch) {
      ComPtr<ID3D12Fence> gate;
      Check(SUCCEEDED(native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))),
            "cold epoch fixture has a real GPU gate");
      struct ReleaseGate {
        ID3D12Fence* fence;
        ~ReleaseGate() { fence->Signal(1); }
      } release_gate{gate.Get()};
      Check(Hooks::Enable(session.handle, 1) == 0 && real_creates == 0, "cold XeFG starts before ordinary has evaluated");
      const auto outgoing_epoch = session.context->epoch;
      Check(!run_xefg(119, true, false, gate.Get()) && xefg::CurrentResult() == fg::Result::PREPARED
                && real_creates == 1 && native_runtime_evaluations.empty(),
            "the outgoing epoch genuinely created only pass one, without touching output");
      const auto warm_context = context_counts();
      Check(warm_context == std::array<uint64_t, 3>{1, 0, 1}, "Service activated one private SDK lifetime");
      const auto& initial_feature = native::features.at(ordinary.Handle());
      const auto* continued_handle = initial_feature.slots[0].handle;
      const auto creation = initial_feature.slots[0].creation_recording;
      Check(continued_handle != nullptr && initial_feature.slots[1].handle == nullptr && !ordinary.warm_color.complete
                && creation != nullptr && creation->closed.load() && creation->submitted.load(),
            "one unpublished pass belongs to the stable ordinary output owner");
      const auto& outgoing = native::fg_service::streams.at(stream_key);
      const FenceRef outgoing_fence(outgoing.fence);
      const auto outgoing_value = outgoing.submitted;
      Check(outgoing_value != 0 && outgoing_fence->GetCompletedValue() < outgoing_value,
            "the first model's actual GPU work has not completed");
      Check(Hooks::Enable(session.handle, 0) == 0 && Hooks::Enable(session.handle, 1) == 0
                && session.context->epoch != outgoing_epoch,
            "actual Off/On changes only the SDK epoch before any ordinary frame");
      const auto next_epoch = session.context->epoch;
      Check(!run_xefg(119, true, false, gate.Get(), true), "no frame may publish while first model's Create is incomplete");
      std::cout << "cold_epoch_held creates=" << real_creates << " evals=" << native_runtime_evaluations.size()
                << " result=" << static_cast<int>(xefg::CurrentResult()) << " old_fence="
                << outgoing_fence->GetCompletedValue() << '/' << outgoing_value << std::endl;
      Check(real_creates == 1 && native_runtime_evaluations.empty() && xefg::CurrentResult() == fg::Result::NOT_SERVED
                && outgoing_fence->GetCompletedValue() < outgoing_value,
            "a new epoch cannot duplicate or use the still-in-flight first model");
      Check(SUCCEEDED(gate->Signal(1)), "release the old model Create only after checking the new epoch");
      wait_fence(outgoing_fence.fence, outgoing_value);
      const auto reads_before = consumer_reads;
      const bool submitted_preparation_color = run_xefg(137);
      std::cout << "cold_epoch_released creates=" << real_creates << " evals=" << native_runtime_evaluations.size()
                << " result=" << static_cast<int>(xefg::CurrentResult()) << std::endl;
      Check(!submitted_preparation_color && xefg::CurrentResult() == fg::Result::PREPARED
                && real_creates == passes && native_runtime_evaluations.empty() && edited(137) == 0
                && consumer_reads == reads_before + 1,
            "after the old fence completes, only pass two is prepared; the SDK still gets the current unedited frame");
      const auto& prepared_feature = native::features.at(ordinary.Handle());
      const auto* new_handle = prepared_feature.slots[1].handle;
      const auto second_creation = prepared_feature.slots[1].preparation_recording;
      Check(prepared_feature.slots[0].handle == continued_handle && new_handle != nullptr
                && second_creation != nullptr && second_creation->closed.load() && second_creation->submitted.load()
                && !ordinary.warm_color.complete,
            "the missing pass is created and submitted under the existing owner without publication");
      Check(run_xefg(149) && edited(149) > 0 && both_reset() && real_creates == passes
                && context_counts() == warm_context && consumer_reads == reads_before + 2,
            "the first mature frame delivers both evaluations and its own color to the SDK");
      const auto& mature_feature = native::features.at(ordinary.Handle());
      Check(mature_feature.slots[0].handle == continued_handle && mature_feature.slots[1].handle == new_handle
                && native_runtime_evaluations[0].model == continued_handle
                && native_runtime_evaluations[1].model == new_handle && ordinary.warm_color.complete,
            "the exact original model and the second model evaluate under a single owner");
      receipt["cold_epoch_handoff"] = {{"old_epoch", outgoing_epoch}, {"new_epoch", next_epoch}, {"initial_creates", 1},
          {"held_original_fence_value", outgoing_value}, {"creates_before_old_gpu_completed", 1},
          {"total_creates", real_creates}, {"first_pass_retained", true}, {"first_ready_frame", frame_id},
          {"first_mature_current_color", true}, {"evals_before_ready", 0}, {"create_second_published_color", false},
          {"both_recorded_models_at_first_sdk_read", true}};
    } else {
      Check(Hooks::Enable(session.handle, 1) == 0, "SDK really enables XeFG");
      bool warm = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !warm; ++attempt) {
        warm = run_xefg(153);
      }
      Check(warm && edited(153) > 0 && ordinary.warm_color.complete && stream_contract(),
            "XeFG establishes the compatible group via the actual signed provider");
      const auto warm_context = context_counts();
      const auto warmed_creates = real_creates;
      Check(warm_context == std::array<uint64_t, 3>{1, 0, 1} && warmed_creates == passes,
            "one real SDK Init and precisely two signed Create results warm the exact owner");
      const auto handles = [&] {
        const auto& slots = native::features.at(ordinary.Handle()).slots;
        return std::array{slots[0].handle, slots[1].handle};
      }();
      const auto with_same_models = [&](size_t before) {
        const auto& slots = native::features.at(ordinary.Handle()).slots;
        return native_runtime_evaluations.size() == before + passes
            && native_runtime_evaluations[before].model == handles[0]
            && native_runtime_evaluations[before + 1].model == handles[1]
            && slots[0].parameters != nullptr && slots[1].parameters != nullptr
            && slots[0].original_nr_init_generation == warm_context[2] && slots[1].original_nr_init_generation == warm_context[2];
      };
      const auto first_owner = ordinary.warm_color.owner;
      {
        // Keep one actual old XeFG Evaluate in flight on its exact queue, and
        // deliver the very first legal app frame through the other queue.
        ComPtr<ID3D12Fence> gate;
        ComPtr<ID3D12CommandQueue> secondary_proxy;
        const D3D12_COMMAND_QUEUE_DESC desc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
        Check(SUCCEEDED(native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))
                  && SUCCEEDED(gpu.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&secondary_proxy))),
              "create the actual second direct queue and fixture-owned GPU gate");
        auto* secondary = secondary_proxy.Get();
        Check(renodx::utils::directx::NativeFromReShadeProxy(&secondary) && secondary != queue
                  && native::InstallQueueCompletionHooks(secondary),
              "the second writer queue has its own real native backing");
        // Provision a real fence for this untouched fixture-owned queue.
        // Only the production Execute hook may assign and Signal its values.
        ComPtr<ID3D12Fence> incoming_fence;
        Check(SUCCEEDED(native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&incoming_fence))),
              "the fixture's second native queue has an actual tracking fence");
        {
          const std::unique_lock lock(renodx::addons::dlss5::queue_completion_mutex);
          auto& completion = renodx::addons::dlss5::queue_completions[secondary];
          Check(completion.fence == nullptr && completion.last_assigned.load() == 0,
                "fixture only supplies tracking for its untouched private queue");
          completion.fence = incoming_fence.Detach();
        }
        struct GateRelease {
          ID3D12Fence* fence;
          ~GateRelease() { fence->Signal(1); }
        } release{gate.Get()};
        // The simulated next buffer has real current pixels. Its upload uses
        // the old queue before the gate, never the incoming handoff queue.
        gpu.Write(next_color.Get(), 177);
        submit();
        Check(native::present_ring.submitted == 0, "this handoff is the first ordinary delivery after the two XeFG Creates");
        {
          const auto& slots = native::features.at(ordinary.Handle()).slots;
          const std::lock_guard lock(submission::tracker_mutex);
          for (const auto& pass : std::span{slots}.first(2)) {
            Check(pass.creation_recording != nullptr && pass.creation_recording->closed.load()
                      && pass.creation_recording->completed_healthy
                      && pass.creation_recording->completed_submission_count == 1,
                  "each initial Create completed on its own closed, once-submitted recording before the newer hold");
          }
        }
        Check(run_xefg(155, true, false, gate.Get()), "outgoing XeFG really records both warm models");
        const auto& old = native::fg_service::streams.at(stream_key);
        const uint64_t old_value = old.submitted;
        const FenceRef old_fence(old.fence);
        const auto held_slot = std::ranges::find_if(old.slots, [&](const auto& slot) { return slot.fence_value == old_value; });
        Check(held_slot != old.slots.end(), "identify the actual outgoing recording slot");
        const auto held = submission::RecordingIdentity(held_slot->list);
        submission::SubmissionFact expected_wait;
        {
          const std::lock_guard lock(submission::tracker_mutex);
          const auto& generation = submission::tracked_lists.at(held.first).generations.at(held.second);
          Check(generation.owner == submission::RecordingOwner::kXeFg
                    && generation.close_reason == submission::CloseReason::kOwnerDiscarded
                    && generation.first_execute != 0 && generation.submissions.size() == 1,
                "the exact previous XeFG recording was submitted once and cannot replay");
          expected_wait = submission::ReadSubmission(generation.submissions.front());
        }
        Check(expected_wait.state == submission::SubmissionProofState::kInFlight &&
                  old_fence->GetCompletedValue() < old_value,
              "the preceding accepted application's GPU use is still outstanding");
        Check(Hooks::Enable(session.handle, 0) == 0, "revoke the outgoing SDK write without waiting for completion");

        ++frame_id;
        consumed.clear();
        std::swap(color, next_color);
        FenceRef current_queue_fence;
        {
          const std::shared_lock lock(renodx::addons::dlss5::queue_completion_mutex);
          current_queue_fence = FenceRef(renodx::addons::dlss5::queue_completions.at(secondary).fence);
        }
        const submission::OutputUse output{.resource = reinterpret_cast<const void*>(color_identity(color.Get())),
            .subresource = 0, .version = frame_id};
        Check(!current_queue_fence.empty(), "the incoming queue's real tracking fence is registered");
        native::WarmPresentQueueWaits proofs;
        const bool can_order = native::ProveWarmPresentQueueOrder(ordinary, std::span{&output, 1}, {},
            current_queue_fence.fence, &proofs);
        const bool would_skip_wait = submission::OutputWriteOrdered(output, {}, current_queue_fence.fence);
        Check(would_skip_wait && can_order && proofs.size() == 1 && proofs.front().first.fence != current_queue_fence.fence
                  && proofs.front().first.fence == expected_wait.fence.fence && proofs.front().second == expected_wait.value
                  && proofs.front().first->GetCompletedValue() < proofs.front().second,
              "production finds one exact signaled non-replayable old queue dependency, not CPU completion");
        const auto writes = native::present_ring.submitted;
        const auto reads = consumer_reads;
        const auto evaluates_before_off = native_runtime_evaluations.size();
        const auto counts_before_wait = context_counts();
        {
          const std::shared_lock lock(renodx::addons::dlss5::queue_completion_mutex);
          held_queue_wait = {.queue = secondary, .fence = expected_wait.fence.fence, .value = expected_wait.value,
              .assigned = &renodx::addons::dlss5::queue_completions.at(secondary).last_assigned};
        }
        // ID3D12CommandQueueVtbl::Wait follows Signal at slot 15.
        real_queue_wait = reinterpret_cast<QueueWaitFn>((*reinterpret_cast<void***>(secondary))[15]);
        guard::Record wait_record;
        const std::array wait_hooks{guard::Hook{"exact incoming native queue Wait", reinterpret_cast<void**>(&real_queue_wait),
            reinterpret_cast<void*>(&ObserveHeldQueueWait), &wait_record}};
        size_t failed_wait_hooks = 0;
        Check(guard::AttachAll(wait_hooks, &failed_wait_hooks) == 1, "observe the real incoming queue GPU Wait");
        const auto unhook_wait = [&](int*) { guard::Detach(wait_hooks.front()); held_queue_wait = {}; };
        const std::unique_ptr<int, decltype(unhook_wait)> scoped_wait(&observer_key, unhook_wait);
        Check(run_ordinary(177, false, secondary, [&] {
          Check(old_fence->GetCompletedValue() < old_value
                    && held_queue_wait.observed == 1 && held_queue_wait.before_submit_with_gate_held,
                "production enqueues exactly one matching GPU Wait before Execute, while the outgoing gate still holds");
          Check(SUCCEEDED(gate->Signal(1)), "release the fixture's old work only after new production submit");
          const std::shared_lock lock(renodx::addons::dlss5::queue_completion_mutex);
          const auto& completion = renodx::addons::dlss5::queue_completions.at(secondary);
          Check(completion.last_assigned.load() != 0 && !completion.faulted,
                "the production recording carries a real Signal on the new writer queue");
          Check(SUCCEEDED(queue->Wait(native::present_ring.fence, native::present_ring.submitted)),
                "the mock consumer follows the actual new writer queue");
        }) && edited(177) > 0 && both_reset() && with_same_models(evaluates_before_off) && real_creates == warmed_creates && context_counts() == counts_before_wait
               && native::present_ring.submitted == writes + 1 && consumer_reads == reads + 1,
            "first new ordinary frame delivers exactly once while the old producer was still in flight");
        const auto first_new_pixels = consumed;
        gpu.Transition(next_color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.RecordTextureReadback(next_color.Get(), transfer);
        gpu.Transition(next_color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit();
        Check(first_new_pixels != gpu.ReadTexture(transfer), "the mock's first edited pixels are not the outgoing actual old frame");
        receipt["in_flight_different_queue"] = {{"dependency_count", proofs.size()},
            {"first_ordinary_use", true}, {"old_create_completed_before_held_evaluate", true},
            {"old_recording", {held.first, held.second}}, {"exact_tracker_signal_value", expected_wait.value},
            {"actual_gpu_waits_before_submit", held_queue_wait.observed},
            {"incoming_surface", "committed D3D12 texture registered by fixture; not a DXGI alternate queue"},
            {"current_frame", frame_id}, {"writes", native::present_ring.submitted - writes},
            {"mock_first_reads", consumer_reads - reads}, {"outgoing_pixels_not_replayed", true}};
        std::swap(color, next_color);
        Check(Hooks::Enable(session.handle, 1) == 0 && run_xefg(157) && edited(157) > 0 && real_creates == warmed_creates,
              "the same group remains available on the following hot On");
      }
      const auto before_off = native_runtime_evaluations.size();
      const auto old_pixels = consumed;
      Check(Hooks::Enable(session.handle, 0) == 0 && run_ordinary(83, false)
                && edited(83) > 0 && consumed != old_pixels && both_reset() && with_same_models(before_off),
            "first Off evaluates the same two models on its original pixels, without fresh guides");
      Check(ordinary.warm_color.owner == first_owner && ordinary.warm_neutral_guides.initialized
                && ordinary.warm_neutral_guides.motion->GetDesc().Width == guide_width
                && ordinary.warm_neutral_guides.depth->GetDesc().Height == guide_height
                && ordinary.warm_neutral_guides.inverted,
            "Optional neutral resources preserve the actual low-resolution model contract");
      const auto before_on = native_runtime_evaluations.size();
      const auto off_pixels = consumed;
      Check(Hooks::Enable(session.handle, 1) == 0 && run_xefg(149, false)
                && edited(149) > 0 && consumed != off_pixels && both_reset() && with_same_models(before_on),
            "first On yields complete current color to the mock SDK's first GPU read");
      const auto before_real_guides = native_runtime_evaluations.size();
      const auto synthetic_pixels = consumed;
      Check(run_xefg(153) && edited(153) > 0 && both_reset() && with_same_models(before_real_guides)
                && consumed != synthetic_pixels && real_creates == warmed_creates && context_counts() == warm_context,
            "real guides return with Reset without changing either model, module, or Init");
      receipt["hot_round_trip"] = {{"warm_init_counts", warm_context}, {"total_compatible_creates", real_creates},
          {"same_two_handles", {reinterpret_cast<uintptr_t>(handles[0]), reinterpret_cast<uintptr_t>(handles[1])}},
          {"same_registered_output_generation", first_owner.generation},
          {"optional_layout", {guide_width, guide_height, contract.create_flags}}, {"real_guides_return_reset", true}};
      Check(stream_contract() && real_creates == warmed_creates && context_counts() == warm_context,
            "all compatible transitions keep the actual original provider and both models");
      bool changed_ready = false;
      for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !changed_ready; ++attempt) {
        const auto seed = static_cast<uint8_t>(190 + attempt);
        changed_ready = run_xefg(seed, true, true);
        if (changed_ready) Check(edited(seed) > 0, "changed contract publishes current color once legal");
      }
      Check(changed_ready && real_creates == warmed_creates + passes, "genuine grid and MVLowRes change still rebuilds");
      receipt["true_contract_change_new_creates"] = real_creates - warmed_creates;
    }
    Check(std::ranges::all_of(native_runtime_evaluation_results, [](auto result) { return NVSDK_NGX_SUCCEED(result); })
              && real_inits == 1 && real_shutdowns == 0 && exact_create_lists == real_creates
              && nr_native_heap_bindings > 0 && nr_bad_heap_bindings == 0 && stream_contract(),
          "successful signed calls for each entry use the same list interface and actual original provider");
    Check(!native::NgxLoaderView().native_ready && !native::direct_api.private_instance,
          "both handoff directions use the production original image without loading a private copy");
    receipt["actual_init"] = real_inits;
    receipt["actual_create"] = real_creates;
    receipt["actual_evaluate"] = native_runtime_evaluations.size();
    receipt["native_heap_bindings"] = nr_native_heap_bindings;
    receipt["mock_first_sdk_gpu_reads"] = consumer_reads;
    receipt["sdk_internal_first_read"] = "not measured by mock SDK";
    std::ofstream result(cold_epoch ? "present-original-handoff-cold.json" : "present-original-handoff.json");
    result << receipt.dump(2) << '\n';
    Check(result.good(), "write the exact measured receipt");
    std::cout << "PASS present-original-handoff " << (cold_epoch ? "cold" : "hot")
              << " NATIVE_GPU+NATIVE_NGX+MOCK_XeFG" << std::endl;
    return 0;
  } catch (const std::exception& problem) {
    std::cerr << "FAIL present-original-handoff: " << problem.what() << '\n';
    return 1;
  }
}

}  // namespace present_original_handoff_cases
#endif
