#include "mapper/AffineImageCoordinateMapper.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

#include "Logger.h"

namespace {

constexpr const char* kIface = "Mapper";

bool isFinite(double value) { return std::isfinite(value); }

bool isVisible(const domain::NormBox& box) { return box.r > 0.0 && box.l < 1.0 && box.b > 0.0 && box.t < 1.0; }

/**
 * @brief   중심을 고정한 채 폭/높이를 scale 배로 확대 (지연 보상 여유)
 *
 * @details
 * 전송/렌더 지연 동안 빠른 객체가 박스를 벗어나는 것을 트래킹 없이 보정한다.
 * 경계 처리는 여기서 하지 않는다 -- 호출부가 곧바로 clampToOutput 으로 [0,1] 에 가두므로,
 * 화면 끝에 걸린 박스는 '안쪽으로만' 자라고 중심은 그만큼 안으로 밀린다(불가피하며 의도된 동작).
 * 분기/할당이 없어 프레임당 비용이 상수다 (compute-server 무할당 규약 유지)
 */
void expandAroundCenter(domain::NormBox& box, double scale) {
    const double cx = (box.l + box.r) * 0.5;
    const double cy = (box.t + box.b) * 0.5;
    const double halfW = (box.r - box.l) * 0.5 * scale;
    const double halfH = (box.b - box.t) * 0.5 * scale;
    box.l = cx - halfW;
    box.r = cx + halfW;
    box.t = cy - halfH;
    box.b = cy + halfH;
}

void clampToOutput(domain::NormBox& box) {
    box.l = std::clamp(box.l, 0.0, 1.0);
    box.r = std::clamp(box.r, 0.0, 1.0);
    box.t = std::clamp(box.t, 0.0, 1.0);
    box.b = std::clamp(box.b, 0.0, 1.0);
}

}  // namespace

AffineImageCoordinateMapper::AffineImageCoordinateMapper(double scaleX, double scaleY, double offsetX, double offsetY,
                                                         double boxScale)
    : scaleX_(scaleX), scaleY_(scaleY), offsetX_(offsetX), offsetY_(offsetY), boxScale_(boxScale) {
    if (!isFinite(scaleX_) || !isFinite(scaleY_) || !isFinite(offsetX_) || !isFinite(offsetY_) || scaleX_ <= 0.0 ||
        scaleY_ <= 0.0) {
        throw std::invalid_argument("image coordinate mapper requires finite positive scales");
    }
    // AppConfig 가 이미 [1.0, 2.0] 으로 clamp 하지만, DI 로 직접 넣는 경로(테스트/다른 조립부)까지
    // 막으려면 여기서도 거부해야 한다. 1.0 미만은 blur 박스를 줄여 대상을 노출시키는 방향이라
    // '조용히 통과'시키면 안 된다
    if (!isFinite(boxScale_) || boxScale_ < 1.0) {
        throw std::invalid_argument("blur box scale must be finite and >= 1.0");
    }
}

void AffineImageCoordinateMapper::map(std::vector<domain::DetectedObject>& objects, veda::ChannelId channelId) const {
    const std::size_t inputCount = objects.size();

    // in-place 필터링: 출력 개수는 항상 입력 이하이므로 별도 벡터를 새로 만들지 않고
    // objects 자체에서 통과하는 원소만 앞으로 당긴 뒤 resize()로 잘라냄
    // (resize()로 줄이는 건 재할당을 유발하지 않음) -> 이 함수의 힙 할당이 0이 됨
    std::size_t writeIdx = 0;
    for (std::size_t readIdx = 0; readIdx < objects.size(); ++readIdx) {
        auto& object = objects[readIdx];

        domain::NormBox mapped;
        mapped.l = object.box.l * scaleX_ + offsetX_;
        mapped.r = object.box.r * scaleX_ + offsetX_;
        mapped.t = object.box.t * scaleY_ + offsetY_;
        mapped.b = object.box.b * scaleY_ + offsetY_;

        // [ 지연 보상 ] 확대를 가시성 판정보다 '먼저' 한다.
        // 화면 가장자리에 반쯤 걸친 객체는 확대 후에야 화면 안으로 들어오는데, 순서를 뒤집으면
        // 그런 객체가 필터에서 탈락해 blur 없이 노출된다 -- 프라이버시 기능이므로 넓은 쪽이 안전하다.
        // NaN/Inf 는 확대 연산을 통과해도 그대로 NaN/Inf 이므로 아래 유한성 검사가 여전히 잡는다
        expandAroundCenter(mapped, boxScale_);

        if (!isFinite(mapped.l) || !isFinite(mapped.r) || !isFinite(mapped.t) || !isFinite(mapped.b) ||
            !isVisible(mapped)) {
            continue;
        }

        // 확대분이 [0,1] 을 넘긴 부분은 여기서 잘린다 -> 확대 후에도 프레임 밖으로 절대 나가지 않음
        clampToOutput(mapped);
        if (mapped.r <= mapped.l || mapped.b <= mapped.t) {
            continue;
        }

        object.box = mapped;
        // touchesBorder/bottomTruncated 는 건드리지 않음: 이 단계는 blur 경로 전용이고
        // 경계 판정은 Metadata 좌표계 기준으로 파서가 이미 한 번만 수행함
        // (여기서 clamp 후 재판정하면 '앱 화면 기준 경계'라는 다른 의미가 섞임)

        if (writeIdx != readIdx) {
            objects[writeIdx] = std::move(object);
        }
        ++writeIdx;
    }
    objects.resize(writeIdx);

    if (inputCount > 0 && writeIdx == 0) {
        logError(kIface, "ch=" + std::to_string(channelId) + " 입력 blur 객체 " + std::to_string(inputCount) +
                             "개가 전부 필터링됨 (scale/offset 설정 확인 필요)");
    }
}
