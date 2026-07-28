# OnvifParser 보안 재검토 보고서

## 1. 문서 정보

| 항목 | 내용 |
|---|---|
| 대상 저장소 | `VEDA-TEAM3/Main_Project` |
| 기준 브랜치 | `develop` |
| 기준 커밋 | `24abcb6` |
| 대상 모듈 | `compute-server/src/parser/OnvifParser.cpp`, `.h` |
| 참조 문서 | `DevSunbi.github.io/parser_security.md`, `parser_reference.md` |
| 분석 방식 | 참조 문서와 현재 구현의 line-by-line 정적 대조 |

이 보고서는 기존 [Secure_report.md](Secure_report.md)의 전체 시스템 보안 진단을 대체하지
않는다. 신뢰 경계의 첫 단계인 `OnvifParser`에 집중해 참조 문서의 주장과 현재 코드가 실제로
일치하는지 다시 검증한다.

---

## 2. 결론

참조 문서가 설명하는 구조적 장점은 대체로 현재 코드와 일치한다.

- 완전한 XML parser/entity resolver를 사용하지 않아 XXE와 entity expansion 공격이 성립하지 않음
- 재귀가 없어 XML 중첩 깊이에 의한 stack overflow가 성립하지 않음
- `string_view`와 단조 증가 cursor를 사용해 주요 loop가 종료됨
- 상류 RTSP 조립 단계에 metadata frame 크기 상한이 존재함

하지만 참조 보안 문서에서 **적용 완료**로 기록한 W1~W3 완화책은 현재 `develop` 코드에
존재하지 않는다.

| 참조 문서의 완화책 | 문서상 상태 | 현재 코드 상태 |
|---|---|---|
| W1 `sanitizeForLog()` | 적용 완료 | **미적용** |
| W2 bbox `std::isfinite()` | 적용 완료 | **미적용** |
| W3 `kMaxObjectsPerFrame = 256` | 적용 완료 | **미적용** |

추가로 숫자 parser가 입력 전체 소비를 확인하지 않고, timestamp의 구분자·범위를 검증하지
않으며, `parse()`가 “예외를 던지지 않는다”는 interface 계약을 코드 수준에서 보장하지 않는다.

### 위험도 요약

| ID | 등급 | 발견사항 |
|---|---|---|
| PARSER-01 | High | 객체 수 상한 부재로 CPU·메모리 DoS 가능 |
| PARSER-02 | Medium | NaN/Inf 좌표가 parser 경계를 통과 |
| PARSER-03 | Medium | 신뢰 불가 Type 문자열이 console/CSV 로그에 직접 유입 |
| PARSER-04 | Medium | 숫자 전체 소비를 확인하지 않아 부분 숫자 입력을 허용 |
| PARSER-05 | Medium | timestamp 형식·범위 검증 부족과 `timegm` 정규화 |
| PARSER-06 | Medium | no-throw 계약이 실제 코드에서 보장되지 않음 |
| PARSER-07 | Low~Medium | 속성 이름 경계 미검증으로 attribute confusion 가능 |
| PARSER-08 | Low | 보안 문서와 구현 불일치로 잘못된 보안 승인 가능 |

---

## 3. 위협 모델

입력은 CCTV가 RTSP interleaved RTP로 전달하는 ONVIF XML metadata다. 카메라가 안전하다고
가정하면 안 된다.

공격자는 다음 능력 중 하나를 가질 수 있다.

- 물리적으로 카메라를 교체하거나 탈취
- 카메라 firmware 또는 관리 계정 장악
- 평문 RTSP 네트워크에서 metadata 변조
- 카메라로 위장해 악성 RTSP server 운영
- 정상 형태처럼 보이는 매우 큰 XML frame 반복 전송

보호 자산:

- 채널 Compute Server의 가용성
- 객체 class, bbox, timestamp의 무결성
- Control Server로 전달되는 TopView/Risk 결과의 신뢰성
- 운영 로그의 무결성과 분석 가능성
- Raspberry Pi의 제한된 CPU·메모리

보안 목표:

1. 임의 입력으로 process가 crash하거나 무한정 자원을 사용하지 않는다.
2. 비정상 좌표·시간·class를 유효한 객체로 하류에 전달하지 않는다.
3. 신뢰 불가 문자열이 로그 구조나 terminal 제어에 영향을 주지 않는다.
4. parser 실패는 명확한 invalid/empty frame으로 표현하고 pipeline을 중단하지 않는다.

---

## 4. 구조적으로 방어되는 공격

## 4.1 XXE 및 외부 entity

상태: **구조적 면역**

현재 parser는 `<!DOCTYPE>`, entity declaration, external URI를 처리하지 않는다. `&xxe;`는
단순 문자열로 남을 뿐 filesystem이나 network 접근으로 연결되지 않는다. 따라서 일반 XML
engine에서 발생하는 file disclosure와 SSRF 형태의 XXE는 성립하지 않는다.

## 4.2 Billion Laughs

상태: **구조적 면역**

Entity 정의와 재귀 expansion 기능이 없으므로 지수적 entity expansion 경로가 존재하지 않는다.

## 4.3 XML 중첩에 의한 stack overflow

상태: **구조적 면역**

`OnvifParser`는 recursive descent를 사용하지 않는다. 주요 처리는 `find()`와 반복문으로
구성되어 XML 중첩 깊이가 call stack 깊이로 변환되지 않는다.

## 4.4 주요 loop 종료성

상태: **대체로 안전**

- `extractQuoted()`의 `searchFrom`은 `keyPos + 1`로 증가한다.
- 객체 loop의 `pos`는 `objEnd + 12`로 증가한다.
- 모든 `find()` 결과는 주요 사용 전에 `npos`를 확인한다.

현재 검토 범위에서 malformed XML만으로 명백한 무한 loop 또는 직접적인 out-of-bounds read
경로는 발견하지 못했다.

---

## 5. 상세 발견사항

## PARSER-01. 객체 수 상한 부재

- 심각도: **High**
- CWE: CWE-400, CWE-770
- 근거:
  - `compute-server/src/parser/OnvifParser.cpp:223-314`
  - `compute-server/src/network/RtspClientV2.cpp:315-320`

### 현재 동작

상류 `RtspClientV2`는 metadata frame을 기본 1 MiB로 제한하지만 parser는 frame 안의
`<tt:Object>` 개수를 제한하지 않는다. 객체마다 `DetectedObject`를 만들어 vector에
`push_back()`한다.

1 MiB 안에 작은 Object element를 대량 배치하면 다음 비용이 발생한다.

- 전체 frame 반복 검색
- 객체별 다수의 `find()`/`from_chars()`
- `DetectedObject` vector 확장
- 후속 sanitizer/router/mapper/transform 처리
- TopView 및 Blur frame 복사·직렬화

상류 byte 상한은 입력 크기를 제한하지만 parser 출력 개수와 하류 계산 증폭을 직접 제한하지
않는다.

### 해결 방안

```cpp
constexpr std::size_t kMaxObjectsPerFrame = 256;

while ((pos = frame.find("<tt:Object", pos)) != std::string_view::npos) {
    if (result.objects.size() >= kMaxObjectsPerFrame) {
        recordParserDrop(raw.channelId, "object limit exceeded");
        break;
    }
    // ...
}
```

권장 사항:

- 상한은 실제 카메라 최대 검출 수와 부하 시험으로 결정
- 상한 초과 시 나머지 객체를 무시하고 rate-limited counter 증가
- frame byte 상한과 object count 상한을 독립적으로 유지
- 후속 단계에도 비정상적으로 큰 collection을 거부하는 방어 추가

### 검증

- 객체 255, 256, 257개 boundary test
- 1 MiB 내 최소 Object 반복 payload의 실행시간/RSS 측정
- 상한 초과 시 반환 object 수가 정확히 상한 이하인지 검증

---

## PARSER-02. NaN/Inf 및 산술 overflow 좌표 통과

- 심각도: **Medium**
- CWE: CWE-20, CWE-682
- 근거:
  - `compute-server/src/parser/OnvifParser.cpp:140-147`
  - `compute-server/src/parser/OnvifParser.cpp:156-166`
  - `compute-server/src/parser/OnvifParser.cpp:265-280`

### 현재 동작

`std::from_chars<double>`은 구현에서 지원하는 `nan`, `inf` 계열 값을 성공으로 처리할 수 있다.
입력 숫자가 유한하더라도 매우 큰 scale과 좌표의 곱셈은 Infinity를 만들 수 있다. 현재 코드는
`normX/normY` 결과를 바로 bbox에 넣고 경계 flag를 계산한다.

후속 Sink가 일부 `isfinite` 검증을 수행하더라도 parser와 Sink 사이의 mapper, ground point,
homography 및 routing 경로가 오염된 값의 영향을 받을 수 있다. 신뢰 경계에서 가능한 빨리
차단하는 것이 안전하다.

### 해결 방안

Transformation 값과 최종 bbox 양쪽을 검사한다.

```cpp
bool finiteTransform(const Transformation& t) {
    return std::isfinite(t.translateX) &&
           std::isfinite(t.translateY) &&
           std::isfinite(t.scaleX) &&
           std::isfinite(t.scaleY);
}

if (!std::isfinite(det.box.l) || !std::isfinite(det.box.t) ||
    !std::isfinite(det.box.r) || !std::isfinite(det.box.b)) {
    continue;
}
```

추가로 bbox의 방향과 허용 범위 정책을 명확히 해야 한다.

- `l <= r`, `t <= b`
- 지나치게 큰 좌표를 clamp할지 객체를 drop할지 결정
- Transformation scale의 운영상 허용 범위 정의

### 검증

- `nan`, `inf`, `-inf`, `1e308`
- 유한 입력의 곱셈 overflow
- 역전 bbox와 범위 밖 bbox
- 어떤 경우에도 하류 frame에 non-finite 값이 없어야 함

---

## PARSER-03. Log/Terminal/CSV injection

- 심각도: **Medium**
- CWE: CWE-117
- 근거:
  - `compute-server/src/parser/OnvifParser.cpp:298-310`
  - `shared/Logger.h:261-265`
  - `shared/Logger.h:287-293`

### 현재 동작

미지원 `<tt:Type>` 내용이 다음과 같이 로그 문자열에 직접 결합된다.

```cpp
" 인식 안 되는 Type=\"" + std::string(typeText) + "\""
```

Logger의 CSV writer는 quote를 escape하므로 단순 double quote와 comma에 대해서는 기본 보호가
있다. 그러나 신뢰 불가 문자열의 다음 요소는 그대로 남는다.

- `\r`, `\n`: terminal 또는 단순 line 기반 log consumer에서 가짜 행처럼 보일 수 있음
- ANSI escape: console 표시 조작
- 매우 긴 문자열: log 확대와 분석 방해
- 제어문자: downstream parser 및 SIEM ingestion 혼란

따라서 참조 문서의 설명처럼 logging boundary에서 길이와 문자 집합을 제한할 필요가 있다.

### 해결 방안

```cpp
std::string sanitizeForLog(std::string_view text) {
    constexpr std::size_t kMaxLoggedTextLen = 50;
    const std::size_t size = std::min(text.size(), kMaxLoggedTextLen);
    std::string out;
    out.reserve(size + 3);

    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        out.push_back(ch < 0x20 || ch == 0x7f ? '_' : static_cast<char>(ch));
    }
    if (text.size() > kMaxLoggedTextLen)
        out += "...";
    return out;
}
```

CSV writer가 구조 escaping을 담당하므로 parser sanitizer는 제어문자와 길이 제한에 집중할 수
있다. 방어를 더 단순하게 하려면 printable ASCII allowlist를 사용할 수 있다.

### 검증

- Type에 CRLF, comma, quote, tab, ESC sequence 삽입
- 100 KiB Type 문자열
- console과 CSV 모두 한 사건이 한 논리적 record로 유지되는지 확인

---

## PARSER-04. 숫자 parser가 입력 전체 소비를 확인하지 않음

- 심각도: **Medium**
- CWE: CWE-20
- 근거: `compute-server/src/parser/OnvifParser.cpp:26-33`

### 현재 동작

```cpp
auto [ptr, ec] = std::from_chars(...);
if (ec != std::errc{})
    return std::nullopt;
return value;
```

`ptr == end`를 확인하지 않는다. 따라서 `123junk`, `1.0evil`처럼 숫자로 시작하는 문자열은
앞부분만 성공적으로 읽고 유효한 값으로 처리될 수 있다.

이는 memory corruption을 직접 만들지는 않지만 malformed 입력을 정상 metadata로 승인하는
validation bypass다.

### 해결 방안

```cpp
const char* begin = sv.data();
const char* end = begin + sv.size();
const auto [ptr, ec] = std::from_chars(begin, end, value);
if (ec != std::errc{} || ptr != end)
    return std::nullopt;
```

선행·후행 whitespace를 허용할지 contract로 결정하고, 허용한다면 parsing 전에 명시적으로
trim한다.

### 검증

- `"123"`, `"-1.25"` 성공
- `"123x"`, `"1.0 "`, `""`, `"+"`, `"--1"` 실패
- integer overflow/underflow 실패

---

## PARSER-05. Timestamp 형식과 calendar 범위 검증 부족

- 심각도: **Medium**
- CWE: CWE-20
- 근거: `compute-server/src/parser/OnvifParser.cpp:71-99`

### 현재 동작

`UtcTime`은 길이가 23 이상인지 확인한 뒤 고정 위치의 숫자만 읽는다. 다음을 확인하지 않는다.

- `-`, `T`, `:`, `.`, `Z` 구분자
- month/day/hour/minute/second/millisecond 범위
- trailing data
- leap day와 실제 calendar date

`timegm()`은 일부 범위 밖 값을 다음 달/해로 정규화한다. 공격자가 잘못된 시각을 유효한 epoch로
변환해 aggregation window, frame ordering, risk event timestamp를 왜곡할 수 있다.

### 해결 방안

1. 정확한 wire format을 결정한다. 예: `YYYY-MM-DDTHH:MM:SS.mmmZ`, 길이 정확히 24.
2. 모든 separator와 마지막 `Z`를 검사한다.
3. 각 필드의 범위를 검사한다.
4. `timegm()` 변환 후 UTC로 역변환하여 원래 calendar field와 같은지 확인한다.
5. 운영 정책에 따라 CCTV timestamp와 수신 시각의 최대 차이를 제한한다.

### 검증

- 정상 leap year
- month 00/13, day 00/32
- 2월 30일
- hour 24, minute/second 60 이상
- separator 변조 및 timezone suffix
- 현재 시각에서 과도하게 벗어난 timestamp

---

## PARSER-06. “예외를 던지지 않는다”는 계약이 보장되지 않음

- 심각도: **Medium**
- CWE: CWE-248
- 근거:
  - `compute-server/src/parser/OnvifParser.h:27-34`
  - `compute-server/src/parser/OnvifParser.cpp:184-316`

### 현재 동작

Header는 오류나 예외 발생 시 빈 frame을 반환한다고 설명하지만 `parse()`는 `noexcept`가 아니며
함수 전체 catch boundary도 없다. 다음 연산은 예외를 던질 수 있다.

- `result.objects.push_back()`의 allocation failure
- 로그 메시지 문자열 생성
- `std::string(typeText)` allocation

특히 PARSER-01의 객체 증폭은 allocation failure 가능성을 높인다. `std::bad_alloc`까지 무조건
복구하는 것이 항상 가능한 것은 아니지만, 문서의 no-throw 계약과 구현은 일치해야 한다.

### 해결 방안

선호 순서:

1. 객체 수와 로그 길이를 먼저 제한해 정상적으로 예외가 발생할 조건을 줄인다.
2. `result.objects.reserve()`는 상한까지만 보수적으로 사용한다.
3. `parse()` 호출 경계 또는 pipeline 경계에서 `std::exception`을 잡아 해당 frame을 drop한다.
4. 실제로 강한 no-throw를 보장할 수 없다면 interface 문서를 “malformed input은 throw하지
   않지만 resource exhaustion은 예외 가능”으로 정확히 수정한다.

`bad_alloc` 후 계속 실행할 메모리도 없을 수 있으므로 단순히 모든 예외를 삼키는 것만으로
가용성이 보장되지는 않는다. resource 상한이 우선이다.

### 검증

- custom allocator/failure injection으로 vector 확장 실패 유도
- parser exception이 process 종료 대신 frame drop 또는 상위 복구로 이어지는지 확인

---

## PARSER-07. 속성 이름 token boundary 검증 부족

- 심각도: **Low~Medium**
- CWE: CWE-20
- 근거: `compute-server/src/parser/OnvifParser.cpp:45-63`

### 현재 동작

`extractQuoted(s, "ObjectId")`는 key의 앞 경계를 확인하지 않는다. 따라서 다음처럼 다른 속성
이름의 suffix가 요청 key와 일치하면 값이 선택될 수 있다.

```xml
<tt:Object FakeObjectId="123">
```

`x`, `y`처럼 짧은 key는 오인 가능성이 더 높다. 현재는 Translate/Scale tag substring으로
범위를 줄여 위험을 낮추지만, 엄격한 XML attribute parser는 아니다.

또한 BoundingBox 종료 `>`가 없으면 객체 나머지 전체를 bbox 검색 범위로 사용해 이후 nested
tag의 `left/top/right/bottom` 문자열을 잘못 선택할 수 있다.

### 해결 방안

- key 앞 문자가 tag 시작 또는 XML whitespace인지 확인
- key 뒤가 정확히 optional whitespace + `=` + optional whitespace + quote인지 확인
- single quote를 지원할지 명시
- `>`가 없는 시작 tag는 해당 객체를 즉시 drop
- 장기적으로는 entity/network 기능을 완전히 끈 검증된 pull parser 사용도 비교 검토

검증된 XML library를 도입할 경우 XXE 방어 옵션을 명시적으로 고정하고 regression test를
유지해야 한다. 현재 수동 parser의 구조적 면역을 잃지 않도록 주의한다.

---

## PARSER-08. 참조 보안 문서와 현재 구현의 불일치

- 심각도: **Low** (감사·승인 과정에서는 Medium)
- CWE: CWE-1059
- 근거:
  - `DevSunbi.github.io/parser_security.md`
  - 현재 `compute-server/src/parser/OnvifParser.cpp`

### 현재 상태

참조 문서는 W1~W3을 패치 완료로 기록하지만 현재 `develop`에는 해당 symbol과 guard가 없다.
가능한 원인은 다음과 같다.

- 패치가 다른 branch 또는 local working tree에만 존재
- 문서가 코드 merge보다 먼저 공개
- 이후 refactor에서 완화 코드가 누락
- 문서가 미래 목표 상태를 완료 상태로 잘못 표시

보안 문서가 구현보다 앞서면 reviewer가 존재하지 않는 방어를 신뢰해 배포를 승인할 수 있다.

### 해결 방안

1. W1~W3 패치를 code review와 test와 함께 같은 PR에 포함한다.
2. 보안 문서에는 기준 commit SHA와 검증 command를 기록한다.
3. 보안 invariant를 테스트로 고정한다.
4. CI에서 다음 symbol/behavior를 테스트한다.
   - object cap
   - non-finite drop
   - log sanitization
5. 문서의 “적용 완료”는 merge된 commit 링크가 있을 때만 사용한다.

---

## 6. 문서 주장 대조표

| 참조 문서 주장 | 현재 구현 검증 | 판정 |
|---|---|---|
| entity resolver가 없음 | XML engine 자체를 사용하지 않음 | 일치 |
| entity expansion 없음 | 단순 `find` scanner | 일치 |
| recursion 없음 | loop 기반 | 일치 |
| 단조 cursor와 `npos` guard | 주요 loop에서 확인 | 일치 |
| frame 최대 1 MiB | `rtspMaxMetadataFrameBytes` 기본값과 runtime guard 존재 | 일치 |
| log sanitizer 적용 | symbol/call site 없음 | **불일치** |
| bbox `isfinite` 적용 | parser에 guard 없음 | **불일치** |
| 객체 최대 256개 | constant/guard 없음 | **불일치** |
| parsing failure는 throw하지 않음 | malformed branch는 빈 frame, allocation 예외 boundary 없음 | 부분 일치 |
| 숫자 parser가 안전함 | `from_chars` error만 보고 trailing data 허용 | 부분 일치 |
| OOB 경로 없음 | 명백한 direct OOB는 발견되지 않음 | 대체로 일치 |

---

## 7. 권장 패치 순서

### P0 — 같은 PR에서 처리

1. `kMaxObjectsPerFrame` 추가
2. Transformation과 bbox `std::isfinite` 검사
3. `parseNumber`의 `ptr == end` 확인
4. `sanitizeForLog`와 최대 로그 길이 적용
5. 위 네 항목의 parser security test 추가

### P1 — 다음 보안 hardening

1. UtcTime exact-format/calendar validation
2. BoundingBox 시작 tag 손상 시 객체 즉시 drop
3. XML attribute token boundary 강화
4. parser/pipeline exception boundary 명확화
5. parser drop reason별 counter와 rate-limited logging

### P2 — 지속 검증

1. libFuzzer/AFL++ harness 추가
2. ASan/UBSan build에서 corpus 실행
3. 카메라별 정상 ONVIF sample corpus 유지
4. CPU time/RSS budget regression test
5. 기준 commit과 보안 문서를 자동 연결

---

## 8. 최소 보안 테스트 세트

| Test | Expected result |
|---|---|
| 빈 payload | 빈 frame, crash 없음 |
| `<tt:Frame>` 종료 tag 누락 | 빈 frame |
| Transformation 누락 | 빈 frame |
| `ObjectId="1junk"` | 객체 drop |
| bbox `nan`/`inf` | 객체 drop |
| scale 곱셈 overflow | 객체 drop |
| Type에 CRLF/ESC 100 KiB | 정화·축약된 단일 log event |
| Object 256개 | 최대 256개 반환 |
| Object 257개 이상 | 나머지 drop, counter 증가 |
| Object start tag의 `>` 누락 | 해당 객체 drop |
| `FakeObjectId`만 존재 | ObjectId 없음으로 drop |
| UtcTime month 13/day 32 | frame drop |
| 1 MiB malformed corpus | bounded time/RSS, crash 없음 |
| random byte fuzz corpus | ASan/UBSan 오류 없음 |

---

## 9. 구현 예시

다음은 방향을 보여주는 최소 예시이며 실제 patch에서는 공통 counter/log 정책과 coding
convention에 맞춰야 한다.

```cpp
#include <algorithm>
#include <cmath>

namespace {

constexpr std::size_t kMaxObjectsPerFrame = 256;
constexpr std::size_t kMaxLoggedTextLength = 50;

template <typename T>
std::optional<T> parseNumber(std::string_view input) {
    T value{};
    const char* begin = input.data();
    const char* end = begin + input.size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || ptr != end)
        return std::nullopt;
    return value;
}

std::string sanitizeForLog(std::string_view input) {
    const std::size_t size = std::min(input.size(), kMaxLoggedTextLength);
    std::string output;
    output.reserve(size + 3);
    for (std::size_t index = 0; index < size; ++index) {
        const unsigned char value = static_cast<unsigned char>(input[index]);
        output.push_back(value < 0x20 || value == 0x7f ? '_' : static_cast<char>(value));
    }
    if (input.size() > kMaxLoggedTextLength)
        output += "...";
    return output;
}

}  // namespace
```

객체 loop:

```cpp
while ((pos = frame.find("<tt:Object", pos)) != std::string_view::npos) {
    if (result.objects.size() >= kMaxObjectsPerFrame) {
        logError(kIface, "object limit exceeded");
        break;
    }

    // parse object...

    if (!std::isfinite(det.box.l) || !std::isfinite(det.box.t) ||
        !std::isfinite(det.box.r) || !std::isfinite(det.box.b)) {
        continue;
    }

    result.objects.push_back(std::move(det));
}
```

---

## 10. 최종 판정

`OnvifParser`는 full XML engine을 사용하지 않는 설계 덕분에 XXE, entity expansion, recursion
stack overflow 공격에 강하다. `string_view` 기반 scanner와 상류 1 MiB frame cap도 좋은
기초 방어다.

그러나 현재 `develop`은 참조 보안 문서가 완료로 기록한 세 가지 핵심 방어를 포함하지 않는다.
특히 객체 수 상한 부재는 제한된 Raspberry Pi 환경에서 가용성 위험이 크고, NaN/Inf와 부분
숫자 parsing은 신뢰 경계 validation을 약화한다.

따라서 현 상태를 “파서 보안 패치 적용 완료”로 승인해서는 안 된다. 최소한 P0 네 항목과
boundary/security test가 같은 commit에 merge된 후 기준 SHA를 보안 문서에 기록해야 한다.
