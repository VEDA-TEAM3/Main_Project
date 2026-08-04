#pragma once

/**
 * @file    EncodedPacket.h
 * @brief   압축 패킷(H.264/H.265) 소유권 핸들 — 페이로드 복사 없음
 */

#include <gst/gst.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace domain {

/**
 * @brief   GstSample 을 참조 계수로 공유하는 RAII 핸들
 *
 * @details
 * 이 타입을 복사해도 **압축 페이로드는 복사되지 않는다.** GstSample 내부의 원자적
 * 참조 계수만 하나 오른다. `tee` 의 두 갈래(디스크/네트워크)가 같은 메모리를 보는 것이
 * 이 파이프라인의 전제이므로, 이 클래스가 그 전제를 타입으로 강제한다.
 *
 * @note [ 왜 std::shared_ptr<GstSample> 이 아닌가 ]
 * `shared_ptr` 는 자기 자신의 **제어 블록을 힙에 따로 할당**한다. GstSample 은 이미
 * 침습적(intrusive) 참조 계수를 갖고 있으므로 그 위에 shared_ptr 을 씌우면 계수가 두 벌이
 * 되고, **패킷마다 힙 할당이 한 번씩 생긴다.** 4채널 × 30fps = 초당 120회이고, 이건
 * 이 저장소가 compute-server/control-server 전 구간에서 지켜 온 '프레임당 무할당'
 * 원칙과 정면으로 어긋난다. 침습적 계수를 그대로 쓰면 복사 비용이 원자적 증감 하나로
 * 끝나고 할당은 0이다. 의미론(공유 소유권)은 shared_ptr 과 동일하다.
 *
 * @warning [ 콜백 인자는 빌린 참조다 ]
 * `IPacketSource::PacketCallback` 은 `const EncodedPacket&` 를 넘긴다. 콜백이 끝난 뒤에도
 * 패킷을 들고 있으려면 **복사해서 보관**해야 한다 (복사가 곧 참조 계수 증가라 저렴하다).
 * 참조를 그대로 저장하면 다음 패킷이 덮어쓴다 — 집계기의 borrowed-buffer 규약과 같다.
 *
 * @note 스레드 안전: 참조 계수 조작은 원자적이라 여러 스레드가 각자의 EncodedPacket 사본을
 *       들고 있어도 안전하다. 다만 **같은 인스턴스**를 동시에 수정하는 것은 안 된다.
 */
class EncodedPacket {
public:
    EncodedPacket() noexcept = default;

    /// @brief GstSample 소유권을 넘겨받는다 (transfer full — 호출자는 unref 하지 않는다)
    explicit EncodedPacket(GstSample* sample) noexcept : sample_(sample) {}

    EncodedPacket(const EncodedPacket& other) noexcept
        : sample_(other.sample_ != nullptr ? gst_sample_ref(other.sample_) : nullptr) {}

    EncodedPacket(EncodedPacket&& other) noexcept : sample_(std::exchange(other.sample_, nullptr)) {}

    EncodedPacket& operator=(const EncodedPacket& other) noexcept {
        if (this != &other) {
            EncodedPacket copy(other);
            swap(copy);
        }
        return *this;
    }

    EncodedPacket& operator=(EncodedPacket&& other) noexcept {
        if (this != &other) {
            reset();
            sample_ = std::exchange(other.sample_, nullptr);
        }
        return *this;
    }

    ~EncodedPacket() { reset(); }

    void swap(EncodedPacket& other) noexcept { std::swap(sample_, other.sample_); }

    void reset() noexcept {
        if (sample_ != nullptr) {
            gst_sample_unref(sample_);
            sample_ = nullptr;
        }
    }

    /// @brief 소유권을 놓지 않고 원시 포인터를 본다 (appsrc 에 push 할 때만 사용)
    GstSample* get() const noexcept { return sample_; }

    /// @brief 소유권을 호출자에게 넘긴다 (호출자가 unref 책임)
    [[nodiscard]] GstSample* release() noexcept { return std::exchange(sample_, nullptr); }

    explicit operator bool() const noexcept { return sample_ != nullptr; }

    GstBuffer* buffer() const noexcept { return sample_ != nullptr ? gst_sample_get_buffer(sample_) : nullptr; }

    /// @brief 압축 페이로드 바이트 수 (매핑하지 않으므로 복사 없음)
    std::size_t byteSize() const noexcept {
        GstBuffer* buf = buffer();
        return buf != nullptr ? gst_buffer_get_size(buf) : 0U;
    }

    /**
     * @brief   키프레임(IDR) 여부
     * @details GStreamer 는 '키프레임 아님'을 DELTA_UNIT 플래그로 표시한다 (반대가 아니다).
     *          splitmuxsink 의 분할 지점과 RTSP 클라이언트의 진입 지점이 모두 여기에 걸린다.
     */
    bool isKeyFrame() const noexcept {
        GstBuffer* buf = buffer();
        return buf != nullptr && !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
    }

    /// @brief PTS (ns). 유효하지 않으면 GST_CLOCK_TIME_NONE
    GstClockTime pts() const noexcept {
        GstBuffer* buf = buffer();
        return buf != nullptr ? GST_BUFFER_PTS(buf) : GST_CLOCK_TIME_NONE;
    }

private:
    GstSample* sample_ = nullptr;
};

inline void swap(EncodedPacket& a, EncodedPacket& b) noexcept { a.swap(b); }

}  // namespace domain
