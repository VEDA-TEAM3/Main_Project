/**
 * @file    main.cpp
 * @brief   Processor의 진입점
 * @details 환경 변수로부터 AppConfig를 로드하고, AppContext로 각 인터페이스
 *          구현체(Network, Parser, Trans, Sender)를 생성한 뒤, Processor를
 *          구성하여 파이프라인을 실행
 */
#include <cstdlib>

#include "core/AppContext.h"
#include "core/Processor.h"

/**
 * @brief   프로그램 진입점
 * @return  정상 종료 시 EXIT_SUCCESS, 초기화 또는 실행 실패 시 EXIT_FAILURE
 * @note    HomographyTrans의 calibrate()/setCameraPosition() 호출은 아직
 *          AppContext::initialize()에 반영되지 않은 상태이며, 이 경우
 *          ITrans::transform()은 캘리브레이션되지 않은 기본값(0,0,0)을 반환한다.
 */
int main() {
    AppContext ctx(AppConfig::fromEnv());
    if (!ctx.initialize())
        return EXIT_FAILURE;

    Processor processor(ctx.network(), ctx.parser(), ctx.trans(), ctx.sender());
    if (!processor.run())
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}