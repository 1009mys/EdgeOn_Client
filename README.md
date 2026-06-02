# EdgeOn_Client

한국어

## 개요

EdgeOn_Client는 Qt + OpenCV 기반의 멀티 채널 RTSP 뷰어입니다.
Windows 환경에서 GPU 우선 디코딩/렌더링 방식으로 여러 스트림을 동시에 표시하도록 설계되었습니다.
이 프로젝트는 주로 JetBrains CLion 환경에서 개발 및 빌드됩니다.

### 주요 기능
- 하나의 창에서 최대 16채널(4x4) 동시 표시
- 다양한 레이아웃 프리셋 지원: 1x1, 2x2, 3x3, 4x4, 2x3, 3x2, focus-ring, 3-large+4-small
- 채널별 독립 스트림 처리를 위한 워커 스레드 구조
- NVDEC 우선 디코딩 경로(`cv::cudacodec::VideoReader`)와 FFmpeg `VideoCapture` 자동 폴백
- 스트림 끊김 시 자동 재연결
- 고부하 상황에서도 UI 반응성을 유지하기 위한 최신 프레임 우선 큐잉
- 드래그 앤 드롭 기반 스트림 할당 및 셀 간 스트림 이동
- YOLOv8 detection 결과를 SQLite `detections` / `detection_frames` 테이블에 저장
- 프레임 UTC 시각(`frame_utc_ms`)과 녹화 세그먼트 시각을 함께 기록해 VOD BBox 정합성을 높임

### 기술 스택
- C++20
- CMake
- JetBrains CLion
- Qt6 (`Widgets`, `Sql`)
- OpenCV 4.13 (CUDA/cudacodec + FFmpeg backend)
- NVIDIA CUDA Toolkit 12.9
- Windows: Direct2D/Direct3D11 for rendering path

### 요구 사항
- Windows 10/11(현재 코드의 주 대상 플랫폼)
- Visual Studio 2022 (MSVC)
- CMake 4.2+
- CUDA Toolkit 12.9
- Qt 6.11.0(또는 `Widgets`/`Sql`을 포함한 호환 Qt6)
- CUDA + FFmpeg 지원으로 빌드된 OpenCV 4.13
- CLion IDE(CMake 프로파일 기반 빌드 워크플로 때문에 필요)


### 사용 방법
1. 앱을 실행합니다.
2. 왼쪽 스트림 목록 패널에서 RTSP URL을 추가합니다.
3. URL을 더블클릭하면 첫 번째 빈 가시 셀에서 재생이 시작됩니다.
4. 또는 원하는 셀로 URL을 드래그 앤 드롭합니다.
5. 활성 셀에서 오른쪽 클릭하여 해당 스트림을 제거할 수 있습니다.
6. 툴바에서 그리드/레이아웃을 변경합니다.

### 런타임 옵션
- `EDGEON_ENABLE_CUDA_D3D_INTEROP=0`
  - CUDA-D3D interop를 비활성화하고 필요 시 폴백 렌더링 경로를 사용합니다.

### 프로젝트 구조
- `main.cpp`: 애플리케이션 진입점
- `mainwindow.h/.cpp`: 메인 UI, 레이아웃 프리셋, 스트림 목록, 워커 생명주기 관리
- `videocell.h/.cpp`: 셀별 렌더링, 드래그/드롭 상호작용, 상태/FPS 오버레이
- `streamworker.h/.cpp`: 스트림 수신/디코딩 스레드(NVDEC + 폴백)
- `Protocol.h`: 카메라/감지 결과 공용 DTO 정의
- `DBManager.h/.cpp`: SQLite 초기화, cameras/detection 테이블 생성/검증, detection 저장/보존 정리
- `onnxinference.h/.cpp`: ONNX Runtime 기반 범용 추론 래퍼(전/후처리 미포함, Qt 미사용)
- `yolov8inference.h/.cpp`: Ultralytics YOLOv8 COCO pretrained 모델용 전용 추론 래퍼(전처리/후처리 포함, Qt 미사용)
- `CMakeLists.txt`: 의존성, 출력/설치 레이아웃, Windows 배포 도우미(`windeployqt`)

### ONNX Runtime 사용
- 기본 경로: `C:/onnxruntime/onnxruntime-win-x64-gpu-1.26.0`
- 다른 경로 사용 시 CMake 캐시 변수 `ONNXRUNTIME_ROOT`를 변경하세요.
- `OnnxInference`는 모델 로드, 입/출력 shape 조회, 더미 입력 기반 테스트 추론을 제공합니다.
- `load()`는 CUDA EP를 먼저 시도하고 실패하면 자동으로 CPU EP로 폴백합니다.

### YOLOv8 추론 사용
- `Yolov8Inference`는 `OnnxInference`를 멤버로 사용하는 전용 래퍼입니다.
- 입력은 OpenCV `cv::Mat`이며, 내부에서 letterbox + RGB 변환 + float32 NCHW 텐서로 변환합니다.
- raw YOLOv8 head(`1x84x8400` 계열)와 end-to-end 출력(`Nx6` 계열)을 모두 후처리할 수 있습니다.
- main 테스트는 `models/yolov8s.onnx`를 로드한 뒤, 인자가 있으면 해당 이미지를 사용하고 없으면 합성 테스트 이미지를 생성합니다.

```cpp
#include "yolov8inference.h"

Yolov8Inference infer;
infer.load("model.onnx");

cv::Mat image = cv::imread("sample.jpg");
auto detections = infer.inference(image);

infer.close();
```

### 참고 사항
- `CMakeLists.txt`에는 로컬 머신에 종속된 OpenCV 경로가 포함되어 있으므로, 다른 사람과 공유하기 전에 경로를 조정해야 합니다.
- 현재 구현은 Windows에 최적화되어 있습니다. Linux용 빌드 파일도 존재하지만 추가 검증과 플랫폼별 렌더링 조정이 필요할 수 있습니다.
- 개발 과정에서는 CLion의 CMake 프로파일을 기본 워크플로로 사용합니다.

### 로드맵 / TODO
- 우선순위가 정리된 개선 아이디어와 예정 기능은 [`TODO.md`](./TODO.md)에서 확인할 수 있습니다.

### 라이선스
MIT
