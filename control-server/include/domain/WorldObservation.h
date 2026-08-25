#pragma once

/**
 * @file    WorldObservation.h
 * @brief   로컬→월드 변환이 끝난 '채널별 관측치' (융합 입력, 월드 좌표)
 *
 * @details
 * 예전에는 ILocalToWorldTransform 이 veda::TopViewFrame 을 in-place 로 고쳐서, 타입은
 * veda::LocalPoint 인데 값은 월드 좌표인 구간이 존재했다 (transform() ~ fuse() 사이).
 * 컴파일러가 전혀 막아주지 못하는 타입 퍼닝이라, 단계 순서를 바꾸거나 그 사이에서 pos 를
 * 읽는 코드가 새로 생기면 '조용히 틀린 좌표'를 쓰게 된다
 * -> 변환 결과를 별도 타입으로 분리해 로컬/월드 경계를 컴파일러가 강제하도록 함
 *
 * @note 아직 채널 간 융합 전이라 gid 가 없다 (융합 후에는 domain::WorldObject 가 됨)
 */

#include <vector>

#include "Contract.h"
#include "domain/WorldPoint.h"

namespace domain {

/// @brief 한 채널이 관측한 객체 하나 (도면 공통 월드 좌표)
struct WorldObservation {
    veda::ObjectId id = 0;  ///< 채널 내부 ObjectId (채널 사이에서는 유일하지 않음)
    veda::ObjectClass cls = veda::ObjectClass::Unknown;
    WorldPoint pos;  ///< 변환이 끝난 월드 좌표
};

/// @brief 한 채널의 한 프레임 관측치 묶음 (월드 좌표)
struct ObservationFrame {
    veda::TimestampMs ts = 0;
    veda::ChannelId ch = 0;
    std::vector<WorldObservation> objects;
};

}  // namespace domain
