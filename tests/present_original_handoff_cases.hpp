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
static reshade::api::effect_runtime* observed_effects_runtime = nullptr;
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

inline int Run(const char* project_id_path, const char* variant) {
  try {
    constexpr UINT width = 960, height = 540;
    constexpr UINT guide_width = 640, guide_height = 360;
    constexpr size_t passes = 2;
    std::string_view case_name;
    if (variant != nullptr) {
      case_name = variant;
      Check(case_name == "hot" || case_name == "cold" || case_name == "core-live" || case_name == "core-idle",
            "present-original-handoff variant must be hot, cold, core-live or core-idle");
    } else {
      const bool legacy_cold = GetEnvironmentVariableW(L"NR_NEXUS_TEST_LAD_COLD_EPOCH", nullptr, 0) != 0;
      wchar_t core_first_mode[16]{};
      GetEnvironmentVariableW(L"NR_NEXUS_TEST_LAD_CORE_FIRST", core_first_mode, std::size(core_first_mode));
      const std::wstring_view core_mode(core_first_mode);
      Check(core_mode.empty() || (!legacy_cold && (core_mode == L"live" || core_mode == L"idle")),
            "Core-first requires live or idle and a fresh process without the cold-epoch switch");
      case_name = (core_mode.empty() ? (legacy_cold ? "cold" : "hot")
                                    : (core_mode == L"idle" ? "core-idle" : "core-live"));
    }
    const bool cold_epoch = case_name == "cold";
    const bool idle_core = case_name == "core-idle";
    const bool core_first = idle_core || case_name == "core-live";
    std::cout << "START present-original-handoff " << case_name << " pid=" << GetCurrentProcessId() << std::endl;
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
    native::PrimeNgxLoaderSymbols(gpu.device.Get());
    if (core_first) {
      std::ifstream project_file(project_id_path);
      std::string project_id;
      Check(project_file && std::getline(project_file, project_id) && project_id.size() == 36,
            "the Core-first run needs the existing NGX ProjectID");
      const auto directory = native::AddonDirectory().wstring();
      const wchar_t* paths[]{directory.c_str()};
      NVSDK_NGX_FeatureCommonInfo common{};
      common.PathListInfo = {paths, 1};
      common.LoggingInfo = {control_mask_gpu_cases::NativeNgxSdkLog, NVSDK_NGX_LOGGING_LEVEL_ON, true};
      const auto result = NVSDK_NGX_D3D12_Init_with_ProjectID(project_id.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM,
          "NR-Nexus-present-core-first", directory.c_str(), gpu.device.Get(), &common);
      Check(NVSDK_NGX_SUCCEED(result), "public NGX Core initializes on the actual ordinary recording device");
      native::PrimeNgxLoaderSymbols(gpu.device.Get());
      Check(native::NgxLoaderView().core_ready, "the production loader found the real Core export family");
    }
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
    xefg::Configure(&native::fg_service::Service, &native::fg_service::PrepareCapture,
                    &native::fg_service::Retire, &native::fg_service::Requested,
                    case_name == "hot", true, &native::fg_service::PrepareRuntime);
    const auto unregister_addon = reinterpret_cast<void (*)(void*)>(GetProcAddress(host, "ReShadeUnregisterAddon"));
    std::unique_ptr<void, decltype(unregister_addon)> real_addon(nullptr, unregister_addon);
    if (case_name == "hot") {
      const auto register_addon = reinterpret_cast<bool (*)(void*, uint32_t)>(GetProcAddress(host, "ReShadeRegisterAddon"));
      const auto register_event = reinterpret_cast<void (*)(void*, reshade::addon_event, void*)>(
          GetProcAddress(host, "ReShadeRegisterEventForAddon"));
      const auto module = GetModuleHandleW(nullptr);
      Check(register_addon != nullptr && unregister_addon != nullptr && register_event != nullptr
                && register_addon(module, RESHADE_API_VERSION),
            "observe the actual host's effect runtime initialization");
      real_addon.reset(module);
      register_event(module, reshade::addon_event::init_effect_runtime, reinterpret_cast<void*>(
          +[](reshade::api::effect_runtime* runtime) { observed_effects_runtime = runtime; }));
    }
    Session session(&gpu, 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, core_first ? queue : nullptr);
    real_addon.reset();
    Check(case_name != "hot" || (session.context->omit_optional_hudless && session.context->init.ui_mode == adapter::kUiModeAuto),
          "production SDK initialization latches the single-backbuffer AUTO policy for hot handoff");
    if (!cold_epoch) {
      // The SDK substitute returns the real ReShade proxy through the public boundary.
      // Its host supplies the actual Present methods and their device identity.
      adapter::modules[0].module = host;
      adapter::modules[0].real[2] = reinterpret_cast<void*>(&FakeReceiptGetSwapchain);
      receipt_proxy = session.proxy.Get();
      ComPtr<IDXGISwapChain3> returned_proxy;
      const auto status = Hooks::GetSwapchain(session.handle, IID_PPV_ARGS(&returned_proxy));
      receipt_proxy = nullptr;
      UINT source_width = 0, source_height = 0;
      Check(status == 0 && returned_proxy.Get() == session.proxy.Get()
                && session.context->bridge_context_ready && session.context->bridge_proxy_ready
                && SUCCEEDED(returned_proxy->GetSourceSize(&source_width, &source_height))
                && source_width == width && source_height == height,
            "SDK GetSwapchain observes the exact proxy, D3D12 device and full source region");
    }
    const auto proxy_originals = adapter::proxies[0].real;
    const auto restore_proxy = [&](auto*) { adapter::proxies[0].real = proxy_originals; };
    const std::unique_ptr<adapter::ProxySlot, decltype(restore_proxy)> proxy_restore(&adapter::proxies[0], restore_proxy);
    adapter::proxies[0].real[0] = reinterpret_cast<void*>(&nr_edit_reuse_xefg_cases::ConsumeSdkPresent);
    adapter::proxies[0].real[1] = reinterpret_cast<void*>(&FakePresent1);
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
    std::function<void()> observe_first_consumer;
    std::function<void()> before_first_consumer;
    nr_edit_reuse_xefg_cases::consume_sdk_present = [&](IDXGISwapChain* output) {
      Check(output == session.proxy.Get(), "first SDK consumer receives the same current output object");
      if (hold_old_service) return;  // Fixture only: leave the prior valid SDK service submission in flight.
      if (before_first_consumer) before_first_consumer();
      if (observe_first_consumer) observe_first_consumer();
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
      gpu.RecordTextureReadback(color.Get(), transfer);
      gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
      submit();
      consumed = gpu.ReadTexture(transfer);
      ++consumer_reads;
    };
    uint32_t frame_id = 70000;
    uint8_t ordinary_output_key = 0;
    std::optional<PresentRingFaceProbe> route;
    const void* ordinary_key = &ordinary_output_key;
    if (core_first || !cold_epoch) {
      route.emplace(&gpu, color.Get(), session.proxy.Get(), PresentRingFaceProbe::Mode::NATIVE_ROUTE);
      native::present_guides = native::kPresentGuidesOptional;
      native::present_pre_fg = 1;
      ordinary_key = &*route;
      native::ConfigureOrdinaryPresentOwner();
    }
    const auto ordinary_owner = reinterpret_cast<uintptr_t>(ordinary_key);
    // The one-queue DXGI swapchain remains real for the ordinary/XeFG
    // roundtrip. The optional second queue exercises an equivalent
    // registered application-color surface, not an illegal write to a DXGI
    // buffer whose Present queue is the first one.
    auto next_color = gpu.Texture(width, height, color->GetDesc().Format);
    const auto color_identity = [](ID3D12Resource* buffer) {
      return reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(buffer));
    };
    // ReShade's event supplies the resource face, while XeFG matches the canonical backbuffer.
    const std::array<uintptr_t, 4> registered_buffers{color_identity(color.Get()), color_identity(next_color.Get()),
        reinterpret_cast<uintptr_t>(color.Get()), reinterpret_cast<uintptr_t>(next_color.Get())};
    renodx::addons::dlss5::output_framerate::RegisterPresentOutput(
        ordinary_owner, reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(gpu.device.Get())),
        adapter::Identity(session.proxy.Get()), registered_buffers);
    auto& ordinary = native::present_streams[ordinary_key];
    ordinary.output = renodx::addons::dlss5::output_framerate::FindPresentOwner(0, registered_buffers[0]);
    Check(ordinary.output && ordinary.output.view == 0
              && renodx::addons::dlss5::output_framerate::MatchPresentOutput(ordinary_owner, registered_buffers[2]),
          "ordinary output is registered under both the event resource face and canonical backbuffer");
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
                                  const std::function<void()>& queued = {}, bool prepared_seed = false) {
      if (secondary == nullptr && !prepared_seed) prepare_seed(value);
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
        if (delivered && !core_first) {
          Check(target.original_signed_runtime && native::direct_api.module == original_module
                    && native::WarmPresentBindingLive(gpu.device.Get()),
                "actual ordinary execution retained the same live original API");
        }
        target.warm_neutral_guides = false;
      }
      if (queued) queued();
      if (!prepared_seed) session.Present(frame_id);
      wait_fence(native::present_ring.fence, native::present_ring.submitted);
      const bool complete = delivered && (core_first || native_runtime_evaluations.size() == before + passes);
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
    const auto model_counts = [&] { return std::array{real_inits, real_shutdowns, real_creates}; };
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
                          {"variant", case_name},
                          {"process_id", GetCurrentProcessId()},
                          {"native_face", reinterpret_cast<uintptr_t>(native_device.Get())},
                          {"proxy_face", reinterpret_cast<uintptr_t>(gpu.device.Get())},
                          {"original_module", reinterpret_cast<uintptr_t>(original_module)},
                          {"original_image_generation", symbols.nr_image_generation},
                          {"runtime_path", symbols.nr_loaded_path.string()}, {"runtime_sha256", symbols.nr_sha256},
                          {"runtime_version", symbols.nr_requirements.file_version},
                          {"driver_x100", symbols.nr_requirements.driver_x100},
                          {"reshade_path", std::filesystem::path(host_path).string()},
                          {"reshade_sha256", native::ComputeRuntimeSha256(host_path)}};
    if (core_first) {
      Check(session.context->device.Get() == native_device.Get() && session.context->proxy_recording_device == nullptr,
            "early SDK calls have the same native device with no ReShade recording interface");
      bool ordinary_ready = false;
      for (uint8_t seed = 48; seed < 64 && !ordinary_ready; ++seed) {
        prepare_seed(seed);
        target.ordinary_attempt = native::BeginOrdinaryPresentAttempt(&route->queue, &*route);
        Check(static_cast<bool>(target.ordinary_attempt), "ordinary Core starts a real pre-effects output attempt");
        ordinary_ready = run_ordinary(seed, true, nullptr, {}, true);
        session.Present(frame_id);
        native::EndOrdinaryPresentAttempt(target.ordinary_attempt);
        if (ordinary_ready) Check(edited(seed) > 0, "ordinary Core double Pass reaches the first consumer");
      }
      const auto& core_slots = native::features.at(ordinary.Handle()).slots;
      Check(ordinary_ready && native::PresentRingComplete() && ordinary.warm_color.complete
                && ordinary.warm_color.real_guides && ordinary.warm_color.guide_width == guide_width
                && ordinary.warm_color.guide_height == guide_height && ordinary.warm_color.flags == contract.create_flags
                && std::ranges::all_of(std::span{core_slots}.first(passes), [](const auto& slot) {
                     return slot.handle != nullptr && slot.parameters != nullptr && slot.via_core && slot.creation_completed;
                   }),
            "ordinary SubmitPresentGroup establishes a completed same-output Core pair with a real low-resolution layout");
      if (idle_core) {
        native::PresentStream active;
        auto active_color = gpu.Texture(width, height, color->GetDesc().Format, NATIVE_INPUT_STATE);
        auto active_target = target;
        active_target.domain = renodx::addons::dlss5::present_restore_scope::Domain::OTHER;
        active_target.swapchain_identity = 0;
        active_target.adaptive_output = {};
        active_target.output_state = NATIVE_INPUT_STATE;
        active_target.workset_key = reinterpret_cast<const ID3D12Resource*>(&active);
        Sleep(static_cast<DWORD>(native::kWorksetIdleRetireNs / 1'000'000 + 100));
        {
          const renodx::addons::dlss5::ngx_serial::PresentTurn turn;
          Check(turn.owns(), "the idle control owns the NGX turn");
          const renodx::addons::dlss5::RuntimeLock lock(native::runtime_mutex);
          ++native::present_generation;
          auto& block = native::FillPresentBlock(&active, active_color.Get(), motion.Get(), depth.Get(),
                                                contract, width, height, 1.f, 0);
          native::NrRecording recording;
          native::ProcessInline(gpu.list.Get(), active.Handle(), &block, nullptr, &active_target, &recording);
          Check(native::worksets.contains({active.Handle(), active_target.workset_key}),
                "the idle sweep has another actual recent workload");
        }
        submit();
        {
          const renodx::addons::dlss5::RuntimeLock lock(native::runtime_mutex);
          native::RetireSupersededWorksets(native::SteadyNowNs());
          native::DrainRetiredResources(true);
        }
        Check(native::features.contains(ordinary.Handle())
                  && std::ranges::all_of(std::span{core_slots}.first(passes), [](const auto& slot) {
                       return slot.handle == nullptr && slot.parameters == nullptr;
                     }),
              "actual idle retirement removes the Core models while leaving the physical owner key");
      }
      const auto before_xefg = context_counts();
      const auto creates_before_xefg = real_creates;
      const auto& ordinary_face = ordinary.ordinary_recording_face;
      const bool ordinary_proxy_face = ordinary_face.device.Get() == gpu.device.Get();
      const bool ordinary_face_completed = ordinary_face.fence.fence != nullptr
          && ordinary_face.fence_value != 0
          && ordinary_face.fence->GetCompletedValue() != UINT64_MAX
          && ordinary_face.fence->GetCompletedValue() >= ordinary_face.fence_value;
      Check(Hooks::Enable(session.handle, 1) == 0, "first actual SDK Enable after Core delivery");
      const auto evaluates_before_start = native_runtime_evaluations.size();
      const bool early_delivered = run_xefg(71, false);
      const auto startup = xefg::CurrentResult();
      const bool startup_without_color = !early_delivered && edited(71) == 0
          && native_runtime_evaluations.size() == evaluates_before_start;
      const auto service = native::fg_service::streams.find(stream_key);
      const auto prepare_fence = (service != native::fg_service::streams.end() ? service->second.submitted : 0);
      const bool preparation_completed = prepare_fence != 0 && service->second.fence != nullptr
          && service->second.fence->GetCompletedValue() >= prepare_fence
          && service->second.fence->GetCompletedValue() != UINT64_MAX;
      std::cout << "CORE_FIRST_START mode=" << (idle_core ? "idle" : "live")
                << " outcome=" << static_cast<int>(startup) << " prepare_fence=" << prepare_fence
                << " creates=" << (real_creates - creates_before_xefg)
                << " consumer_unedited=" << startup_without_color
                << " ordinary_proxy_face=" << ordinary_proxy_face
                << " ordinary_face_completed=" << ordinary_face_completed << std::endl;

      Check(Hooks::Enable(session.handle, 0) == 0, "SDK Disable while the direct pair is still preparing");
      const auto creates_before_off = real_creates;
      prepare_seed(72);
      Check(adapter::ProxyHooks<0>::Present(session.proxy.Get(), 0, 0) == S_OK && edited(72) == 0
                && real_creates == creates_before_off && native_runtime_evaluations.size() == evaluates_before_start,
            "a cold Off call without a pre-effects writer reads current original without preparing or editing color");
      Check(Hooks::Enable(session.handle, 1) == 0, "SDK Enable resumes the same physical output");
      prepare_seed(73);
      const DXGI_PRESENT_PARAMETERS full_present{};
      Check(!session.context->fresh_id && adapter::ProxyHooks<0>::Present1(session.proxy.Get(), 0, 0, &full_present) == S_OK,
            "the first re-enabled application Present1 has no new SDK ID or recording face");
      const auto restart_evaluates = native_runtime_evaluations.size() - evaluates_before_start;
      Check(restart_evaluates == 0 ? edited(73) == 0 : (restart_evaluates == passes && edited(73) > 0),
            "the cold On first read contains either the full direct pair or untouched current color");

      // Supply a genuinely recorded SDK tag on the real proxy face. This keeps
      // the owner-selection red observable even if the early face was refused.
      bool first_ready = false;
      uint8_t first_ready_seed = 79;
      for (; first_ready_seed < 94; ++first_ready_seed) {
        first_ready = run_xefg(first_ready_seed);
        if (first_ready) break;
      }
      const auto owner = native::features.find(ordinary.Handle());
      const bool stable_direct_owner = owner != native::features.end()
          && std::ranges::all_of(std::span{owner->second.slots}.first(passes), [](const auto& slot) {
               return slot.handle != nullptr && slot.parameters != nullptr && !slot.via_core
                   && slot.creation_completed && native::WarmPresentModelOnBinding(slot);
             });
      std::cout << "CORE_FIRST_OWNER mode=" << (idle_core ? "idle" : "live")
                << " ready=" << first_ready << " stable_direct_owner=" << stable_direct_owner
                << " creates=" << (real_creates - creates_before_xefg) << std::endl;
      Check(first_ready && edited(first_ready_seed) > 0 && stable_direct_owner,
            "first real-guide full delivery uses two mature direct models at the existing physical owner key");
      Check(startup == fg::Result::PREPARED && startup_without_color && preparation_completed,
            "first no-SDK-face frame submits preparation using proven ordinary recording and keeps current color");
      Check(real_creates == creates_before_xefg + passes && context_counts() == before_xefg && stream_contract(),
            "the whole startup window pays for one direct pair with no extra Init or Shutdown");
      receipt["core_first"] = {{"initial_slot_state", (idle_core ? "cleaned-slots" : "live-core-slots")},
          {"prepare_fence_value", prepare_fence}, {"preparation_published_color", false},
          {"cold_restart_evaluates", restart_evaluates},
          {"startup_create_delta", real_creates - creates_before_xefg}, {"first_ready_frame", frame_id}};
      const std::array models{owner->second.slots[0].handle, owner->second.slots[1].handle};
      const auto mature_counts = model_counts();
      receipt["mature_frames"] = nlohmann::json::array();
      const auto mature_present = [&](const char* name, uint8_t seed, bool use_present1, bool tagged,
                                       bool game_reset, bool expected_reset, bool off, bool ordinary_precedes,
                                       bool allow_ordinary, bool expect_write) {
        prepare_seed(seed);
        Check(!session.context->fresh_id, "every application call begins with fresh state from the prior outer return");
        if (tagged) {
          session.extent_width = guide_width;
          session.extent_height = guide_height;
          session.Tag(frame_id, DEPTH, depth.Get());
          session.Tag(frame_id, MOTION, motion.Get());
          const xefg::abi::Constants constants{.jitter_x = .25f, .jitter_y = -.5f,
              .motion_scale_x = 8.f, .motion_scale_y = -8.f, .reset = static_cast<uint32_t>(game_reset),
              .frame_time_ms = 16.5f};
          Check(Hooks::Constants(session.handle, frame_id, &constants) == sdk_status
                    && Hooks::PresentId(session.handle, frame_id) == sdk_status,
                "current real tags and game Reset reach the SDK through their public hooks");
          session.extent_width = width;
          session.extent_height = height;
        }
        const auto evaluations = native_runtime_evaluations.size();
        const auto reads = consumer_reads;
        const auto ordinary_writes = native::present_ring.submitted;
        const auto application_frame = session.context->application_frame;
        fg::xefg::OrdinaryPresentAttempt attempt;
        const auto enter_pre_effects = [&] {
          attempt = native::BeginOrdinaryPresentAttempt(&route->queue, &*route);
          if (!attempt) {
            const auto* turn = xefg::CurrentOrdinaryOffPresent();
            const auto output = renodx::addons::dlss5::output_framerate::MatchPresentOutput(
                ordinary_owner, route->get_current_back_buffer().handle);
            const auto access = xefg::QueryOrdinaryPresent(adapter::Identity(session.proxy.Get()), color_identity(color.Get()), output);
            std::cout << "CORE_FIRST_OFF_INPUT " << nlohmann::json{
                {"owner", ordinary_owner}, {"backbuffer", color_identity(color.Get())}, {"output_generation", output.generation},
                {"callback_call", access.call}, {"sdk_call", turn != nullptr ? turn->access.call : 0},
                {"sdk_output_generation", turn != nullptr ? turn->access.output.generation : 0},
                {"sdk_physical_owner", turn != nullptr ? turn->physical_output.swapchain : 0},
                {"sdk_rejection", turn != nullptr && turn->input_rejection != nullptr ? turn->input_rejection : "none"},
                {"sdk_queue_matches", turn != nullptr && renodx::utils::directx::SameNativeObject(turn->queue, queue)},
                {"sdk_proxy_ready", session.context->bridge_proxy_ready}}.dump() << std::endl;
          }
          Check(static_cast<bool>(attempt), "ordinary Present reaches the pre-effects event with this frame's output");
          native::present_sdr_white_polled_ns = native::SteadyNowNs();
          native::RunPresentPath(&route->queue, &*route);
        };
        if (allow_ordinary) {
          if (ordinary_precedes) {
            enter_pre_effects();
          } else {
            before_first_consumer = enter_pre_effects;
          }
        }
        nlohmann::json first_write;
        observe_first_consumer = [&] {
          Check(consumer_reads == reads, "the original callback has exactly one first GPU consumer");
          Check(native_runtime_evaluations.size() == evaluations + (expect_write ? passes : 0),
                "both live Passes complete recording before the original callback's first GPU read");
          if (!expect_write) return;
          const auto& first = native_runtime_evaluations[evaluations];
          const auto& second = native_runtime_evaluations[evaluations + 1];
          Check(first.model == models[0] && second.model == models[1] && first.list == second.list
                    && NVSDK_NGX_SUCCEED(native_runtime_evaluation_results[evaluations])
                    && NVSDK_NGX_SUCCEED(native_runtime_evaluation_results[evaluations + 1]),
                "the same successful direct model pair records on one list before the first GPU read");
          const auto recording = submission::RecordingIdentity(first.list);
          const std::lock_guard lock(submission::tracker_mutex);
          const auto& generation = submission::tracked_lists.at(recording.first).generations.at(recording.second);
          const auto current = color_identity(color.Get());
          const auto output_use = std::ranges::find_if(generation.classified_uses, [=](const auto& use) {
            return use.kind == submission::UseKind::kOutputColor && use.output.subresource == 0
                && reinterpret_cast<uintptr_t>(use.output.resource) == current;
          });
          Check(generation.first_execute != 0 && generation.submissions.size() == 1
                    && generation.unreserved_submissions == 0 && output_use != generation.classified_uses.end(),
                "the two Pass recording has one actual Execute writing the current BB before its first read");
          const auto signal = submission::ReadSubmission(generation.submissions.front());
          Check(signal.value != 0 && signal.fence.fence != nullptr
                    && (signal.state == submission::SubmissionProofState::kInFlight
                        || signal.state == submission::SubmissionProofState::kComplete),
                "the unique GPU writer carries its own healthy queue Signal");
          first_write = {{"first_execute", generation.first_execute}, {"fence_value", signal.value},
              {"recording", {recording.first, recording.second}}, {"backbuffer", current}};
        };
        std::cout << "CORE_FIRST_MATURE " << name << " seed=" << static_cast<uint32_t>(seed) << std::endl;
        const DXGI_PRESENT_PARAMETERS parameters{};
        const HRESULT status = (use_present1 ? adapter::ProxyHooks<0>::Present1(session.proxy.Get(), 0, 0, &parameters)
                                             : adapter::ProxyHooks<0>::Present(session.proxy.Get(), 0, 0));
        before_first_consumer = {};
        observe_first_consumer = {};
        if (attempt) native::EndOrdinaryPresentAttempt(attempt);
        Check(status == S_OK && consumer_reads == reads + 1 && session.context->application_frame == application_frame + 1,
              "outer Present forwards exactly one original return and one application-frame consumer");
        Check(model_counts() == mature_counts && native::present_ring.submitted == ordinary_writes + (off && expect_write ? 1 : 0),
              "mature transitions neither recreate models nor submit duplicate ordinary writes");
        Check(expect_write ? edited(seed) > 0 : edited(seed) == 0,
              "the first GPU image contains the expected edit or original of this application frame");
        for (size_t offset = 3; offset < consumed.size(); offset += 4) {
          Check(consumed[offset] == seed, "the unique current-frame alpha marker survives the first read");
        }
        if (expected_reset) Check(both_reset(), "both current Passes receive the source-change or game Reset");
        if (expect_write && tagged) {
          const auto& first = native_runtime_evaluations[evaluations];
          Check(first.depth != nullptr && first.motion != nullptr && first.motion != ordinary.warm_neutral_guides.motion,
                "true guide recovery selects real motion and depth on its first delivered frame");
        }
        receipt["mature_frames"].push_back({{"phase", name}, {"application_frame", application_frame + 1},
            {"marker", seed}, {"present1", use_present1}, {"first_output_write", first_write}, {"sdk_return", status},
            {"first_reads", consumer_reads - reads}, {"real_guides", tagged}, {"game_reset", game_reset}});
      };
      mature_present("first missing guides while enabled", 101, false, false, false, true, false, false, false, true);
      Check(Hooks::Enable(session.handle, 0) == 0, "SDK Disable before the first menu frame");
      mature_present("first Off enters pre-effects inside Present", 103, true, false, false, true, true, false, true, true);
      mature_present("ordinary write precedes outer Present once", 107, false, false, false, false, true, true, true, true);
      mature_present("no valid pre-effects ordinary input", 109, true, false, false, false, true, false, false, false);
      Check(Hooks::Enable(session.handle, 1) == 0, "SDK Enable before the first untagged application frame");
      mature_present("first On without a new SDK ID", 113, true, false, false, true, false, false, false, true);
      mature_present("first real-guide recovery", 127, false, true, false, true, false, false, false, true);
      mature_present("game Reset with real guides", 131, true, true, true, true, false, false, false, true);
    } else if (cold_epoch) {
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
                && real_creates == passes && native_runtime_evaluations.empty(),
            "the outgoing epoch records preparation of both passes without touching output");
      const auto warm_context = context_counts();
      Check(warm_context == std::array<uint64_t, 3>{1, 0, 1}, "Service activated one original SDK lifetime");
      const auto& initial_feature = native::features.at(ordinary.Handle());
      const std::array models{initial_feature.slots[0].handle, initial_feature.slots[1].handle};
      const auto creation = initial_feature.slots[0].preparation_recording;
      Check(models[0] != nullptr && models[1] != nullptr && models[0] != models[1] && !ordinary.warm_color.complete
                && creation != nullptr && creation == initial_feature.slots[1].preparation_recording
                && creation->closed.load() && creation->submitted.load(),
            "both unpublished passes belong to one stable owner and one submitted preparation recording");
      const auto& outgoing = native::fg_service::streams.at(stream_key);
      const FenceRef outgoing_fence(outgoing.fence);
      const auto outgoing_value = outgoing.submitted;
      Check(outgoing_value != 0 && outgoing_fence->GetCompletedValue() < outgoing_value,
            "the two models' actual GPU preparation has not completed");
      Check(Hooks::Enable(session.handle, 0) == 0 && Hooks::Enable(session.handle, 1) == 0
                && session.context->epoch != outgoing_epoch,
            "actual Off/On changes only the SDK epoch before any ordinary frame");
      const auto next_epoch = session.context->epoch;
      Check(!run_xefg(119, true, false, gate.Get(), true), "no frame may publish while the pair's preparation is incomplete");
      std::cout << "cold_epoch_held creates=" << real_creates << " evals=" << native_runtime_evaluations.size()
                << " result=" << static_cast<int>(xefg::CurrentResult()) << " old_fence="
                << outgoing_fence->GetCompletedValue() << '/' << outgoing_value << std::endl;
      Check(real_creates == passes && native_runtime_evaluations.empty() && xefg::CurrentResult() != fg::Result::SUBMITTED
                && !ordinary.warm_color.complete && outgoing_fence->GetCompletedValue() < outgoing_value,
            "a new epoch cannot duplicate, mature or use the still-in-flight pair");
      Check(SUCCEEDED(gate->Signal(1)), "release the old model preparation only after checking the new epoch");
      wait_fence(outgoing_fence.fence, outgoing_value);
      const auto reads_before = consumer_reads;
      const bool first_ready = run_xefg(137);
      std::cout << "cold_epoch_released creates=" << real_creates << " evals=" << native_runtime_evaluations.size()
                << " result=" << static_cast<int>(xefg::CurrentResult()) << std::endl;
      Check(first_ready && edited(137) > 0 && both_reset() && real_creates == passes
                && context_counts() == warm_context && consumer_reads == reads_before + 1,
            "the first mature frame delivers both evaluations and its own color to the SDK");
      const auto& mature_feature = native::features.at(ordinary.Handle());
      Check(mature_feature.slots[0].handle == models[0] && mature_feature.slots[1].handle == models[1]
                && native_runtime_evaluations[0].model == models[0]
                && native_runtime_evaluations[1].model == models[1] && ordinary.warm_color.complete,
            "the exact prepared pair evaluates under the same owner");
      receipt["cold_epoch_handoff"] = {{"old_epoch", outgoing_epoch}, {"new_epoch", next_epoch}, {"initial_creates", passes},
          {"held_original_fence_value", outgoing_value}, {"creates_before_old_gpu_completed", passes},
          {"total_creates", real_creates}, {"first_pass_retained", true}, {"both_passes_retained", true}, {"first_ready_frame", frame_id},
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

      auto* const effects = observed_effects_runtime;
      Check(effects != nullptr && color_identity(reinterpret_cast<ID3D12Resource*>(effects->get_current_back_buffer().handle))
                == color_identity(color.Get()),
            "the host initialized an effects runtime for the same current SDK backbuffer");
      renodx::addons::dlss5::RegisterReShadeCallbacks();
      const auto observe_fx_begin = [&] {
        output_framerate_registration_cases::Dispatch<reshade::addon_event::reshade_begin_effects>(
            effects, static_cast<reshade::api::command_list*>(nullptr),
            reshade::api::resource_view{}, reshade::api::resource_view{});
      };
      const auto pre_effects_rejection = [&] {
        return native::ordinary_present_attempts.at(&*route).pre_effects_rejection;
      };

      constexpr uint8_t fx_marker = 239;
      auto marker = gpu.Texture(1, 1, color->GetDesc().Format);
      gpu.Write(marker.Get(), fx_marker);
      submit();
      const auto warm_optional = [&] {
        for (const auto seed : std::array<uint8_t, 3>{161, 163, 167}) {
          const auto before = native_runtime_evaluations.size();
          Check(run_xefg(seed, false) && edited(seed) > 0 && with_same_models(before),
                "fixed Optional neutral warmup delivers the original two models on each frame");
        }
      };
      namespace rate = renodx::addons::dlss5::output_framerate;
      funnel_accounting_tests::Restore sr_restore;
      sr_restore.Value(&native::ngx_slot_real[0]);
      native::ngx_slot_real[0].create = reinterpret_cast<void*>(&nr_edit_reuse_render_cases::CreateSr);
      native::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(&nr_edit_reuse_gpu_cases::CopySr);
      native::ngx_slot_real[0].release = reinterpret_cast<void*>(+[](NVSDK_NGX_Handle*) {
        return NVSDK_NGX_Result_Success;
      });
      const auto source_input = gpu.Texture(width, height, color->GetDesc().Format);
      auto source_desc = color->GetDesc();
      source_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      const D3D12_HEAP_PROPERTIES source_heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
      ComPtr<ID3D12Resource> source_output;
      Check(SUCCEEDED(gpu.device->CreateCommittedResource(&source_heap, D3D12_HEAP_FLAG_NONE, &source_desc,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&source_output))),
            "real GPU output for the game's SR substitute");
      renodx::addons::dlss5::bridge::OwnedParameters sr_parameters;
      sr_parameters.Set(NVSDK_NGX_Parameter_Color, source_input.Get());
      sr_parameters.Set(NVSDK_NGX_Parameter_Output, source_output.Get());
      sr_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
      sr_parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
      sr_parameters.Set(NVSDK_NGX_Parameter_Width, guide_width);
      sr_parameters.Set(NVSDK_NGX_Parameter_Height, guide_height);
      sr_parameters.Set(NVSDK_NGX_Parameter_OutWidth, width);
      sr_parameters.Set(NVSDK_NGX_Parameter_OutHeight, height);
      sr_parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, contract.create_flags);
      sr_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, contract.motion_scale_x);
      sr_parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, contract.motion_scale_y);
      NVSDK_NGX_Handle* sr = nullptr;
      Check(NVSDK_NGX_SUCCEED(native::HookedCreateFeatureSlot<0>(gpu.list.Get(), native::kFeatureDlss, &sr_parameters, &sr))
                && sr != nullptr,
            "the game SR substitute supplies a real producer identity through the production create hook");
      const auto produce_guided_sr = [&](uint8_t seed) {
        ++frame_id;
        ++native::present_generation;
        consumed.clear();
        gpu.Write(source_input.Get(), seed);
        for (auto* resource : {source_input.Get(), depth.Get(), motion.Get()}) {
          gpu.Transition(resource, D3D12_RESOURCE_STATE_COMMON, NATIVE_INPUT_STATE);
        }
        Check(NVSDK_NGX_SUCCEED(native::HookedEvaluateFeatureSlot<0>(gpu.list.Get(), sr, &sr_parameters, nullptr)),
              "the current real-guide SR substitute executes through the production evaluate hook");
        gpu.Transition(source_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.list->CopyResource(color.Get(), source_output.Get());
        gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        gpu.Transition(source_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (auto* resource : {source_input.Get(), depth.Get(), motion.Get()}) {
          gpu.Transition(resource, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COMMON);
        }
        submit();
      };
      // The production hook learns the game's recording device from its first successful Evaluate.
      produce_guided_sr(160);
      const auto& sr_feature = native::features.at(sr);
      const rate::SourceIdentity sr_identity{.handle = reinterpret_cast<uintptr_t>(sr), .generation = sr_feature.source_generation,
          .device = reinterpret_cast<uintptr_t>(sr_feature.source_device12_identity.Get()),
          .resource = color_identity(source_output.Get()), .resource_generation = native::AdaptiveResourceGeneration(source_output.Get())};
      rate::BindSourceOutput(sr_identity, ordinary.output);
      Check(rate::FindSourceOutput(sr_identity) == session.context->output_identity,
            "the game SR output is associated with the same physical backbuffer and SDK view");
      warm_optional();

      receipt["late_real_guides"] = nlohmann::json::array();
      for (const auto seed : std::array<uint8_t, 3>{162, 164, 166}) {
        const bool first_recovery = seed == 162;
        produce_guided_sr(seed);
        const auto evaluations = native_runtime_evaluations.size();
        const auto ordinary_writes = native::present_ring.submitted;
        const auto service_writes = native::fg_service::streams.at(stream_key).submitted;
        const auto reads = consumer_reads;
        Check(session.context->enabled && !session.context->fresh_id,
              "the normal game frame reaches ordinary pre-FX before publishing its SDK ID or guides");
        const auto attempt = native::BeginOrdinaryPresentAttempt(&route->queue, &*route);
        Check(static_cast<bool>(attempt), "the late-guide frame has an actual ordinary pre-FX event");
        native::present_sdr_white_polled_ns = native::SteadyNowNs();
        native::RunPresentPath(&route->queue, &*route);
        const auto rejection = pre_effects_rejection();
        const auto pre_fx_evaluates = native_runtime_evaluations.size() - evaluations;
        const auto pre_fx_writes = native::present_ring.submitted - ordinary_writes;
        observe_fx_begin();
        native::OnUpdatePresent(effects);
        native::OnFinishPresent(&route->queue, &*route);
        session.extent_width = guide_width;
        session.extent_height = guide_height;
        session.Guides(frame_id, depth.Get(), motion.Get());
        session.extent_width = width;
        session.extent_height = height;
        session.Present(frame_id);
        const auto& handoff = session.context->last_handoff_write;
        std::cout << "LATE_REAL_GUIDES first_recovery=" << first_recovery << " pre_fx_writes=" << pre_fx_writes
                  << " pre_fx_evaluates=" << pre_fx_evaluates << " result=" << static_cast<int>(xefg::CurrentResult())
                  << " pre_effects_rejection=" << rejection << std::endl;
        Check(pre_fx_evaluates == 0 && pre_fx_writes == 0 && native::present_ring.submitted == ordinary_writes
                  && native::fg_service::streams.at(stream_key).submitted == service_writes + 1
                  && handoff.application_frame == session.context->application_frame && handoff.backbuffer == color_identity(color.Get())
                  && handoff.has_guides && handoff.color_executed && !handoff.ordinary_executed
                  && handoff.service_submission.signaled && handoff.sdk_result == S_OK,
              "late current guides get one XeFG write, with no speculative ordinary neutral edit");
        Check(with_same_models(evaluations) && consumer_reads == reads + 1 && edited(seed) > 0
                  && context_counts() == warm_context && (!first_recovery || both_reset()),
              "first and continuing real-guide frames use the same pair and deliver to the first SDK read");
        for (const auto& evaluated : std::span{native_runtime_evaluations}.subspan(evaluations, passes)) {
          Check(evaluated.depth != nullptr && evaluated.motion != nullptr
                    && evaluated.depth != ordinary.warm_neutral_guides.depth
                    && evaluated.motion != ordinary.warm_neutral_guides.motion,
                "both evaluations use this frame's real guides");
        }
        receipt["late_real_guides"].push_back({{"first_recovery", first_recovery}, {"application_frame", handoff.application_frame},
            {"ordinary_writes", pre_fx_writes}, {"sdk_writes", 1}, {"real_guide_passes", passes}, {"first_reads", consumer_reads - reads}});
      }
      receipt["late_disable"] = nlohmann::json::array();
      const D3D12_COMMAND_QUEUE_DESC alternate_queue_desc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
      ComPtr<ID3D12CommandQueue> alternate_queue;
      ComPtr<ID3D12Device> alternate_device;
      Check(SUCCEEDED(gpu.device->CreateCommandQueue(&alternate_queue_desc, IID_PPV_ARGS(&alternate_queue)))
                && SUCCEEDED(alternate_queue->GetDevice(IID_PPV_ARGS(&alternate_device)))
                && renodx::utils::directx::SameNativeObject(alternate_device.Get(), gpu.device.Get()),
            "the alternate real queue belongs to the same physical device");
      const auto alternate_queue_identity = reinterpret_cast<uintptr_t>(
          renodx::utils::directx::NativeIdentity(alternate_queue.Get()));
      Check(alternate_queue_identity != 0
                && alternate_queue_identity != reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(queue)),
            "the alternate queue has a distinct native identity on this device");
      const auto late_disable = [&](const char* phase, uint8_t seed, bool effects_already_started, bool composition) {
        const bool expect_edit = !effects_already_started && !composition;
        const auto before_warmup = native_runtime_evaluations.size();
        Check(run_xefg(159) && edited(159) > 0 && with_same_models(before_warmup),
              "the current SDK declaration first publishes the two real-guide passes");
        warm_optional();
        prepare_seed(seed);
        const auto evaluations = native_runtime_evaluations.size();
        const auto writes = native::present_ring.submitted;
        const auto reads = consumer_reads;
        const auto context_before = context_counts();
        Check(session.context->enabled && !session.context->fresh_id && !before_first_consumer,
              "late Disable starts before any outer SDK call, with FG On and no fresh guide ID");
        session.context->ui_composition = composition;
        if (effects_already_started) {
          observe_fx_begin();
        }
        const auto attempt = native::BeginOrdinaryPresentAttempt(&route->queue, &*route);
        Check(static_cast<bool>(attempt), "the real ordinary event receives this exact backbuffer before Disable");
        if (expect_edit) {
          const auto& event = native::ordinary_present_attempts.at(&*route);
          const auto current_queue = xefg::QueryOrdinaryPreEffects(event.event_access, attempt, event.queue, width, height);
          const auto wrong_queue = xefg::QueryOrdinaryPreEffects(event.event_access, attempt, alternate_queue_identity, width, height);
          std::cout << "LATE_DISABLE_QUEUE current=" << static_cast<bool>(current_queue.attempt)
                    << " alternate=" << static_cast<bool>(wrong_queue.attempt) << std::endl;
          Check(!wrong_queue.attempt, "a different actual queue cannot take this otherwise eligible pre-FX write");
        }
        native::present_sdr_white_polled_ns = native::SteadyNowNs();
        native::RunPresentPath(&route->queue, &*route);
        const auto rejection = pre_effects_rejection();
        if (!effects_already_started) {
          observe_fx_begin();
        }
        const auto pre_fx_writes = native::present_ring.submitted - writes;
        const auto pre_fx_evaluates = native_runtime_evaluations.size() - evaluations;
        if (pre_fx_writes != 0) {
          wait_fence(native::present_ring.fence, native::present_ring.submitted);
        }
        gpu.Transition(marker.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_TEXTURE_COPY_LOCATION marker_src{.pResource = marker.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
        const D3D12_TEXTURE_COPY_LOCATION marker_dst{.pResource = color.Get(),
            .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0};
        gpu.list->CopyTextureRegion(&marker_dst, 0, 0, 0, &marker_src, nullptr);
        gpu.Transition(color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        gpu.Transition(marker.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit();
        native::OnUpdatePresent(effects);
        native::OnFinishPresent(&route->queue, &*route);
        Check(Hooks::Enable(session.handle, 0) == 0 && !session.context->enabled,
              "Disable returns after effects and finish of the same application backbuffer");
        const DXGI_PRESENT_PARAMETERS full_present{};
        Check(adapter::ProxyHooks<0>::Present1(session.proxy.Get(), 0, 0, &full_present) == S_OK && consumer_reads == reads + 1,
              "the exact outer Off forwards one original call without a nested ordinary event");
        Check(consumed.size() == static_cast<size_t>(width) * height * 4
                  && std::ranges::all_of(std::span{consumed}.first(4), [](uint8_t channel) { return channel == fx_marker; }),
              "the first original SDK GPU read preserves the composed FX/UI marker");
        size_t edited_pixels = 0;
        for (size_t offset = 4; offset < consumed.size(); offset += 4) {
          Check(consumed[offset + 3] == seed, "the first original SDK GPU read uses the current frame's alpha");
          edited_pixels += consumed[offset] != seed || consumed[offset + 1] != seed || consumed[offset + 2] != seed;
        }
        std::cout << "LATE_DISABLE phase=" << phase << " ordinary_writes=" << pre_fx_writes
                  << " pre_fx_evaluates=" << pre_fx_evaluates << " first_read_edited_pixels=" << edited_pixels
                  << " pre_effects_outcome=" << (pre_fx_writes != 0 ? "submitted" : rejection)
                  << " sdk_queue_face=" << adapter::Identity(session.context->queue.Get())
                  << " native_queue_identity=" << reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(queue))
                  << " omit_optional_hudless=" << session.context->omit_optional_hudless << std::endl;
        Check(native::present_ring.submitted == writes + pre_fx_writes
                  && native_runtime_evaluations.size() == evaluations + pre_fx_evaluates && context_counts() == context_before,
              "no work is recreated or recorded after FX, Disable or during the first Original read");
        Check(expect_edit ? (pre_fx_writes == 1 && with_same_models(evaluations) && edited_pixels > 0)
                          : (pre_fx_writes == 0 && pre_fx_evaluates == 0 && edited_pixels == 0),
              "only a proven pre-FX single-color frame delivers exactly one edit before late Disable");
        if (expect_edit) {
          Check(native_runtime_evaluations[evaluations].list == native_runtime_evaluations[evaluations + 1].list,
                "the exact two models share the one pre-FX recording");
          prepare_seed(181);
          const auto next_evaluations = native_runtime_evaluations.size();
          const auto next_reads = consumer_reads;
          const auto next_attempt = native::BeginOrdinaryPresentAttempt(&route->queue, &*route);
          Check(static_cast<bool>(next_attempt), "the next ordinary frame gets its own pre-FX attempt");
          native::RunPresentPath(&route->queue, &*route);
          Check(adapter::ProxyHooks<0>::Present(session.proxy.Get(), 0, 0) == S_OK
                    && consumer_reads == next_reads + 1 && edited(181) > 0 && with_same_models(next_evaluations)
                    && native::present_ring.submitted == writes + pre_fx_writes + 1 && context_counts() == context_before,
                "the next Off frame gets its own single ordinary edit and preserves both warm models");
          native::OnFinishPresent(&route->queue, &*route);
        }
        receipt["late_disable"].push_back({{"phase", phase}, {"pre_fx_writes", pre_fx_writes},
            {"pre_fx_evaluates", pre_fx_evaluates}, {"first_read_edited_pixels", edited_pixels},
            {"first_read_count", 1}, {"fx_marker_preserved", true}});
        session.context->ui_composition = false;
        Check(Hooks::Enable(session.handle, 1) == 0, "the next independent frame begins with XeFG On");
      };
      late_disable("effects-already-started", 169, true, false);
      late_disable("composition-unconfirmed", 173, false, true);
      late_disable("ordinary-before-effects-and-disable", 179, false, false);
      Check(NVSDK_NGX_SUCCEED(native::HookedReleaseFeatureSlot<0>(sr)), "release the game SR substitute through the production hook");

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
              && (core_first || (real_inits == 1 && real_shutdowns == 0)) && exact_create_lists == real_creates
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
    const auto receipt_path = (case_name == "hot" ? "present-original-handoff.json"
        : "present-original-handoff-" + std::string(case_name) + ".json");
    std::ofstream result(receipt_path);
    result << receipt.dump(2) << '\n';
    Check(result.good(), "write the exact measured receipt");
    std::cout << "PASS present-original-handoff " << case_name
              << " NATIVE_GPU+NATIVE_NGX+MOCK_XeFG" << std::endl;
    return 0;
  } catch (const std::exception& problem) {
    std::cerr << "FAIL present-original-handoff: " << problem.what() << '\n';
    return 1;
  }
}

}  // namespace present_original_handoff_cases
#endif
