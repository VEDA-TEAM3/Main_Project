# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Ponytail, lazy senior dev mode

You are a lazy senior developer. Lazy means efficient, not careless. The best code is the code never written.

Before writing any code, stop at the first rung that holds:

1. Does this need to be built at all? (YAGNI)
2. Does it already exist in this codebase? Reuse the helper, util, or pattern that's already here, don't re-write it.
3. Does the standard library already do this? Use it.
4. Does a native platform feature cover it? Use it.
5. Does an already-installed dependency solve it? Use it.
6. Can this be one line? Make it one line.
7. Only then: write the minimum code that works.

The ladder runs after you understand the problem, not instead of it: read the task and the code it touches, trace the real flow end to end, then climb.

Bug fix = root cause, not symptom: a report names a symptom. Grep every caller of the function you touch and fix the shared function once — one guard there is a smaller diff than one per caller, and patching only the path the ticket names leaves a sibling caller still broken.

Rules:

- No abstractions that weren't explicitly requested.
- No new dependency if it can be avoided.
- No boilerplate nobody asked for.
- Deletion over addition. Boring over clever. Fewest files possible.
- Shortest working diff wins, but only once you understand the problem. The smallest change in the wrong place isn't lazy, it's a second bug.
- Question complex requests: "Do you actually need X, or does Y cover it?"
- Pick the edge-case-correct option when two stdlib approaches are the same size, lazy means less code, not the flimsier algorithm.
- Mark deliberate simplifications that cut a real corner with a known ceiling (global lock, O(n²) scan, naive heuristic) with a `ponytail:` comment naming the ceiling and upgrade path.

Not lazy about: understanding the problem (read it fully and trace the real flow before picking a rung, a small diff you don't understand is just laziness dressed up as efficiency), input validation at trust boundaries, error handling that prevents data loss, security, accessibility, the calibration real hardware needs (the platform is never the spec ideal, a clock drifts, a sensor reads off), anything explicitly requested. Lazy code without its check is unfinished: non-trivial logic leaves ONE runnable check behind, the smallest thing that fails if the logic breaks (an assert-based demo/self-check or one small test file; no frameworks, no fixtures). Trivial one-liners need no test.

(Yes, this file also applies to agents working on the ponytail repo itself. Especially to them.)

## 저장소 위치

이 디렉터리(`client/`)는 monorepo `Main_Project`의 Qt 데스크톱 클라이언트입니다. 저장소 루트에는
`compute-server`, `control-server`, `shared`, `tests`, `manual`이 함께 있고, 루트 `CMakeLists.txt`는
**서버만** 빌드합니다(클라이언트는 포함되지 않음). 클라이언트는 항상 `client/`에서 자체 preset으로 빌드합니다.
`.clang-format`, `.clang-tidy`, `manual/`은 저장소 루트에 있습니다.

## 빌드

```powershell
cmake --preset debug-ninja
cmake --build --preset debug-ninja
```

전체 재빌드가 잦을 때는 Unity Build preset(`fast-debug-ninja`)을 씁니다. preset은 Qt 6.11.1 MinGW
(`C:/Qt/6.11.1/mingw_64`), MinGW 13.1, Ninja 경로를 하드코딩하고 있으므로 환경이 다르면 `CMakePresets.json`을 수정합니다.
GStreamer 기본 경로는 `GSTREAMER_ROOT`(`C:/Program Files/gstreamer/1.0/mingw_x86_64`) 캐시 변수입니다.

**소스 목록이 명시적입니다.** `CMakeLists.txt`의 `qt_add_executable`에 헤더/소스를 한 줄씩 나열하므로,
새 파일을 추가하면 반드시 여기에 등록해야 합니다(glob 없음). 새 아이콘/QSS도 `qt_add_resources` 목록에 추가합니다.

클라이언트에는 테스트 프레임워크가 없고, 대신 `tests/`에 프레임워크 없는 자체 검사(`*Check.cpp`)가 있습니다.
`QTCCTV_BUILD_CHECKS=ON`(debug preset 기본값)이면 각각 독립 실행 파일로 빌드되고, 실패하면 0이 아닌 값을 냅니다:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" --build --preset debug-ninja --target table_model_roles_check
$env:Path = "C:\Qt\6.11.1\mingw_64\bin;" + $env:Path
build\debug-ninja\table_model_roles_check.exe
```

새 검사를 추가할 때는 `tests/`에 파일을 만들고 `CMakeLists.txt`의 `QTCCTV_BUILD_CHECKS` 블록에 target을 등록합니다.
`ctest`로 도는 테스트는 저장소 루트의 compute-server 대상뿐입니다
(루트에서 `cmake -B build -S . -DBUILD_TESTING=ON` 후 `ctest`). UI 쪽 변경은 자체 검사로 잡히지 않으므로
**실제 실행**이 유일한 수단입니다. UI를 건드렸으면 아래 실행 절차로 최소 30초 띄워보고 크래시 여부까지 확인합니다.

## 실행과 검증

빌드 산출물은 `build/debug-ninja/Qtcctvclient.exe`이고, GStreamer와 Qt DLL이 PATH에 있어야 뜹니다:

```powershell
$env:Path = "C:\Program Files\gstreamer\1.0\mingw_x86_64\bin;C:\Qt\6.11.1\mingw_64\bin;" + $env:Path
build\debug-ninja\Qtcctvclient.exe
```

브로커/RTSP 없이도 창은 뜨고 내장 데모가 돕니다(로그에 RTSP·MQTT 오류가 잔뜩 찍히는 건 정상).
크래시는 조용히 죽는 형태라 종료 코드로 판별합니다: `-1073740940`(0xC0000374)은 heap corruption,
`-1073741819`(0xC0000005)는 access violation입니다. 원인 추적은 `gdb --batch -ex run -ex bt --args Qtcctvclient.exe`.

PR CI와 같은 검사를 로컬에서 돌립니다(둘 다 저장소 루트 기준):

```powershell
& "C:\Qt\Tools\QtCreator\bin\clang\bin\clang-format.exe" --dry-run --Werror client/src/ui/mainwindow.cpp
& "C:\Qt\6.11.1\mingw_64\bin\qmllint.exe" -I client/qml client/qml/PanelHeader.qml
```

## 실행 설정

`config/app_config.json`은 gitignore 대상(RTSP 비밀번호 포함)입니다. 최초 1회:

```powershell
Copy-Item config/app_config.example.json config/app_config.json
```

CMake는 로컬 파일이 있으면 그것을, 없으면 example을 빌드 디렉터리로 복사합니다. 환경 변수가 JSON보다
**우선**합니다: `VEDA_CONFIG_FILE`, `VEDA_RTSP_URL_1..4`, `VEDA_MQTT_*`, `VEDA_MAP_MIN_*`/`VEDA_MAP_MAX_*`/
`VEDA_MAP_INVERT_Y`, `QTCCTV_BLUR_SYNC_OFFSET_MS`, `QTCCTV_DECODER_MODE`, Slack 신고용 `SLACK_*`.
자세한 키 목록은 [README.md](README.md), 프로토콜 계약은 [MQTT_INTEGRATION.md](MQTT_INTEGRATION.md)에 있습니다.

## 아키텍처

`main.cpp`가 모든 factory를 생성해 `MainWindow`에 주입하는 **생성자 주입** 구조입니다. 상위 계층은 항상
추상 인터페이스(`StreamReceiverFactory`, `DeviceStatusGatewayFactory`, `DashboardPanelFactory`,
`ReportGateway`, `MqttTransport`)에만 의존하고, 구현체(`Gst*`, `QtMqtt*`, `Slack*`)는 `main.cpp`에서만 선택됩니다.

계층은 `include/`와 `src/`가 동일한 트리로 대응됩니다: `config/`, `model/`, `network/`, `overlays/`, `ui/`, `video/`.

### MQTT 경로

`QtMqttTransport` → `MqttMessageRouter` → 토픽별 `MqttTopicHandler`(Device/Risk/Blur) → `MqttMessageBatch`
→ `MqttDeviceStatusGateway` → `DeviceStatusService`(전용 `QThread`) → queued signal로 UI·영상 스레드에 전달.

- **새 MQTT 데이터 종류는 `MqttTopicHandler` 구현을 추가**하고 라우터 핸들러 목록에 넣어 확장합니다.
  토픽 문자열과 QoS는 코드에 박지 않고 `app_config.json`의 `mqtt.topics`에서 옵니다.
- 고빈도 데이터(risk/blur)는 직접 signal로 쏘지 않고 `LatestRiskFrameBuffer`/`LatestBlurFrameBuffer`에
  최신값만 보관하고 `RiskFrameDispatcher`/`BlurFrameDispatcher`가 주기적으로 flush합니다. UI 갱신도
  `DashboardPanelCoordinator`에서 타이머로 배치 처리합니다. 이 버퍼링을 우회하면 UI/영상 스레드가 밀립니다.
- **MQTT payload는 신뢰할 수 없는 입력입니다.** 공통 상한은 `include/network/parsing/MqttPayloadLimits.h`
  한곳에 있습니다(payload 256KB, Risk 객체 256개, `ts`는 로컬 UTC 기준 -60초~+5초). 새 파서를 추가하면
  `isFreshSourceTimestamp`를 **반드시** 부르세요. 미래 `ts`를 한 번 받아들이면 `BlurProcessor`가 최신
  timestamp를 그쪽으로 끌어올려 뒤이어 오는 정상 metadata를 전부 과거로 보고 버리고, **블러가 조용히
  꺼진 채 얼굴과 번호판이 그대로 나갑니다.** 그래서 미래 쪽 한계는 `blur.historyMs`보다 작아야 합니다.
  상한을 넘긴 메시지는 잘라 쓰지 말고 통째로 거부합니다(블러 영역 64개만 예외 — 일부라도 가리는 편이 낫습니다).
- **프로토콜 오류는 `emitProtocolError`에서 초당 하나로 제한됩니다.** 오류 하나마다 스레드 경계를 넘는
  signal이 하나 나가므로, 제한을 풀면 잘못된 메시지를 쏟아붓는 것만으로 크기 상한보다 먼저 UI가 밀립니다.

### 채널 번호 규칙 (자주 틀리는 부분)

내부 인덱스는 항상 0-based(0..3, 표시는 `CH 01..04`)입니다. 와이어 포맷은 토픽마다 다릅니다:
`veda/hw/ch/+/status`와 topview는 0-based, 중앙 상태/이벤트의 `channelId`와 blur의 `ch`는 **1-based**입니다.
파서는 토픽 채널과 payload 채널의 일치를 검증합니다. 새 메시지를 다룰 때 `MQTT_INTEGRATION.md`의 표를 먼저 확인하세요.

### 영상 경로

`StreamSessionManager`가 채널별 `StreamReceiver`(`GstRtspReceiver`)를 생성해 각각 전용 `QThread`에서
실행하고, 출력은 네이티브 `WId`로 GStreamer sink에 바로 연결합니다(`ClickableVideoWidget`).
`BlurProcessor`/`BlurVideoFilter`는 `VideoUtcClockMapper`로 RTSP 지연(`blur.syncOffsetMs`, 기본 300ms)을 보정해
blur 메타데이터의 UTC `ts`를 실제 표시 프레임에 맞춘 뒤 sink 직전에 box blur를 적용합니다.
새 수신 방식은 `StreamReceiver` + `StreamReceiverFactory` 구현으로 확장합니다.

**영상 포맷은 파이프라인 전체가 NV12입니다.** 블러가 CPU에서 돌기 때문에 프레임은 시스템 메모리로 한 번
내려왔다가 sink에서 다시 올라가는데, 예전처럼 BGRA로 바꾸면 픽셀당 4바이트 풀프레임 변환과 2.7배 큰 왕복
전송을 매 프레임 물어야 합니다. `qtblur`는 휘도·색차 평면을 각각 처리하고, `videobalance`/`gamma`/
`d3d11videosink`가 모두 NV12를 받으므로 d3d11 경로에서는 `videoconvert`가 passthrough로 빠집니다.
**블러 코드를 고칠 때 BGRA로 되돌리지 마세요.** 자세한 근거와 실측치는 [VIDEO_SETTINGS.md](VIDEO_SETTINGS.md)에 있습니다.

### 디지털 트윈 맵

**맵은 전부 QML입니다.** `DigitalTwinMapWidget`은 QQuickWidget 하나를 담은 QWidget이고 그림은
`qml/DigitalTwinMap.qml`이 그립니다. C++은 데이터만 맡아 risk 프레임을 world 좌표 → 도면 좌표로
변환한 뒤 QML 속성에 밀어 넣고, QML이 올려 보내는 구역 클릭을 `zoneSelected`로 다시 냅니다.

**도면 기하의 원본은 `qml/ParkingPlan.js` 하나입니다.** 주차 구획·설비실·문·램프·구역 격자가 전부
여기서 나오고, C++은 구역별 객체 영역을 `objectAreas` 속성으로 **읽어** 씁니다. 양쪽에 같은 치수를
두면 언젠가 한쪽만 고쳐져 객체가 도면 밖에 찍힙니다.

객체는 구역 셀 안의 **정사각형 영역**에만 그려지고 `digitalTwin.world.zones`의 월드 상자가
여기에 늘려 맞춰집니다. 그래서 **월드 상자도 정사각형이어야** 가로·세로 배율이 같아집니다(README 참고).
world 좌표는 Y가 위쪽 양수이므로 화면 매핑 시 Y를 뒤집습니다(`invertY`). 보정된 `VEDA_MAP_*` 경계가 없으면
수신 좌표에서 자동으로 경계를 확장하지만, 정확한 채널 사분면 배치에는 고정 경계가 필요합니다.
실 데이터가 처음 들어오면 내장 데모(`DigitalTwinSimulationWorker`)가 중지되고, 5초간 프레임이 없는 채널은 제거됩니다.

**자동 경계 확장은 단조라 되돌아오지 않습니다.** 그래서 `RiskObjectTracker::submitFrame`은 필터를 돌리기
전에 현재 경계를 크게 벗어난 좌표를 버립니다(`removeOutOfRangeObjects`). 중앙값 필터와 속도 상한은 이미
본 gid에만 걸리므로 처음 보는 gid의 첫 좌표는 둘 다 우회하고, 그 한 좌표가 경계를 벌리면 그 세션 내내
정상 객체가 지도 한 점에 뭉칩니다. **좌표를 경계로 clamp하지 마세요** — 잘못된 위치가 정상처럼 보입니다.
warmup 구간에는 비교 기준이 없어 이 검사가 놀고, 고정 경계 설정이 유일한 방어입니다.

**구역 수는 `video.areas` 개수를 따르고 상한은 6입니다**(도면 격자가 3열 x 2행, `digitalTwinMaximumZoneCount`).
설정 로더가 `digitalTwin.world.zones` 길이를 `video.areas`와 같게 맞추므로 둘은 항상 짝이 맞습니다.
지도 쪽 구역 수는 `zoneCount` 속성 하나라 값을 바꾸면 QML이 알아서 다시 그립니다(예전 QGraphicsScene
구현은 scene을 한 번만 세울 수 있어 재시작이 필요했습니다). 다만 **영상 4분할 페이지는 여전히
재시작해야** 반영되므로 설정 팝업 "구역 관리" 탭은 그대로 `ApplicationConfigWriter::appendArea`/
`updateArea`로 설정 파일에만 쓰고 재시작을 안내합니다.
앱이 읽고 쓰는 파일은 빌드 디렉터리 사본이므로, `CMakeLists.txt`의 `configure_file`은 **원본이 더
새로울 때만** 복사합니다(무조건 덮으면 UI로 추가한 구역이 재구성 때 사라집니다).

### Qt Quick(QML) 계층

UI는 **QWidget 골격 + 부분 QML** 하이브리드입니다. `qml/`의 컴포넌트를 `MainWindow::createQuickView()`가
`QQuickWidget`으로 만들어 기존 `.ui` 레이아웃 자리에 끼워 넣고, 원래 있던 위젯은 `hideLayoutContents()`로
숨깁니다(로딩 실패 시 위젯이 그대로 남아 화면이 비지 않도록). 색·글꼴 값은 `qml/Theme.js` 하나에서만 옵니다.
QML 파일도 `CMakeLists.txt`의 `qt_add_resources(qml_resources)`에 등록해야 `qrc:/qml/...`로 잡힙니다.

현재 QML로 옮긴 범위: 상단 표시줄, CCTV 툴바, 5개 패널 중 4개의 제목(`PanelHeader.qml`), 상태 범례,
구역 선택·신고 다이얼로그, 표 2종(객체 목록·이벤트 로그, `DataTable.qml`),
설정 팝업(`SettingsDialog.qml` + `OptionCheckBox`/`OptionComboBox`/`OptionSlider`/`OptionTextField`),
장비 상태 패널(`DeviceStatusView.qml`),
**디지털 트윈 2D 맵**(`DigitalTwinMap.qml` + `ParkingPlan.js` + `PlanRoom`/`PlanDoor`/`ZoneStation`/`MapObjectItem`).
영상 타일과 안내 팝업(`InformationDialog`)은 위젯 그대로입니다.

맵은 곡선·부채꼴·파선이 많아 `QtQuick.Shapes`를 씁니다(`Qt6::QuickShapes` 링크). `Shape`는
`preferredRendererType: Shape.CurveRenderer`로 두어야 배율이 커져도 획이 계단지지 않습니다.
도면은 plan 좌표로 그리고 `stage` Item 하나에만 `scale`을 겁니다 — 자식마다 좌표를 곱하지 않아도 되고
글자는 distance field로 렌더되어 배율이 바뀌어도 뭉개지지 않습니다.

설정 팝업의 컨트롤은 **QtQuick.Controls.Basic**을 테마에 맞게 재스타일해 씁니다(`Qt6::QuickControls2` 링크).
값은 컨트롤이 직접 들고 C++은 `property alias`로 읽고 씁니다 — `checked: someProperty` 식으로 바인딩하면
사용자가 클릭하는 순간 바인딩이 끊겨 C++이 되돌려 쓴 값이 반영되지 않습니다.
사용자 조작만 C++에 올릴 때는 `ComboBox.activated`, `Slider.moved`, `AbstractButton.clicked`를 씁니다
(`currentIndexChanged`/`toggled`는 프로그램적 갱신에도 울립니다).
QML 루트의 `signal`은 동적 metaobject에만 있으므로 C++ 연결은 `SIGNAL()`/`SLOT()` 문자열로 합니다.

이 조합에서 반복해서 발목을 잡는 것들:

- **자식 QQuickWidget의 배경 투명**은 `setClearColor(Qt::transparent)`만으로는 안 되고
  `WA_TranslucentBackground` + `WA_AlwaysStackOnTop`이 **함께** 있어야 합니다. 하나라도 빠지면 검은 박스가 됩니다.
- **화면 전체를 덮는 자식 위젯을 띄우면 그 창의 QQuickWidget이 전부 사라집니다.** Qt가 텍스처 합성을
  건너뛰기 때문입니다. 그래서 `MapSettingsDialog`와 QML 오버레이는 자식이 아니라 **독립 최상위 창**
  (`Qt::Dialog | Qt::FramelessWindowHint`)으로 띄웁니다. 새 팝업을 만들 때도 이 규칙을 따르세요.
  덧붙여 최상위 QQuickWidget은 `WA_TranslucentBackground`를 걸면 아무것도 렌더되지 않고,
  윈도우 플래그는 반드시 `setSource()` **이전에** 지정해야 합니다(이후에 바꾸면 scene graph가 깨집니다).
- **QQuickWidget을 담은 최상위 창 위에 또 다른 QQuickWidget 최상위 창을 띄우면 안쪽이 검은 상자가 됩니다.**
  설정 팝업(QML) 위에서 안내 팝업을 QML로 띄워 봤더니 QML 오류 하나 없이 470x230 검은 사각형만 나왔고,
  `QDialog`→`QWidget`(`Qt::Dialog | Qt::FramelessWindowHint`) 교체로도 안 고쳐졌습니다.
  그래서 `InformationDialog`는 **의도적으로 위젯으로 남겨** 두었습니다. 설정 팝업에서 여는 2차 팝업을
  QML로 바꾸려면 이 문제부터 푸세요(별도 창 대신 설정 QML 안의 오버레이로 그리는 쪽이 현실적입니다).
- **최상위 QQuickWidget 창은 창 자체를 애니메이션하면 죽습니다.** 로그인 창을 걷어내려고
  `windowOpacity`를 `QPropertyAnimation`으로 내렸더니 heap이 깨졌고(`0xC0000374`), `pos`를
  움직이는 방식으로 바꿨더니 같은 자리에서 access violation(`0xC0000005`)이 났습니다. 둘 다
  로그인 성공 0.5초 뒤에 죽고, 백트레이스는 `QObject::event` → `RtlFreeHeap`으로 찍히지만
  거기는 손상된 heap을 만지는 곳일 뿐입니다. `windowOpacity`는 Windows에서 `WS_EX_LAYERED`를
  걸기 때문에 "투명 배경을 걸면 아무것도 렌더되지 않는다"는 위 항목과 같은 뿌리로 보입니다.
  **창 단위 전환 연출은 포기하고 QML 안에서 끝내세요.** `LoginScreen.qml`은 카드를 띄워 지운 뒤
  검은 사각형으로 덮고, C++은 이미 검게 된 창을 `hide()`만 합니다.
  `close()`도 쓰면 안 됩니다 — 마지막 창 닫힘 판정을 타서 앱이 통째로 종료됩니다.
- **`roleNames()`를 override할 땐 반드시 한 번 만든 값을 돌려주세요.** 호출할 때마다 새 `QHash`를 만들면
  QML에 붙이는 순간 heap이 깨집니다(`0xC0000374`/`0xC0000005`). 표를 QML로 옮기는 시도를 네 번 말아먹은
  원인이 이거였습니다.

  ```cpp
  // 이렇게 (include/ui/TableModelRoles.h)
  inline QHash<int, QByteArray> tableModelRoleNames() {
      static const QHash<int, QByteArray> names = { ... };
      return names;   // 암시적 공유 → 매 호출이 같은 실체
  }
  ```

  `QQmlAdaptorModel`은 `roleNames()`를 여러 번 부른 뒤 **서로 다른 호출 결과의 iterator를 짝지어** 씁니다.
  Qt 기본 구현은 정적 hash 하나를 공유해 돌려주므로 우연히 안전하고, 매번 새로 만들면 다른 실체의
  `begin()`/`end()`를 순회해 heap이 망가집니다. 증상은 delegate 생성 중 `RtlFreeHeap`에서 즉사이고
  백트레이스는 `QQmlTableInstanceModel::resolveModelItem`(TableView) 또는 `QQmlDelegateModel::cancel`
  (ListView)로 찍히지만, **거기는 손상된 heap을 처음 만지는 곳일 뿐 원인이 아닙니다.**
  역할 이름의 개수나 내용은 무관합니다 — `{display}` 하나만 돌려줘도 매번 새로 만들면 똑같이 죽습니다.

  이 함정 때문에 헛다리를 짚은 것들(전부 무관으로 실측 확인): RHI 백엔드(d3d11·opengl·software),
  `ListView`↔`TableView`↔`Flickable`+`Repeater`, `reuseItems`, add 트랜지션, `pragma ComponentBehavior`,
  delegate 복잡도, 엔진 분리/공유, QQuickWidget 투명 속성, 부모 `QFrame`의 QSS 배경/모서리,
  model reset 여부, 패널 위치.

  `QAbstractTableModel`에는 `ListView`가 아니라 **`TableView`를 씁니다**(delegate가 `row`/`column`/역할을
  자동으로 받고, 열 너비는 `columnWidthProvider`로 정합니다). `DataTable.qml`이 그 형태입니다.
  열 제목은 C++에서 넘기지 않고 model의 `headerData()`를 QML에서 직접 부릅니다.
- **표 delegate의 역할 속성은 `required property var`로 받고 기본값으로 막습니다.** 행이 새로 생기는
  프레임에는 역할 값이 아직 안 채워져 `undefined`이고, `color`/`string`으로 선언해 두면 그 한 프레임에
  셀에 "undefined"가 찍히고 `QQuickColorValueType ... undefined` 경고가 초당 수십 줄씩 쏟아집니다.
  같은 이유로 model의 `data()`는 범위를 벗어난 index에도 역할별 빈 값을 돌려줍니다
  (`emptyCellValueForRole()`). 두 가지를 다 해야 로그가 깨끗해집니다.
- **`dataChanged`에 역할을 나열할 거면 QML이 읽는 역할을 빠짐없이 넣으세요.** 빠진 역할은 갱신되지 않아
  아이콘은 새 행, 글자는 옛 행이 되는 식으로 어긋납니다. 표가 작으면 그냥 역할 인자를 생략하는 편이 안전합니다.
- **QML로 넘기는 목록에 `QVariantMap`을 담지 마세요. 평평한 `QVariantList`로 넘깁니다.**
  장비 상태 패널을 만들 때 채널마다 `QVariantMap`을 담은 `QVariantList`를 만들면 그 목록이 소멸하는
  자리에서 heap이 깨졌습니다(`RtlFreeHeap` 즉사, 시작 4초 안에). 같은 자리에서 int 목록, 문자열 240개
  목록은 멀쩡했고 `QVariantList`를 원소로 담으면 정상입니다 — 전부 실측으로 확인했습니다.
  그래서 `DeviceStatusPanel::refreshChannels()`는 자리 순서를 정해 평평한 배열로 넘기고
  `DeviceStatusView.qml`이 `modelData[0]`처럼 위치로 읽습니다(양쪽 주석에 순서를 적어 두었습니다).
  덧붙여 `QVariantList`를 원소로 넣을 때는 **반드시 `QVariant()`로 감싸세요.**
  `channels.append(innerList)`는 `QList::append(const QList&)` 오버로드가 골라져 통째로 펼쳐집니다.

### 스레딩

GUI 객체는 GUI 스레드에서만 갱신합니다. MQTT 게이트웨이와 각 RTSP 수신기는 별도 스레드에서 돌고,
스레드 경계는 queued signal/slot 또는 락이 있는 버퍼(`BlurProcessor`의 `QMutex`, atomic 플래그)로만 넘습니다.

## 코딩 규칙

`manual/coding_convention.md`가 원본이고 `.clang-format`/`.clang-tidy`가 강제합니다. 요점:

- 헤더 가드는 `#pragma once`만, 확장자는 `.c`/`.cpp`/`.h`만 사용
- include 순서: C 시스템 → C++ 표준 → 외부/프로젝트 헤더 (clang-format `IncludeBlocks: Regroup`)
- C++ 변수·함수 `camelCase`, 클래스·구조체 `PascalCase`, 매크로 `UPPER_CASE`, 멤버 변수는 `trailing_` 언더스코어
- C++에서 `#define` 상수 금지 → `constexpr`/`const`, `new`/`delete`/`malloc` 직접 호출 금지 → `std::shared_ptr` 기본
- Doxygen 주석은 클래스·함수 단위에만 (기존 코드처럼 한국어로 작성)
- 4칸 들여쓰기, 120열, 포인터는 왼쪽 정렬(`int* p`)

QML에는 clang-format이 적용되지 않습니다. 색·글꼴은 리터럴 대신 `Theme.js`를 쓰고, 글꼴은 `font.family`
하나만 지정합니다(QML `font` 값 타입에 `families`는 없습니다 — qmllint가 잡아줍니다).
위젯 QSS(`styles/app.qss`)와 QML은 같은 글꼴 목록을 쓰지만 래스터라이저가 달라 미세하게 다르게 보입니다.
`QQuickWindow::setTextRenderType(NativeTextRendering)`으로 맞출 수 있지만 QML 쪽 모양이 바뀌므로 임의로 켜지 마세요.

## Git

`manual/git-flow.md` 기준입니다. 브랜치는 `feature/TP-<번호>`, 커밋 메시지는 `tag: [TP-<번호>] 내용`
(한국어 본문). `main`/`develop` 직접 push 금지 — 반드시 PR을 거칩니다. PR CI(format/tidy/build)는
저장소 전체 C/C++ 파일을 검사하므로 클라이언트 코드도 clang-format 결과와 일치해야 합니다.
