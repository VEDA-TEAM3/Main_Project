# OnvifParser 보안 권고 (Security Advisory)

> **대상 모듈**: `src/parser/OnvifParser.cpp`, `OnvifParser.h`
> **문서 성격**: 보안 감사 결과 + 적용된 완화책 기록

---

| Date | Version | Writer | Summary |
| --- | --- | --- | ---|
| 2026.07.27 | v1.0.0 | Mangjun | ONVIF XML 파서 보안 감사 결과 및 심층 방어(Log Injection, DoS, 비정상 좌표 차단) 패치 내역 문서화 |

---

## 1. 위협 모델 (Threat Model)

`OnvifParser`는 **외부 ONVIF CCTV가 RTSP 인터리브로 실어보내는 원본 XML 바이트 페이로드**(`domain::RawPacket`)를 파이프라인 내부 표현(`domain::ChannelFrame`)으로 변환하는 **파이프라인의 첫 단계**다.

이 모듈이 다루는 입력은 **신뢰할 수 없는 외부 입력**이다:

- 카메라는 물리적으로 현장에 설치된 장비이며, **탈취·스푸핑·펌웨어 변조**가 가능하다. 공격자가 카메라를 장악하거나 카메라인 척 위장하면 **임의의 바이트열**을 파서에 밀어넣을 수 있다.
- 파서는 그 바이트를 직접 스캔하므로, **시스템 전체에서 신뢰 경계의 최전선**이자 **주된 공격 표면** 이다.
- 파서 하나가 무너지면(크래시/무한 루프/메모리 고갈) 해당 채널의 compute-server 프로세스가 죽고, control-server는 채널 생존 신호로 이를 감지하지만 **해당 교차로의 위험 판정이 중단**된다.

따라서 파서는 "**어떤 입력에도 죽지 않고, 하류로 오염된 데이터를 흘리지 않으며, 자원을 고갈시키지 않는다**"는 것을 설계 목표로 한다.

### 관련 방어선 (상류)

파서 입력 크기 자체는 상류 `RtspClientV2`가 **프레임당 1 MiB 상한**(`maxMetadataFrameSize_`)으로 제한한다. 이 상한을 넘는 프레임은 파서에 도달하기 전에 폐기된다. 단, 이는 상류 한 곳에만 의존하는 방어선이므로, 파서는 아래 W3처럼 **자체 방어선**도 함께 갖춘다.

---

## 2. 구조적 면역 (Structural Immunities) — All Clear

가장 중요한 설계 결정은 **완전한 XML 엔진을 의도적으로 사용하지 않고, 재귀 없는 `std::string_view` 기반 경량 스캐너를 직접 구현**한 것이다. 이 선택이 XML 계열의 상위 공격군을 **필터링이 아니라 구조적으로 성립 불가능하게** 만든다.

### 2.1 XXE (XML External Entity) — 구조적 면역

- 파서에는 **엔티티 해석 엔진 자체가 없다.** `<!DOCTYPE>`를 처리하지 않고, `&entity;`를 확장하지 않으며, 외부 리소스(파일/URL)를 절대 참조하지 않는다.
- `<tt:Type>` 텍스트에 `&xxe;` 같은 엔티티가 들어와도 **단순 바이트열**로 취급되어 `veda::objectClassFromString()`에 그대로 전달되고 `Unknown`으로 분류될 뿐이다.
- 즉 **공격할 대상(엔티티 리졸버)이 존재하지 않으므로**, 파일 노출·SSRF·외부 엔티티 주입이 원천 차단된다.

### 2.2 Entity Expansion DoS ("Billion Laughs") — 구조적 면역

- Billion Laughs는 **엔티티가 다른 엔티티를 재귀적으로 참조·확장**해 메모리/CPU를 지수적으로 폭발시키는 공격이다.
- 파서는 엔티티를 **전혀 확장하지 않으므로** 폭발할 확장 단계가 존재하지 않는다.

### 2.3 스택 오버플로 (깊은 중첩) — 구조적 면역

- 파서는 **재귀를 전혀 사용하지 않는다.** 모든 구조는 평면적인 `while`/`for` 루프로 순회한다.
- 아무리 깊게 중첩된 XML이라도 **스택 프레임이 깊어지지 않고 반복 횟수만 늘어난다** → 중첩 깊이로 인한 스택 고갈이 불가능하다.

### 2.4 무한 루프 — 구조적 면역

- 모든 루프는 **단조 증가하는 커서**(`extractQuoted`의 `searchFrom`, 객체 루프의 `pos`)와 **`npos` 종료 가드**를 가진다 → 어떤 입력에도 반드시 종료한다.

### 2.5 경계 밖 읽기(OOB) — 검증 완료

- 모든 `find` 결과는 사용 전에 `npos`와 비교되고, 모든 직접 인덱싱(`s[afterKey]`, `s[afterKey+1]`)은 `afterKey + 1 < s.size()`로 경계 보호되며, 모든 `substr`의 오프셋/길이는 `find` 결과로부터 유도되어 언더플로가 없다.
- 손상/절단된 XML에 대해 **크래시 경로가 발견되지 않았다.**

---

## 3. 적용된 완화책 (Applied Mitigations) — Defense-in-Depth

2번의 구조적 면역과 별개로, 신뢰 불가 입력을 **하류로 흘리거나 로그·자원에 영향**을 줄 수 있는 3개의 방어 심층화 지점을 감사에서 식별하고 패치했다.

### W1. Log / CSV Injection

| 항목 | 내용 |
|------|------|
| **문제** | 카메라가 통제하는 `<tt:Type>` 문자열이 정화 없이 로그 문자열에 삽입됨. `Logger`는 **고정 이름 CSV(`veda.csv`)** 에 append하므로, 개행(`\r\n`)으로 **가짜 로그 행**을, 콤마(`,`)/큰따옴표(`"`)로 **가짜 CSV 열**을 주입할 수 있었음 (로그 위조·CSV 손상) |
| **완화** | 신규 `sanitizeForLog()` 헬퍼를 도입해 **로깅 직전에** 무력화 <br> 제어문자·콤마·큰따옴표를 `_`로 치환하고, 최대 `kMaxLoggedTextLen`(50자)로 자른 뒤 초과 시 `...`를 부착 |
| **효과** | 카메라가 보낸 어떤 문자열도 CSV 구조를 깨거나 로그 행을 위조할 수 없음 (로그 폭주도 길이 상한으로 억제) |

```cpp
std::string sanitizeForLog(std::string_view text) {
    const std::size_t n = std::min(text.size(), kMaxLoggedTextLen);
    std::string out;
    out.reserve(n + 3);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        out.push_back((c < 0x20 || c == 0x7F || c == ',' || c == '"') ? '_' : static_cast<char>(c));
    }
    if (text.size() > kMaxLoggedTextLen) {
        out += "...";
    }
    return out;
}
```

> 적용 지점: 인식 안 되는 `Type` 진단 로그에서 `std::string(typeText)` → `sanitizeForLog(typeText)`

### W2. NaN / Inf 기하 이상값

| 항목 | 내용 |
|------|------|
| **문제** | `std::from_chars`는 `"inf"`/`"nan"` 문자열을 그대로 파싱하고, 거대한 `scale`/`translate` 값은 곱셈 오버플로로 `±Inf`를 낳음 <br> 파서가 이런 좌표를 그대로 하류로 내보내면 **NaN/Inf 좌표를 가진 bbox**가 파이프라인에 유입됨 |
| **완화** | 좌표 정규화(`normX`/`normY`) 직후 **`std::isfinite` 검사**를 추가 <br> bbox의 네 좌표 중 하나라도 유한하지 않으면 해당 객체를 `continue`로 폐기 |
| **효과** | 하류(`HomographyTransform`·`AffineImageCoordinateMapper`)의 `isfinite` 검사에 더해 **파서에서 먼저** 이상 기하를 걸러내는 방어 심층화 |

```cpp
if (!std::isfinite(det.box.l) || !std::isfinite(det.box.r) ||
    !std::isfinite(det.box.t) || !std::isfinite(det.box.b)) {
    continue;
}
```

### W3. DoS 증폭 (메모리 고갈)

| 항목 | 내용 |
|------|------|
| **문제** | 파서에 자체 객체 수 상한이 없어, 상한(1 MiB) 프레임 안에 아주 작은 `<tt:Object>`를 대량으로 채우면 `result.objects` 벡터가 수만 개까지 부풀어 **메모리 증폭**이 발생 <br> (하류 `ContainmentSanitizer`는 128개 초과 시 fail-open으로 중복 제거를 건너뛰므로 증폭을 막지 못함) |
| **완화** | `kMaxObjectsPerFrame`(256) 하드 상한을 도입 <br> 파싱된 객체 수가 상한에 닿으면 객체 루프를 **조기 종료**하고 진단 로그를 남김 |
| **효과** | 증폭된 페이로드로 인한 메모리 고갈을 파서 자체에서 차단 <br> 상류 1 MiB 프레임 상한과 **독립적인** 자체 방어선 |

```cpp
while ((pos = frame.find("<tt:Object", pos)) != std::string_view::npos) {
    if (result.objects.size() >= kMaxObjectsPerFrame) {
        logError(kIface, "ch=" + std::to_string(raw.channelId) + " 파싱 객체 수가 상한(" +
                             std::to_string(kMaxObjectsPerFrame) + ")에 도달 - 나머지 객체 무시");
        break;
    }
    ...
}
```

---

## 4. 요약

| 위협 | 분류 | 상태 |
|------|------|------|
| XXE (외부 엔티티) | 구조적 면역 | ✅ 성립 불가 (엔티티 엔진 없음) |
| Entity Expansion (Billion Laughs) | 구조적 면역 | ✅ 성립 불가 (엔티티 확장 없음) |
| 스택 오버플로 (깊은 중첩) | 구조적 면역 | ✅ 성립 불가 (재귀 없음) |
| 무한 루프 | 구조적 면역 | ✅ 성립 불가 (단조 커서 + npos 가드) |
| OOB 읽기 | 검증 완료 | ✅ 크래시 경로 없음 |
| W1. Log/CSV Injection | 완화 적용 | ✅ `sanitizeForLog` |
| W2. NaN/Inf 기하 이상값 | 완화 적용 | ✅ `std::isfinite` 검사 |
| W3. DoS 증폭 | 완화 적용 | ✅ `kMaxObjectsPerFrame`(256) 상한 |

**총평**: 파서의 메모리 안전성·DoS 내성은 구조적으로 견고하며, 감사에서 식별된 3개 방어 심층화 지점(W1–W3)을 모두 패치해 신뢰 불가 입력이 로그·하류·자원에 미치는 영향을 차단했다.
