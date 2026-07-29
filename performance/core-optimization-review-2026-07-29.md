# Control-server `core` 최적화 검토 보고서

- 작성일: 2026-07-29
- 대상:
  - `control-server/src/core/Controller.cpp`
  - `control-server/include/core/Controller.h`
  - `control-server/src/core/AppContext.cpp`
- 결론: 프로덕션 코드 변경 없음

## 1. 결론

현재 `core`에는 성능 개선이라고 입증할 수 있는 변경이 없다.

확인된 유일한 미세 최적화 후보는 집계 콜백이 소유한
`std::vector<veda::TopViewFrame>`을 `processPipeline()`의 값 매개변수로 한 번 더 이동하는
부분이다.

```cpp
// 현재
[this](std::vector<veda::TopViewFrame> frames) {
    this->processPipeline(std::move(frames));
}

void Controller::processPipeline(std::vector<veda::TopViewFrame> frames);
```

후보 변경은 `processPipeline()`이 `const&`를 받도록 해 벡터 move-construction 한 번을
제거하는 것이다.

```cpp
// 검토 후보
[this](std::vector<veda::TopViewFrame> frames) {
    this->processPipeline(frames);
}

void Controller::processPipeline(
    const std::vector<veda::TopViewFrame>& frames);
```

그러나 4프레임 측정값은 `59.36 ns → 59.11 ns`로 차이가 `0.25 ns`뿐이었고, 독립 실행
변동폭보다 작았다. 1, 16, 64프레임에서는 후보가 오히려 느렸다. 따라서 후보 변경을 성능
개선이라고 주장할 수 없으며 프로덕션 코드에 적용하지 않았다.

기능 불변이 최우선이라는 조건에서, 입증되지 않은 변경을 강제로 넣는 것은 최적화가 아니다.

## 2. 후보 변경 전후 측정

아래 값은 동일 실행 파일을 3회 독립 실행하고, 각 실행에서 11개 측정값의 중앙값을 구한 뒤
다시 3회 실행값의 중앙값을 취한 결과다.

| 프레임 수 | 현재 값 전달 | 후보 `const&` 전달 | 배속 | 지연 감소율 |
|---:|---:|---:|---:|---:|
| 1 | 58.57 ns | 61.91 ns | 0.95× | -5.70% |
| 4 | 59.36 ns | 59.11 ns | 1.00× | 0.42% |
| 16 | 85.92 ns | 89.20 ns | 0.96× | -3.82% |
| 64 | 157.93 ns | 163.84 ns | 0.96× | -3.74% |

계산식:

```text
배속 = 현재 시간 / 후보 시간
지연 감소율(%) = (1 - 후보 시간 / 현재 시간) × 100
```

4프레임 계산:

```text
배속 = 59.36 / 59.11 = 1.0042배
지연 감소율 = (1 - 59.11 / 59.36) × 100 = 0.42%
절대 차이 = 59.36 - 59.11 = 0.25 ns/call
```

현재 주석에 적힌 기본 실행 빈도인 초당 10회로 단순 환산하면 다음과 같다.

```text
0.25 ns/call × 10 call/s = 2.5 ns/s
```

이 값은 실제 서버 전체 지연 감소량이 아니다. 측정 변동보다 작은 후보 차이를 단순 환산한
상한 성격의 숫자이며, 성능 향상 근거로 사용할 수 없다.

### 독립 실행 변동

| 프레임 수 | 현재 3회 범위 | 후보 3회 범위 |
|---:|---:|---:|
| 1 | 58.32~59.61 ns | 58.95~62.58 ns |
| 4 | 56.24~63.88 ns | 55.08~60.77 ns |
| 16 | 69.95~88.28 ns | 87.63~107.68 ns |
| 64 | 155.71~166.87 ns | 150.66~168.97 ns |

4프레임 후보 차이 `0.25 ns`는 현재 구현 실행 범위 `7.64 ns`와 후보 실행 범위
`5.69 ns`보다 훨씬 작다. 통계적으로 유의한 개선이라고 판정할 근거가 없다.

## 3. 정적 분석 결과

### `Controller`는 계산 모듈이 아니라 오케스트레이터

프레임 hot path는 다음 순서로 각 구현에 작업을 위임한다.

```text
transform
  → fuse
  → zone assign
  → risk evaluate
  → hardware dispatch
  → sink send
```

좌표 변환, 객체 융합, 구역 검색과 같은 계산량은 각 하위 모듈에 있고 `Controller` 자체에는
프레임 또는 객체 수에 비례하는 계산이 거의 없다.

유일한 반복문은 Debug 로그에서 최고 위험도를 찾는 루프다. 이 루프는
`isLogEnabled(LogLevel::Debug)`가 참일 때만 실행되므로 일반 로그 레벨에서는 비용이 없다.

### 변환 출력 버퍼는 이미 재사용

`observations_`는 `Controller` 멤버이며 매 윈도우마다 새 벡터 객체를 생성하지 않는다.
`ILocalToWorldTransform::transform()`도 호출자 소유 출력 버퍼를 받도록 정의되어 있다.

```cpp
std::vector<domain::ObservationFrame> observations_;
```

### 상태 처리의 lock 범위는 이미 제한됨

`onChannelAlive()`와 `onHardwareStatus()`는 mutex 안에서 상태 스냅샷만 갱신한다.
로그 문자열 생성, 시각 취득, MQTT 상태 발행은 mutex 밖에서 수행한다. 네트워크 발행을
lock 안으로 이동하지 않아 다른 상태 콜백을 불필요하게 막지 않는다.

### `AppContext`는 시작 시 한 번 실행

`AppContext::buildController()`는 의존 객체를 생성하고 연결하는 조립 코드다. 프레임마다
반복되는 경로가 아니므로 생성자 미세 조정은 steady-state 프레임 지연을 줄이지 않는다.
Receiver와 Sink도 같은 `MqttTransport` 인스턴스를 이미 공유한다.

## 4. 적용하지 않은 변경

### 파이프라인 병렬화

적용하지 않았다. `fuse`는 전역 ID 추적 상태를 가지고 있고 `zone`, `risk`, `dispatch`,
`sink`는 앞 단계 결과와 순서에 의존한다. 병렬 실행이나 프레임 동시 처리는 출력 순서와
상태 전이를 바꿀 수 있어 기능 불변 조건을 만족한다고 증명되지 않았다.

### 가상 함수와 `shared_ptr` 제거

적용하지 않았다. 프레임당 인터페이스 호출 수는 고정되어 있지만 그 비용을 분리 측정하지
않았고, 구체 타입 결합은 의존성 주입과 테스트 대역 사용 방식을 바꾼다. 성능 자료 없이
구조적 비용을 지불할 근거가 없다.

### Debug 최고 위험도 루프 제거

`ThresholdRiskPolicy` 구현에서는 `WorldFrame::level`이 `zoneLevels`의 최고값으로 설정되지만,
`Controller`는 `IRiskPolicy` 인터페이스에 의존한다. 다른 정책 구현까지 같은 관계를
보장한다는 인터페이스 계약은 확인되지 않았다. 로그 결과가 달라질 가능성이 있어 변경하지
않았다.

## 5. 벤치마크 방법

벤치마크 소스:

```text
performance/CoreBoundaryBenchmark.cpp
```

측정 대상은 현재 콜백 구조의 추가 벡터 move와 후보 참조 전달 구조의 차이다.
`std::vector`의 move는 내부 버퍼 소유권을 옮기며 요소별 복사를 하지 않는다. 이 비용을
프로젝트 외부 라이브러리 없이 분리하기 위해 합성 `BenchmarkFrame`을 사용했다.

측정 환경:

- 컴파일러: MinGW-w64 GCC 13.1.0
- 옵션: `-std=c++20 -O2 -DNDEBUG`
- 운영체제: Microsoft Windows NT 10.0.26200.0
- 프로세서 식별자: Intel64 Family 6 Model 140 Stepping 1
- 논리 프로세서: 8
- 워밍업: 각 trial 전 1,000회
- 각 케이스: 11개 trial의 중앙값
- 전체 프로그램: 3회 독립 실행 후 중앙값

실행 명령:

```powershell
g++ -std=c++20 -O2 -DNDEBUG `
  performance\CoreBoundaryBenchmark.cpp `
  -o build\CoreBoundaryBenchmark.exe

1..3 | ForEach-Object {
  .\build\CoreBoundaryBenchmark.exe
}
```

## 6. 검증 범위와 한계

- 프로덕션 `core` 코드는 변경하지 않았으므로 이번 작업으로 인한 기능 회귀는 없다.
- 벤치마크 소스는 GCC 13.1.0 `-O2`로 컴파일됐고 3회 실행됐다.
- 현재 Windows 환경에는 CMake 명령과 프로젝트가 요구하는 `nlohmann/json.hpp`가 없어
  control-server 전체 빌드 및 실제 `veda::TopViewFrame` 기반 링크 벤치마크는 실행하지
  못했다.
- 합성 프레임은 벡터 소유권 이동 경계만 검증한다. 실제 transform/fuse/zone/risk/MQTT가
  포함된 end-to-end 지연을 나타내지 않는다.
- 이번 결론은 “core 비용이 0”이라는 뜻이 아니다. 확인한 후보가 측정 잡음보다 작아서
  프로덕션 변경을 정당화하지 못했다는 뜻이다.

## 7. 최종 판단

`core`는 현 상태를 유지한다.

실제 처리량 개선은 이미 개별 알고리즘을 가진 `transform`, `fuse`, `zone`처럼 입력 크기에
따라 연산량이 증가하는 모듈에서 수행하는 것이 타당하다. `core`에 변경을 넣으려면 실제
배포 장비의 프로파일에서 `Controller` 자체가 병목이라는 증거가 먼저 필요하다.
