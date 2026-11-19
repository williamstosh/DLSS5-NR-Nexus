/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#define CINTERFACE
#include <d3d12.h>
#include <cstddef>
#include <cmd_slots.hpp>

namespace addon = renodx::addons::dlss5;
static_assert(offsetof(ID3D12GraphicsCommandListVtbl, CopyResource) / sizeof(void*) == addon::kCmdSlotCopyResource);
static_assert(offsetof(ID3D12GraphicsCommandListVtbl, CopyTextureRegion) / sizeof(void*) == addon::kCmdSlotCopyTextureRegion);
static_assert(offsetof(ID3D12GraphicsCommandListVtbl, ResourceBarrier) / sizeof(void*) == addon::kCmdSlotResourceBarrier);
static_assert(offsetof(ID3D12GraphicsCommandList7Vtbl, Barrier) / sizeof(void*) == addon::kCmdSlot7Barrier);
int main() { return 0; }
