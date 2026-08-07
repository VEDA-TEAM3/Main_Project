# 축소 스케일 캘리브레이션 및 실행 매뉴얼 (실내 데모)

> **대상**: 현장 엔지니어
> **목표**: 10 × 10 m 실내 공간을 40 × 40 m 주차장으로 시뮬레이션하여 물리 카메라 → `compute-server` → `control-server` → Qt 전 구간을 실제 하드웨어로 시연한다.
> **전제 조건**: CCTV 물리적 설치 및 배선은 이미 완료된 상태를 가정한다.

---

| Date | Version | Writer | Summary |
| :--- | :--- | :--- | :--- |
| 2026-08-02 | 1.0 | Mangjun | CCTV 180도 플립, 대화형 호모그래피 캘리브레이션 도구(k=4) 실행 절차, 데모 치환 스위치 |

---

## 0. 원리 — 배율은 호모그래피에만 넣는다

축소 공간에서 시연하려면 어딘가에 배율 `k = 4` 를 넣어야 한다. **반드시 호모그래피 행렬 한 곳에만 넣는다.** `warningDistance`, `trackMaxDistance`, `hysteresisMargin` 같은 임계값은 **하나도 건드리지 않는다.**

이유는 두 가지다.
* 임계값을 줄이면 **시연한 설정과 배포할 설정이 달라진다.** 배율을 호모그래피에 넣으면 하류 전체가 운영과 **완전히 동일한 값**으로 돈다.
* `control-server` 의 로컬→월드 변환은 `|det| = 1` 인 강체 변환이라 **배율을 넣으면 안 된다.** 미터의 정의를 바꿀 수 있는 곳은 호모그래피뿐이다.

`k = 4` 일 때 실제 물리 거리로 환산한 값 — 현장에서 줄자로 확인할 값이다.

| 운영 설정값 (건드리지 않음) | 실제 물리 거리 |
|---|---|
| `warningDistance` 5.0 m | 1.25 m |
| `dangerousDistance` 2.0 m | 0.50 m |
| `dedupMergeDistance` 1.0 m | 0.25 m |
| `trackMaxDistance` 4.0 m | 1.00 m (= 물리 속도 상한 4.5 m/s) |
| `hysteresisMargin` 0.5 m | 0.125 m |
| `positionJitterRadius` 0.15 m | 3.75 cm |

---

## 1. 펌웨어 180도 플립 및 확인 (필수)

카메라가 뒤집어 설치된 것을 가정하므로 영상이 상하 반전된다. **Edge AI 의 사람 검출기는 똑바로 선 사람으로 학습되어 있어 반전 영상에서는 사람을 거의 검출하지 못한다.** 

### 1.1 180도 플립 설정
* 카메라 웹 UI 에서 영상 회전 180° (Image → Rotate / Flip → 180°) 를 켠다.
* **⚠️ 순서가 절대적이다 — 플립을 먼저 켜고, 그 다음에 캘리브레이션한다.**
* 캘리브레이션을 먼저 하고 나중에 플립을 켜면 이미지 좌표가 `(u, v) → (1-u, 1-v)` 로 바뀐다. 호모그래피는 그대로이므로 좌표가 **원점 대칭으로 뒤집힌 값**이 나온다. 크기는 그럴듯하고, 예외도 경고도 로그도 없이 객체가 반대쪽 사분면에 나타나고 엉뚱한 채널의 경광등이 켜지게 된다.
* 플립을 켠 뒤 카메라 미리보기에서 **사람이 똑바로 서 보이는지** 눈으로 확인하고 넘어간다.

### 1.2 플립 확인 (검출 테스트)
* 사람 한 명이 각 채널 앞을 천천히 걸어 지나가게 한다.
* 카메라 UI 의 스마트 이벤트 / 객체 검출 오버레이에서 **바운딩 박스가 그려지는지** 확인한다.
* 박스가 안 그려지면 플립이 안 걸렸거나 검출 최소 크기 미달이므로 카메라에서 3 m 이상 떨어져 다시 시도한다.

---

## 2. 사전 준비 및 도구 설치

### 2.1 사전 준비물
* 줄자 (5 m 이상), 마스킹 테이프, 유성 마커
* 노트북 (Python + OpenCV, `mosquitto-clients`)
* 각 채널의 스냅샷을 받을 수 있는 카메라 웹 UI 접근 권한

### 2.2 캘리브레이션 도구 설치
노트북에서 다음 명령어를 통해 필요한 패키지를 설치한다.
```bash
pip install opencv-python numpy
```

> ⚠️ **opencv-python-headless 를 설치하지 말 것**
> headless 빌드에는 cv2.imshow가 없어 클릭 창이 뜨지 않고 cv2.error로 죽는다. 이미 깔려 있다면 지우고 다시 깐다.

## 3. 4점 바닥 측정 및 스냅샷 준비
채널마다 따로 진행한다.

### 3.1 바닥 마크 배치
* 해당 채널이 보는 사분면 바닥에 테이프로 마크 4개를 붙인다.
* 4점이 한 직선 위에 있으면 안 된다. (되도록 큰 사각형을 이루게 배치한다.)
* 화면 안에서 골고루 퍼지게 한다. (한쪽에 몰리면 반대쪽 정확도가 무너진다.)
* 카메라 바로 아래(1m 이내)와 화면 맨 위쪽 끝(지평선 근처)은 피한다.

### 3.2 로컬 좌표 측정
각 마크에 대해 카메라 바로 아래 바닥점을 원점으로 두 값을 잰다.
* `y` = 카메라 전방 거리(m)
* `x` = 전방축 기준 좌우 오프셋(m), 카메라가 보는 방향 기준 오른쪽이(+)
> 좌우 부호는 `lateralSign = 1`로 고정하고, `+x`를 항상 카메라 오른쪽으로 잰다.
> <i>부호 규약을 채널마다 다르게 잡으면 좌우가 조용히 뒤집히는데, 대칭 배치에서는 눈으로 잡아내기 매우 어렵다.</i>

### 3.3 스냅샷 저장
* 180도 플립을 켠 상태로 카메라 웹 UI에서 채널별 정지 영상을 저장한다.
* 바닥 마크 4개가 모두 보이는지 확인한다.
* 파일명에 채널 번호를 넣어 헷갈리지 않게 한다.

## 4. `calibrate.py` 실행 및 호모그래피 계산
노트북 터미널에서 저장한 스냅샷 이미지를 대상으로 도구를 실행한다. 여기서에서 축소 배율 `k=4.0`을 바로 주입한다.

```bash
python deploy/calibrate.py ch0.png --channel 0 --k 4.0 --output ch0.json
```

### 4.1 클릭 단계
* 창이 뜨면 바닥 마크의 바닥 접점을 `A → B → C → D` 순서로 4번 좌클릭한다.
* 우클릭으로 마지막 점 취소, `r`로 전부 초기화, `Enter`로 입력을 완료할 수 있다.

### 4.2 실측 좌표 입력
* 클릭 순서대로 줄자 값을 프롬프트에 입력한다.
* 배율 `k`는 여기서 곱하지 않는다. 실측한 물리 미터를 그대로 넣으면 도구가 `k`를 적용한다.

```plaintext
A x (오른쪽 +): -1.5
A y (전방 +): 2.0
```

### 4.3 출력 확인
도구가 `compute-server`의 `HomographyTransform` 생성자와 같은 계산으로 검증을 수행한다.
* 검증 결과 확인: `[정보] 기준점 재투영 최대 오차 = 0.0000 m` 등의 메시지를 확인한다. 실패 시 에러 내용을 확인하고 다시 실행한다.
* 출력된 config 조각 확보: 화면에 출력되거나 지정한 `--output` 경로(`ch0.json`)에 저장된 JSON 블록을 가져온다.
> <i>이 과정을 채널 4개에 대해 모두 반복한다.</i> `k`는 4채널 모두 동일한 값(4.0)이어야 한다.

## 5. 서버 설정

### 5.1 `compute-server` 설정 (채널마다 1개)
`calibrate.py`가 출력한 JSON 블록을 해당 채널의 `config.json`에 그대로 붙여넣는다.

```JSON
{
  "channelId": 0,
  "homography": [ ... 9개 ... ],
  "homographySpace": "pixel",
  "imageWidth": 1920.0,
  "imageHeight": 1080.0,
  "localBoundsEnabled": true,
  "localMinX": -24.0,
  "localMaxX": 24.0,
  "localMinY": 0.0,
  "localMaxY": 33.94,
  "riskEdgePolicy": "dropBottomTruncated"
}
```
* `localMin/Max*`를 임의로 좁히지 말 것 <br> 이 값은 FOV 마스크가 아니라 캘리브레이션 오류 감지용 sanity 필터다. 좁히면 정상 검출이 `toLocal()`에서 조용히 폐기하고 파이프라인이 텅 빈 채로 돈다.

**기동 확인**: 서버를 띄우고 로그에 아래가 나오면 정상이다.

```plaintext
[Transform] 픽셀 좌표계 호모그래피를 정규화 좌표계로 환산함 (W=1920.000000, H=1080.000000)
[Transform] 로컬 좌표 범위 검사 활성화 x=[-24.000000, 24.000000], y=[0.000000, 33.940000]
```

### 5.2 `control-server` 설정
작업 디렉터리 `config.json`에 아래 설정을 적용한다. `risk` 블록은 통째로 넣지 않고 운영 기본값을 그대로 쓴다.

```JSON
{
  "channelCount": 4,
  "windowSizeMs": 220,

  "demoPedestrianProxy": true,

  "hysteresisMargin": 0.5,

  "worldBounds": { "enabled": true, "minX": -20.0, "maxX": 20.0, "minY": -20.0, "maxY": 20.0 },

  "zones": [
    { "zoneId": 0, "minX":   0.0, "maxX":  20.0, "minY":   0.0, "maxY":  20.0 },
    { "zoneId": 1, "minX":   0.0, "maxX":  20.0, "minY": -20.0, "maxY":   0.0 },
    { "zoneId": 2, "minX": -20.0, "maxX":   0.0, "minY": -20.0, "maxY":   0.0 },
    { "zoneId": 3, "minX": -20.0, "maxX":   0.0, "minY":   0.0, "maxY":  20.0 }
  ],

  "cameraCalibrations": [
    { "channelId": 0, "cameraPosX": 0.0, "cameraPosY": 0.0, "facingAngleDeg":  0, "lateralSign": -1 },
    { "channelId": 1, "cameraPosX": 0.0, "cameraPosY": 0.0, "facingAngleDeg": 90, "lateralSign": 1 },
    { "channelId": 2, "cameraPosX": 0.0, "cameraPosY": 0.0, "facingAngleDeg": 180, "lateralSign": 1 },
    { "channelId": 3, "cameraPosX": 0.0, "cameraPosY": 0.0, "facingAngleDeg": 270, "lateralSign": -1 }
  ]
}
```
* `demoPedestrianProxy`가 `true`인 경우 수신 최외곽에서 `Human`을 `Vehicle`로 바꾼다.
* **⚠️ 운영 배포에서는 반드시 `false`로 설정해야 한다.**

## 6. 시연 후 원복
시연 종료 후 설정 복원 리스트는 다음과 같다.

| 항목 | 원복 값 |
| --- | --- |
| `control-server` `demoPedestrianProxy` | `false` |
| `compute-server` `homography` | 현장 실측 행렬 (k 미적용) |
| `compute-server` `localMin/Max*` | 실제 현장 범위 |
| `control-server` `zones` / `worldBounds` / `cameraCalibrations` | 실제 도면 값 |
| 카메라 180도 플립 | 정방향 거치라면 해제 |

> <i>임계값(`risk` 블록)은 데모 중에도 운영값이었으므로 원복할 것이 없다.</i>