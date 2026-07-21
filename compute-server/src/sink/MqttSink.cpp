#include "MqttSink.h"

#include <mosquitto.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace {

constexpr int kChannelCount = 4;
constexpr int kDefaultMqttPort = 8883;
constexpr int kKeepAliveSeconds = 30;

std::string readEnvironment(
    const char* name,
    const char* fallback
) {
    const char* value = std::getenv(name);

    if (value == nullptr || *value == '\0') {
        return fallback;
    }

    return value;
}

int readMqttPort() noexcept {
    const char* value = std::getenv("VEDA_MQTT_PORT");

    if (value == nullptr || *value == '\0') {
        return kDefaultMqttPort;
    }

    try {
        const int port = std::stoi(value);

        if (port > 0 && port <= 65535) {
            return port;
        }
    } catch (...) {
    }

    std::fprintf(
        stderr,
        "[MqttSink] invalid VEDA_MQTT_PORT=%s; using %d\n",
        value,
        kDefaultMqttPort
    );

    return kDefaultMqttPort;
}

std::string createClientId() {
    const auto ticks =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    return
        "veda-compute-" +
        std::to_string(ticks);
}

bool isValidFrame(
    const veda::TopViewFrame& frame
) noexcept {
    if (frame.v != veda::kSchemaVersion) {
        return false;
    }

    if (frame.ts <= 0) {
        return false;
    }

    if (frame.ch < 0 || frame.ch >= kChannelCount) {
        return false;
    }

    for (const auto& object : frame.objects) {
        if (!veda::isRiskClass(object.cls)) {
            return false;
        }

        if (!std::isfinite(object.pos.x) ||
            !std::isfinite(object.pos.y)) {
            return false;
        }
    }

    // objects가 비어 있는 프레임은 정상 프레임입니다.
    return true;
}

class MqttPublisher final {
public:
    explicit MqttPublisher(
        veda::ChannelId initialChannel
    )
        : initialChannel_(initialChannel),
          host_(
              readEnvironment(
                  "VEDA_MQTT_HOST",
                  "172.20.27.174"
              )
          ),
          port_(readMqttPort()),
          caFile_(
              readEnvironment(
                  "VEDA_MQTT_CA_FILE",
                  "/etc/veda/certs/ca.crt"
              )
          ),
          clientId_(createClientId()) {
        initialize();
    }

    ~MqttPublisher() noexcept {
        shutdown();
    }

    MqttPublisher(const MqttPublisher&) = delete;
    MqttPublisher& operator=(const MqttPublisher&) = delete;

    void send(
        const veda::TopViewFrame& frame
    ) noexcept {
        if (!isValidFrame(frame)) {
            recordDrop("invalid TopViewFrame");
            return;
        }

        if (!ready_.load(std::memory_order_acquire)) {
            recordDrop("MQTT client not ready");
            return;
        }

        if (!connected_.load(std::memory_order_acquire)) {
            recordDrop("broker not connected");
            return;
        }

        try {
            const std::string topic =
                veda::topic::topView(frame.ch);

            const std::string payload =
                veda::encode(frame);

            if (payload.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<int>::max()
                )) {
                recordDrop("payload too large");
                return;
            }

            const int result = mosquitto_publish(
                client_,
                nullptr,
                topic.c_str(),
                static_cast<int>(payload.size()),
                payload.data(),
                veda::qos::kTopView,
                false
            );

            if (result != MOSQ_ERR_SUCCESS) {
                recordDrop(mosquitto_strerror(result));
                return;
            }

            publishedCount_.fetch_add(
                1,
                std::memory_order_relaxed
            );

            std::fprintf(
                stdout,
                "[MQTT PUB] topic=%s ts=%lld objects=%zu payload=%s\n",
                topic.c_str(),
                static_cast<long long>(frame.ts),
                frame.objects.size(),
                payload.c_str()
            );
        } catch (const std::exception& error) {
            recordDrop(error.what());
        } catch (...) {
            recordDrop("unknown publish exception");
        }
    }

private:
    void initialize() noexcept {
        if (initialChannel_ < 0 ||
            initialChannel_ >= kChannelCount) {
            std::fprintf(
                stderr,
                "[MqttSink] invalid initial channel: %d\n",
                initialChannel_
            );
            return;
        }

        int result = mosquitto_lib_init();

        if (result != MOSQ_ERR_SUCCESS) {
            std::fprintf(
                stderr,
                "[MqttSink] mosquitto_lib_init failed: %s\n",
                mosquitto_strerror(result)
            );
            return;
        }

        libraryInitialized_ = true;

        client_ = mosquitto_new(
            clientId_.c_str(),
            true,
            this
        );

        if (client_ == nullptr) {
            std::fprintf(
                stderr,
                "[MqttSink] mosquitto_new failed\n"
            );

            mosquitto_lib_cleanup();
            libraryInitialized_ = false;
            return;
        }

        mosquitto_connect_callback_set(
            client_,
            &MqttPublisher::onConnect
        );

        mosquitto_disconnect_callback_set(
            client_,
            &MqttPublisher::onDisconnect
        );

        mosquitto_reconnect_delay_set(
            client_,
            1,
            10,
            true
        );

        result = mosquitto_int_option(
            client_,
            MOSQ_OPT_PROTOCOL_VERSION,
            MQTT_PROTOCOL_V311
        );

        if (result == MOSQ_ERR_SUCCESS) {
            try {
                const std::string aliveTopic =
                    veda::topic::alive(initialChannel_);

                result = mosquitto_will_set(
                    client_,
                    aliveTopic.c_str(),
                    1,
                    "0",
                    veda::qos::kAlive,
                    true
                );
            } catch (...) {
                result = MOSQ_ERR_NOMEM;
            }
        }

        if (result == MOSQ_ERR_SUCCESS) {
            result = mosquitto_tls_set(
                client_,
                caFile_.c_str(),
                nullptr,
                nullptr,
                nullptr,
                nullptr
            );
        }

        if (result == MOSQ_ERR_SUCCESS) {
            // 인증서의 IP/SAN 검증을 유지합니다.
            result = mosquitto_tls_insecure_set(
                client_,
                false
            );
        }

        if (result == MOSQ_ERR_SUCCESS) {
            result = mosquitto_connect_async(
                client_,
                host_.c_str(),
                port_,
                kKeepAliveSeconds
            );
        }

        if (result == MOSQ_ERR_SUCCESS) {
            result = mosquitto_loop_start(client_);
        }

        if (result != MOSQ_ERR_SUCCESS) {
            std::fprintf(
                stderr,
                "[MqttSink] initialization failed: %s\n",
                mosquitto_strerror(result)
            );

            mosquitto_destroy(client_);
            client_ = nullptr;

            mosquitto_lib_cleanup();
            libraryInitialized_ = false;
            return;
        }

        ready_.store(
            true,
            std::memory_order_release
        );

        std::fprintf(
            stdout,
            "[MqttSink] connecting: host=%s port=%d "
            "tls=on clientId=%s\n",
            host_.c_str(),
            port_,
            clientId_.c_str()
        );
    }

    void shutdown() noexcept {
        shuttingDown_.store(
            true,
            std::memory_order_release
        );

        ready_.store(
            false,
            std::memory_order_release
        );

        if (client_ != nullptr) {
            if (connected_.load(
                    std::memory_order_acquire
                )) {
                publishAlive(false);
            }

            connected_.store(
                false,
                std::memory_order_release
            );

            mosquitto_disconnect(client_);
            mosquitto_loop_stop(client_, true);
            mosquitto_destroy(client_);

            client_ = nullptr;
        }

        if (libraryInitialized_) {
            mosquitto_lib_cleanup();
            libraryInitialized_ = false;
        }
    }

    void publishAlive(bool alive) noexcept {
        if (client_ == nullptr) {
            return;
        }

        try {
            const std::string topic =
                veda::topic::alive(initialChannel_);

            const char payload =
                alive ? '1' : '0';

            const int result = mosquitto_publish(
                client_,
                nullptr,
                topic.c_str(),
                1,
                &payload,
                veda::qos::kAlive,
                true
            );

            if (result != MOSQ_ERR_SUCCESS) {
                std::fprintf(
                    stderr,
                    "[MqttSink] alive publish failed: %s\n",
                    mosquitto_strerror(result)
                );
            }
        } catch (...) {
            std::fprintf(
                stderr,
                "[MqttSink] alive publish exception\n"
            );
        }
    }

    void recordDrop(
        const char* reason
    ) noexcept {
        const std::uint64_t count =
            droppedCount_.fetch_add(
                1,
                std::memory_order_relaxed
            ) + 1;

        if (count == 1 || count % 100 == 0) {
            std::fprintf(
                stderr,
                "[MqttSink] dropped=%llu reason=%s\n",
                static_cast<unsigned long long>(count),
                reason != nullptr ? reason : "unknown"
            );
        }
    }

    static void onConnect(
        struct mosquitto*,
        void* userData,
        int resultCode
    ) {
        auto* publisher =
            static_cast<MqttPublisher*>(userData);

        if (publisher == nullptr) {
            return;
        }

        if (publisher->shuttingDown_.load(
                std::memory_order_acquire
            )) {
            return;
        }

        if (resultCode == 0) {
            publisher->connected_.store(
                true,
                std::memory_order_release
            );

            publisher->publishAlive(true);

            std::fprintf(
                stdout,
                "[MqttSink] connected: host=%s port=%d\n",
                publisher->host_.c_str(),
                publisher->port_
            );
        } else {
            publisher->connected_.store(
                false,
                std::memory_order_release
            );

            std::fprintf(
                stderr,
                "[MqttSink] connection rejected: rc=%d\n",
                resultCode
            );
        }
    }

    static void onDisconnect(
        struct mosquitto*,
        void* userData,
        int resultCode
    ) {
        auto* publisher =
            static_cast<MqttPublisher*>(userData);

        if (publisher == nullptr) {
            return;
        }

        publisher->connected_.store(
            false,
            std::memory_order_release
        );

        if (resultCode != 0 &&
            !publisher->shuttingDown_.load(
                std::memory_order_acquire
            )) {
            std::fprintf(
                stderr,
                "[MqttSink] unexpectedly disconnected: "
                "rc=%d message=%s\n",
                resultCode,
                mosquitto_strerror(resultCode)
            );
        }
    }

    veda::ChannelId initialChannel_;

    std::string host_;
    int port_;
    std::string caFile_;
    std::string clientId_;

    struct mosquitto* client_ = nullptr;

    bool libraryInitialized_ = false;

    std::atomic_bool ready_{false};
    std::atomic_bool connected_{false};
    std::atomic_bool shuttingDown_{false};

    std::atomic_uint64_t publishedCount_{0};
    std::atomic_uint64_t droppedCount_{0};
};

}  // namespace

void publishTopViewToMqtt(
    const veda::TopViewFrame& frame
) noexcept {
    /*
     * 함수 내부 static이므로 최초 호출 때 한 번만 생성됩니다.
     * AppContext에서 MQTT 객체를 직접 생성할 필요가 없습니다.
     */
    static std::mutex publisherMutex;
    static std::unique_ptr<MqttPublisher> publisher;

    MqttPublisher* currentPublisher = nullptr;

    try {
        {
            std::lock_guard<std::mutex> lock(
                publisherMutex
            );

            if (!publisher) {
                if (frame.ch < 0 ||
                    frame.ch >= kChannelCount) {
                    std::fprintf(
                        stderr,
                        "[MqttSink] cannot initialize: "
                        "invalid channel=%d\n",
                        frame.ch
                    );
                    return;
                }

                publisher =
                    std::make_unique<MqttPublisher>(
                        frame.ch
                    );
            }

            currentPublisher = publisher.get();
        }

        if (currentPublisher != nullptr) {
            currentPublisher->send(frame);
        }
    } catch (const std::exception& error) {
        std::fprintf(
            stderr,
            "[MqttSink] publish entry failed: %s\n",
            error.what()
        );
    } catch (...) {
        std::fprintf(
            stderr,
            "[MqttSink] publish entry failed: "
            "unknown exception\n"
        );
    }
}