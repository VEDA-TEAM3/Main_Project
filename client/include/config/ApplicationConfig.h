#pragma once

#include <QString>
#include <QStringList>

#include "model/DigitalTwinRuntimeConfig.h"
#include "network/transport/MqttRuntimeConfig.h"
#include "video/VideoRuntimeConfig.h"

struct ApplicationWindowConfig {
    int width = 0;
    int height = 0;
};

struct ApplicationLoggingConfig {
    bool enabled = true;
};

struct ApplicationConfig {
    ApplicationWindowConfig window;
    ApplicationLoggingConfig logging;
    DigitalTwinRuntimeConfig digitalTwin;
    VideoRuntimeConfig video;
    MqttRuntimeConfig mqtt;
};

struct ApplicationConfigLoadResult {
    ApplicationConfig config;
    QString sourcePath;
    QString error;
    bool successful = false;
};

class ApplicationConfigLoader final {
public:
    static ApplicationConfigLoadResult load();
};

/**
 * @brief 설정 파일에 CCTV 구역을 덧붙입니다.
 *
 * @details 전체 설정을 다시 직렬화하지 않고 video.areas / video.streams /
 *          digitalTwin.world.zones 세 키만 갈아 끼운다. 손으로 조정해 둔 나머지 값이
 *          왕복 변환으로 뭉개지는 것을 막기 위해서다.
 */
class ApplicationConfigWriter final {
public:
    /**
     * @brief                  구역 하나와 그 RTSP 채널들을 설정 파일 끝에 추가합니다.
     * @param configPath       ApplicationConfigLoadResult::sourcePath
     * @param areaName         화면에 표시할 구역 이름
     * @param streamAddresses  구역당 채널 수만큼의 RTSP 주소 (계정 없이)
     * @param userName         네 채널에 공통으로 넣을 계정, 비우면 주소를 그대로 씁니다
     * @param password         userName의 비밀번호
     * @return                 실패 원인, 성공하면 빈 문자열
     *
     * @details 계정을 주소와 나눠 받는 이유는 두 가지다. 입력 화면에서 비밀번호만 가릴 수 있고,
     *          '@'나 ':'가 든 비밀번호가 percent-encoding을 거쳐 URL 파싱을 깨뜨리지 않는다.
     */
    static QString appendArea(const QString& configPath, const QString& areaName, const QStringList& streamAddresses,
                              const QString& userName = {}, const QString& password = {});

    /**
     * @brief                  이미 있는 구역의 이름과 RTSP 채널을 바꿔 씁니다.
     * @param configPath       ApplicationConfigLoadResult::sourcePath
     * @param areaIndex        video.areas 안에서의 위치
     * @param areaName         새 구역 이름
     * @param streamAddresses  구역당 채널 수만큼의 RTSP 주소 (계정 없이)
     * @param userName         네 채널에 공통으로 넣을 계정
     * @param password         userName의 비밀번호
     * @return                 실패 원인, 성공하면 빈 문자열
     */
    static QString updateArea(const QString& configPath, int areaIndex, const QString& areaName,
                              const QStringList& streamAddresses, const QString& userName = {},
                              const QString& password = {});

    /**
     * @brief           주소와 계정을 하나의 RTSP URL로 합칩니다.
     * @param address   계정이 없는 RTSP 주소
     * @param userName  계정, 비어 있으면 주소를 그대로 돌려줍니다
     * @param password  비밀번호
     *
     * @details 저장한 값을 화면에 되돌려 쓸 때 설정 파일과 같은 형태가 되도록 공개합니다.
     */
    static QString composeUrl(const QString& address, const QString& userName, const QString& password);
};
