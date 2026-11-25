#pragma once

// Compressed capture payload decoder (MPEG chain stage 3). Owns the FFmpeg
// decoder that turns the direct-connect capture payloads (MJPEG / H.264 /
// HEVC / AV1 / VP9) into frames the capture ingress understands. It is used
// from the single capture decode worker thread and is not thread safe.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "veyra/source/CaptureCodec.h"

struct AVFrame;
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace veyra::pipeline { struct ColorDescription; struct HardwareSurfaceInput; }

namespace veyra::source {

// Apply the capture range after decode so the graph's explicit AVFrame
// metadata read preserves the override on subsequent color resolution.
pipeline::ColorDescription resolveCompressedCaptureColor(AVFrame&,const pipeline::ColorDescription&,unsigned rangeOverride);

struct CompressedPayload {
    // FFmpeg bitstream readers may inspect AV_INPUT_BUFFER_PADDING_SIZE bytes
    // after the logical end. Keep those bytes in the vector while `size`
    // remains the compressed payload length handed to the decoder.
    std::vector<uint8_t> storage;
    size_t size = 0;

    void assign(const uint8_t* data, size_t bytes);
    const uint8_t* data() const { return storage.data(); }
};
using CompressedPayloadPtr = std::shared_ptr<CompressedPayload>;

class CaptureCompressedDecoder {
public:
    CaptureCompressedDecoder();
    ~CaptureCompressedDecoder();

    CaptureCompressedDecoder(const CaptureCompressedDecoder&) = delete;
    CaptureCompressedDecoder& operator=(const CaptureCompressedDecoder&) = delete;

    // Backend order: MJPEG always decodes in software (drivers disagree on
    // 4:2:0 vs 4:2:2 and there is no 4:2:2 hardware path); everything else
    // tries D3D12VA on the shared Veyra device first. A hardware decoder whose
    // first frame cannot be imported by the graph is closed and replaced by
    // the software decoder, which converts into `nv12Target` instead.
    //
    // `extradata` (SPS/PPS from MPEG2VIDEOINFO) may be empty; the bitstream's
    // in-band parameter sets are then required.
    // Streaming sources (PC and Xbox streaming) call this before open(): AV1
    // then uses the native decoder with D3D12VA (the libdav1d decoder FFmpeg
    // would pick has no hardware path), HEVC decodes through D3D11VA on its own
    // device (HEVC D3D12VA fails at 4K and can remove the shared device on
    // NVIDIA drivers, see FFmpegVideoDecoder::openD3D11VA), and the software
    // decoder runs several low-delay threads. Both capture and stream decoders
    // retry the same payload in software if hardware fails before the first picture.
    void setStreamProfile(bool enabled);
    // Xbox targets begin with an estimate. In this mode the decoder can resize
    // an owned CPU target to the actual coded dimensions after the first frame.
    void preserveSoftwareStreamExtent(bool enabled);

    bool open(CaptureCodec codec, unsigned width, unsigned height,
        const uint8_t* extradata, size_t extradataBytes,
        ID3D12Device* device, ID3D12CommandQueue* queue);

    // Decodes one payload; data=nullptr, bytes=0 receives remaining output
    // without submitting input or EOS. Drain this way before the next payload.
    // On success:
    //  - hardware=true: `*out` is a decoder-owned D3D12 surface whose lifetime
    //    the caller must not extend past the next decode();
    //  - hardware=false: `*out` is caller-owned. It is `nv12Target` when that
    //    target has the output format the picture needs; otherwise it is a
    //    newly allocated frame. NV12 remains the 8-bit output; 10-bit and
    //    deeper 4:2:0 sources use P010 instead of being quantized to 8-bit.
    //    `nv12Target` may be null, in which case the correct target is allocated.
    // Returns false when the payload produced no frame (pending input) or the
    // decoder reported an error; see lastError()/waitingForInput().
    bool decode(const uint8_t* data, size_t bytes, int64_t pts100ns,
        AVFrame* nv12Target, AVFrame** out, bool& hardware);

    // Same as decode(), but reuses the capture callback's already copied,
    // padding-equipped payload through FFmpeg's refcounted AVBuffer API
    // instead of copying it a second time into an AVPacket. A null payload is
    // the receive-only drain form.
    bool decodeOwned(const CompressedPayloadPtr& payload, int64_t pts100ns,
        AVFrame* nv12Target, AVFrame** out, bool& hardware);

    // For D3D11VA frames (AV_PIX_FMT_D3D11) the texture the graph reads is not
    // inside the AVFrame: this fills the borrowed view of the last frame.
    bool hardwareSurface(pipeline::HardwareSurfaceInput& out) const;

    bool opened() const;
    bool hardwareActive() const;
    bool waitingForInput() const;
    // Last decoded software output format, or AV_PIX_FMT_NONE before the first
    // software picture. Capture workers use it to recycle a matching target.
    int softwareTargetFormat() const;
    void recoverAtKeyframe();
    const char* backendName() const;
    const std::string& lastError() const;
    uint64_t framesDecoded() const;
    uint64_t decodeFailures() const;
    uint64_t hardwareFallbacks() const;
    // Running averages of the two halves of a software decode() call.
    double decodeMsAverage() const;
    double convertMsAverage() const;
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace veyra::source
