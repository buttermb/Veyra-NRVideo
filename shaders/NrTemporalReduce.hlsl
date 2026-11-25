// Half-resolution observation pass for the low-frequency anti-flicker route.
//
// Ported from SAOG0721/Magpie, GPL-3.0, commit
// 3841698348bfb246623d4acf791984c8b68a577b
// src/Magpie.Core/DLSSNRTemporalShader.h (DLSSNR_TEMPORAL_REDUCE_SHADER).
// Veyra: D3D12 Load-based reads, reset via zero weight, post-protection limit.
//
// Only the "low-frequency temporal reconstruction" route uses this. It folds a
// 2x2 block of the exact post-chain difference into one signed observation plus
// one guide value, so the temporal filter can work on a signal that is already
// narrow-band; the carrier keeps this frame's high frequency untouched.
#include "NrProtection.hlsli"
Texture2D<float4> Input : register(t0);   // NR base (the image NR consumed)
Texture2D<float4> Base : register(t1);    // alias of Input in this graph
Texture2D<float4> Raw : register(t2);     // NR result
RWTexture2D<float4> Residual : register(u0);
RWTexture2D<float4> Guide : register(u1);
cbuffer Settings : register(b0) {
    uint2 Size;        // full extent
    uint2 LowSize;     // half extent (ceil)
    float Reserved0;   // legacy layout lane; the guide encoding is unconditional
    float Protected;
    float Feather;
    float Reserved1;   // keeps Regions float4-aligned in root constants
    float4 Regions[4];
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= LowSize)) return;
    float3 residual = 0, guide = 0;
    float count = 0;
    bool valid = true;
    [unroll] for (uint y = 0; y < 2; ++y) {
        [unroll] for (uint x = 0; x < 2; ++x) {
            const uint2 p = id.xy * 2 + uint2(x, y);
            // Odd dimensions keep their last sample rather than dropping it.
            if (any(p >= Size)) continue;
            const float4 i = Input.Load(int3(p, 0));
            const float4 b = Base.Load(int3(p, 0));
            const float4 o = Raw.Load(int3(p, 0));
            // Only the 2x2 block containing a protected pixel loses support;
            // previously one enabled protection region disabled the low-frequency
            // route across the entire frame.
            if (!all(isfinite(i)) || !all(isfinite(b)) || !all(isfinite(o)) ||
                NrProtection(float2(p)+.5,Size,Protected,Feather,Regions)>0) { valid = false; continue; }
            residual += o.rgb - b.rgb;
            // This must be the same perceptual guide encoding NrTemporal uses
            // for full-resolution history. Feeding the temporal pass a linear
            // or unencoded half-resolution guide made edge acceptance depend on
            // the wrong units, especially in HDR shadows/highlights.
            const float3 bounded=abs(i.rgb)/(1+abs(i.rgb));
            const float3 encoded=select(bounded<=.0031308,12.92*bounded,1.055*pow(bounded,1.0/2.4)-.055);
            guide += sign(i.rgb)*encoded;
            count += 1;
        }
    }
    // A protected pixel must not publish an observation at all; a partial block
    // would otherwise smuggle the protected value into the temporal filter.
    if (!valid || count == 0) { Residual[id.xy] = 0; Guide[id.xy] = 0; return; }
    Residual[id.xy] = float4(clamp(residual / count, -65504, 65504), 1);
    Guide[id.xy] = float4(guide / count, 1);
}
