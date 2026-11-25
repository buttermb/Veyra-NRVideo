# Community build and corresponding sources

The player is based on upstream Veyra 2.2.1. The fork integrates the PS5 capture work and Devin's decoder, chroma, and denoise changes through the faster Video Denoise shader. The public application checkpoint `5794c8185c58dbb38f3ed90ccf60fba5308627a6` has the same Git tree as the tested build. The community tag adds the portable packaging and preset. [Community source tag](https://github.com/buttermb/Veyra-NRVideo/tree/v2.2.1-ps5-nis.1) · [Upstream Veyra](https://github.com/Likely7/Veyra-NRVideo/tree/v2.2.1)

## Build the player

Use the upstream Windows x64 build instructions with Visual Studio 2022 Build Tools, the Windows SDK and DXC, CMake/Ninja, and the **Qt 6.9.3 MSVC 2022 x64** kit. This release uses a Release build with a direct D3D12 renderer. Configure the FFmpeg and OpenSSL roots and the public NVIDIA SDK header and library paths listed in the project's CMake configuration. [Upstream build instructions](https://github.com/Likely7/Veyra-NRVideo/blob/v2.2.1/docs/BUILD_2.2.1.md) · [Common dependency build instructions](https://github.com/Likely7/Veyra-NRVideo/blob/v2.2.1/docs/BUILD_2.2.0.md)

| CMake setting | Release value |
| --- | --- |
| `CMAKE_BUILD_TYPE`, `VEYRA_DISPLAY_VERSION` | `Release`, `2.2.1-Devin-NIS` |
| `VEYRA_BUILD_QML_UI`, `VEYRA_ENABLE_EXPERIMENTAL_DLSSNR`, `VEYRA_ENABLE_NVOF` | `ON` |
| `VEYRA_ENABLE_DLSS_FG`, `VEYRA_ENABLE_DLSS_SR` | `ON` |
| `VEYRA_CUDA_DRIVER_INCLUDE_DIR` | The SDK directory containing `cuda.h`. This enables native NVIDIA VFG in the player. |
| `VEYRA_DLSS_SDK_ROOT`, `VEYRA_NVOF_SDK_ROOT`, `VEYRA_NVENC_HEADERS_ROOT` | The matching DLSS, optical flow, and codec SDK locations |
| `VEYRA_ENABLE_D3D12_DEBUG`, `VEYRA_ENABLE_LIBDOVI`, `VEYRA_ENABLE_REMOTEPLAY`, `VEYRA_ENABLE_MOONLIGHT`, `VEYRA_ENABLE_XBOX` | `OFF` |

Build `veyra_qml_ui` with the configured tree. The target builds the executable, QML resources and 108 compiled shaders. The packaging script uses Python 3, the Qt kit's `windeployqt.exe`, the Windows .NET Framework C# compiler, and the upstream NVIDIA 2.2.1 portable ZIP. Run from the source root in a native tools shell, substituting your paths.

```powershell
cmake --build build --config Release --parallel 2 --target veyra_qml_ui
python packaging/ps5-community/package.py --build build --qt C:/Qt/6.9.3/msvc2022_64 --base-zip C:/packages/Veyra-2.2.1-NVIDIA-win64-portable.zip --out dist
```

The packager creates the downloadable ZIP and `SHA256SUMS.txt`. The launcher verifies the community payload and the upstream ZIP before creating the portable app directory. It can run without opening a window with `Veyra.exe --prepare-only`. An existing profile in that portable folder is preserved.

## Dependency sources

- The community payload contains the compiled Veyra app, shaders, QML, Qt 6.9.3 redistributables, launcher, and preset. Qt uses unmodified dynamic libraries from the Qt 6.9.3 Windows MSVC kit. The release includes Qt's license texts and the kit's software bills of materials under `app/licenses/qt`. Corresponding Qt module sources are available from the Qt Project. [Qt Base](https://download.qt.io/archive/qt/6.9/6.9.3/submodules/qtbase-everywhere-src-6.9.3.tar.xz) · [Qt Declarative](https://download.qt.io/archive/qt/6.9/6.9.3/submodules/qtdeclarative-everywhere-src-6.9.3.tar.xz) · [Qt SVG](https://download.qt.io/archive/qt/6.9/6.9.3/submodules/qtsvg-everywhere-src-6.9.3.tar.xz) · [Qt Translations](https://download.qt.io/archive/qt/6.9/6.9.3/submodules/qttranslations-everywhere-src-6.9.3.tar.xz)
- NVIDIA Image Scaling uses NVIDIA's MIT-licensed NIS SDK. The Video Denoise shader includes the upstream filter attribution and GPL notice. [NIS sources](https://github.com/buttermb/Veyra-NRVideo/tree/v2.2.1-ps5-nis.1/third_party/nis) · [Full third-party notices](https://github.com/buttermb/Veyra-NRVideo/blob/v2.2.1-ps5-nis.1/THIRD_PARTY_NOTICES.md)
- The launcher obtains the binary dependencies, vendor runtime files and associated notices from the upstream NVIDIA portable package. Its SHA-256 is `e09150d1594aa9416fa27457a42b4d50b5d814e05471216bef95b9303544386b`. Upstream supplies the matching FFmpeg and dependency source packages. [Upstream runtime release](https://github.com/Likely7/Veyra-NRVideo/releases/tag/v2.2.1) · [Corresponding dependency sources](https://github.com/Likely7/Veyra-NRVideo/releases/tag/v2.2.1-sources) · [Earlier dependency sources](https://github.com/Likely7/Veyra-NRVideo/releases/tag/v2.2.0-sources)
