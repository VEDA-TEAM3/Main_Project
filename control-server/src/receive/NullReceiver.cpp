#include "receive/NullReceiver.h"

#include <chrono>

NullReceiver::~NullReceiver() { NullReceiver::stop(); }

void NullReceiver::setCallback(FrameCallback callback) { callback_ = std::move(callback); }

void NullReceiver::setAliveCallback(AliveCallback callback) { aliveCallback_ = std::move(callback); }

void NullReceiver::start() {
    if (isRunning_)
        return;
    isRunning_ = true;
    worker_ = std::thread(&NullReceiver::generateDataLoop, this);

    if (aliveCallback_) {
        aliveCallback_(1, true);
        aliveCallback_(2, true);
    }
}

void NullReceiver::stop() {
    if (!isRunning_)
        return;
    isRunning_ = false;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void NullReceiver::generateDataLoop() {
    while (isRunning_) {
        if (callback_) {
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

            // ---------------------------------------------------------
            // 채널 1 (카메라 1) 데이터 생성: 차량 1대
            // ---------------------------------------------------------
            veda::TopViewFrame frame1;
            frame1.ch = 1;          // channelId -> ch
            frame1.ts = timestamp;  // timestamp -> ts

            veda::TopViewObject obj1;
            obj1.id = 101;  // objectId -> id
            obj1.cls = veda::ObjectClass::Vehicle;
            obj1.pos.x = 10.0;  // x -> pos.x
            obj1.pos.y = 20.0;  // y -> pos.y
            frame1.objects.push_back(obj1);

            callback_(frame1);  // Aggregator로 전송

            // 네트워크 비동기 지연을 시뮬레이션하기 위해 약간 대기 (5ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            // ---------------------------------------------------------
            // 채널 2 (카메라 2) 데이터 생성: 사람 1명
            // ---------------------------------------------------------
            veda::TopViewFrame frame2;
            frame2.ch = 2;          // channelId -> ch
            frame2.ts = timestamp;  // timestamp -> ts (채널 1과 동일)

            veda::TopViewObject obj2;
            obj2.id = 201;                        // objectId -> id
            obj2.cls = veda::ObjectClass::Human;  // Person -> Human
            obj2.pos.x = 12.0;                    // x -> pos.x
            obj2.pos.y = 22.0;                    // y -> pos.y
            frame2.objects.push_back(obj2);

            callback_(frame2);  // Aggregator로 전송
        }

        // 약 30FPS (약 33ms) 주기로 다음 프레임 세트 발생을 위해 대기
        std::this_thread::sleep_for(std::chrono::milliseconds(28));
    }
}