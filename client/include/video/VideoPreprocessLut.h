#pragma once

#include <array>

#include "model/VideoPreprocessingSettings.h"

/**
 * @brief 밝기·대비·감마를 하나로 합친 Y 평면 룩업 테이블.
 *
 * @details 세 보정 모두 8비트 Y에 대한 순수 per-pixel 함수라 표 하나로 합쳐진다. 예전에는
 *          GStreamer의 videobalance와 gamma 두 요소가 Y 평면을 각각 한 번씩, 모두 두 번
 *          훑었다. 합성한 표는 그 두 단계와 **값이 완전히 같고**(입력 256개를 실제 요소와
 *          대조해 검증한다 - tests/VideoPreprocessLutCheck.cpp) 순회는 한 번이면 된다.
 */
using VideoPreprocessLut = std::array<unsigned char, 256>;

/**
 * @brief          보정이 아무것도 하지 않는 설정인지 확인합니다.
 * @param settings 확인할 전처리 설정
 * @return         항등이면 true
 *
 * @details 항등이면 필터를 passthrough로 내려야 합니다. passthrough가 아니면
 *          GstBaseTransform이 매 프레임 버퍼를 쓰기 가능 상태로 만들고, 버퍼가 쓰기
 *          불가능하면 프레임 전체를 복사합니다. 보정을 끈 채널에서는 그 비용이 전부 낭비입니다.
 */
bool isNeutralVideoPreprocessing(const VideoPreprocessingSettings& settings);

/**
 * @brief          설정에서 합성 룩업 테이블을 만듭니다.
 * @param settings 밝기(-100~100), 대비(0~2), 감마(0.01~10)
 * @return         Y 값 256개를 그대로 대응시키는 표
 *
 * @details 설정이 바뀔 때만 부르면 됩니다. 프레임마다 부를 필요가 없습니다.
 */
VideoPreprocessLut buildVideoPreprocessLut(const VideoPreprocessingSettings& settings);
