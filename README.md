# EdgeOn_Client

English | [한국어](#한국어)

## English

### Overview
EdgeOn_Client is a Qt + OpenCV based multi-channel RTSP viewer.
It is designed to display many streams at once with GPU-first decoding/rendering on Windows.

### Key Features
- Up to 16 channels (4x4) in one window.
- Multiple layout presets: 1x1, 2x2, 3x3, 4x4, 2x3, 3x2, focus-ring, and 3-large+4-small.
- Per-channel worker thread for independent stream handling.
- NVDEC-first decode path (`cv::cudacodec::VideoReader`) with automatic fallback to FFmpeg `VideoCapture`.
- Auto reconnect when stream is disconnected.
- Latest-frame-only queueing to keep UI responsive under high load.
- Drag & drop stream assignment and stream move between cells.
- Optional CUDA-D3D11 interop path for Windows rendering.

### Tech Stack
- C++20
- CMake
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

### Build (Windows, MSVC)
Update library paths first:
- `CMakeLists.txt` currently contains local absolute paths for OpenCV.
- You can also override Qt root via `QT_ROOT_OVERRIDE`.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DQT_ROOT_OVERRIDE="C:/Qt/6.11.0/msvc2022_64" -DOpenCV_DIR="E:/library_source/opencv/opencv-4.13.0/build"
cmake --build build --config Release
```

Run:

```powershell
.\build\Release\EdgeOn_Client.exe
```

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
- `CMakeLists.txt`: dependencies, link settings, Windows deploy helper (`windeployqt`).

### Notes
- `CMakeLists.txt` includes machine-specific OpenCV paths; adjust them before sharing with others.
- Current implementation is optimized for Windows. Linux build files exist but may require extra validation and platform-specific rendering adjustments.

### License
Add your license here (for example: MIT).

---

## 한국어

### 개요
EdgeOn_Client는 Qt + OpenCV 기반의 멀티채널 RTSP 뷰어입니다.
Windows 환경에서 GPU 우선 디코딩/렌더링을 활용해 다중 스트림 표시를 목표로 합니다.

### 주요 기능
- 한 화면에서 최대 16채널(4x4) 표시.
- 다양한 레이아웃 프리셋: 1x1, 2x2, 3x3, 4x4, 2x3, 3x2, 메인+주변, 대형3+소형4.
- 채널별 독립 워커 스레드 기반 스트림 처리.
- NVDEC(`cv::cudacodec::VideoReader`) 우선 디코딩 + FFmpeg `VideoCapture` 자동 폴백.
- 스트림 끊김 시 자동 재연결.
- 최신 프레임만 유지하는 방식으로 UI 반응성 개선.
- 드래그 앤 드롭으로 스트림 할당/셀 간 이동.
- Windows에서 CUDA-D3D11 interop 렌더링 경로 지원.

### 기술 스택
- C++20
- CMake
- Qt6 (`Widgets`, `Sql`)
- OpenCV 4.13 (CUDA/cudacodec + FFmpeg 백엔드)
- NVIDIA CUDA Toolkit 12.9
- Windows: Direct2D/Direct3D11 렌더링

### 요구 사항
- Windows 10/11 (현재 코드 기준 주 타겟)
- Visual Studio 2022 (MSVC)
- CMake 4.2+
- CUDA Toolkit 12.9
- Qt 6.11.0 (또는 Widgets/Sql 포함 호환 Qt6)
- CUDA + FFmpeg 지원으로 빌드된 OpenCV 4.13

### 빌드 (Windows, MSVC)
먼저 라이브러리 경로를 환경에 맞게 수정하세요.
- `CMakeLists.txt`에 OpenCV 절대 경로가 하드코딩되어 있습니다.
- Qt 경로는 `QT_ROOT_OVERRIDE`로 덮어쓸 수 있습니다.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DQT_ROOT_OVERRIDE="C:/Qt/6.11.0/msvc2022_64" -DOpenCV_DIR="E:/library_source/opencv/opencv-4.13.0/build"
cmake --build build --config Release
```

실행:

```powershell
.\build\Release\EdgeOn_Client.exe
```

### 사용 방법
1. 앱을 실행합니다.
2. 좌측 스트림 목록 패널에 RTSP URL을 추가합니다.
3. URL을 더블클릭하면 현재 보이는 영역의 첫 빈 셀에서 재생됩니다.
4. 또는 URL을 원하는 셀로 드래그해 시작할 수 있습니다.
5. 활성 셀 우클릭으로 스트림을 제거할 수 있습니다.
6. 툴바에서 그리드/레이아웃을 변경합니다.

### 런타임 옵션
- `EDGEON_ENABLE_CUDA_D3D_INTEROP=0`
  - 필요 시 CUDA-D3D interop를 비활성화하고 폴백 렌더링 경로를 사용합니다.

### 프로젝트 구조
- `main.cpp`: 앱 진입점.
- `mainwindow.h/.cpp`: 메인 UI, 레이아웃 프리셋, 스트림 목록, 워커 생명주기 관리.
- `videocell.h/.cpp`: 셀 렌더링, 드래그/드롭, 상태/FPS 오버레이.
- `streamworker.h/.cpp`: 스트림 수신/디코딩 스레드(NVDEC + 폴백).
- `CMakeLists.txt`: 의존성 설정, 링크, Windows 배포(`windeployqt`) 보조.

### 참고
- `CMakeLists.txt`의 OpenCV 경로는 개발 PC 기준 값이므로 공유 전 수정이 필요합니다.
- Linux 빌드 폴더는 존재하지만, 현재 구현은 Windows 최적화 경로 비중이 높아 추가 검증이 필요합니다.

### 라이선스
원하는 라이선스를 기입하세요 (예: MIT).

