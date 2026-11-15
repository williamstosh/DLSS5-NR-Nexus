/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "runtime_image.hpp"

namespace {

namespace image = renodx::addons::dlss5::runtime_image;
using Microsoft::WRL::ComPtr;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct ForwardingDeviceObject final : ID3D12Object {
  ID3D12Object* native = nullptr;
  ULONG references = 1;

  explicit ForwardingDeviceObject(ID3D12Object* object) : native(object) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (iid != IID_IUnknown && iid != __uuidof(ID3D12Object)) return E_NOINTERFACE;
    *object = static_cast<ID3D12Object*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
    return native->GetPrivateData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
    return native->SetPrivateData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override {
    return native->SetPrivateDataInterface(guid, data);
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return native->SetName(name); }
};

struct Fixture {
  using InitFn = uint32_t(WINAPI*)(ID3D12Object*, uint32_t);
  using CreateFn = uint32_t(WINAPI*)(ID3D12Object*, uint32_t);
  using ShutdownFn = uint32_t(WINAPI*)(ID3D12Object*);
  using CountFn = uint32_t(WINAPI*)();
  using PathFn = DWORD(WINAPI*)(wchar_t*, DWORD);

  InitFn init = nullptr;
  CreateFn create = nullptr;
  ShutdownFn shutdown = nullptr;
  CountFn shutdown_count = nullptr;
  PathFn path = nullptr;
};

Fixture Exports(HMODULE module) {
  Fixture fixture{
      .init = reinterpret_cast<Fixture::InitFn>(GetProcAddress(module, "FixtureInit")),
      .create = reinterpret_cast<Fixture::CreateFn>(GetProcAddress(module, "FixtureCreate")),
      .shutdown = reinterpret_cast<Fixture::ShutdownFn>(GetProcAddress(module, "FixtureShutdown")),
      .shutdown_count = reinterpret_cast<Fixture::CountFn>(GetProcAddress(module, "FixtureShutdownCount")),
      .path = reinterpret_cast<Fixture::PathFn>(GetProcAddress(module, "FixtureModulePath"))};
  Check(fixture.init != nullptr && fixture.create != nullptr && fixture.shutdown != nullptr
          && fixture.shutdown_count != nullptr && fixture.path != nullptr,
      "fixture exports resolved from the selected HMODULE");
  return fixture;
}

uint32_t load_calls = 0;
std::filesystem::path source_path;

HMODULE Load(const wchar_t* path, DWORD*) {
  ++load_calls;
  return LoadLibraryW(path);
}

HMODULE RedirectToShared(const wchar_t*, DWORD*) {
  ++load_calls;
  return LoadLibraryW(source_path.c_str());
}

HMODULE FailLoad(const wchar_t*, DWORD*) {
  ++load_calls;
  SetLastError(ERROR_MOD_NOT_FOUND);
  return nullptr;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 2) {
      source_path = std::filesystem::absolute(std::filesystem::path(argv[1]));
      const HMODULE shared_module = LoadLibraryW(source_path.c_str());
      Check(shared_module != nullptr, "load-only: resolve the selected runtime's dependencies");
      const auto private_image = image::LoadPrivate(source_path,
          std::filesystem::current_path() / L"private-images", shared_module, Load);
      Check(private_image.ready && private_image.module != shared_module,
          "load-only: actual runtime uses a distinct image and path");
      for (const char* name : {"NVSDK_NGX_D3D12_Init_Ext", "NVSDK_NGX_D3D12_CreateFeature",
               "NVSDK_NGX_D3D12_EvaluateFeature", "NVSDK_NGX_D3D12_ReleaseFeature", "NVSDK_NGX_D3D12_Shutdown1"}) {
        const auto address = GetProcAddress(private_image.module, name);
        HMODULE owner = nullptr;
        Check(address != nullptr && address != GetProcAddress(shared_module, name)
                && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(address), &owner)
                && owner == private_image.module,
            "load-only: every required runtime export belongs to the distinct private image");
      }
      // No NGX API is called. Leave both references alive until this process
      // exits: a vendor DLL can start workers even before Init is called.
      std::cout << "PASS runtime-image-load-only evidence=Windows-loader distinct-image=1"
                   " required-exports-owned=5 NGX_Init=NOT_RUN NGX_Create=NOT_RUN GPU=NOT_RUN game=NOT_RUN\n";
      return 0;
    }
    Check(argc == 1, "provide no argument for the synthetic cache test, or one runtime path for load-only");
    source_path = std::filesystem::current_path() / L"nvngx_dlssnr.dll";
    const auto cache = std::filesystem::current_path() / L"private-images";
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))
            && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))
            && SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))),
        "explicit WARP device, without hardware fallback");
    ForwardingDeviceObject proxy(device.Get());
    const HMODULE shared_module = LoadLibraryW(source_path.c_str());
    Check(shared_module != nullptr, "load the synthetic game/shared runtime");
    const Fixture shared = Exports(shared_module);
    Check(shared.init(&proxy, 1) == 1 && shared.init(device.Get(), 0) == 2,
        "shared image reuses the proxy context through a forwarding device-private key");
    Check(shared.create(device.Get(), 0) == 0 && shared.create(&proxy, 1) == 1,
        "a second native Init on the same shared image does not replace its cached proxy context");

    const auto private_image = image::LoadPrivate(source_path, cache, shared_module, Load);
    Check(private_image.ready && private_image.module != nullptr && private_image.module != shared_module,
        "production loader acquires a distinct Windows image for the private runtime");
    Check(private_image.path.filename() == source_path.filename()
            && std::filesystem::equivalent(private_image.source_path, source_path),
        "same runtime source and basename are retained");
    const Fixture isolated = Exports(private_image.module);
    wchar_t actual_path[32768]{};
    const DWORD length = isolated.path(actual_path, static_cast<DWORD>(std::size(actual_path)));
    Check(length != 0 && length < std::size(actual_path)
            && std::filesystem::equivalent(std::filesystem::path(actual_path), private_image.path)
            && isolated.init != shared.init && isolated.create != shared.create,
        "actual loaded path and function addresses belong to the private image");
    Check(isolated.init(device.Get(), 0) == 1 && isolated.init(&proxy, 1) == 2
            && isolated.create(device.Get(), 0) == 1 && isolated.create(&proxy, 1) == 0,
        "the same device key selects a new native context in the distinct image");
    Check(shared.create(&proxy, 1) == 1 && shared.shutdown_count() == 0,
        "private initialization does not replace or shut down the game's shared context");
    Check(isolated.shutdown(device.Get()) == 1 && shared.shutdown_count() == 0
            && shared.create(&proxy, 1) == 1,
        "private Shutdown affects only the private image");
    const auto private_path = private_image.path;
    Check(FreeLibrary(private_image.module) != FALSE && image::RemoveFiles(private_image)
            && !std::filesystem::exists(private_path),
        "an explicitly unloaded private image releases only its own files");

    const auto redirected = image::LoadPrivate(source_path, cache, shared_module, RedirectToShared);
    Check(!redirected.ready && redirected.module == shared_module,
        "a loader redirected to the shared HMODULE is rejected as unisolated");
    Check(FreeLibrary(redirected.module) != FALSE && image::RemoveFiles(redirected)
            && shared.create(&proxy, 1) == 1,
        "rejecting an extra shared reference leaves the original shared image alive");
    const auto failed = image::LoadPrivate(source_path, cache, shared_module, FailLoad);
    Check(!failed.ready && failed.module == nullptr && image::RemoveFiles(failed)
            && shared.shutdown_count() == 0,
        "private load failure has no shared-runtime fallback or Shutdown");
    const auto calls = load_calls;
    const auto missing = image::LoadPrivate(source_path.parent_path() / L"missing.dll", cache, shared_module, Load);
    Check(!missing.ready && missing.module == nullptr && load_calls == calls && image::RemoveFiles(missing),
        "missing input is rejected before invoking the loader");
    Check(shared.shutdown(&proxy) == 1 && proxy.references == 1,
        "only the synthetic game owner closes its original context");
    Check(FreeLibrary(shared_module) != FALSE, "release the synthetic game's DLL reference");
    std::cout << "PASS private-runtime-image evidence=Windows-loader+WARP+MOCK"
                 " shared-key-reuse=observed distinct-image=1 independent-context=1"
                 " private-shutdown-only=1 redirect-rejected=1 failure-no-fallback=1"
                 " real_NGX=NOT_RUN game=NOT_RUN\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
