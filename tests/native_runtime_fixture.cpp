/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include <d3d12.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace {

// Synthetic test identity, unrelated to NVIDIA's device-private keys.
constexpr GUID kDeviceKey{0x9d6d38cd, 0x2b6d, 0x4c0c, {0x9c, 0xe3, 0x6c, 0x8b, 0xbb, 0x87, 0x9b, 0xa0}};

struct Context {
  ID3D12Object* device = nullptr;
  uint32_t face = 0;
};

std::atomic_uint64_t next_device_key{0};
std::unordered_map<uint64_t, Context> contexts;
uint32_t shutdowns = 0;

uint64_t DeviceKey(ID3D12Object* device) {
  if (device == nullptr) return 0;
  uint64_t key = 0;
  UINT size = sizeof(key);
  if (FAILED(device->GetPrivateData(kDeviceKey, &size, &key))) {
    key = ++next_device_key;
    if (FAILED(device->SetPrivateData(kDeviceKey, sizeof(key), &key))) return 0;
  }
  return key;
}

}  // namespace

extern "C" __declspec(dllexport) uint32_t WINAPI FixtureInit(ID3D12Object* device, uint32_t face) {
  const uint64_t key = DeviceKey(device);
  if (key == 0) return 0;
  if (contexts.contains(key)) return 2;
  device->AddRef();
  contexts.emplace(key, Context{.device = device, .face = face});
  return 1;
}

extern "C" __declspec(dllexport) uint32_t WINAPI FixtureCreate(ID3D12Object* device, uint32_t face) {
  const auto context = contexts.find(DeviceKey(device));
  return context != contexts.end() && context->second.face == face;
}

extern "C" __declspec(dllexport) uint32_t WINAPI FixtureShutdown(ID3D12Object* device) {
  const auto context = contexts.find(DeviceKey(device));
  if (context == contexts.end()) return 0;
  context->second.device->Release();
  contexts.erase(context);
  return ++shutdowns;
}

extern "C" __declspec(dllexport) uint32_t WINAPI FixtureShutdownCount() {
  return shutdowns;
}

extern "C" __declspec(dllexport) DWORD WINAPI FixtureModulePath(wchar_t* path, DWORD capacity) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&FixtureModulePath), &module)) return 0;
  return GetModuleFileNameW(module, path, capacity);
}
