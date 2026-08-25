include(GoogleTest)

# @param name 테스트 이름 (= 소스 파일 이름)
# @param ARGN 함께 링크할 control-server 프로덕션 소스들
function(veda_add_control_unit_test name)
    add_executable(${name} ${name}.cpp ${ARGN})

    target_include_directories(${name} PRIVATE
        ${CONTROL_DIR}/include
        ${CONTROL_DIR}/src
    )

    target_link_libraries(${name} PRIVATE
        shared
        GTest::gtest_main
        GTest::gmock
        Threads::Threads
    )

    target_compile_options(${name} PRIVATE
        -Wno-mismatched-new-delete
    )

    gtest_discover_tests(${name}
        PROPERTIES TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
    )
endfunction()

find_package(Threads REQUIRED)

veda_add_control_unit_test(AggregatorTest
    ${CONTROL_DIR}/src/aggregate/TimeWindowAggregatorV2.cpp
)

veda_add_control_unit_test(FuserTest
    ${CONTROL_DIR}/src/fuse/ConcatFuser.cpp
    ${CONTROL_DIR}/src/fuse/GridFuser.cpp
)

veda_add_control_unit_test(TransformEquivalenceTest
    ${CONTROL_DIR}/src/transform/AffineLocalToWorldTransform.cpp
)

veda_add_control_unit_test(ZoneMapperEquivalenceTest
    ${CONTROL_DIR}/src/zone/SpatialZoneMapper.cpp
)

veda_add_control_unit_test(MetricTest
    ${CONTROL_DIR}/src/metric/EuclideanMetric.cpp
)

# ThresholdRiskPolicy 는 IDistanceMetric 구현이 필요하다 (EuclideanMetric 을 실물로 주입)
veda_add_control_unit_test(RiskTest
    ${CONTROL_DIR}/src/risk/ThresholdRiskPolicy.cpp
    ${CONTROL_DIR}/src/metric/EuclideanMetric.cpp
)