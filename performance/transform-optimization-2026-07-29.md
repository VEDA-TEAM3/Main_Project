# Control-server `transform` 최적화 보고서

- 작성일: 2026-07-29
- 대상: `control-server/src/transform/AffineLocalToWorldTransform`
- 기능 기준: 수정 전 프로덕션 구현
- 적용 방식: 회전계수 사전 계산, 중첩 출력 버퍼 재사용, 무경계 고속 경로

## 1. 결론

`AffineLocalToWorldTransform`의 좌표식, 캘리브레이션 선택, 미보정 채널 정책, 월드 경계
필터링과 출력 순서를 유지하면서 반복 계산과 할당을 제거했다.

아래 값은 수정 전 프로덕션 소스로 먼저 만든 실행 파일과 수정 후 프로덕션 소스로 만든 실행
파일을 같은 입력으로 측정한 결과다. 전체 프로그램을 각각 3번 독립 실행했고, 각 실행 내부에서
9개 측정값의 중앙값을 구한 뒤 세 실행의 중앙값을 표에 사용했다.

| 채널 | 프레임당 객체 | 총 객체 | 경계 검사 | 최적화 전 | 최적화 후 | 배속 | 지연 감소 |
|---:|---:|---:|:---:|---:|---:|---:|---:|
| 4 | 0 | 0 | 꺼짐 | 239.56 ns | 27.95 ns | 8.57× | 88.33% |
| 4 | 1 | 4 | 꺼짐 | 487.89 ns | 56.48 ns | 8.64× | 88.42% |
| 4 | 8 | 32 | 꺼짐 | 596.49 ns | 100.62 ns | 5.93× | 83.13% |
| 4 | 32 | 128 | 꺼짐 | 1,177.10 ns | 265.31 ns | 4.44× | 77.46% |
| 4 | 128 | 512 | 꺼짐 | 3,694.08 ns | 904.32 ns | 4.08× | 75.52% |
| 4 | 32 | 128 | 켜짐 | 1,273.96 ns | 975.72 ns | 1.31× | 23.41% |
| 64 | 8 | 512 | 꺼짐 | 10,370.64 ns | 1,719.60 ns | 6.03× | 83.42% |

계산식:

```text
배속 = 최적화 전 시간 / 최적화 후 시간
지연 감소율(%) = (1 - 최적화 후 시간 / 최적화 전 시간) × 100
```

4채널·총 512개 객체의 계산:

```text
배속 = 3,694.08 / 904.32 = 4.08배
지연 감소율 = (1 - 904.32 / 3,694.08) × 100 = 75.52%
```

## 2. 변경 내용

### 2.1 회전계수 사전 계산

기존 구현은 프레임이 들어올 때마다 채널별로 다음 계산을 반복했다.

```cpp
theta = (90 - facingAngleDeg) * pi / 180
forwardX = cos(theta)
forwardY = sin(theta)
```

카메라 캘리브레이션은 `AffineLocalToWorldTransform` 생성 이후 바뀌지 않는다. 따라서 유한한
`facingAngleDeg`는 생성자에서 다음 여섯 계수로 한 번만 컴파일한다.

```text
cameraPosX, cameraPosY
forwardX, forwardY
perpendicularX, perpendicularY
```

프레임 처리 경로에서는 저장된 계수를 바로 사용한다.

### 2.2 비유한 각도 폴백

초기 구현 후 `facingAngleDeg=NaN` 테스트에서 결과 좌표는 모두 `NaN`이지만 payload 비트가
기존 구현과 다를 수 있다는 사실이 확인됐다. 기능 보존 기준을 수치상 `NaN`으로 완화하지 않고
비트 단위로 유지하기 위해 다음 정책을 적용했다.

```text
유한 facingAngleDeg
  → 생성자에서 sin/cos 1회 계산

NaN 또는 ±Inf facingAngleDeg
  → 기존과 동일하게 transform() 호출마다 sin/cos 계산
```

정상 운영 캘리브레이션은 최적화 경로를 사용하고, 예외 입력은 기존 동작을 그대로 재현한다.

### 2.3 프레임별 중첩 벡터 재사용

기존 코드는 다음 순서였다.

```cpp
out.clear();
out.reserve(in.size());
out.push_back(observed);
```

바깥 `out` 벡터의 capacity는 남지만 `clear()`가 기존 `ObservationFrame`을 파괴하므로, 각
프레임 내부의 `objects` 벡터 capacity는 호출마다 사라졌다. 수정 후에는 다음처럼 처리한다.

```cpp
out.resize(in.size());
observed.objects.clear();
```

따라서 바깥 프레임 버퍼와 프레임별 객체 버퍼가 모두 다음 윈도우에서 재사용된다. 입력 프레임
수가 줄면 `resize()`가 초과 프레임을 제거하고, 각 프레임 객체 수가 줄면 `objects.clear()` 후
정확한 수만 다시 채우므로 이전 호출의 객체가 남지 않는다.

### 2.4 경계 검사가 꺼진 고속 경로

`worldBounds.enabled=false`이면 변환된 객체가 폐기되지 않으므로 출력 객체 수가 입력 객체 수와
항상 같다. 이 경우 출력 벡터 크기를 한 번에 맞추고 인덱스로 직접 기록한다.

```text
경계 검사 꺼짐
  → objects.resize(input.objects.size())
  → 인덱스로 결과 직접 기록
  → 객체별 경계 분기와 push_back 제거

경계 검사 켜짐
  → 기존 순서대로 좌표 계산
  → 범위 밖 객체 폐기
  → 통과한 객체만 push_back
```

경계 검사가 켜진 케이스의 개선 폭이 23.41%로 상대적으로 작은 이유도 이 필터링과 조건부
출력을 그대로 유지해야 하기 때문이다.

## 3. 기능 동일성 검증

추가 파일:

```text
tests/control-server/TransformTestSupport.h
tests/control-server/unit/TransformEquivalenceTest.cpp
performance/TransformBenchmark.cpp
```

테스트 실행 결과:

```text
[==========] 4 tests from 1 test suite ran.
[  PASSED  ] 4 tests.
```

검증 범위:

1. 채널별 회전·이동 좌표식
2. 양수·음수 `lateralSign`
3. 중복 채널 캘리브레이션의 first-match 규칙
4. 음수 캘리브레이션 채널 무시
5. 미보정 채널의 폐기 정책
6. `dropUncalibrated=false`의 좌표 그대로 통과 정책
7. 월드 경계 포함 및 범위 밖 객체 폐기
8. 프레임 타임스탬프, 채널, 객체 ID, 클래스와 순서
9. 큰 출력 다음 작은 출력을 처리할 때 이전 객체가 남지 않는지
10. 고정 시드 `0x5452414E53464F52`의 500개 연속 무작위 윈도우
11. `NaN`, `+Inf`, `-Inf` 로컬 좌표
12. `NaN` 카메라 방위각

좌표의 `double` 값은 허용 오차 비교가 아니라 IEEE-754 비트값을 직접 비교했다. 최적화 후
`AppContext.cpp`도 `-fsyntax-only` 컴파일을 통과했다.

## 4. 벤치마크 방법

수정 전 측정 순서:

1. 원래 `AffineLocalToWorldTransform.cpp`를 직접 컴파일
2. `TransformBenchmarkBaseline.exe` 생성
3. 3회 독립 실행
4. 결과 저장
5. 그 후에만 프로덕션 코드 수정

수정 후 측정:

1. 변경된 동일 프로덕션 소스를 직접 컴파일
2. 같은 `TransformBenchmark.cpp`와 같은 입력 사용
3. 3회 독립 실행

환경:

- 컴파일러: MinGW-w64 GCC 13.1.0
- 옵션: `-std=c++20 -O2 -DNDEBUG -pthread`
- 운영체제: Windows NT 10.0.26200, x64
- 프로세서 식별자: Intel64 Family 6 Model 140 Stepping 1
- 논리 프로세서: 8
- 워밍업: 각 trial 전 50회
- 각 케이스: 9개 trial의 중앙값
- 전체 실행: 수정 전·후 각각 3회 후 중앙값
- 생성자 시간: 제외

입력 좌표와 캘리브레이션은 실행마다 동일하게 생성된다. 결과가 사용되지 않아 컴파일러가 호출을
제거하지 못하도록 출력 프레임·객체·ID로 checksum도 계산한다.

## 5. 해석과 한계

빈 프레임과 객체가 적은 프레임에서는 프레임별 삼각함수와 중첩 벡터 생명주기 비용이 대부분이라
8배 이상 개선됐다. 객체가 많아지면 실제 좌표 곱셈·덧셈과 결과 기록 비중이 커지므로 개선 폭은
4배 수준으로 내려간다.

이번 숫자는 Windows/MinGW 합성 입력의 steady-state 결과다. 배포 Raspberry Pi/Linux에서의
절대 지연시간이라고 주장할 수 없다. 실제 배포 판단에는 타깃 CPU에서 실제 MQTT 프레임 기록으로
재측정해야 한다.

4개 명시적 테스트와 500개 생성 윈도우의 비트 단위 일치는 강한 회귀 증거지만, 모든 가능한
입력에 대한 형식적 증명은 아니다. 또한 애플리케이션이 실행 도중 부동소수점 반올림 모드를
의도적으로 바꾸는 비표준 상황에서는 유한 각도의 사전 계산 시점이 의미를 가질 수 있다. 현재
저장소에는 반올림 모드를 변경하는 코드가 확인되지 않았다.

현재 머신에는 저장소의 기존 Linux 전체 빌드 캐시가 요구하는 `/usr/bin/cmake`,
`/usr/bin/c++`, 시스템 `nlohmann_json`, `libmosquitto` 조합이 없다. 따라서 전체 서버 링크는
실행하지 못했다. 대신 변경된 transform 프로덕션 소스를 직접 컴파일·링크하고, GTest를 실행하고,
실제 조립 코드인 `AppContext.cpp`의 구문 컴파일을 확인했다.
