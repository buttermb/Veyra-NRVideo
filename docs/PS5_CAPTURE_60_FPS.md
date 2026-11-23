# PS5 capture at 60 FPS with NVIDIA frame generation

This Veyra 2.2.0 branch includes an opt-in preview mode for games that render at 30 FPS over a 60 Hz capture link. Veyra selects 30 source samples per second and displays one NVIDIA-generated frame between consecutive originals. The test setup used a Portta DX20R USB capture card, a GeForce RTX 5070 Ti, and a Gigabyte M34WQ running at 3440 × 1440 and 144 Hz.

## Run the branch

Clone the tested branch and build the Windows x64 QML player with `VEYRA_BUILD_QML_UI=ON` and the NVIDIA Video Effects runtime available to the player. The upstream build notes list the compiler, Qt version, and runtime package. [Veyra 2.2.0 build notes](BUILD_2.2.0.md)

```powershell
git clone --depth 1 --branch ps5-capture-60fps https://github.com/buttermb/Veyra-NRVideo.git
```

From the directory containing the built player and its runtime, start the preview in PowerShell.

```powershell
$env:VEYRA_CAPTURE_REFRESH_PACED_FG = '1'
.\veyra_qml_ui.exe --fullscreen
```

## Tested settings

| Control | Setting |
| --- | --- |
| Capture source | DX20R Video Device, native NV12, 2560 × 1440 at 60 Hz |
| Content rate | 60 Hz capture → 30 FPS source |
| Super resolution | RTX Video SR, High Bitrate family, High quality. Set the target to 3840 × 2160 and the output to Source (2560 × 1440). |
| Frame generation | NVIDIA VFG, 2×, Medium quality |
| Picture | SDR, Texture +18. The test display used Temperature −20 and Tint +24 as a visual preference. |
| Presentation | V-Sync on, low-latency queue on, NVIDIA Low Latency on, variable refresh off, output rate set to Follow Display |
| Display | Fixed 144 Hz. The NVIDIA application profile also used Vertical sync On. |

High upscaling preserved edges in the captured map text and car trim more closely than Medium in the two stills tested. During preview, the new scheduler makes generated frames eligible as soon as the GPU and display queue are ready. It schedules each original from its generated frame’s actual submission time. Repeated pictures stay on the capture cadence, and the first changed picture after a sustained still can start a new pair early.

For SDR capture, the branch also recovers color information that a driver advertises during setup and omits from the connected format. Recovery requires matching native layouts and fills only missing fields.

## Measured on the DX20R and RTX 5070 Ti

Each row covers a 90-second live run at 144 Hz with V-Sync enabled and frame generation at Medium 2×.

| High Bitrate upscaling quality | Average submitted FPS | Original capture to refresh | Generated capture to refresh |
| --- | ---: | ---: | ---: |
| Medium | 59.97 | 25.6 ms | 11.7 ms |
| High | 59.98 | 29.1 ms | 15.1 ms |

The High run logged zero dropped capture samples and zero missed generated-frame deadlines. A separate test with both upscaling and frame generation on High fell to 30 submitted FPS on this machine.

The latency columns report the median of the one-second medians from the USB capture callback to the DXGI refresh that displayed the frame. For a generated frame, timing starts at the newer source frame. Controller input, console rendering, HDMI transport, and panel response sit outside this measurement.

To check another setup, look for `fastPair=true` in the `live-anchor` log, `sync=1 flags=0x0` in `present-contract`, and both `realSamples` and `generatedSamples` in `capture-scanout`. The timing and capture-color suites passed 387 and 299 checks, respectively, on the tested build.
