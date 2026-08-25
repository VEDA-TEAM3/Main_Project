# ---------------------------------------------------------------------------
# compute-server GTest/GMock 단위 테스트 (104개)
#
# 이 테스트들은 보안 감사에서 적용한 방어들이 조용히 회귀하지 않도록 고정한다.
#   ParserTest    : 256객체 상한, NaN/Inf 거부, 로그 살균
#   SanitizerTest : 임계값 범위 밖 throw, 파서와의 상한 정렬
#   RouterTest    : out-parameter 무할당 계약(clear() 누락 = 유령 객체), 클래스 별칭 정규화
#   MapperTest    : 특이/비유한 행렬 거부, 지평선 가드, 아핀 in-place 무할당
#   NetworkTest   : INetwork 생명주기 계약, inet_pton 목적지 검증, cancel 멱등성
#   SourceTest    : lost wakeup 수정 (실제로 블록된 next() 를 stop() 이 깨우는지)
#
# @note 각 테스트는 검증 대상의 .cpp '만' 링크한다 (tests/CMakeLists.txt 의 주석 참고)
# ---------------------------------------------------------------------------

include(GoogleTest)

# @param name 테스트 이름 (= 소스 파일 이름)
# @param ARGN 함께 링크할 compute-server 프로덕션 소스들
function(veda_add_unit_test name)
    add_executable(${name} ${name}.cpp ${ARGN})

    target_include_directories(${name} PRIVATE
        ${COMPUTE_DIR}/include
        ${COMPUTE_DIR}/src
    )

    target_link_libraries(${name} PRIVATE
        shared
        GTest::gtest_main
        GTest::gmock
        Threads::Threads
    )

    target_compile_options(${name} PRIVATE
        # 일부 테스트가 힙 할당을 세려고 전역 operator new/delete 를 대체한다.
        # 짝이 맞지 않는다는 경고가 뜨지만 의도된 계측이다
        -Wno-mismatched-new-delete
        # OpenSSL 3.0 에서 MD5 계열이 deprecated (RtspClientV2 의 Digest 인증)
        -Wno-deprecated-declarations
    )

    # ctest 에 테스트를 '케이스 단위'로 등록한다.
    # 실패했을 때 어느 TEST() 가 깨졌는지 ctest 출력만으로 알 수 있다
    gtest_discover_tests(${name}
        PROPERTIES TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
    )
endfunction()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

veda_add_unit_test(ParserTest
    ${COMPUTE_DIR}/src/parser/OnvifParser.cpp
)

veda_add_unit_test(SanitizerTest
    ${COMPUTE_DIR}/src/sanitize/ContainmentSanitizer.cpp
)

veda_add_unit_test(RouterTest
    ${COMPUTE_DIR}/src/route/ParentBasedRouter.cpp
)

veda_add_unit_test(MapperTest
    ${COMPUTE_DIR}/src/mapper/AffineImageCoordinateMapper.cpp
    ${COMPUTE_DIR}/src/transform/HomographyTransform.cpp
)

# RtspClientV2 는 Digest 인증에 OpenSSL MD5 를 쓴다
veda_add_unit_test(NetworkTest
    ${COMPUTE_DIR}/src/network/RtspClientV2.cpp
)
target_link_libraries(NetworkTest PRIVATE OpenSSL::Crypto)

veda_add_unit_test(SourceTest
    ${COMPUTE_DIR}/src/source/RtspOnvifSourceV2.cpp
    ${COMPUTE_DIR}/src/network/RtspClientV2.cpp
)
target_link_libraries(SourceTest PRIVATE OpenSSL::Crypto)