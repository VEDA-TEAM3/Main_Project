# Jira Guide

---

## 1️⃣ Epic & Task

우리는 매주 목표(`Epic`)를 세우고, 이를 달성하기 위한 기능(`Task`)들을 쪼개어 1주일 동안 개발합니다.

1. 에픽(`Epic`) - 주간 목표
    - 이번 주의 핵심 테마입니다. `ex) 프로젝트 기획 및 기획서 작성`
2. 작업(`Task`) - 기능(`PR` 단위)
    - 에픽을 달성하기 위해 1~2일 내에 완료할 수 있는 기능 단위로 쪼갠 단위
    - 이 카드가 하나의 `Github` `branch`와 `PR`이 됩니다.
    - 이슈 키는 `TP-키값` 형태로 생성됩니다. `ex) TP-1`
3. 하위 작업(`Sub-task`) - 개인 체크리스트
    - 작업을 맡은 담당자가 해당 기능을 구현하기 위해 해야 할 일들을 스스로 쪼갠 단위 `ex) Task: [TP-1] 회원가입 API, Sub-task: DB schema 설계, password hash 적용, unit test 작성 등`

> ⚠️ **`Epic`과 `Task`는 회의한 후 팀장이 작성하는 단위이고, `Sub-task`는 `Task`를 할당 받은 담당자가 작성하는 단위입니다.또한 `Sub-task`는 따로 `branch`를 만들지 않습니다.**
> 

---

## 2️⃣ Board & Github workflow

`Jira`에서의 보드는 `Github`와 연동되어 있습니다.

| Status | Explain |
| --- | --- |
| 할 일 | 이번 주 스프린트에 할당된 `Task` 모임 |
| 진행 중 | 내가 진행할 `Task`를 할 일 보드에서 진행 중으로 이동
→ 개발 탭에 브랜치 만들기 클릭 후 브랜치 생성 (브랜치 명명 규칙에 따름) |
| 검토 중 | 개발 완료 후, `Github`에서 `PR`을 생성 및 `Code Review` 요청
→ `Jira`에서 보드가 자동적으로 검토 중으로 이동 |
| 완료 | 코드 리뷰를 마치고 `PR`을 `Merge`→ 자동적으로 완료로 이동 |

!image.png

> ⚠️ **만약 `PR`이 `Merge` 되지 않고 `Close` 되거나 거절되면, 보드는 다시 진행 중으로 자동 복귀합니다.**
> 

---

## 3️⃣ Daily Log

매일 개발을 마칠 때, 본인이 담당하고 있는 `Task`의 댓글에 오늘 하루의 진행 상황을 작성합니다.

- **데일리 로그 템플릿** (복사해서 사용)

```
### 📅 [2026.07.01] 데일리 진행 상황
* **현재 상태:** ⏳ 진행 중 (또는 ⚠️ 트러블슈팅 중 / 🔍 PR 리뷰 대기)

* **오늘 진행한 내용 및 핵심 설계 이유:**
  * 진행 내용: JWT 기반 로그인 API 기초 로직 구현
  * 구현 및 설계 이유: 세션 기반 대신 JWT를 선택한 이유는 추후 서버 확장(Scale-out) 시 상태를 유지(Stateless)하기 유리하다고 판단했기 때문임. Access Token 만료 시간은 보안을 위해 30분으로 짧게 설정함.

* **🔥 트러블슈팅:**
  * **[미해결] 이슈 발생:** Spring Security 필터 체인 과정에서 CORS 에러가 계속 발생함.
  * **접근 방안 1:** WebMvcConfigurer에서 CORS 허용 설정을 추가함 -> ❌ 실패 (Security 필터가 먼저 동작하여 막힘)
  * **접근 방안 2 (내일 진행 예정):** SecurityConfig 내부에서 `corsConfigurationSource`를 직접 빈으로 등록하여 필터 단에서 허용해 볼 예정.

* **💡 내일 할 일:**
  * CORS 트러블슈팅 해결
  * 로그인 성공 시 Refresh Token Redis 저장 로직 구현
```

> ⚠️ **댓글은 각 `Task`에 대한 내용만 작성합니다. 전체 일일 로그는 문서 탭에 작성합니다.**
> 

---

## 4️⃣ Git Commit Rule

`Jira`와 `Github`의 자동 연동을 위해 `commit message`는 **반드시 대괄호 안에 이슈 키를 넣어야 합니다.**

```bash
git commit -m "feat: [TP-2] DB Member schema 설계"
```

---

## 5️⃣ Workflow

전반적인 흐름입니다.

```bash
# 회의가 끝난 후 팀장이 Epic과 Task를 만들어주고 sprint가 시작되는 것을 기다립니다.

# 생성이 되었다면 나에게 할당받은 Task를 확인한 후, Sub-task를 작성합니다.

# 진행할 기능을 할 일에서 진행 중으로 옮긴 후 브랜치 명명 규칙에 맞게 브랜치 만들기를 합니다.
# 로컬에서도 브랜치를 맞춰줍니다.
git checkout -b branch-name

# 개발 중...

# 개발 완료
git add work file
git commit -m "tag: [TP-키값] 개발 내용 요약본"
git push origin branch-name

# Github에서 Pull Request로 왼쪽에 develop 오른쪽에 자신이 작업한 branch

# 코드 리뷰 기다리면서 윗 작업 반복합니다.

# 개발 중...

# 하루가 끝났다면, 현재 작업 중인 Task 기준으로 데일리 로그를 남깁니다.
# jira에서 현재 진행 중인 Task를 찾은 후 댓글에 내용을 알맞게 수정한 후 작성합니다.
```