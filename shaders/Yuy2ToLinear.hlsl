// Native 4:2:2 capture. One RGBA8 texel stores Y0 U Y1 V for two pixels.
// No CPU RGB expansion, intermediate 8-bit RGB rounding, or 4:2:0 conversion.
#include "HdrColor.hlsli"
#include "ColorGrade.hlsli"
cbuffer Params : register(b0) {
    uint width; uint height; uint transfer; uint reserved;
    float limited; float matrix709; float primaries2020; float chromaModeBits;
    float4 colorRow0; float4 colorRow1; float4 colorRow2; float4 colorControls; float4 colorFlags;
};
Texture2D<float4> packedYuy2 : register(t0);
SamplerState packedLinearClamp : register(s1);
RWTexture2D<float4> linearRgb : register(u0);
float decode(float v) {
    if(transfer==0)return v;
    if(transfer==3)return pow(max(v,0),2.4);
    if(transfer==2)return v<0.081?v/4.5:pow((v+0.099)/1.099,1.0/0.45);
    return v<=0.04045?v/12.92:pow((v+0.055)/1.055,2.4);
}
float2 pairChroma(uint pairX, uint y) { return packedYuy2[uint2(pairX, y)].yw; }

// Native 4:2:2 chroma is horizontally subsampled. The legacy path reused one
// chroma pair for both luma pixels, which is nearest-neighbour chroma and
// leaves colour steps on capture-card edges. Reconstruct the missing sample
// positions linearly; unknown siting uses the MPEG/HD-video left convention.
float2 reconstructChroma422(uint x, uint y)
{
    const uint mode=uint(chromaModeBits);
    const uint pairX=x/2;
    if(mode==0)return pairChroma(pairX,y);
    const float horizontalOrigin=(mode==2||mode==4||mode==6)?0.5:0.0;
    const float position=(float(x)-horizontalOrigin)*0.5;
    // Chroma occupies the same byte lanes in every packed pair texel, so the
    // texture unit's linear filter interpolates exactly the two chroma taps;
    // the luma lanes it also filters are simply discarded.
    const float chromaWidth=float((width+1)/2);
    return packedYuy2.SampleLevel(packedLinearClamp,float2((position+0.5)/chromaWidth,(float(y)+0.5)/float(height)),0).yw;
}

[numthreads(16,16,1)]
void main(uint3 p : SV_DispatchThreadID) {
    if(p.x>=width||p.y>=height)return;
    const float4 pair=packedYuy2[uint2(p.x/2,p.y)];
    float y=(p.x&1)?pair.z:pair.x;
    float2 uv=reconstructChroma422(p.x,p.y)-128.0/255.0;
    if(limited>0.5){y=(y-16.0/255.0)*(255.0/219.0);uv*=255.0/224.0;}
    // Preserve head/footroom until the final RGB gamut clamp. Clipping Y
    // before the matrix changes saturated pixels with below-black luma.
    float3 rgb=matrix709>0.5?float3(y+1.5748*uv.y,y-0.187324*uv.x-0.468124*uv.y,y+1.8556*uv.x):
        float3(y+1.402*uv.y,y-0.344136*uv.x-0.714136*uv.y,y+1.772*uv.x);
    if(matrix709>1.5)rgb=float3(y+1.4746*uv.y,y-.164553*uv.x-.571353*uv.y,y+1.8814*uv.x);
    rgb=saturate(rgb);
    float3 decoded=float3(decode(rgb.r),decode(rgb.g),decode(rgb.b));
    if(primaries2020>0.5)decoded=HdrTo709(decoded);
    if(colorFlags.x>0.5){ColorGradeParams grade={colorRow0,colorRow1,colorRow2,colorControls,colorFlags};decoded=ColorGradeApplyWithFlags(decoded,grade);}
    linearRgb[p.xy]=float4(decoded,1);
}
