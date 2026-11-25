// N1: packed capture ingress. The driver's bytes are uploaded 1:1 into an
// R8G8B8A8_UNORM texture; this shader unpacks BGR24 / RGB555 / RGB565 /
// UYVY / YVYU with the same color contract as RgbToLinear / Yuy2ToLinear.
// packing: 1 BGR24, 2 RGB555, 3 RGB565, 4 UYVY, 5 YVYU.
#include "HdrColor.hlsli"
#include "HdrToSdr.hlsli"
#include "ColorGrade.hlsli"
cbuffer Params : register(b0) {
    uint width; uint height; uint transfer; uint limitedU;
    float limited; float matrix709; float primaries2020; uint packing;
    float hdrWorking; float sourcePeak; float targetPeak; float chromaModeBits;
    float4 colorRow0; float4 colorRow1; float4 colorRow2; float4 colorControls; float4 colorFlags;
};
Texture2D<float4> packedBytes : register(t0);
SamplerState packedLinearClamp : register(s1);
RWTexture2D<float4> linearRgb : register(u0);
float decode(float v) {
    if (transfer == 3) return pow(max(v, 0), 2.4);
    if (transfer == 2) return v < 0.081 ? v / 4.5 : pow((v + 0.099) / 1.099, 1.0 / 0.45);
    if (transfer == 0) return v;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}
uint byteAt(uint y, uint index) {
    const float4 t = packedBytes[uint2(index >> 2, y)];
    return uint(round(saturate(t[index & 3]) * 255.0));
}
uint wordAt(uint y,uint index){
    // A 32-bit packed word occupies exactly one RGBA8 texel. Reading that
    // texel once replaces four dependent byteAt loads; this is hot for v210
    // capture, which previously read sixteen texels per output pixel.
    const float4 t=packedBytes[uint2(index,y)];
    const uint4 b=uint4(round(saturate(t)*255.0));
    return b.x|(b.y<<8)|(b.z<<16)|(b.w<<24);
}
float2 packedChromaAt(uint pairX, uint y) {
    const float4 pair = packedBytes[uint2(pairX, y)];
    return packing == 4 ? pair.xz : pair.wy;
}
// Reconstruct the missing horizontal 4:2:2 chroma positions instead of
// repeating one sample across both luma pixels. Unknown siting follows MPEG
// left placement; center siting is JPEG-style placement.
float2 reconstructChroma422(uint x, uint y) {
    const uint mode = asuint(chromaModeBits);
    const uint pairX = x / 2;
    if (mode == 0) return packedChromaAt(pairX, y);
    const float horizontalOrigin = (mode == 2 || mode == 4 || mode == 6) ? 0.5 : 0.0;
    const float position = (float(x) - horizontalOrigin) * 0.5;
    // UYVY/YVYU keep chroma in fixed byte lanes of every pair texel. Hardware
    // filtering therefore performs the two-tap chroma interpolation in one
    // sample; the coincidentally filtered luma lanes are discarded.
    const float2 dimensions=float2((width+1)/2,height);
    const float4 filtered=packedBytes.SampleLevel(packedLinearClamp,float2(position+0.5,float(y)+0.5)/dimensions,0);
    return packing == 4 ? filtered.xz : filtered.wy;
}

float2 v210ChromaAt(uint pairX, uint y) {
    const uint base=(pairX/3)*4;
    const uint local=pairX%3;
    if(local==0){
        const uint a=wordAt(y,base);
        return float2(a&1023,(a>>20)&1023);
    }
    if(local==1){
        const uint b=wordAt(y,base+1),c=wordAt(y,base+2);
        return float2((b>>10)&1023,c&1023);
    }
    const uint c=wordAt(y,base+2),d=wordAt(y,base+3);
    return float2((c>>20)&1023,(d>>10)&1023);
}
float2 v210ChromaFromWords(uint4 words, uint local) {
    if(local==0)return float2(words.x&1023,(words.x>>20)&1023);
    if(local==1)return float2((words.y>>10)&1023,words.z&1023);
    return float2((words.z>>20)&1023,(words.w>>10)&1023);
}
float2 v210ChromaCached(uint pairX, uint y, int groupPairBase, uint4 words) {
    const int local=int(pairX)-groupPairBase;
    if(local>=0&&local<3)return v210ChromaFromWords(words,uint(local));
    return v210ChromaAt(pairX,y); // only needed at a six-pixel group boundary
}
// v210 uses the same horizontal 4:2:2 siting in 10-bit integer units. The
// current pixel's group words are reused, so interior pixels add no texture
// fetches for chroma reconstruction; only group-boundary taps read a neighbor.
float2 reconstructChromaV210(uint x, uint y, uint4 words) {
    const uint mode=asuint(chromaModeBits);
    const uint pairX=x/2;
    const int groupPairBase=int(x/6)*3;
    if(mode==0)return v210ChromaFromWords(words,pairX-uint(groupPairBase));
    const float horizontalOrigin=(mode==2||mode==4||mode==6)?0.5:0.0;
    const float position=(float(x)-horizontalOrigin)*0.5;
    const int base=int(floor(position));
    const float fraction=frac(position);
    const int last=int((width+1)/2)-1;
    const uint first=uint(clamp(base,0,last));
    const uint second=uint(clamp(base+1,0,last));
    const float2 a=v210ChromaCached(first,y,groupPairBase,words);
    // MPEG-left even luma positions land exactly on the stored chroma sample,
    // and edge clamping can make both taps identical. Do not decode/load the
    // neighbouring six-pixel group when it cannot contribute.
    if(fraction==0||first==second)return a;
    const float2 b=v210ChromaCached(second,y,groupPairBase,words);
    return lerp(a,b,fraction);
}
[numthreads(16,16,1)]
void main(uint3 p : SV_DispatchThreadID) {
    if (p.x >= width || p.y >= height) return;
    float3 rgb;
    if (packing == 6) {
        const uint base=(p.x/6)*4,i=p.x%6;
        const uint4 words=uint4(wordAt(p.y,base),wordAt(p.y,base+1),wordAt(p.y,base+2),wordAt(p.y,base+3));
        const uint a=words.x,b=words.y,c=words.z,d=words.w;
        const uint ys[6]={(a>>10)&1023,b&1023,(b>>20)&1023,(c>>10)&1023,d&1023,(d>>20)&1023};
        float y=float(ys[i])/1023.0;
        const float2 chroma=reconstructChromaV210(p.x,p.y,words);
        float2 uv=(chroma-512.0)/1023.0;
        if(limited>0.5){y=(float(ys[i])-64.0)/876.0;uv=(chroma-512.0)/896.0;}
        rgb=matrix709>1.5?float3(y+1.4746*uv.y,y-.164553*uv.x-.571353*uv.y,y+1.8814*uv.x)
            :matrix709>.5?float3(y+1.5748*uv.y,y-.187324*uv.x-.468124*uv.y,y+1.8556*uv.x)
            :float3(y+1.402*uv.y,y-.344136*uv.x-.714136*uv.y,y+1.772*uv.x);
        rgb=saturate(rgb);
    } else if(packing==7){
        const uint v=wordAt(p.y,p.x);
        rgb=float3((v>>22)&1023,(v>>12)&1023,(v>>2)&1023)/1023.0;
        if(limitedU!=0)rgb=saturate((rgb-64.0/1023.0)*(1023.0/876.0));
    } else if (packing == 4 || packing == 5) {
        const float4 pair = packedBytes[uint2(p.x / 2, p.y)];
        const bool second = (p.x & 1) != 0;
        float y;
        if (packing == 4) y = second ? pair.w : pair.y;
        else y = second ? pair.z : pair.x;
        float2 uv = reconstructChroma422(p.x, p.y) - 128.0 / 255.0;
        if (limited > 0.5) { y = (y - 16.0 / 255.0) * (255.0 / 219.0); uv *= 255.0 / 224.0; }
        rgb = matrix709 > 0.5 ? float3(y + 1.5748 * uv.y, y - 0.187324 * uv.x - 0.468124 * uv.y, y + 1.8556 * uv.x)
                              : float3(y + 1.402 * uv.y, y - 0.344136 * uv.x - 0.714136 * uv.y, y + 1.772 * uv.x);
        if (matrix709 > 1.5) rgb = float3(y + 1.4746 * uv.y, y - 0.164553 * uv.x - 0.571353 * uv.y, y + 1.8814 * uv.x);
        rgb = saturate(rgb);
    } else if (packing == 1) {
        const uint base = p.x * 3;
        rgb = float3(byteAt(p.y, base + 2), byteAt(p.y, base + 1), byteAt(p.y, base)) / 255.0;
    } else {
        const uint base = p.x * 2;
        const uint v = byteAt(p.y, base) | (byteAt(p.y, base + 1) << 8);
        if (packing == 2) rgb = float3(((v >> 10) & 31) / 31.0, ((v >> 5) & 31) / 31.0, (v & 31) / 31.0);
        else rgb = float3(((v >> 11) & 31) / 31.0, ((v >> 5) & 63) / 63.0, (v & 31) / 31.0);
    }
    if (limitedU != 0 && packing <= 3) rgb = saturate((rgb - 16.0 / 255.0) * (255.0 / 219.0));
    float3 decoded;
    if(transfer>=4){
        float3 nits;
        if(transfer==4){
            const float3 q=pow(saturate(rgb),32.0/2523.0);
            nits=10000.0*pow(max(q-3424.0/4096.0,0)/max(2413.0/128.0-2392.0/128.0*q,1e-6),16384.0/2610.0);
        }else{
            const float3 scene=select(rgb<=.5,rgb*rgb/3.0,(exp((rgb-.55991073)/.17883277)+.28466892)/12.0);
            nits=1000.0*scene*pow(max(dot(scene,float3(.2627,.6780,.0593)),0),.2);
        }
        float3 linear709=primaries2020>.5?HdrTo709(nits):nits;
        if(colorFlags.x>.5){ColorGradeParams grade={colorRow0,colorRow1,colorRow2,colorControls,colorFlags};linear709=ColorGradeApplyWithFlags(linear709/203.0,grade)*203.0;}
        decoded=hdrWorking>.5?linear709/80.0:HdrToSdr(linear709,sourcePeak,targetPeak);
    }else{
        decoded=float3(decode(rgb.r),decode(rgb.g),decode(rgb.b));
        if(primaries2020>.5)decoded=HdrTo709(decoded);
        if(colorFlags.x>.5){ColorGradeParams grade={colorRow0,colorRow1,colorRow2,colorControls,colorFlags};decoded=ColorGradeApplyWithFlags(decoded,grade);}
    }
    linearRgb[p.xy] = float4(decoded, 1);
}
