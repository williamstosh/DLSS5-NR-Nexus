// Copyright (C) 2026
// SPDX-License-Identifier: MIT

#include "codec_math.hlsli"

Texture2D<float4> Proxy : register(t0);
Texture2D<float4> Neural : register(t1);
Texture2D<float2> Motion : register(t2);
Texture2D<float> Depth : register(t3);
Texture2D<float4> Original : register(t4);
RWTexture2D<float4> Target : register(u0);
// 32 bytes/pixel: float4(log2 edit RGB, valid guide), float4(linear P RGB, depth).
RWByteAddressBuffer Anchor : register(u1);
RWByteAddressBuffer PreviousMotion : register(u2);  // float2 render-pixel displacement
RWByteAddressBuffer Validity : register(u3);        // uint 0 invalid, 1 valid
RWTexture2D<float2> ComposedMotion : register(u4);

cbuffer ReuseConstants : register(b0) {
  uint2 Size;
  uint2 MotionSize;
  float2 MotionScale;
  uint Phase;
  uint Padding;
  float2 RasterOffset;
  float2 PreviousMotionOffset;
};

static const float kEditFloor = 1.0 / 1024.0;
static const float kReferenceLimit = 0.125;  // stops, max per channel

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 pixel = dispatch_id.xy;
  if (Phase == 2u || Phase == 3u) {
    if (any(pixel >= MotionSize)) return;
    const uint address = (pixel.y * MotionSize.x + pixel.x) * 8u;
    const float2 d1 = Motion.Load(int3(pixel, 0)) * MotionScale;
    if (Phase == 3u) {
      PreviousMotion.Store2(address, asuint(d1));
      return;
    }
    const float2 previous = float2(pixel) + d1 + PreviousMotionOffset - RasterOffset;
    float2 d2 = 0.0;
    if (all(isfinite(previous)) && all(previous >= 0.0) && all(previous <= float2(MotionSize - 1u))) {
      const uint2 lo = uint2(floor(previous));
      const uint2 hi = min(lo + 1u, MotionSize - 1u);
      const float2 f = frac(previous);
      const float2 a = asfloat(PreviousMotion.Load2((lo.y * MotionSize.x + lo.x) * 8u));
      const float2 b = asfloat(PreviousMotion.Load2((lo.y * MotionSize.x + hi.x) * 8u));
      const float2 c = asfloat(PreviousMotion.Load2((hi.y * MotionSize.x + lo.x) * 8u));
      const float2 d = asfloat(PreviousMotion.Load2((hi.y * MotionSize.x + hi.x) * 8u));
      d2 = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
    }
    const float2 composed = (d1 + d2) / MotionScale;
    ComposedMotion[pixel] = all(isfinite(composed)) ? composed : 0.0;
    return;
  }
  if (any(pixel >= Size)) return;
  const uint index = pixel.y * Size.x + pixel.x;
  if (Phase == 4u) {
    if (Validity.Load(index * 4u) == 0u) Target[pixel] = Original.Load(int3(pixel, 0));
    return;
  }
  const float2 guide_position = (float2(pixel) + 0.5) * float2(MotionSize) / float2(Size) + RasterOffset;
  const bool guide_valid = all(guide_position >= 0.0) && all(guide_position < float2(MotionSize));
  const uint2 guide = uint2(clamp(floor(guide_position), 0.0, float2(MotionSize - 1u)));
  const float depth = Depth.Load(int3(guide, 0));
  const float4 encoded = Proxy.Load(int3(pixel, 0));
  const float3 reference = SrgbDecode(encoded.rgb);
  if (Phase == 0u) {
    const float3 neural = SrgbDecode(Neural.Load(int3(pixel, 0)).rgb);
    const float3 edit = log2(max(neural, kEditFloor)) - log2(max(reference, kEditFloor));
    Anchor.Store4(index * 32u, asuint(float4(edit, guide_valid ? 1.0 : 0.0)));
    Anchor.Store4(index * 32u + 16u, asuint(float4(reference, depth)));
    return;
  }
  const float2 d = Motion.Load(int3(guide, 0)) * MotionScale * float2(Size) / float2(MotionSize);
  const float2 previous = float2(pixel) + d;
  bool valid = guide_valid && all(isfinite(previous)) && all(previous >= 0.0) && all(previous <= float2(Size - 1u))
      && isfinite(depth) && all(isfinite(reference)) && all(reference >= kEditFloor);
  float3 edit = 0.0;
  if (valid) {
    const uint2 lo = uint2(floor(previous));
    const uint2 hi = min(lo + 1u, Size - 1u);
    const uint2 taps[4] = {lo, uint2(hi.x, lo.y), uint2(lo.x, hi.y), hi};
    const float2 f = frac(previous);
    const float weights[4] = {(1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y};
    for (uint tap = 0u; tap < 4u; ++tap) {
      if (weights[tap] <= 0.0) continue;
      const uint address = (taps[tap].y * Size.x + taps[tap].x) * 32u;
      const float4 cached_edit = asfloat(Anchor.Load4(address));
      const float4 cached = asfloat(Anchor.Load4(address + 16u));
      const float3 change = abs(log2(max(reference, kEditFloor)) - log2(max(cached.rgb, kEditFloor)));
      valid = valid && cached_edit.a > 0.5 && all(isfinite(cached)) && all(isfinite(cached_edit))
          && abs(depth - cached.a) <= 0.01 + 0.02 * abs(depth)
          && all(change <= kReferenceLimit);
      edit += weights[tap] * cached_edit.rgb;
    }
  }
  // A neutral edit selects the untouched source at the final boundary rather
  // than relying on a codec round trip to reproduce half-float source bits.
  valid = valid && any(abs(edit) > 1e-6);
  const float3 neural = valid ? SrgbEncode(reference * exp2(edit)) : encoded.rgb;
  Target[pixel] = float4(neural, encoded.a);
  Validity.Store(index * 4u, valid ? 1u : 0u);
}
