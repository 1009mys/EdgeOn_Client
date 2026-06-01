# 녹화 기능 요구사항 반영 분석

## 목표
다음 4개 요구사항을 동시에 만족하는 구조를 설계한다.

1. 좌측 스트림 목록에서 녹화 여부를 선택 가능해야 한다.
2. `VideoCell`에서 보고 있지 않은 영상도 백그라운드에서 디코딩하여 녹화해야 한다.
3. `VideoCell`에서 보고 있어도 녹화하지 않을 수 있어야 한다.
4. 녹화와 영상 확인을 동시에 하는 경우 디코딩은 1번만 수행해야 한다.

---

## 현재 구조에서의 한계
현재는 `MainWindow`가 `cellId -> StreamWorker` 형태로 스트림 생명주기를 관리한다.

- 기준 코드
  - `include/mainwindow.h`의 `m_workers`
  - `src/mainwindow.cpp`의 `addStream(int cellId, const QString& url)`, `removeStream(int cellId)`, `onFrameReady(int cellId)`

이 구조의 문제점:

- 스트림이 셀에 종속되어, 셀 미리보기 없이 녹화만 유지하는 모델이 어렵다.
- 미리보기 상태와 녹화 상태를 독립적으로 관리하기 어렵다.
- 잘못 확장하면 미리보기용/녹화용 워커가 분리되어 동일 스트림을 2회 디코딩할 위험이 있다.

---

## 권장 런타임 모델
핵심은 `cell 중심`에서 `camera/session 중심`으로 기준을 바꾸는 것이다.

### 상태 모델
카메라 단위로 아래 상태를 관리한다.

- `attachedCells`: 해당 카메라를 보고 있는 셀 목록
- `recordRequested`: 좌측 목록에서 사용자가 지정한 녹화 on/off
- `shouldDecode = (!attachedCells.empty() || recordRequested)`

이 모델이면 요구사항 2, 3을 동시에 만족한다.

### 디코딩 1회 원칙
카메라당 디코더(`StreamWorker`)는 1개만 유지하고 결과를 fan-out 한다.

- `preview sink`(`VideoCell`)로 전달
- `record sink`(`RecorderWorker` 또는 `RecorderSink`)로 전달

즉, decode once + multi consumer 구조로 요구사항 4를 만족한다.

---

## 구현안 비교

### 최소 수정안
`MainWindow` 내부에 세션 레지스트리를 추가하고 기존 클래스를 최대한 재사용한다.

- 장점: 빠른 구현
- 단점: `MainWindow` 책임 과다

### 권장 구조안
`CameraSession`/`RecorderWorker`를 분리 도입한다.

- 장점: 역할 분리, 유지보수성 높음
- 단점: 초기 변경량 증가

실사용/확장성을 고려하면 권장 구조안을 추천한다.

---

## 파일별 수정/추가 포인트

## 1) 목록 UI: 녹화 여부 선택
- 수정: `src/mainwindow.cpp`, `include/mainwindow.h`
- 변경 포인트:
  - 스트림 목록 항목에 녹화 체크 상태를 추가
  - 체크 변경 이벤트에서 `camera_id` 기준 녹화 요청 상태 갱신

권장:
- 단기: `QListWidgetItem` check state 사용
- 중기: `QTreeWidget`/`QTableWidget`로 컬럼 분리(이름/URL/활성/녹화/상태)

## 2) 세션 수명주기 분리
- 수정: `include/mainwindow.h`, `src/mainwindow.cpp`
- 기존 `m_workers(cellId)` 중심을 `m_sessions(cameraId)` 중심으로 전환
- 추가 관리:
  - `cellId -> cameraId` 바인딩 맵
  - `bindCellToCamera()`, `unbindCell()`
  - `setCameraRecordingRequested()`
  - `syncSessionLifetime()`

## 3) 디코더 단일화 + fan-out
- 수정: `include/streamworker.h`, `src/streamworker.cpp`
- `cellId` 기반 시그널을 `cameraId` 기반으로 전환
- 디코딩 루프는 1개 유지하고 결과를 미리보기/녹화 경로로 동시에 전달

주의:
- 녹화 경로로 전달되는 프레임은 버퍼 생명주기 안전성을 위해 독립 복사본 필요

## 4) 녹화 저장 모듈 분리
- 추가(권장): `include/recorderworker.h`, `src/recorderworker.cpp`
- 역할:
  - 파일 open/close
  - 프레임 write
  - 오류 상태 전달

## 5) `VideoCell` 역할 재정의
- 수정(선택): `include/videocell.h`, `src/videocell.cpp`
- 원칙:
  - `VideoCell`은 미리보기 렌더링 전용
  - 녹화 제어의 소스 오브 트루스는 좌측 목록/세션 상태
- 표시 확장:
  - `REC` 뱃지/텍스트 같은 상태 표시는 가능

## 6) DB 스키마 확장(녹화 기본값 저장 시)
- 수정: `include/Protocol.h`, `include/DBManager.h`, `src/DBManager.cpp`, `src/DBManager_cameras.cpp`
- 예시 컬럼:
  - `record_enabled INTEGER NOT NULL DEFAULT 0`

중요:
- 기존 DB 호환을 위해 `validateCamerasTable()`에서 컬럼 부재 시 `ALTER TABLE` 마이그레이션 필요

## 7) 빌드 파일
- 수정: `CMakeLists.txt`
- 신규 소스(`recorderworker.*`)를 타겟에 추가

---

## 요구사항 매핑

1. 목록에서 녹화 선택
   - 목록 아이템 체크 상태 -> `recordRequested` 반영

2. 미리보기 없이 백그라운드 녹화
   - `attachedCells`가 비어도 `recordRequested == true`면 세션 유지

3. 미리보기 중이어도 녹화 비활성 가능
   - 미리보기/녹화 상태를 독립 플래그로 분리

4. 미리보기+녹화 동시 시 디코딩 1회
   - 카메라당 `StreamWorker` 1개 + fan-out

---

## 단계별 구현 순서(권장)

1. 데이터 모델/DB 확장
   - `record_enabled` 저장 및 마이그레이션
2. 목록 UI에 녹화 체크 추가
3. `cellId` 중심 생명주기를 `cameraId(session)` 중심으로 전환
4. `StreamWorker` 단일 디코딩 + preview/record fan-out 반영
5. `RecorderWorker` 연결 및 파일 저장 안정화
6. `VideoCell`에는 상태 표시만 유지

---

## 리스크 및 검증 포인트

- 리스크
  - 세션 종료 조건 처리 실수 시 불필요 디코딩 지속
  - 녹화 큐 버퍼 관리 부실 시 프레임 손실/메모리 증가
  - DB 스키마 변경 시 기존 사용자 DB 호환 이슈

- 검증
  - 케이스 A: 미리보기 on + 녹화 off
  - 케이스 B: 미리보기 off + 녹화 on
  - 케이스 C: 미리보기 on + 녹화 on (디코딩 스레드 1개 확인)
  - 케이스 D: 셀에서 제거 후 녹화 지속 여부 확인
  - 케이스 E: 녹화 체크 토글 시 즉시 반영 및 파일 close/open 안정성 확인

