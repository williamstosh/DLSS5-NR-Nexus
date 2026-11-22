#pragma once

#if defined(NR_NEXUS_TEST_NATIVE_NGX)
namespace bridge_xefg_provider_cases {

struct RuntimeCalls {
  native::DirectApi original;
  uint32_t init = 0;
  uint32_t creates = 0;
  uint32_t shutdown = 0;
};
inline RuntimeCalls* observed_runtime = nullptr;

inline NVSDK_NGX_Result NVSDK_CONV ObserveInit(unsigned long long app, const wchar_t* path,
    ID3D12Device* device, NVSDK_NGX_Version version, const NVSDK_NGX_Parameter* parameters) {
  ++observed_runtime->init;
  return observed_runtime->original.init(app, path, device, version, parameters);
}

inline NVSDK_NGX_Result NVSDK_CONV ObserveCreate(ID3D12GraphicsCommandList* list,
    NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  ++observed_runtime->creates;
  return observed_runtime->original.create(list, feature, parameters, handle);
}

inline NVSDK_NGX_Result NVSDK_CONV ObserveShutdown(ID3D12Device* device) {
  ++observed_runtime->shutdown;
  return observed_runtime->original.shutdown(device);
}

template <typename Submit, typename Wait>
void Run(Gpu& gpu, ID3D12Resource* depth12, ID3D12Resource* motion12, const Submit& submit, const Wait& wait) {
  const auto private_api = native::NgxLoaderView().native_api;
  Check(private_api.private_instance && private_api.module != nullptr
            && private_api.module != native::NgxLoaderView().nr_module
            && private_api.init != nullptr && private_api.create != nullptr && private_api.shutdown != nullptr,
        "shared-provider fixture requires the real private NVIDIA image");
  RuntimeCalls calls{.original = private_api};
  observed_runtime = &calls;
  const auto install_observers = [](native::DirectApi* api) {
    api->init = ObserveInit;
    api->create = ObserveCreate;
    api->shutdown = ObserveShutdown;
  };
  install_observers(&native::ngx_loader_staging.native_api);
  native::PublishNgxLoaderSymbols();
  if (native::direct_api.module == private_api.module && native::direct_api.private_instance) {
    install_observers(&native::direct_api);
  }
  const auto restore_observers = [&](RuntimeCalls*) {
    if (native::direct_api.module == private_api.module && native::direct_api.private_instance) {
      native::direct_api.init = private_api.init;
      native::direct_api.create = private_api.create;
      native::direct_api.shutdown = private_api.shutdown;
    }
    native::ngx_loader_staging.native_api = private_api;
    native::PublishNgxLoaderSymbols();
    observed_runtime = nullptr;
  };
  auto observer_scope = std::unique_ptr<RuntimeCalls, decltype(restore_observers)>(&calls, restore_observers);

  const UINT width = gpu.swapchain_width;
  const UINT height = gpu.swapchain_height;
  ComPtr<IDXGIAdapter> dxgi_adapter;
  Check(SUCCEEDED(gpu.factory->EnumAdapterByLuid(gpu.device->GetAdapterLuid(), IID_PPV_ARGS(&dxgi_adapter))),
        "Bridge selects fixture XeFG adapter");
  ComPtr<ID3D11Device> device11;
  ComPtr<ID3D11DeviceContext> context11;
  Check(SUCCEEDED(D3D11CreateDevice(dxgi_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device11, nullptr, &context11)),
        "real Bridge D3D11 device and immediate context");
  const auto release_bridge = [](ID3D11DeviceContext*) {
    if (native::DrainBridgeQueue()) {
      native::ReleaseBridge();
    }
  };
  auto bridge_scope = std::unique_ptr<ID3D11DeviceContext, decltype(release_bridge)>(context11.Get(), release_bridge);
  Check(native::EnsureBridgeUp(context11.Get()) == native::kBridgeReady,
        "production Bridge creates its own D3D12 queue and lists");
  Check(native::bridge.queue != gpu.queue.Get(), "Bridge and XeFG use independent queues");
  ComPtr<ID3D12Device> bridge_device;
  Check(SUCCEEDED(native::bridge.ring[0].list->GetDevice(IID_PPV_ARGS(&bridge_device)))
            && bridge_device.Get() == native::bridge.device12 && bridge_device.Get() == gpu.device.Get(),
        "Bridge and XeFG use the exact raw ID3D12Device interface");

  constexpr UINT pixel_bytes = 4;
  const auto make_texture11 = [&](DXGI_FORMAT format, D3D11_USAGE usage, const void* data = nullptr) {
    const D3D11_TEXTURE2D_DESC desc{.Width = width, .Height = height, .MipLevels = 1, .ArraySize = 1,
        .Format = format, .SampleDesc = {1, 0}, .Usage = usage,
        .BindFlags = (usage == D3D11_USAGE_DEFAULT ? UINT{D3D11_BIND_SHADER_RESOURCE} : 0u),
        .CPUAccessFlags = (usage == D3D11_USAGE_STAGING ? UINT{D3D11_CPU_ACCESS_READ} : 0u)};
    const D3D11_SUBRESOURCE_DATA init{.pSysMem = data, .SysMemPitch = width * pixel_bytes};
    ComPtr<ID3D11Texture2D> texture;
    Check(SUCCEEDED(device11->CreateTexture2D(&desc, (data != nullptr ? &init : nullptr), &texture)),
          "Bridge color, guides and staging textures");
    return texture;
  };
  std::vector<uint32_t> bridge_seed(static_cast<size_t>(width) * height);
  auto color11 = make_texture11(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT);
  auto motion11 = make_texture11(DXGI_FORMAT_R16G16_FLOAT, D3D11_USAGE_DEFAULT, bridge_seed.data());
  auto depth11 = make_texture11(DXGI_FORMAT_R32_FLOAT, D3D11_USAGE_DEFAULT, bridge_seed.data());
  auto staging11 = make_texture11(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_STAGING);
  const native::FeatureState contract{.input_width = width, .input_height = height};
  native::PresentStream bridge_stream;
  native::PresentTarget bridge_target{
      .workset_key = reinterpret_cast<const ID3D12Resource*>(&bridge_stream),
      .swapchain_format = DXGI_FORMAT_R8G8B8A8_UNORM};
  native::SnapshotPresentRestoreConfiguration(&bridge_target);
  bridge_target.diagnostic.bypass_host_look = true;
  bridge_target.pass_transfer_strength[0] = 1.f;
  bridge_target.pass_color_strength[0] = 1.f;
  const auto* bridge_handle = bridge_stream.Handle();

  Session session(&gpu);
  session.extent_left = session.extent_top = 0;
  session.extent_width = width;
  session.extent_height = height;
  session.context->ui_composition = false;
  Check(Hooks::Enable(session.handle, 1) == 0 && xefg::Enabled(), "XeFG owner remains enabled");
  const auto stream_key = std::pair{false, reinterpret_cast<uintptr_t>(session.handle)};
  const auto close_service = [&](Session*) {
    if (auto it = native::fg_service::streams.find(stream_key); it != native::fg_service::streams.end()) {
      wait(it->second.fence, it->second.submitted);
      native::fg_service::Release(&it->second);
      native::fg_service::streams.erase(it);
    }
  };
  auto service_scope = std::unique_ptr<Session, decltype(close_service)>(&session, close_service);
  ComPtr<ID3D12Resource> color12;
  Check(SUCCEEDED(session.proxy->GetBuffer(session.proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&color12))),
        "XeFG fixture's actual D3D12 backbuffer");
  const auto transfer12 = gpu.CreateTextureTransfer(color12.Get());

  uint32_t present_id = 900;
  const auto run_xefg = [&](uint8_t shade) {
    gpu.Write(color12.Get(), shade);
    submit();
    ++present_id;
    session.Guides(present_id, depth12, motion12);
    session.Present(present_id);
    const auto it = native::fg_service::streams.find(stream_key);
    if (it == native::fg_service::streams.end()) return false;
    wait(it->second.fence, it->second.submitted);
    return xefg::CurrentResult() == fg::Result::SUBMITTED;
  };
  const auto run_bridge = [&](uint32_t seed) {
    for (UINT y = 0; y < height; ++y) {
      for (UINT x = 0; x < width; ++x) {
        const bool tile = ((x / 40 + y / 30) % 2) != 0;
        bridge_seed[static_cast<size_t>(y) * width + x] = seed + (tile ? 0x00241915u : 0u);
      }
    }
    context11->UpdateSubresource(color11.Get(), 0, nullptr, bridge_seed.data(), width * pixel_bytes, 0);
    auto& parameters = native::FillPresentBlock(&bridge_stream, color11.Get(),
        motion11.Get(), depth11.Get(), contract, width, height, 1.f, 0);
    const renodx::addons::dlss5::EvaluateChainScope chain;
    return native::RunBridge(context11.Get(), bridge_handle, &parameters, nullptr, &bridge_target);
  };
  const auto private_active = [&] {
    return native::direct_api.private_instance && native::direct_api.module == private_api.module
        && native::direct_device == bridge_device.Get() && !native::parameter_runtime_via_core.load()
        && native::direct_api.init == ObserveInit && native::direct_api.create == ObserveCreate;
  };

  bool xefg_ready = false;
  for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !xefg_ready; ++attempt) {
    xefg_ready = run_xefg(160);
  }
  Check(xefg_ready && private_active(), "XeFG service warms one isolated NVIDIA runtime");
  const auto warm_init = calls.init;
  const auto warm_shutdown = calls.shutdown;

  auto& stream = native::fg_service::streams.at(stream_key);
  const native::WorksetKey bridge_key{bridge_handle, bridge_target.workset_key};
  const native::WorksetKey xefg_key{
      stream.roles[0].Handle(), reinterpret_cast<const ID3D12Resource*>(&stream.roles[0])};
  constexpr size_t passes = 2;
  const auto features_for = [](const native::WorksetKey& key) {
    std::array<const NVSDK_NGX_Handle*, passes> models{};
    if (const auto it = native::features.find(key.handle); it != native::features.end()) {
      for (size_t pass = 0; pass < passes; ++pass) {
        models[pass] = it->second.slots[pass].handle;
      }
    }
    return models;
  };
  const auto xefg_features = features_for(xefg_key);
  Check(std::ranges::all_of(xefg_features, [](auto* model) { return model != nullptr; }),
        "XeFG owns both warmed feature handles before Bridge starts");
  bool bridge_ready = false;
  for (uint32_t attempt = 0; attempt < native::kContractDebounceFrames + 8 && !bridge_ready; ++attempt) {
    const auto before = native_runtime_evaluations.size();
    const bool delivered = run_bridge(0xff203068u);
    const auto models = features_for(bridge_key);
    bridge_ready = delivered && native_runtime_evaluations.size() == before + passes
        && std::ranges::all_of(models, [](auto* model) { return model != nullptr; });
    Check(private_active() && calls.init == warm_init && calls.shutdown == warm_shutdown,
          "ordinary DX11 Bridge must retain the active private runtime on first use");
  }
  const auto bridge_features = features_for(bridge_key);
  Check(bridge_ready && std::ranges::all_of(bridge_features, [&](auto* model) {
    return model != nullptr && std::ranges::find(xefg_features, model) == xefg_features.end();
  }), "Bridge and XeFG warm distinct real NGX features");
  const auto warm_creates = calls.creates;
  Check(warm_creates == 2 * passes && stream.queue == gpu.queue.Get() && stream.queue != native::bridge.queue,
        "four actual Create calls warm distinct routes on separate queues");
  Check(native::worksets.contains(bridge_key) && native::worksets.contains(xefg_key),
        "Bridge and XeFG own distinct resident worksets");

  const auto addresses = [](const auto& handles) {
    std::array<uintptr_t, passes> values{};
    for (size_t index = 0; index < values.size(); ++index) {
      values[index] = reinterpret_cast<uintptr_t>(handles[index]);
    }
    return values;
  };
  nlohmann::json receipt{
      {"module", reinterpret_cast<uintptr_t>(private_api.module)},
      {"device", reinterpret_cast<uintptr_t>(bridge_device.Get())},
      {"bridge_queue", reinterpret_cast<uintptr_t>(native::bridge.queue)},
      {"xefg_queue", reinterpret_cast<uintptr_t>(gpu.queue.Get())},
      {"bridge_features", addresses(bridge_features)}, {"xefg_features", addresses(xefg_features)},
      {"bridge_workset_key", reinterpret_cast<uintptr_t>(bridge_key.resource)},
      {"xefg_workset_key", reinterpret_cast<uintptr_t>(xefg_key.resource)},
      {"warm_init_calls", warm_init}, {"warm_creates", warm_creates}, {"warm_shutdown_calls", warm_shutdown},
      {"rounds", nlohmann::json::array()}};
  auto& rounds = receipt["rounds"];
  const auto check_evaluations = [&](size_t index, const auto& handles, const auto& slots) {
    Check(native_runtime_evaluations.size() == index + passes, "real NGX evaluates both warmed passes");
    auto* list = native_runtime_evaluations[index].list;
    Check(std::ranges::any_of(slots, [&](const auto& slot) { return slot.list == list; }),
          "Evaluate uses this route's owned native D3D12 list");
    for (size_t pass = 0; pass < passes; ++pass) {
      const auto& seen = native_runtime_evaluations[index + pass];
      Check(seen.list == list && seen.model == handles[pass]
                && NVSDK_NGX_SUCCEED(native_runtime_evaluation_results[index + pass]),
            "NGX evaluates each route's own feature on the observed list");
    }
    ComPtr<ID3D12Device> actual_device;
    Check(SUCCEEDED(list->GetDevice(IID_PPV_ARGS(&actual_device))) && actual_device.Get() == bridge_device.Get(),
          "observed real Evaluate list returns the exact shared raw device");
    return list;
  };
  for (uint32_t round = 0; round < 3; ++round) {
    const auto bridge_before = native::bridge_submitted_value.load();
    const auto bridge_eval_index = native_runtime_evaluations.size();
    Check(run_bridge(0xff203068u + (round << 16)), "real D3D11 Bridge returns edited output");
    const auto bridge_fence = native::bridge_submitted_value.load();
    const auto bridge_done = native::bridge.fence_done12->GetCompletedValue();
    Check(bridge_fence > bridge_before && bridge_done >= bridge_fence && bridge_done != UINT64_MAX,
          "Bridge completes its own GPU submission");
    const auto* bridge_list = check_evaluations(bridge_eval_index, bridge_features, native::bridge.ring);

    context11->CopyResource(staging11.Get(), color11.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(context11->Map(staging11.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
          "read actual Bridge-delivered D3D11 color");
    std::array<uint8_t, pixel_bytes> bridge_pixel{};
    std::memcpy(bridge_pixel.data(), mapped.pData, bridge_pixel.size());
    size_t bridge_edited_pixels = 0;
    const auto* seed_bytes = reinterpret_cast<const uint8_t*>(bridge_seed.data());
    for (UINT y = 0; y < height; ++y) {
      const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
      const auto* seed_row = seed_bytes + static_cast<size_t>(y) * width * pixel_bytes;
      for (UINT x = 0; x < width; ++x) {
        bridge_edited_pixels += std::memcmp(row + x * pixel_bytes, seed_row + x * pixel_bytes, 3) != 0;
      }
    }
    context11->Unmap(staging11.Get(), 0);

    const auto xefg_before = stream.submitted;
    const auto xefg_eval_index = native_runtime_evaluations.size();
    const auto xefg_seed = static_cast<uint8_t>(160 + round * 16);
    Check(run_xefg(xefg_seed), "real XeFG private recording delivers");
    const auto xefg_done = stream.fence->GetCompletedValue();
    Check(stream.submitted > xefg_before && xefg_done >= stream.submitted && xefg_done != UINT64_MAX,
          "XeFG completes its own GPU submission");
    const auto* xefg_list = check_evaluations(xefg_eval_index, xefg_features, stream.slots);
    Check(bridge_list != xefg_list, "the two routes use different command lists");

    gpu.Transition(color12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.RecordTextureReadback(color12.Get(), transfer12);
    gpu.Transition(color12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    submit();
    const auto pixels12 = gpu.ReadTexture(transfer12);
    Check(pixels12.size() == static_cast<size_t>(width) * height * pixel_bytes,
          "read actual XeFG-delivered D3D12 color");
    std::array<uint8_t, pixel_bytes> xefg_pixel{};
    std::memcpy(xefg_pixel.data(), pixels12.data(), xefg_pixel.size());
    size_t xefg_edited_pixels = 0;
    for (size_t index = 0; index < pixels12.size(); index += pixel_bytes) {
      xefg_edited_pixels += pixels12[index] != xefg_seed || pixels12[index + 1] != xefg_seed
          || pixels12[index + 2] != xefg_seed;
    }
    Check(bridge_edited_pixels > 0 && xefg_edited_pixels > 0,
          "both delivered outputs differ from their original RGB seeds");
    Check(!std::equal(bridge_pixel.begin(), bridge_pixel.begin() + 3, xefg_pixel.begin()),
          "Bridge and XeFG retain different RGB outputs");
    Check(private_active() && calls.init == warm_init && calls.creates == warm_creates
              && calls.shutdown == warm_shutdown
              && features_for(bridge_key) == bridge_features && features_for(xefg_key) == xefg_features
              && native::worksets.contains(bridge_key) && native::worksets.contains(xefg_key),
          "alternating Bridge and XeFG retains one runtime, four handles and two worksets without Init/Create");
    rounds.push_back({{"bridge_list", reinterpret_cast<uintptr_t>(bridge_list)},
        {"xefg_list", reinterpret_cast<uintptr_t>(xefg_list)},
        {"bridge_fence", bridge_fence}, {"bridge_fence_completed", bridge_done},
        {"xefg_fence", stream.submitted}, {"xefg_fence_completed", xefg_done},
        {"bridge_rgba", bridge_pixel}, {"xefg_rgba", xefg_pixel},
        {"readback_pixels_per_route", static_cast<size_t>(width) * height},
        {"bridge_edited_rgb_pixels", bridge_edited_pixels}, {"xefg_edited_rgb_pixels", xefg_edited_pixels}});
  }
  receipt["measured_init_calls"] = calls.init - warm_init;
  receipt["measured_create_calls"] = calls.creates - warm_creates;
  receipt["measured_shutdown_calls"] = calls.shutdown - warm_shutdown;
  std::ofstream output("bridge-xefg-private-provider.json");
  output << receipt.dump(2) << '\n';
  output.close();
  Check(!output.fail(), "write shared provider evidence");
  {
    const renodx::addons::dlss5::RuntimeLock lock(native::runtime_mutex);
    native::ForgetSourceHandleLocked(bridge_handle);
  }
  std::cout << "PASS Bridge+XeFG shared private runtime: 3 round trips, no additional Init/Create/Shutdown, "
               "four feature identities, two worksets, independent fences and color readback; "
               "evidence=NATIVE_GPU+NATIVE_NGX+MOCK_XeFG\n";
}

}  // namespace bridge_xefg_provider_cases
#endif
