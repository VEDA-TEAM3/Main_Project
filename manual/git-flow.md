# Git Flow Guide

---

## 📝 Summary

- **브랜치 명명 규칙**: `feature/TP-key` `ex) feature/TP-1`
- **커밋 메시지 규칙**: `tag: [TP-key] 내용` `ex) feat: [TP-1] 회원가입 API 구현 완료`
- `main`이나 `develop`에 직접 `push`하는 것은 금지합니다. → **무조건 `PR`을 거쳐서 하도록 강제합니다.**

---

## 0️⃣ Setup

처음 컴퓨터 환경 설정입니다. 딱 한 번만 진행합니다.

1. 사용자 정보 등록

```bash
git config --global user.name "Github 이름"

git config --global user.email "Github 이메일 주소"

# git config --list로 등록되었는지 확인
```

---

1. 원격 저장소 복제

```bash
git clone https://github.com/VEDA-TEAM3/Main_Project.git

cd Main_Project

# 추후에 각 서브 모듈에 맞게 수정
```

> ⚠️ **GitHub 인증 오류 해결법**
로그인 창이 뜨면 `Sign in with your browser`을 선택하거나, GitHub 설정에서 발급받은 `Personal Access Token`을 비밀번호 대신 입력해야 합니다.
> 

---

## 1️⃣ Develop

Jira에서 새로운 `Task`를 할당받고 진행 중으로 옮겼다면, `~~branch`가 생성되어 있습니다.~~

> 🔥 Trouble Shotting
`Jira` → `Github` 연동이 잘 안 되서 수동으로 `branch` 생성해야 할듯…
> 
1. 개발 준비

```bash
git pull branch-name

git pull origin develop

git checkout branch-name

# 개발을 시작하면 됩니다.
```

---

1. 작업 종료 및 개발 완료

```bash
git status

# 작업한 내용 중에서 올려야 하는 파일만 올립니다.
git add work-file

git commit -m "tag: [TP-key] DB Member schema 설계"

git push origin branch-name
```

---

- Commit Tag
    - `feat`: : 새로운 기능 추가
    - `update`: : 기존 로직 수정 및 개선
    - `fix`: : 버그 수정
    - `docs`: : 문서 작성 및 수정
    - `refactor`: : 코드 리팩토링
    - `test`: : 테스트 코드 작성 및 수정
    - `chore`: : 빌드 환경 설정, `Makefile` 수정, 잡무 등
    - `wip`: 작업 진행 중

---

## 2️⃣ PR

`Task`의 기능을 구현했고 `branch`에 `push`까지 완료되었다면, `PR`을 생성합니다.

- PR 본문 (복사해서 사용)

```
## 📌 관련 이슈
* 연동 티켓: [TP-1]

## 📝 작업 내용 요약
* 이메일, 비밀번호 입력 폼 UI 구현
* 정규표현식을 활용한 실시간 유효성 검사 로직 추가

## ⚠️ 리뷰 요구사항 (고민했던 점)
* 비밀번호 유효성 검사 정규식이 너무 긴 것 같은데, 더 깔끔하게 분리할 방법이 있을지 코드 리뷰 부탁드립니다!
```

---

```
PR 생성 방법

1. Github 레포지토리에서 Compare & pull request가 뜬다면 버튼 클릭
1.1 안 뜬다면 Pull requests로 직접 들어가서 New pull request 클릭

2. 브랜치가 base: develop, compare: Task-branch-name이 맞는지 확인

3. 제목은 Task의 이름, 본문은 위에 양식을 복사해서 알맞게 수정

4. 오른쪽 사이드바에 Reviewers는 코드 리뷰할 사람 지정, Assignees는 자신을 지정

5. Create pull request 버튼 클릭
```

---

## 3️⃣ Code Review

코드 리뷰를 배정 받은 팀원들은 코드를 확인하고 피드백을 남깁니다. 이상이 없다면 `Approve`를 누릅니다.

---

## 4️⃣ Next Task

다음 `Task`를 위해 작업 환경을 정리합니다.

```bash
git checkout develop

# 원격 저장소와 로컬 저장소 동기화
git pull origin develop

# 디스크 정리
git branch -D branch-name
```