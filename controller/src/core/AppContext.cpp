/**
 * @file    AppContext.cpp
 * @brief   AppContext 구현부
 * @todo    생성자 본문은 두 가지 별개의 이유로 아직 채울 수 없다.
 *          1) INetwork 3개(subscribeNetwork_, driverPubNetwork_, appPubNetwork_)는
 *             .so 플러그인 팩토리 함수 계약이 정해지지 않았다.
 *          2) parser_/riskDetector_/appSender_/driverSender_는 dlopen과 무관하게,
 *             그 구현체 클래스 자체가 아직 코드로 존재하지 않는다.
 *          두 문제 모두 해결되어야 생성자를 완성할 수 있다.
 */
#include "core/AppContext.h"

AppContext::AppContext(const AppConfig& config) : config_(config) {
    // TODO: 위 @todo 두 가지가 해결된 후 아래 순서로 조립한다.
    //   1) subscribeNetwork_, driverPubNetwork_, appPubNetwork_ 각각 dlopen 후 생성
    //   2) parser_, riskDetector_(config_.riskThreshold 주입), appSender_(appPubNetwork_.get() 주입),
    //      driverSender_(driverPubNetwork_.get() 주입) 생성
    //   3) controller_ = std::make_unique<Controller>(subscribeNetwork_.get(), parser_.get(),
    //      riskDetector_.get(), appSender_.get(), driverSender_.get(), config_.frameIntervalMs)
}

AppContext::~AppContext() {
    // controller_(unique_ptr) 소멸이 가장 먼저 일어나 스레드가 정지되고,
    // 이후 나머지 unique_ptr 멤버들이 선언 역순으로 소멸한다.
    // TODO: dlopen 핸들 정리(dlclose)는 팩토리 계약 확정 후 커스텀 삭제자 안에서 처리한다.
}

void AppContext::run() { controller_->start(); }

void AppContext::shutdown() { controller_->stop(); }