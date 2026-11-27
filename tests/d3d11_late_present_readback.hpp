/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace renodx::addons::dlss5::present_order_witness {

struct Dx11Roi {
  uint32_t left = 0;
  uint32_t top = 0;
  uint32_t right = 0;
  uint32_t bottom = 0;
};

// Samples the exact texture and subresource at the calling immediate-context
// operation. Its event proves completion of this copy, not of a later Present.
class Dx11ColorSample {
 public:
  HRESULT Record(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                 uint32_t subresource, Dx11Roi roi) {
    if (event_ != nullptr) return E_UNEXPECTED;
    if (context == nullptr || source == nullptr
        || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1 || desc.MipLevels == 0
        || subresource >= desc.MipLevels * desc.ArraySize
        || roi.left >= roi.right || roi.top >= roi.bottom) return E_INVALIDARG;
    const uint32_t mip = subresource % desc.MipLevels;
    const uint32_t width = roi.right - roi.left;
    const uint32_t height = roi.bottom - roi.top;
    if (roi.right > (std::max)(1u, desc.Width >> mip) || roi.bottom > (std::max)(1u, desc.Height >> mip)
        || width > 1280 || height > 720) return E_INVALIDARG;  // Owned temporary witness; no pixel files.

    uint32_t bytes_per_pixel = 4;
    switch (desc.Format) {
      case DXGI_FORMAT_R16G16B16A16_FLOAT:  bytes_per_pixel = 8; break;
      case DXGI_FORMAT_R10G10B10A2_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: break;
      default:                              return E_NOTIMPL;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11Device> receiver;
    context->GetDevice(&device);
    source->GetDevice(&receiver);
    if (device == nullptr || device.Get() != receiver.Get()) return E_INVALIDARG;

    Microsoft::WRL::ComPtr<IUnknown> source_identity;
    HRESULT result = source->QueryInterface(IID_PPV_ARGS(&source_identity));
    if (FAILED(result)) return result;

    const D3D11_TEXTURE2D_DESC staging_desc{
        .Width = width, .Height = height, .MipLevels = 1, .ArraySize = 1, .Format = desc.Format, .SampleDesc = {1, 0}, .Usage = D3D11_USAGE_STAGING, .CPUAccessFlags = D3D11_CPU_ACCESS_READ};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    result = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(result)) return result;

    const D3D11_QUERY_DESC query_desc{.Query = D3D11_QUERY_EVENT};
    Microsoft::WRL::ComPtr<ID3D11Query> event;
    result = device->CreateQuery(&query_desc, &event);
    if (FAILED(result)) return result;

    const D3D11_BOX box{roi.left, roi.top, 0, roi.right, roi.bottom, 1};
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source, subresource, &box);
    context->End(event.Get());
    staging_ = std::move(staging);
    event_ = std::move(event);
    context_ = context;
    source_identity_ = std::move(source_identity);
    subresource_ = subresource;
    format_ = desc.Format;
    roi_ = roi;
    row_bytes_ = width * bytes_per_pixel;
    return S_OK;
  }

  HRESULT ReadIfReady(ID3D11DeviceContext* context, std::vector<uint8_t>* pixels) {
    if (context == nullptr || context != context_.Get() || pixels == nullptr) return E_INVALIDARG;
    BOOL done = FALSE;
    const HRESULT status = context->GetData(event_.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (FAILED(status)) return status;
    if (status != S_OK || !done) return S_FALSE;

    const uint32_t height = roi_.bottom - roi_.top;
    std::vector<uint8_t> result(static_cast<size_t>(height) * row_bytes_);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT map = context->Map(staging_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (map == DXGI_ERROR_WAS_STILL_DRAWING) return S_FALSE;
    if (FAILED(map)) return map;
    for (uint32_t row = 0; row < height; ++row) {
      std::memcpy(result.data() + size_t{row} * row_bytes_,
                  static_cast<const uint8_t*>(mapped.pData) + size_t{row} * mapped.RowPitch, row_bytes_);
    }
    context->Unmap(staging_.Get(), 0);
    *pixels = std::move(result);
    return S_OK;
  }

  [[nodiscard]] const void* Source() const { return source_identity_.Get(); }
  [[nodiscard]] Dx11Roi Region() const { return roi_; }
  [[nodiscard]] uint32_t Subresource() const { return subresource_; }
  [[nodiscard]] DXGI_FORMAT Format() const { return format_; }

 private:
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
  Microsoft::WRL::ComPtr<ID3D11Query> event_;
  Microsoft::WRL::ComPtr<IUnknown> source_identity_;
  Dx11Roi roi_;
  uint32_t subresource_ = 0;
  uint32_t row_bytes_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

}  // namespace renodx::addons::dlss5::present_order_witness
