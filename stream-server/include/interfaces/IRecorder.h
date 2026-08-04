#pragma once

/**
 * @file    IRecorder.h
 * @brief   롤링 DVR 제어 포트 — 데이터 경로가 없는 것이 핵심
 */

#include <cstdint>
#include <functional>
#include <string>

#include "Contract.h"

/**
 * @brief 파이프라인 내부 녹화 갈래의 제어 표면
 *
 * @details
 * **이 인터페이스에는 `onPacket` 이 없다. 그게 설계다.**
 * 압축 패킷은 `tee` → `queue` → `splitmuxsink` 로 GStreamer 안에서 곧장 흐르고,
 * 우리 코드는 한 번도 만지지 않는다. 여기서 하는 일은 세그먼트 정책과 보존 정책뿐이다.
 *
 * 패킷을 C++ 로 끌어올려 파일에 쓰면 (a) 사용자 스레드가 지연되는 순간 tee 가 막혀
 * 중계까지 함께 죽고, (b) muxer 재구현이라는 부채가 생긴다. 둘 다 얻는 것이 없다.
 */
class IRecorder {
public:
    virtual ~IRecorder() = default;

    /// @brief 세그먼트 파일이 하나 닫힐 때 통지 (보존 정책 실행 지점)
    using SegmentClosedCallback = std::function<void(const std::string& path, std::uint64_t bytes)>;

    virtual void setSegmentClosedCallback(SegmentClosedCallback callback) = 0;

    /**
     * @brief 보존 정책 적용 — 오래된 세그먼트를 지운다
     *
     * @details 세그먼트가 닫힐 때마다 호출된다. 용량 상한과 개수 상한 중 **먼저 걸리는
     *          쪽**으로 지운다. 디스크가 가득 차면 splitmuxsink 가 EOS 를 내고 파이프라인
     *          전체가 멈추므로, 이 정리는 선택이 아니라 필수다.
     */
    virtual void enforceRetention() = 0;

    /// @brief 현재 사용 중인 디스크 바이트 (관측용)
    virtual std::uint64_t usedBytes() const noexcept = 0;

    /// @brief 현재 세그먼트 파일 경로 (진단용, 비어 있으면 녹화 중이 아님)
    virtual std::string currentSegment() const = 0;

    virtual veda::ChannelId channelId() const noexcept = 0;
};
