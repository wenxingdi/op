# OP - Windows Automation Plugin (this repo is an iteration fork)

[中文](README.md)

> **Fork notice**: this repository is forked from [WallBreaker2/op](https://github.com/WallBreaker2/op), based on upstream `0.4.8.3` (2026-07-07).
> It carries extensive fixes and extensions on top of upstream (see [docs/CHANGELOG.md](docs/CHANGELOG.md)), and the build workflow has been replaced with this repo's own toolchain.
> The upstream [GitHub Wiki](https://github.com/WallBreaker2/op/wiki) still applies to the base API, but for OCR/YOLO integration and build commands this document prevails.

OP (Operator & Open) is a Windows automation plugin: window discovery, background binding, screen capture, mouse/keyboard input, color/image search, OCR, OpenCV, YOLO detection and process memory access behind one interface.

Core is C++17, exposing COM and C API (x86/x64). Capture backends cover plain GDI, DXGI, WGC, DirectX Hook, OpenGL and OpenGL ES; dictionaries and image templates load from file or memory.

## OCR / YOLO (major differences from upstream)

**HTTP remote-service integration has been completely removed; everything runs in-process:**

| Capability | Upstream 0.4.8.3 | This fork |
|---|---|---|
| General OCR | requires a standalone HTTP service (op_ocr_engine) | **Built-in ONNX engine** (PP-OCRv4 model embedded in the DLL, zero external dependencies) |
| Fixed-font OCR | bitmap dictionary | bitmap dictionary (unchanged; compatible with OP binary dicts and 大漠 text dicts) |
| YOLO detection | standalone HTTP service | **Local `.onnx` models** (`SetYoloEngine("xxx.onnx",...)` single entry, self-trained models) |

- `SetOcrEngine("onnx"/empty, ...)` selects the built-in engine; passing `http(s)://` or legacy remote aliases (tesseract/paddle) returns 0 with a removal notice.
- New OCR family: `AutoOcr` / `AutoOcrLine` (fast single-line) / `AutoOcrEx` (structured bbox+confidence output) / no-dict auto recognition; dictionary results are joined in reading order.
- Capture fix: `gdi/gdi2/dx2` now use `PW_RENDERFULLCONTENT` with an all-black fallback, so UWP (Calculator) / Chromium (Electron, CEF) windows no longer capture as pure black.

## Documentation

- **[docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md) — full build & environment guide for beginners (read first)**
- [docs/api_reference.html](docs/api_reference.html) — API quick reference (223 interfaces, 100% parameter annotations)
- [docs/CHANGELOG.md](docs/CHANGELOG.md) — iteration log
- [CLAUDE.md](CLAUDE.md) / [AGENTS.md](AGENTS.md) — accurate build/collaboration guide for AI coding assistants
- Base API examples: [upstream Wiki](https://github.com/WallBreaker2/op/wiki)

## Quick start

Grab the DLLs from `bin/x64/` (or a release package) matching your host bitness. COM usage requires registration (as administrator):

```powershell
regsvr32 .\op_x64.dll   # 64-bit host
```

Python via COM:

```python
from win32com.client import Dispatch
op = Dispatch("op.opsoft")
print("op version:", op.Ver())
```

Or call the C API directly via ctypes (registration-free) — see `bindings/python` and `docs/api_reference.html`.

The companion test tool **OPTestTool** (.NET 10 WinForms, run as administrator) lives in `D:\AutoPro\OPTool` and drives this plugin through `op_c_api_x64.dll`.

## Building from source

Full requirements and step-by-step instructions (with verification commands) are in **[docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md)**. Summary:

- Windows 10+ / VS2022 (MSVC 14.44) / Windows SDK 10.0.26100 / CMake / Python 3.12
- Fresh machine: `python build.py` to bootstrap dependencies, then cmake-configure `build/nmake-x64-Release` (exact command in the guide)
- Daily incremental build: `python build/_wb_build.py`
- Tests: `powershell -File scripts/run_tests.ps1` (baseline 270 ran / 262 PASS)
- After building, sync release binaries: `D:\AutoPro\OPTool\sync_op_dll.py`

## License

[MIT License](LICENSE) (inherited from upstream)
