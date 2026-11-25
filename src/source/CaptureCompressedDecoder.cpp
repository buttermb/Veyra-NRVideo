#include "veyra/source/CaptureCompressedDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <d3d12.h>
#include <thread>
#include <emmintrin.h>
#include <format>

#include "veyra/Log.h"
#include "veyra/media/FFmpegVideoDecoder.h"
#include "veyra/pipeline/CaptureUploadFrame.h"
#include "veyra/pipeline/FramePacket.h"
#include "veyra/pipeline/ColorMetadata.h"
#include "veyra/source/CaptureColorOverride.h"

namespace veyra::source {

pipeline::ColorDescription resolveCompressedCaptureColor(AVFrame& frame,const pipeline::ColorDescription& fallback,unsigned rangeOverride) {
    auto color=pipeline::resolveFrameColor(frame,fallback);
    if(rangeOverride){
        applyCaptureColorOverride(color,rangeOverride);
        frame.color_range=color.range==pipeline::ColorRange::Limited?AVCOL_RANGE_MPEG:AVCOL_RANGE_JPEG;
    }
    return color;
}

namespace {

AVCodecID captureAvCodecId(CaptureCodec codec)
{
    switch (codec) {
    case CaptureCodec::Mjpeg: return AV_CODEC_ID_MJPEG;
    case CaptureCodec::H264: return AV_CODEC_ID_H264;
    case CaptureCodec::Hevc: return AV_CODEC_ID_HEVC;
    case CaptureCodec::Av1: return AV_CODEC_ID_AV1;
    case CaptureCodec::Vp9: return AV_CODEC_ID_VP9;
    case CaptureCodec::None: break;
    }
    return AV_CODEC_ID_NONE;
}

// swscale colour coefficients for the bitstream's declared matrix; the same
// coefficients are used for both sides so no hidden matrix conversion is
// introduced, only the range change.
int swsColorspaceOf(AVColorSpace space, unsigned height)
{
    switch (space) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
    default: break;
    }
    return height > 576 ? SWS_CS_ITU709 : SWS_CS_ITU601;
}

void releaseCompressedPayload(void* opaque, uint8_t*)
{
    delete static_cast<CompressedPayloadPtr*>(opaque);
}

} // namespace

void CompressedPayload::assign(const uint8_t* data, size_t bytes)
{
    // Never shrink the backing vector: a smaller packet must not make the next
    // larger packet pay for value-initializing bytes that memcpy immediately
    // overwrites. `size` remains the logical packet length; the retained tail
    // only supplies FFmpeg's required zero padding.
    const size_t required = bytes + AV_INPUT_BUFFER_PADDING_SIZE;
    if (storage.size() < required) storage.resize(required);
    if (bytes != 0) std::memcpy(storage.data(), data, bytes);
    std::memset(storage.data() + bytes, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    size = bytes;
}

struct CaptureCompressedDecoder::Impl {
    CaptureCodec codec = CaptureCodec::None;
    unsigned width = 0, height = 0;
    media::FFmpegVideoDecoder decoder;
    AVCodecParameters* parameters = nullptr;
    SwsContext* sws = nullptr;
    bool nativeSoftwareExtent = false;
    ID3D12Device* uploadDevice = nullptr;
    std::atomic<int> softwareFormat{int(AV_PIX_FMT_NONE)};
    bool stream = false;               // setStreamProfile(): PC / Xbox streaming
    bool hardwareRequested = false;
    bool hardware = false;
    bool hardwareChecked = false;
    bool waiting = false;
    bool depthWarningLogged = false;
    std::string error;
    uint64_t frames = 0, failures = 0, fallbacks = 0;
    double decodeMs = 0, convertMs = 0;

    ~Impl() { close(); }

    AVPixelFormat softwareOutputFormat(const AVFrame& frame) const
    {
        const auto* description = av_pix_fmt_desc_get(AVPixelFormat(frame.format));
        return description && description->comp[0].depth > 8 ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
    }

    AVFrame* allocateSoftwareTarget(AVPixelFormat format, int outputWidth, int outputHeight) const
    {
        if (uploadDevice) {
            if (AVFrame* upload = pipeline::allocCaptureUploadFrame(uploadDevice, format, unsigned(outputWidth), unsigned(outputHeight))) return upload;
        }
        AVFrame* target = av_frame_alloc();
        if (!target) return nullptr;
        target->format = int(format);
        target->width = outputWidth;
        target->height = outputHeight;
        if (av_frame_get_buffer(target, 256) < 0) {
            av_frame_free(&target);
            return nullptr;
        }
        return target;
    }

    // Planar 10-bit FFmpeg output stores the sample in the low bits of uint16;
    // P010 stores the same sample in the high bits. The analysis proxy must be
    // the actual P010 container bytes because the graph reads the high byte.
    void samplePlanar10P010Proxy(AVFrame& target, const AVFrame& frame)
    {
        auto* info = pipeline::captureUploadFrame(&target);
        if (!info || info->pixelBytes != 2 || frame.format != AV_PIX_FMT_YUV420P10LE || !frame.data[0]) return;
        uint8_t* out = info->proxy;
        for (unsigned y = 0; y < pipeline::CaptureUploadFrame::kProxyHeight; ++y) {
            const unsigned row = y * unsigned(frame.height) / pipeline::CaptureUploadFrame::kProxyHeight;
            const auto* line = reinterpret_cast<const uint16_t*>(frame.data[0] + ptrdiff_t(row) * frame.linesize[0]);
            for (unsigned x = 0; x < pipeline::CaptureUploadFrame::kProxyWidth; ++x) {
                const uint16_t value = uint16_t(line[x * unsigned(frame.width) / pipeline::CaptureUploadFrame::kProxyWidth] << 6);
                *out++ = uint8_t(value);
                *out++ = uint8_t(value >> 8);
            }
        }
        info->proxyValid = true;
    }

    // 8-bit planar 4:2:0 of the target size goes straight to NV12 (luma row
    // copies, chroma interleave). This is a pure repacking: unlike swscale it
    // cannot introduce an extra range/resize filtering step. MJPEG 4:2:2 also
    // uses it after averaging vertical chroma pairs; other codecs' 4:2:2 stays
    // on swscale because that subsampling conversion needs a real filter.
    // The writer never reads the target, so the target may be write-combined
    // GPU upload memory.
    bool planarToNv12(const AVFrame& frame, AVFrame* target)
    {
        const unsigned outputWidth = unsigned(target->width), outputHeight = unsigned(target->height);
        const bool is422 = frame.format == AV_PIX_FMT_YUVJ422P || frame.format == AV_PIX_FMT_YUV422P;
        const bool is420 = frame.format == AV_PIX_FMT_YUVJ420P || frame.format == AV_PIX_FMT_YUV420P;
        const bool supported = is420 || (codec == CaptureCodec::Mjpeg && is422);
        if (!supported || frame.width != int(outputWidth) || frame.height != int(outputHeight) ||
            outputWidth % 2 || outputHeight % 2 || !frame.data[0] || !frame.data[1] || !frame.data[2] || !target->data[0] || !target->data[1] ||
            target->linesize[0] < int(outputWidth) || target->linesize[1] < int(outputWidth)) return false;
        // Decoder and upload pitches commonly agree at widths aligned to the
        // D3D12 pitch granularity (for example 3840). Copy the whole plane in
        // one call then instead of issuing thousands of row copies per frame.
        if (target->linesize[0] == frame.linesize[0] && outputHeight > 1)
            std::memcpy(target->data[0], frame.data[0], size_t(frame.linesize[0]) * (outputHeight - 1) + outputWidth);
        else for (unsigned y = 0; y < outputHeight; ++y)
            std::memcpy(target->data[0] + ptrdiff_t(y) * target->linesize[0], frame.data[0] + ptrdiff_t(y) * frame.linesize[0], outputWidth);
        const bool streaming = pipeline::captureUploadDirectTarget(*target);
        const unsigned chromaWidth = outputWidth / 2;
        for (unsigned row = 0; row < outputHeight / 2; ++row) {
            const unsigned sourceRow = is422 ? row * 2 : row;
            const uint8_t* u0 = frame.data[1] + ptrdiff_t(sourceRow) * frame.linesize[1];
            const uint8_t* v0 = frame.data[2] + ptrdiff_t(sourceRow) * frame.linesize[2];
            uint8_t* out = target->data[1] + ptrdiff_t(row) * target->linesize[1];
            unsigned x = 0;
            if (is422) {
                const uint8_t* u1 = u0 + frame.linesize[1];
                const uint8_t* v1 = v0 + frame.linesize[2];
                for (; x + 16 <= chromaWidth; x += 16) {
                    const __m128i u = _mm_avg_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(u0 + x)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(u1 + x)));
                    const __m128i v = _mm_avg_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(v0 + x)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(v1 + x)));
                    pipeline::captureStore128(out + 2 * x, _mm_unpacklo_epi8(u, v), streaming);
                    pipeline::captureStore128(out + 2 * x + 16, _mm_unpackhi_epi8(u, v), streaming);
                }
                for (; x < chromaWidth; ++x) {
                    out[2 * x] = uint8_t((unsigned(u0[x]) + u1[x] + 1) >> 1);
                    out[2 * x + 1] = uint8_t((unsigned(v0[x]) + v1[x] + 1) >> 1);
                }
            } else {
                // Native 4:2:0 only changes planar storage into NV12's UVUV
                // interleave. Do not average a row with itself through the
                // MJPEG 4:2:2 path.
                for (; x + 16 <= chromaWidth; x += 16) {
                    const __m128i u = _mm_loadu_si128(reinterpret_cast<const __m128i*>(u0 + x));
                    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(v0 + x));
                    pipeline::captureStore128(out + 2 * x, _mm_unpacklo_epi8(u, v), streaming);
                    pipeline::captureStore128(out + 2 * x + 16, _mm_unpackhi_epi8(u, v), streaming);
                }
                for (; x < chromaWidth; ++x) { out[2 * x] = u0[x]; out[2 * x + 1] = v0[x]; }
            }
        }
        pipeline::finishCaptureStreamingStores(streaming);
        pipeline::sampleCaptureUploadProxy(*target, frame.data[0], size_t(frame.linesize[0]), false);
        return true;
    }

    // Planar YUV420P10 is the normal software output of 10-bit HEVC/AV1/VP9.
    // Repack it as P010 by shifting each low-bit 10-bit sample into the high
    // bits of the 16-bit container. Already-semiplanar P010 is copied as-is.
    // This is still a pure repacking path: no swscale filtering, range change,
    // or precision loss is introduced.
    bool planarToP010(const AVFrame& frame, AVFrame* target)
    {
        const unsigned outputWidth = unsigned(target->width), outputHeight = unsigned(target->height);
        const bool planar10 = frame.format == AV_PIX_FMT_YUV420P10LE;
        const bool semiplanar10 = frame.format == AV_PIX_FMT_P010;
        if ((!planar10 && !semiplanar10) || target->format != AV_PIX_FMT_P010 ||
            frame.width != int(outputWidth) || frame.height != int(outputHeight) ||
            outputWidth % 2 || outputHeight % 2 || !frame.data[0] || !frame.data[1] || !target->data[0] || !target->data[1] ||
            target->linesize[0] < int(outputWidth * 2) || target->linesize[1] < int(outputWidth * 2)) return false;
        const size_t rowBytes = size_t(outputWidth) * 2;
        const bool streaming = pipeline::captureUploadDirectTarget(*target);
        if (semiplanar10) {
            if (target->linesize[0] == frame.linesize[0] && outputHeight > 1)
                std::memcpy(target->data[0], frame.data[0], size_t(frame.linesize[0]) * (outputHeight - 1) + rowBytes);
            else for (unsigned y = 0; y < outputHeight; ++y)
                std::memcpy(target->data[0] + ptrdiff_t(y) * target->linesize[0], frame.data[0] + ptrdiff_t(y) * frame.linesize[0], rowBytes);
            const unsigned chromaRows = outputHeight / 2;
            if (target->linesize[1] == frame.linesize[1] && chromaRows > 1)
                std::memcpy(target->data[1], frame.data[1], size_t(frame.linesize[1]) * (chromaRows - 1) + rowBytes);
            else for (unsigned y = 0; y < chromaRows; ++y)
                std::memcpy(target->data[1] + ptrdiff_t(y) * target->linesize[1], frame.data[1] + ptrdiff_t(y) * frame.linesize[1], rowBytes);
        } else {
            if (!frame.data[2] || frame.linesize[0] < int(rowBytes) ||
                frame.linesize[1] < int(rowBytes / 2) || frame.linesize[2] < int(rowBytes / 2)) return false;
            for (unsigned y = 0; y < outputHeight; ++y) {
                const auto* in = reinterpret_cast<const uint16_t*>(frame.data[0] + ptrdiff_t(y) * frame.linesize[0]);
                uint8_t* out = target->data[0] + ptrdiff_t(y) * target->linesize[0];
                unsigned x = 0;
                for (; x + 8 <= outputWidth; x += 8) {
                    const __m128i value = _mm_slli_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in + x)), 6);
                    pipeline::captureStore128(out + 2 * x, value, streaming);
                }
                for (; x < outputWidth; ++x) {
                    const uint16_t value = uint16_t(in[x] << 6);
                    out[2 * x] = uint8_t(value);
                    out[2 * x + 1] = uint8_t(value >> 8);
                }
            }
            const unsigned chromaWidth = outputWidth / 2;
            for (unsigned row = 0; row < outputHeight / 2; ++row) {
                const auto* u0 = reinterpret_cast<const uint16_t*>(frame.data[1] + ptrdiff_t(row) * frame.linesize[1]);
                const auto* v0 = reinterpret_cast<const uint16_t*>(frame.data[2] + ptrdiff_t(row) * frame.linesize[2]);
                uint8_t* out = target->data[1] + ptrdiff_t(row) * target->linesize[1];
                unsigned x = 0;
                for (; x + 8 <= chromaWidth; x += 8) {
                    const __m128i u = _mm_slli_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(u0 + x)), 6);
                    const __m128i v = _mm_slli_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(v0 + x)), 6);
                    pipeline::captureStore128(out + 4 * x, _mm_unpacklo_epi16(u, v), streaming);
                    pipeline::captureStore128(out + 4 * x + 16, _mm_unpackhi_epi16(u, v), streaming);
                }
                for (; x < chromaWidth; ++x) {
                    const uint16_t u = uint16_t(u0[x] << 6), v = uint16_t(v0[x] << 6);
                    out[4 * x] = uint8_t(u);
                    out[4 * x + 1] = uint8_t(u >> 8);
                    out[4 * x + 2] = uint8_t(v);
                    out[4 * x + 3] = uint8_t(v >> 8);
                }
            }
        }
        pipeline::finishCaptureStreamingStores(streaming);
        if (planar10) samplePlanar10P010Proxy(*target, frame);
        else pipeline::sampleCaptureUploadProxy(*target, frame.data[0], size_t(frame.linesize[0]), false);
        return true;
    }

    void copyMetadataSideData(const AVFrame& frame, AVFrame* target, AVFrameSideDataType type)
    {
        // Targets are pooled and reused. Remove stale metadata first: a frame
        // whose bitstream drops HDR metadata must not inherit the previous one.
        while (av_frame_get_side_data(target, type)) av_frame_remove_side_data(target, type);
        const AVFrameSideData* source = av_frame_get_side_data(&frame, type);
        if (!source) return;
        AVFrameSideData* copy = av_frame_new_side_data(target, type, source->size);
        if (!copy) return;
        std::memcpy(copy->data, source->data, source->size);
        av_dict_copy(&copy->metadata, source->metadata, 0);
    }

    void copyFrameProperties(const AVFrame& frame, AVFrame* target, bool targetFullRange)
    {
        target->pts = frame.pts;
        target->best_effort_timestamp = frame.best_effort_timestamp;
        target->pkt_dts = frame.pkt_dts;
        target->duration = frame.duration;
        target->time_base = frame.time_base;
        target->flags = frame.flags;
        target->colorspace = frame.colorspace;
        target->color_primaries = frame.color_primaries;
        target->color_trc = frame.color_trc;
        target->color_range = targetFullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        target->chroma_location = frame.chroma_location;
        // Software conversion produces a fresh AVFrame. Preserve the HDR
        // metadata the pipeline actually consumes; otherwise a compressed HDR
        // capture loses its mastering peak/MaxCLL whenever it uses the software
        // fallback even though the hardware path carries those side data through.
        copyMetadataSideData(frame, target, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        copyMetadataSideData(frame, target, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
        // JPEG/MJPEG chroma is centred when the bitstream carries no newer
        // explicit siting tag; H.264/HEVC keep their own MPEG-style default.
        if (target->chroma_location == AVCHROMA_LOC_UNSPECIFIED && codec == CaptureCodec::Mjpeg)
            target->chroma_location = AVCHROMA_LOC_CENTER;
    }

    void close()
    {
        if (sws) { sws_freeContext(sws); sws = nullptr; }
        if (parameters) { avcodec_parameters_free(&parameters); parameters = nullptr; }
        if (uploadDevice) { uploadDevice->Release(); uploadDevice = nullptr; }
        softwareFormat.store(int(AV_PIX_FMT_NONE), std::memory_order_relaxed);
        decoder.close();
        codec = CaptureCodec::None;
        width = height = 0;
        hardwareRequested = hardware = hardwareChecked = waiting = false;
        decoder.setPreferNativeAv1Hardware(false);
        depthWarningLogged = false;
        frames = failures = fallbacks = 0;
        decodeMs = convertMs = 0;
        error.clear();
    }

    // Streams only: a GPU without this codec (for example AV1 before RTX 30)
    // or a driver that rejects it fails before the first picture. Switch to
    // software once; the caller asks the host for a key frame anyway.
    void fallBackBeforeFirstPicture()
    {
        if (!stream || !hardware || frames != 0) return;
        log::warn("capture-decode", std::format("codec={} hardware decode failed before the first picture; switching to the software decoder",
            captureCodecKey(codec)));
        decoder.close();
        hardware = false;
        if (!openSoftwareDecoder()) {
            error = "software fallback after a failed hardware decoder failed";
            return;
        }
        decoder.recoverAtKeyframe();
        ++fallbacks;
    }

    bool openSoftwareDecoder()
    {
        if (parameters == nullptr) return false;
        // Slice/tile threading is safe for live capture because low-delay mode
        // explicitly disables frame threading: work is parallel inside the same
        // picture, so no extra frame of output latency is introduced. This was
        // previously limited to streaming sources, but capture-card software
        // fallback needs the same throughput when hardware decode is absent or
        // rejects a codec. MJPEG stays single-threaded per decoder because the
        // capture source already runs multiple independent MJPEG decoders.
        const unsigned threads = codec != CaptureCodec::Mjpeg
            ? std::clamp(std::thread::hardware_concurrency() / 2u, 2u, 8u) : 1u;
        return decoder.openSoftware(parameters, 1, 10000000, threads, true);
    }

    bool convertSoftware(const AVFrame& frame, AVFrame* target)
    {
        AVPixelFormat sourceFormat = AVPixelFormat(frame.format);
        if (sourceFormat == AV_PIX_FMT_YUVJ422P) sourceFormat = AV_PIX_FMT_YUV422P;
        else if (sourceFormat == AV_PIX_FMT_YUVJ420P) sourceFormat = AV_PIX_FMT_YUV420P;
        else if (sourceFormat == AV_PIX_FMT_YUVJ444P) sourceFormat = AV_PIX_FMT_YUV444P;
        const AVPixelFormat outputFormat = AVPixelFormat(target->format);
        if (const auto* description = av_pix_fmt_desc_get(sourceFormat)) {
            if (description->comp[0].depth > 10 && !depthWarningLogged) {
                depthWarningLogged = true;
                log::warn("capture-decode", std::format(
                    "codec={} bitstream is {}-bit; converting to the 10-bit P010 capture contract",
                    captureCodecKey(codec), description->comp[0].depth));
            }
        }
        if ((outputFormat == AV_PIX_FMT_NV12 && planarToNv12(frame, target)) ||
            (outputFormat == AV_PIX_FMT_P010 && planarToP010(frame, target))) {
            // Repacking never changes code values. Preserve explicit full range
            // instead of quantizing it to limited; use limited only as the
            // ordinary compressed-video fallback when the bitstream is silent.
            const bool fullRange = codec == CaptureCodec::Mjpeg || frame.color_range == AVCOL_RANGE_JPEG ||
                frame.format == AV_PIX_FMT_YUVJ420P || frame.format == AV_PIX_FMT_YUVJ422P;
            copyFrameProperties(frame, target, fullRange);
            return true;
        }
        // swscale cannot supply the analysis pixels of an upload-backed target.
        pipeline::useCaptureUploadCpuPlanes(*target);
        sws = sws_getCachedContext(sws, frame.width, frame.height, sourceFormat,
            target->width, target->height, outputFormat, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws == nullptr) {
            error = "swscale context creation failed";
            ++failures;
            return false;
        }
        const int* coefficients = sws_getCoefficients(swsColorspaceOf(frame.colorspace, unsigned(frame.height)));
        // MJPEG samples are full-range JPEG planes and the declared ingress
        // contract for that path is full-range NV12. Compressed video keeps
        // the bitstream's own range and lands on the standard limited target.
        const bool jpegCoded = frame.format == AV_PIX_FMT_YUVJ420P || frame.format == AV_PIX_FMT_YUVJ422P || frame.format == AV_PIX_FMT_YUVJ444P;
        const int sourceFullRange = (codec == CaptureCodec::Mjpeg || frame.color_range == AVCOL_RANGE_JPEG || jpegCoded) ? 1 : 0;
        const int targetFullRange = codec == CaptureCodec::Mjpeg ? 1 : 0;
        sws_setColorspaceDetails(sws, coefficients, sourceFullRange, coefficients, targetFullRange, 0, 1 << 16, 1 << 16);
        uint8_t* destination[4] = { target->data[0], target->data[1], nullptr, nullptr };
        const int destinationStride[4] = { target->linesize[0], target->linesize[1], 0, 0 };
        if (sws_scale(sws, frame.data, frame.linesize, 0, frame.height, destination, destinationStride) != target->height) {
            error = "swscale conversion failed";
            ++failures;
            return false;
        }
        copyFrameProperties(frame, target, targetFullRange != 0);
        return true;
    }
};

CaptureCompressedDecoder::CaptureCompressedDecoder() : p_(std::make_unique<Impl>()) {}
CaptureCompressedDecoder::~CaptureCompressedDecoder() = default;

bool CaptureCompressedDecoder::open(CaptureCodec codec, unsigned width, unsigned height,
    const uint8_t* extradata, size_t extradataBytes, ID3D12Device* device, ID3D12CommandQueue* queue)
{
    auto& p = *p_;
    p.close();
    if (codec == CaptureCodec::None || !width || !height) return false;
    p.codec = codec;
    p.width = width;
    p.height = height;
    // Software frames are written straight into the same upload-backed target
    // contract as capture. Keep one reference even when hardware open fails;
    // the decoder and device are both owned by the capture source and close in
    // reverse order.
    p.uploadDevice = device;
    if (p.uploadDevice) p.uploadDevice->AddRef();
    p.parameters = avcodec_parameters_alloc();
    if (p.parameters == nullptr) {
        p.error = "avcodec_parameters_alloc failed";
        return false;
    }
    p.parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    p.parameters->codec_id = captureAvCodecId(codec);
    p.parameters->width = int(width);
    p.parameters->height = int(height);
    if (extradata != nullptr && extradataBytes != 0) {
        p.parameters->extradata = static_cast<uint8_t*>(av_mallocz(extradataBytes + AV_INPUT_BUFFER_PADDING_SIZE));
        if (p.parameters->extradata == nullptr) {
            p.error = "extradata allocation failed";
            p.close();
            return false;
        }
        std::memcpy(p.parameters->extradata, extradata, extradataBytes);
        p.parameters->extradata_size = int(extradataBytes);
    }
    // MJPEG stays software: UVC drivers disagree about 4:2:0 vs 4:2:2 and the
    // hardware path only exists for 4:2:0 on one vendor. Everything else gets
    // the D3D12VA session on the shared Veyra device, with the software
    // decoder as the recorded fallback.
    if (codec != CaptureCodec::Mjpeg && device != nullptr && queue != nullptr) {
        p.hardwareRequested = true;
        bool opened = false;
        if (p.stream && codec == CaptureCodec::Hevc) {
            const LUID luid = device->GetAdapterLuid();
            opened = p.decoder.openD3D11VA(p.parameters, 1, 10000000,
                (uint64_t(uint32_t(luid.HighPart)) << 32) | uint64_t(luid.LowPart), device, true);
        } else {
            p.decoder.setPreferNativeAv1Hardware(p.stream);
            opened = p.decoder.openD3D12VA(p.parameters, 1, 10000000, device, queue, true);
        }
        if (opened) {
            p.hardware = true;
        } else {
            log::warn("capture-decode", std::format(
                "D3D12VA open failed codec={}; falling back to the software decoder", captureCodecKey(codec)));
        }
    }
    if (!p.hardware && !p.openSoftwareDecoder()) {
        p.error = std::format("no usable decoder for codec={}", captureCodecKey(codec));
        log::error("capture-decode", p.error);
        p.close();
        return false;
    }
    log::info("capture-decode", std::format(
        "compressed decoder opened codec={} {}x{} backend={} extradataBytes={}",
        captureCodecKey(codec), width, height, backendName(), extradataBytes));
    return true;
}

bool CaptureCompressedDecoder::decode(const uint8_t* data, size_t bytes, int64_t pts100ns,
    AVFrame* nv12Target, AVFrame** out, bool& hardware)
{
    if (data == nullptr && bytes == 0) return decodeOwned(nullptr, pts100ns, nv12Target, out, hardware);
    if (data == nullptr || bytes == 0) {
        *out = nullptr;
        hardware = false;
        p_->error = "decode called without compressed payload bytes";
        ++p_->failures;
        return false;
    }
    // Compatibility path for streaming sources. CaptureCardSource uses
    // decodeOwned() with its recycled payload allocation instead.
    auto payload = std::make_shared<CompressedPayload>();
    payload->assign(data, bytes);
    return decodeOwned(std::move(payload), pts100ns, nv12Target, out, hardware);
}

bool CaptureCompressedDecoder::decodeOwned(const CompressedPayloadPtr& payload, int64_t pts100ns,
    AVFrame* nv12Target, AVFrame** out, bool& hardware)
{
    auto& p = *p_;
    *out = nullptr;
    hardware = false;
    p.waiting = false;
    const bool receiveOnly = payload == nullptr;
    if (!p.decoder.opened() || (!receiveOnly && (payload->size == 0 || payload->size > size_t(INT_MAX)))) {
        p.error = "decode called without an open decoder or a valid payload";
        ++p.failures;
        return false;
    }
    // Two attempts at most: the second one re-sends the same payload after a
    // hardware decoder whose first frame cannot be imported has been replaced
    // by the software decoder.
    const auto started = std::chrono::steady_clock::now();
    const auto average = [](double& value, std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
        const double ms = std::chrono::duration<double, std::milli>(to - from).count();
        value = value > 0 ? value * 0.95 + ms * 0.05 : ms;
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!receiveOnly) {
            AVPacket* packet = av_packet_alloc();
            if (packet == nullptr) {
                p.error = "av_packet_alloc failed";
                ++p.failures;
                return false;
            }
            // avcodec_send_packet() references rather than copies packet
            // buffers. Hand it one refcounted view of the capture payload
            // instead of paying for a second full MJPEG/H.264 copy in
            // av_new_packet().
            // The shared_ptr holder is released by FFmpeg's AVBuffer callback,
            // so decoder reference frames may retain the packet safely even if
            // the worker has already moved on.
            auto* holder = new CompressedPayloadPtr(payload);
            AVBufferRef* buffer = av_buffer_create(const_cast<uint8_t*>(payload->data()), int(payload->size),
                releaseCompressedPayload, holder, AV_BUFFER_FLAG_READONLY);
            if (buffer == nullptr) {
                delete holder;
                av_packet_free(&packet);
                p.error = "compressed payload buffer reference failed";
                ++p.failures;
                return false;
            }
            packet->buf = buffer;
            packet->data = buffer->data;
            packet->size = int(buffer->size);
            packet->pts = pts100ns;
            packet->dts = AV_NOPTS_VALUE; // capture PTS is not decode order for B frames
            const bool sent = p.decoder.sendPacket(packet);
            av_packet_free(&packet);
            if (!sent) {
                p.error = "decoder rejected the compressed payload";
                ++p.failures;
                p.fallBackBeforeFirstPicture();
                return false;
            }
        }
        const AVFrame* frame = p.decoder.receiveFrame();
        // Damaged compressed input can be concealed into a frame without a
        // fatal avcodec error. Reject it before it reaches the stream mailbox;
        // XboxSessionSource then flushes references and requests an IDR.
        if (frame && p.stream &&
            ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags)) {
            p.error = "damaged compressed stream frame";
            ++p.failures;
            return false;
        }
        if (frame == nullptr) {
            if (p.decoder.receiveStatus() == media::DecodeReceiveStatus::Error) {
                p.error = "decoder receive failed";
                ++p.failures;
                p.fallBackBeforeFirstPicture();
            } else {
                p.error.clear();
                p.waiting = true;
            }
            return false;
        }
        if (p.hardware && !p.hardwareChecked) {
            p.hardwareChecked = true;
            if (!p.decoder.hardwareFrameImportable()) {
                log::warn("capture-decode", std::format(
                    "codec={} hardware frame is not an importable NV12/P010 surface; switching to the software decoder",
                    captureCodecKey(p.codec)));
                p.decoder.close();
                if (!p.openSoftwareDecoder()) {
                    p.error = "software fallback after an unimportable hardware frame failed";
                    ++p.failures;
                    return false;
                }
                p.hardware = false;
                p.decoder.recoverAtKeyframe();
                ++p.fallbacks;
                if (receiveOnly) { p.waiting=true; return false; }
                continue; // only a new key frame can rebuild the lost reference chain
            }
        }
        if (p.hardware) {
            *out = const_cast<AVFrame*>(frame);
            hardware = true;
            ++p.frames;
            p.error.clear();
            return true;
        }
        const auto decoded = std::chrono::steady_clock::now();
        const AVPixelFormat desiredFormat = p.softwareOutputFormat(*frame);
        p.softwareFormat.store(int(desiredFormat), std::memory_order_relaxed);
        const bool native = p.stream && p.nativeSoftwareExtent;
        const int outputWidth = native ? frame->width : int(p.width);
        const int outputHeight = native ? frame->height : int(p.height);
        if (outputWidth <= 0 || outputHeight <= 0 || outputWidth > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            outputHeight > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            p.error = "invalid decoded video extent";
            ++p.failures;
            return false;
        }
        AVFrame* target = nv12Target;
        const bool sameFormat = target && target->format == int(desiredFormat);
        const bool compatible = sameFormat && target->width == outputWidth && target->height == outputHeight &&
            target->data[0] && target->data[1];
        if (!compatible) {
            AVFrame* replacement = p.allocateSoftwareTarget(desiredFormat, outputWidth, outputHeight);
            if (replacement && native && sameFormat) {
                // The stream caller reuses this AVFrame while the negotiated picture size changes.
                av_frame_unref(target);
                av_frame_move_ref(target, replacement);
                av_frame_free(&replacement);
            } else {
                target = replacement;
            }
        }
        if (target == nullptr) {
            p.error = "software output target allocation failed";
            ++p.failures;
            return false;
        }
        // Internally allocated upload frames start with direct=false until the
        // target is selected. This is also harmless for the worker-supplied
        // target, which has already made the same selection.
        pipeline::selectCaptureUploadTarget(*target);
        if (!p.convertSoftware(*frame, target)) {
            if (target != nv12Target) av_frame_free(&target);
            return false;
        }
        average(p.decodeMs, started, decoded);
        average(p.convertMs, decoded, std::chrono::steady_clock::now());
        *out = target;
        hardware = false;
        ++p.frames;
        p.error.clear();
        return true;
    }
    p.error = "hardware-to-software fallback did not converge";
    ++p.failures;
    return false;
}

void CaptureCompressedDecoder::preserveSoftwareStreamExtent(bool enabled) { p_->nativeSoftwareExtent=enabled; }

bool CaptureCompressedDecoder::opened() const { return p_->decoder.opened(); }
bool CaptureCompressedDecoder::hardwareActive() const { return p_->hardware; }
bool CaptureCompressedDecoder::waitingForInput() const { return p_->waiting; }
int CaptureCompressedDecoder::softwareTargetFormat() const { return p_->softwareFormat.load(std::memory_order_relaxed); }
void CaptureCompressedDecoder::recoverAtKeyframe() { p_->decoder.recoverAtKeyframe(); }
const char* CaptureCompressedDecoder::backendName() const
{
    if (p_->hardware) return p_->stream && p_->codec == CaptureCodec::Hevc ? "d3d11va" : "d3d12va";
    if (p_->hardwareRequested) return "software(after d3d12va)";
    return p_->decoder.opened() ? "software" : "none";
}
const std::string& CaptureCompressedDecoder::lastError() const { return p_->error; }
uint64_t CaptureCompressedDecoder::framesDecoded() const { return p_->frames; }
uint64_t CaptureCompressedDecoder::decodeFailures() const { return p_->failures; }
uint64_t CaptureCompressedDecoder::hardwareFallbacks() const { return p_->fallbacks; }
double CaptureCompressedDecoder::decodeMsAverage() const { return p_->decodeMs; }
double CaptureCompressedDecoder::convertMsAverage() const { return p_->convertMs; }
void CaptureCompressedDecoder::close() { p_->close(); }
void CaptureCompressedDecoder::setStreamProfile(bool enabled) { p_->stream = enabled; }
bool CaptureCompressedDecoder::hardwareSurface(pipeline::HardwareSurfaceInput& out) const
{
    const auto& view = p_->decoder.hardwareSurface();
    if (!p_->hardware || view.texture == nullptr) return false;
    out.texture = view.texture;
    out.subresourceIndex = view.subresourceIndex;
    out.waitFence = view.waitFence;
    out.waitValue = view.waitValue;
    out.textureWidth = view.textureWidth;
    out.textureHeight = view.textureHeight;
    return true;
}

} // namespace veyra::source
