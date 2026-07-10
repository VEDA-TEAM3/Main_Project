/**
 * @file    NullSender.cpp
 * @brief   아무 전송도 수행하지 않는 ISender 구현체
 * @details NullSender 익명 네임스페이스 안에 숨겨져 있으며, 외부에는
 *          createSender() 팩토리 함수를 통해서만 ISender로 노출
 */
#include "interfaces/ISender.h"

namespace {

/**
 * @brief   실제 전송을 수행하지 않는 ISender 구현체
 * @details MQTT 등 실제 전송 프로토콜이 아직 구현되지 않은 상태에서,
 *          Processor 파이프라인 전체(수신 → 파싱 → 변환 → 패킷 생성 → 전송 호출)를
 *          테스트하기 위한 임시 스텁
 */
class NullSender : public ISender {
public:
    NullSender() = default;
    ~NullSender() override = default;

    bool send(const std::string& packet) override {
        (void)packet;
        return true;
    }
};

}  // namespace

std::shared_ptr<ISender> createSender() { return std::make_shared<NullSender>(); }