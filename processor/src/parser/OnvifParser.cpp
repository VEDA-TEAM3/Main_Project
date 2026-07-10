/**
 * @file    OnvifParser.cpp
 * @brief   OnvifParser 구현
 */
#include "parser/OnvifParser.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>

namespace {

/**
 * @brief   ONVIF UtcTime 문자열("YYYY-MM-DDTHH:MM:SS.sssZ")을
 *          Unix epoch 기준 밀리초로 변환
 * @param   utc_str UtcTime 속성값 문자열
 * @return  변환된 epoch 밀리초. 파싱 실패 시 std::nullopt
 * @note    Timestamp 가공 필요성의 여부를 고민중.. 일단 만들어 둠 (2026.07.10)
 */
std::optional<int64_t> parseUtcTimeMs(const std::string& utc_str) {
    int year = 0, mon = 0, day = 0, hour = 0, min = 0;
    float sec = 0.f;

    int matched = sscanf(utc_str.c_str(), "%d-%d-%dT%d:%d:%f", &year, &mon, &day, &hour, &min, &sec);
    if (matched != 6)
        return std::nullopt;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = static_cast<int>(sec);

    std::time_t epoch_sec = timegm(&tm);
    if (epoch_sec == -1)
        return std::nullopt;

    float frac_ms = (sec - static_cast<int>(sec)) * 1000.f;
    return static_cast<int64_t>(epoch_sec) * 1000 + static_cast<int64_t>(frac_ms);
}

/**
 * @brief   <tt:Transformation> 블록에서 Translate/Scale 계수를 파싱
 * @param   frame_str   <tt:Frame>...</tt:Frame> 범위의 XML 문자열
 * @param   translate_x [out] Translate x 값
 * @param   translate_y [out] Translate y 값
 * @param   scale_x     [out] Scale x 값
 * @param   scale_y     [out] Scale y 값
 * @return  파싱 성공 시 true, <tt:Transformation>을 찾지 못하거나
 *          Translate/Scale 중 하나라도 파싱 실패 시 false
 */
bool parseTransformation(const std::string& frame_str, double& translate_x, double& translate_y, double& scale_x,
                         double& scale_y) {
    size_t trans_pos = frame_str.find("<tt:Transformation");
    if (trans_pos == std::string::npos)
        return false;

    size_t translate_pos = frame_str.find("<tt:Translate", trans_pos);
    size_t scale_pos = frame_str.find("<tt:Scale", trans_pos);
    if (translate_pos == std::string::npos || scale_pos == std::string::npos)
        return false;

    if (sscanf(frame_str.c_str() + translate_pos, "<tt:Translate x=\"%lf\" y=\"%lf\"", &translate_x, &translate_y) != 2)
        return false;

    if (sscanf(frame_str.c_str() + scale_pos, "<tt:Scale x=\"%lf\" y=\"%lf\"", &scale_x, &scale_y) != 2)
        return false;

    return true;
}

/**
 * @brief   픽셀 좌표를 Translate/Scale 계수로 정규화된 좌표(-1.0~1.0)로 변환
 * @param   pixel_val   픽셀 단위 좌표값
 * @param   translate   해당 축의 Translate 계수
 * @param   scale       해당 축의 Scale 계수
 * @return  정규화된 좌표값
 */
float normalize(float pixel_val, double translate, double scale) {
    return static_cast<float>(pixel_val * scale + translate);
}

}  // namespace

ParsedFrame OnvifParser::parse(std::string_view payload) {
    ParsedFrame frame_result;

    size_t frame_pos = payload.find("<tt:Frame");
    if (frame_pos == std::string_view::npos) {
        return frame_result;
    }

    size_t frame_end = payload.find("</tt:Frame>", frame_pos);
    if (frame_end == std::string_view::npos)
        return frame_result;

    std::string frame_str(payload.substr(frame_pos, frame_end - frame_pos));

    size_t utc_pos = frame_str.find("UtcTime=\"");
    if (utc_pos == std::string::npos)
        return frame_result;

    char utc_buf[64] = {0};
    if (sscanf(frame_str.c_str() + utc_pos, "UtcTime=\"%63[^\"]\"", utc_buf) != 1)
        return frame_result;

    auto timestamp = parseUtcTimeMs(utc_buf);
    if (!timestamp.has_value())
        return frame_result;

    double translate_x = 0, translate_y = 0, scale_x = 0, scale_y = 0;
    if (!parseTransformation(frame_str, translate_x, translate_y, scale_x, scale_y)) {
        return frame_result;
    }

    frame_result.timestamp_ms = timestamp.value();

    size_t pos = 0;
    while ((pos = frame_str.find("<tt:Object", pos)) != std::string::npos) {
        size_t end_pos = frame_str.find("</tt:Object>", pos);
        if (end_pos == std::string::npos)
            break;

        std::string obj_str(frame_str.substr(pos, end_pos - pos));
        DetectedObject obj;
        char type_buf[64] = "Unknown";

        size_t id_pos = obj_str.find("ObjectId=\"");
        if (id_pos != std::string::npos)
            sscanf(obj_str.c_str() + id_pos, "ObjectId=\"%d\"", &obj.id);

        float raw_left = 0, raw_top = 0, raw_right = 0, raw_bottom = 0;
        size_t bbox_pos = obj_str.find("<tt:BoundingBox");
        if (bbox_pos != std::string::npos)
            sscanf(obj_str.c_str() + bbox_pos, "<tt:BoundingBox left=\"%f\" top=\"%f\" right=\"%f\" bottom=\"%f\"",
                   &raw_left, &raw_top, &raw_right, &raw_bottom);

        float raw_cx = 0, raw_cy = 0;
        size_t cog_pos = obj_str.find("<tt:CenterOfGravity");
        if (cog_pos != std::string::npos)
            sscanf(obj_str.c_str() + cog_pos, "<tt:CenterOfGravity x=\"%f\" y=\"%f\"", &raw_cx, &raw_cy);

        size_t type_pos = obj_str.find("<tt:Type Likelihood=\"");
        if (type_pos != std::string::npos) {
            sscanf(obj_str.c_str() + type_pos, "<tt:Type Likelihood=\"%f\">%63[^<]", &obj.likelihood, type_buf);
            obj.type = type_buf;
        }

        obj.left = normalize(raw_left, translate_x, scale_x);
        obj.right = normalize(raw_right, translate_x, scale_x);
        obj.top = normalize(raw_top, translate_y, scale_y);
        obj.bottom = normalize(raw_bottom, translate_y, scale_y);
        obj.cx = normalize(raw_cx, translate_x, scale_x);
        obj.cy = normalize(raw_cy, translate_y, scale_y);

        if (obj.id != -1)
            frame_result.objects.push_back(obj);

        pos = end_pos + 12;
    }

    return frame_result;
}

std::shared_ptr<IParser> createParser() { return std::make_shared<OnvifParser>(); }