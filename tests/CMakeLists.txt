# ---------------------------------------------------------------------------
# 중앙 집중 테스트 트리
#
# 저장소의 '모든' 테스트가 여기 있다. compute-server/ 와 control-server/ 는
# 프로덕션 빌드만 담당하며 테스트 타깃을 갖지 않는다
#
#   tests/compute-server/unit/     GTest/GMock 단위 테스트
#
# @note [ 왜 '라이브러리 타깃'이 아니라 소스 파일을 직접 링크하는가 ]
# 각 테스트는 검증 대상의 .cpp '만' 링크한다. 이는 의도된 성질이다
# -- 순수 계산 경로를 검증하는 테스트가 libmosquitto/OpenSSL 없이도 빌드·실행되고,
#    한 계층의 링크 의존성이 늘어나면 관련 없는 테스트까지 끌려가는 일이 없다.
# 서버 전체를 하나의 라이브러리 타깃으로 묶어 링크하면 이 성질이 사라진다.
# ---------------------------------------------------------------------------

set(COMPUTE_DIR ${CMAKE_SOURCE_DIR}/compute-server)

find_package(Threads REQUIRED)

# GTest 단위 테스트
add_subdirectory(compute-server/unit)