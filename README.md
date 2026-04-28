# EdgeOn_Client

English | [한국어](#한국어)

## English

### Overview
EdgeOn_Client is a Qt + OpenCV based multi-channel RTSP viewer.
It is designed to display many streams at once with GPU-first decoding/rendering on Windows.
This project is developed and built primarily with JetBrains CLion.

### Key Features
- Up to 16 channels (4x4) in one window.
- Multiple layout presets: 1x1, 2x2, 3x3, 4x4, 2x3, 3x2, focus-ring, and 3-large+4-small.
- Per-channel worker thread for independent stream handling.
- NVDEC-first decode path (`cv::cudacodec::VideoReader`) with automatic fallback to FFmpeg `VideoCapture`.
- Auto reconnect when stream is disconnected.
- Latest-frame-only queueing to keep UI responsive under high load.
- Drag & drop stream assignment and stream move between cells.

### Tech Stack
- C++20
- CMake
- JetBrains CLion
- Qt6 (`Widgets`, `Sql`)
- OpenCV 4.13 (CUDA/cudacodec + FFmpeg backend)
- NVIDIA CUDA Toolkit 12.9
- Windows: Direct2D/Direct3D11 for rendering path

### Requirements
- Windows 10/11 (primary target in current code)
- Visual Studio 2022 (MSVC)
- CMake 4.2+
- CUDA Toolkit 12.9
- Qt 6.11.0 (or compatible Qt6 with Widgets/Sql)
- OpenCV 4.13 build with CUDA + FFmpeg support
- Clion IDE (required for building because of CMake profiles)


### Usage
1. Launch the app.
2. Add RTSP URLs from the left stream list panel.
3. Double-click a URL to start in the first empty visible cell.
4. Or drag a URL onto a specific cell.
5. Right-click an active cell to remove its stream.
6. Change grid/layout from the toolbar.

### Runtime Options
- `EDGEON_ENABLE_CUDA_D3D_INTEROP=0`
  - Disables CUDA-D3D interop and uses fallback rendering path when needed.

### Project Structure
- `main.cpp`: app entry point.
- `mainwindow.h/.cpp`: main UI, layout presets, stream list, worker lifecycle.
- `videocell.h/.cpp`: per-cell rendering, drag/drop interactions, status/FPS overlay.
- `streamworker.h/.cpp`: stream ingest/decode thread (NVDEC + fallback).
- `CMakeLists.txt`: dependencies, output/install layout, Windows deploy helper (`windeployqt`).

### Notes
- `CMakeLists.txt` includes machine-specific OpenCV paths; adjust them before sharing with others.
- Current implementation is optimized for Windows. Linux build files exist but may require extra validation and platform-specific rendering adjustments.
- CLion CMake profiles are the primary workflow used during development.

### License
MIT
