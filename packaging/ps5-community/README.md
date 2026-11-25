# Veyra PS5 Capture Community Edition

A Windows x64 capture player for a PS5 connected over HDMI. This NVIDIA build includes Video Denoise, NVIDIA Image Scaling (NIS), SMAA High, and 2× NVIDIA video frame generation. It was tested on a Portta DX20R and a GeForce RTX 5070 Ti. Start with the **PS5 Denoise + NIS** preset for 30 fps games carried over 60 Hz HDMI. [Download the Windows package](https://github.com/buttermb/Veyra-NRVideo/releases/download/v2.2.1-ps5-nis.1/Veyra-2.2.1-PS5-NIS-win64.zip)

## Set up and play

1. Extract the **entire** community ZIP into a new writable folder. Keep at least 2.5 GB free. Double-click **Veyra.exe**.
2. On the first launch, Veyra downloads the official NVIDIA portable package from the upstream GitHub release (763 MB), checks its SHA-256, and installs the runtime beside the launcher. Subsequent launches use the local files. To set up offline, place the official ZIP beside **Veyra.exe** with its original filename before launching. [Upstream NVIDIA portable ZIP](https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.2.1/Veyra-2.2.1-NVIDIA-win64-portable.zip)
3. On the **Professional** page, open **Source**, select your capture card and its audio input, and start capture. On the tested DX20R, use **2560 × 1440, 60 fps, NV12**. Match the capture color range to the console and card. The tested SDR setup used **Rec.709 limited**.
4. Enter fullscreen. The preset uses the full image stretched to the display. You can change the aspect mode in Veyra. Reopen **Veyra.exe** to resume with the card and preferences you selected.

Your settings are stored in the portable folder's `profile` directory. Choose **PS5 Denoise + NIS** in Presets to restore the picture and frame-generation settings after experimenting. Future community versions will appear on this fork's Releases page. [Community releases](https://github.com/buttermb/Veyra-NRVideo/releases)

## Picture and motion

| Control | Preset |
| --- | --- |
| Dark-scene cleanup | Video Denoise at source resolution, 20% luma and 60% chroma. The launcher also enables the DX20R shadow cleanup. |
| Detail and color | SMAA High, Texture +18, Temperature −20, Tint +24. The color adjustment was tuned for the tested setup. Temperature and Tint at zero give you a neutral starting point on another display. |
| Upscaling | NIS from the capture resolution to 3840 × 2160 with 0.25 sharpness. RTX Video SR remains selectable in the app. |
| Presentation | 60 fps output target, V-Sync, low queue, HUD protection, and refresh-aware capture pacing. The tested display was 3440 × 1440 at 144 Hz. |
| Frame generation | The saved 60-to-30 capture mode takes 30 real pictures per second from the 60 Hz signal. NVIDIA VFG adds an intermediate frame for 60 fps output. For a game already rendering 60 distinct frames per second, select transport cadence and turn frame generation off to keep its native motion. |

This release includes the Alt+Tab and capture-pacing fixes, corrected chroma positioning for planar and packed formats, 10-bit HDR and full-range decode preservation, first-frame recovery, and the faster Video Denoise shader. The HDMI capture build has the optional network Remote Play, Moonlight, and Xbox clients disabled.

## Measurements

On the RTX 5070 Ti, the faster denoise shader took **0.276 ms median at 4K** versus **0.446–0.448 ms** in the original comparison. The SDR/HDR denoise checks, one-pixel SDR crosshair, HDR gamut and zero-strength checks passed. Packed-format tests also covered YUY2, UYVY, YVYU, NV12, P010 and v210. These GPU timings describe the filter. Controller-to-display latency requires a separate measurement.

## Build and license

Veyra is licensed under GPLv3. The Qt libraries and upstream runtime components have their own notices. The public tag includes the capture and shader changes, the portable launcher, and the three hardware-independent preset files. [Tagged source](https://github.com/buttermb/Veyra-NRVideo/tree/v2.2.1-ps5-nis.1) · [Dependency sources and build instructions](https://github.com/buttermb/Veyra-NRVideo/blob/v2.2.1-ps5-nis.1/packaging/ps5-community/SOURCES.md)
