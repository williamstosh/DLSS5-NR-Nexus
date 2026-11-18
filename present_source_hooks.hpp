/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

// Included inside the Present implementation namespace. Only commands that
// can write the still-UAV SR image, or bypass the legacy layout contract,
// need observation. CopyResource and CopyTextureRegion already have hooks.
namespace present_source_hooks {

struct Hook {
  void** original;
  void* callback;
  void* target = nullptr;
};

template <typename List, size_t Slot, bool UnknownLayout, typename... Args>
struct Command {
  inline static constexpr size_t slot = Slot;
  inline static void* original = nullptr;
  static void STDMETHODCALLTYPE Call(List* list, Args... args) {
    const CallbackScope callback;
    const PresentCommandScope scope;
    if (callback && scope.outermost) {
      GuardHook([&] {
        if constexpr (Slot == 26) {
          ObservePresentSourceBarriers(list, args...);
        } else {
          ObservePresentSourceWork(list, UnknownLayout);
        }
      });
    }
    reinterpret_cast<void(STDMETHODCALLTYPE*)(List*, Args...)>(original)(list, args...);
  }
  inline static Hook hook{&original, reinterpret_cast<void*>(&Call)};
};

using Draw = Command<ID3D12GraphicsCommandList, 12, false, UINT, UINT, UINT, UINT>;
using DrawIndexed = Command<ID3D12GraphicsCommandList, 13, false, UINT, UINT, UINT, INT, UINT>;
using Dispatch = Command<ID3D12GraphicsCommandList, 14, false, UINT, UINT, UINT>;
using CopyTiles = Command<ID3D12GraphicsCommandList, 18, true, ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
    const D3D12_TILE_REGION_SIZE*, ID3D12Resource*, UINT64, D3D12_TILE_COPY_FLAGS>;
using Barriers = Command<ID3D12GraphicsCommandList, 26, false, UINT, const D3D12_RESOURCE_BARRIER*>;
using Bundle = Command<ID3D12GraphicsCommandList, 27, true, ID3D12GraphicsCommandList*>;
using ClearUInt = Command<ID3D12GraphicsCommandList, 49, false, D3D12_GPU_DESCRIPTOR_HANDLE,
    D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const UINT*, UINT, const D3D12_RECT*>;
using ClearFloat = Command<ID3D12GraphicsCommandList, 50, false, D3D12_GPU_DESCRIPTOR_HANDLE,
    D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const FLOAT*, UINT, const D3D12_RECT*>;
using Discard = Command<ID3D12GraphicsCommandList, 51, true, ID3D12Resource*, const D3D12_DISCARD_REGION*>;
using Indirect = Command<ID3D12GraphicsCommandList, 59, true, ID3D12CommandSignature*, UINT,
    ID3D12Resource*, UINT64, ID3D12Resource*, UINT64>;
using InitMeta = Command<ID3D12GraphicsCommandList4, 70, true, ID3D12MetaCommand*, const void*, SIZE_T>;
using ExecuteMeta = Command<ID3D12GraphicsCommandList4, 71, true, ID3D12MetaCommand*, const void*, SIZE_T>;
using Rays = Command<ID3D12GraphicsCommandList4, 76, false, const D3D12_DISPATCH_RAYS_DESC*>;
using Mesh = Command<ID3D12GraphicsCommandList6, 79, false, UINT, UINT, UINT>;
using EnhancedBarriers = Command<ID3D12GraphicsCommandList7, 80, true, UINT32, const D3D12_BARRIER_GROUP*>;
using Graph = Command<ID3D12GraphicsCommandList10, 85, true, const D3D12_DISPATCH_GRAPH_DESC*>;

inline std::array hooks{&Draw::hook, &DrawIndexed::hook, &Dispatch::hook, &CopyTiles::hook,
    &Barriers::hook, &Bundle::hook, &ClearUInt::hook, &ClearFloat::hook, &Discard::hook, &Indirect::hook,
    &InitMeta::hook, &ExecuteMeta::hook, &Rays::hook, &Mesh::hook, &EnhancedBarriers::hook, &Graph::hook};
inline std::shared_mutex mutex;

}  // namespace present_source_hooks

inline bool InstallPresentSourceHooks(ID3D12GraphicsCommandList* list) {
  using namespace present_source_hooks;
  if (list == nullptr) return false;
  renodx::utils::directx::NativeFromReShadeProxy(&list);
  const auto entries = [](IUnknown* object) { return *reinterpret_cast<void***>(object); };
  const auto base = entries(list);
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> list4;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> list6;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> list7;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList10> list10;
  list->QueryInterface(IID_PPV_ARGS(&list4));
  list->QueryInterface(IID_PPV_ARGS(&list6));
  list->QueryInterface(IID_PPV_ARGS(&list7));
  list->QueryInterface(IID_PPV_ARGS(&list10));
  const auto ray_tracing = (list4 != nullptr ? entries(list4.Get()) : nullptr);
  const std::array targets{base[Draw::slot], base[DrawIndexed::slot], base[Dispatch::slot], base[CopyTiles::slot],
      base[Barriers::slot], base[Bundle::slot], base[ClearUInt::slot], base[ClearFloat::slot], base[Discard::slot],
      base[Indirect::slot], (ray_tracing != nullptr ? ray_tracing[InitMeta::slot] : nullptr),
      (ray_tracing != nullptr ? ray_tracing[ExecuteMeta::slot] : nullptr), (ray_tracing != nullptr ? ray_tracing[Rays::slot] : nullptr),
      (list6 != nullptr ? entries(list6.Get())[Mesh::slot] : nullptr),
      (list7 != nullptr ? entries(list7.Get())[EnhancedBarriers::slot] : nullptr),
      (list10 != nullptr ? entries(list10.Get())[Graph::slot] : nullptr)};
  std::lock_guard lock(mutex);
  if (Barriers::hook.target != nullptr) {
    for (size_t i = 0; i < hooks.size(); ++i) {
      if (targets[i] != hooks[i]->target) return false;
    }
    return true;
  }
  if (!fg::PinRuntimeCallback()) return false;
  std::lock_guard transaction(renodx::utils::vtable::TransactionMutex());
  if (!renodx::utils::vtable::BeginTransaction()) return false;
  DetourUpdateThread(GetCurrentThread());
  for (size_t i = 0; i < hooks.size(); ++i) {
    if (targets[i] == nullptr) continue;
    auto* hook = hooks[i];
    *hook->original = targets[i];
    if (DetourAttach(hook->original, hook->callback) != NO_ERROR) {
      DetourTransactionAbort();
      return false;
    }
  }
  if (DetourTransactionCommit() != NO_ERROR) return false;
  for (size_t i = 0; i < hooks.size(); ++i) {
    hooks[i]->target = targets[i];
  }
  return true;
}

inline void UnhookPresentSourceHooks() {
  using namespace present_source_hooks;
  std::lock_guard lock(mutex);
  if (Barriers::hook.target == nullptr) return;
  std::lock_guard transaction(renodx::utils::vtable::TransactionMutex());
  if (!renodx::utils::vtable::BeginTransaction()) return;
  DetourUpdateThread(GetCurrentThread());
  for (auto* hook : hooks) {
    if (hook->target != nullptr && DetourDetach(hook->original, hook->callback) != NO_ERROR) {
      DetourTransactionAbort();
      return;
    }
  }
  if (DetourTransactionCommit() == NO_ERROR) {
    for (auto* hook : hooks) {
      hook->target = nullptr;
    }
  }
}
