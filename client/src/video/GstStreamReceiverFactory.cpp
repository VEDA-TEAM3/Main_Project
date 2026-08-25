#include "video/GstStreamReceiverFactory.h"

#include <utility>

#include "video/GstRtspReceiver.h"

/**
 * @brief        모든 채널 수신기에 공통 적용할 RTSP 설정을 보관합니다.
 * @param config JSON 검증을 통과한 GStreamer 수신 설정
 */
GstStreamReceiverFactory::GstStreamReceiverFactory(GstRtspReceiverConfig config) : config_(std::move(config)) {}

/**
 * @brief                    GStreamer 기반 RTSP 수신기를 생성합니다.
 * @param outputWindowHandle  영상을 출력할 네이티브 윈도우 핸들
 * @return                   StreamReceiver 인터페이스로 반환되는 수신기
 */
std::shared_ptr<StreamReceiver> GstStreamReceiverFactory::create(WId outputWindowHandle) const {
    return std::make_shared<GstRtspReceiver>(static_cast<guintptr>(outputWindowHandle), config_);
}
