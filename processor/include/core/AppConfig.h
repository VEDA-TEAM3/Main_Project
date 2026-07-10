/**
 * @file    AppConfig.h
 * @brief   CCTV RTSP 연결 설정을 담는 구조체 정의
 */
#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

/**
 * @brief   CCTV RTSP Config 구조체
 */
struct AppConfig {
    std::string ip;        /**< CCTV ip address */
    int port = 0;          /**< CCTV port */
    int channel = 0;       /**< Camera channel */
    int profile = 0;       /**< CCTV profile */
    std::string user;      /**< CCTV user */
    std::string pass;      /**< CCTV password */
    std::string rtsp_uri;  /**< CCTV RTSP URI */
    std::string setup_uri; /**< CCTV RTSP Metadata URI */

    /**
     * @brief   환경 변수(.env)로부터 설정 값을 읽어 AppConfig를 생성
     * @return  환경 변수 값으로 채워진 AppConfig 인스턴스
     * @note    환경 변수 중 하나라도 비어있다면 에러 메시지를 출력하고 std::exit(EXIT_FAILURE)로 종료
     */
    static AppConfig fromEnv() {
        auto get_env_safe = [](const char* key) -> std::string {
            const char* val = std::getenv(key);
            return (val == nullptr) ? "" : std::string(val);
        };

        AppConfig cfg;
        cfg.ip = get_env_safe("CAM_IP");
        std::string port_str = get_env_safe("CAM_PORT");
        std::string chan_str = get_env_safe("CAM_CHANNEL");
        std::string prof_str = get_env_safe("CAM_PROFILE");
        cfg.user = get_env_safe("CAM_USER");
        cfg.pass = get_env_safe("CAM_PASS");

        if (cfg.ip.empty() || port_str.empty() || chan_str.empty() || prof_str.empty() || cfg.user.empty() ||
            cfg.pass.empty()) {
            std::cerr << "[-] Fatal Error: Missing required environment variables.\n";
            std::exit(EXIT_FAILURE);
        }

        cfg.port = std::stoi(port_str);
        cfg.channel = std::stoi(chan_str);
        cfg.profile = std::stoi(prof_str);
        cfg.rtsp_uri = "rtsp://" + cfg.ip + "/" + chan_str + "/profile" + prof_str + "/media.smp";
        cfg.setup_uri = cfg.rtsp_uri + "/trackID=m";
        return cfg;
    }
};