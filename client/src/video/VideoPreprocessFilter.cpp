#include "video/VideoPreprocessFilter.h"

#include <gst/base/gstbasetransform.h>
#include <gst/video/gstvideofilter.h>
#include <gst/video/video-frame.h>

#include <mutex>
#include <opencv2/core.hpp>

#include "video/VideoPreprocessLut.h"

namespace {
constexpr auto preprocessFactoryName = "qtpreprocess";

struct GstQtPreprocessFilter {
    GstVideoFilter parent;
    /// 설정이 바뀔 때만 다시 만든다. 프레임마다 만들 이유가 없다.
    /// GObject가 인스턴스를 할당하므로 값으로 담는다(std::array는 별도 생성자가 필요 없다)
    VideoPreprocessLut lut;
};

struct GstQtPreprocessFilterClass {
    GstVideoFilterClass parentClass;
};

G_DEFINE_TYPE(GstQtPreprocessFilter, gst_qt_preprocess_filter, GST_TYPE_VIDEO_FILTER)

/**
 * @brief        Y 평면에 합성 룩업 테이블을 적용합니다.
 * @param filter 전처리 필터 인스턴스
 * @param frame  쓰기 가능한 NV12 영상 프레임
 * @return       다음 요소로 프레임을 전달하기 위한 흐름 상태
 *
 * @details 크로마는 건드리지 않습니다. 밝기·대비·감마는 셋 다 휘도 보정이고, 예전
 *          videobalance도 채도(saturation)를 중립으로 둔 채 Y만 바꿨습니다.
 *
 *          cv::Mat이 NV12의 Y 평면을 **복사 없이** 감쌉니다. 색공간 변환이 없으므로
 *          프레임당 추가 비용은 룩업 한 번뿐입니다.
 */
GstFlowReturn transformFrameInPlace(GstVideoFilter* filter, GstVideoFrame* frame) {
    auto* preprocessFilter = reinterpret_cast<GstQtPreprocessFilter*>(filter);
    if (!frame || GST_VIDEO_FRAME_FORMAT(frame) != GST_VIDEO_FORMAT_NV12) {
        return GST_FLOW_OK;
    }

    auto* planeData = static_cast<unsigned char*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 0));
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
    const int width = GST_VIDEO_FRAME_WIDTH(frame);
    const int height = GST_VIDEO_FRAME_HEIGHT(frame);
    if (!planeData || stride <= 0 || width <= 0 || height <= 0) {
        return GST_FLOW_OK;
    }

    // 설정 변경은 다른 스레드에서 온다. 표를 잠깐 잠그고 복사한 뒤 풀어서, 프레임 하나가
    // 옛 값과 새 값이 섞인 표로 처리되는 일이 없게 한다. 256바이트 복사라 비용이 없다시피 하고,
    // 락을 프레임 전체 동안 들고 있지 않으므로 설정 변경도 막히지 않는다
    VideoPreprocessLut lut{};
    GST_OBJECT_LOCK(filter);
    lut = preprocessFilter->lut;
    GST_OBJECT_UNLOCK(filter);

    cv::Mat luma(height, width, CV_8UC1, planeData, static_cast<size_t>(stride));
    const cv::Mat lookup(1, 256, CV_8UC1, lut.data());
    cv::LUT(luma, lookup, luma);
    return GST_FLOW_OK;
}

/**
 * @brief       필터 타입의 caps와 변환 함수를 등록합니다.
 * @param klass 초기화할 필터 클래스
 */
void gst_qt_preprocess_filter_class_init(GstQtPreprocessFilterClass* klass) {
    auto* elementClass = GST_ELEMENT_CLASS(klass);
    auto* videoFilterClass = GST_VIDEO_FILTER_CLASS(klass);

    // 디코더가 내는 NV12를 그대로 받는다. 예전 videobalance/gamma도 NV12를 네이티브로
    // 지원했으므로 이 교체로 색공간 변환이 새로 생기지 않는다
    GstCaps* caps = gst_caps_from_string("video/x-raw,format=(string)NV12");
    GstPadTemplate* sinkTemplate = gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, gst_caps_ref(caps));
    GstPadTemplate* sourceTemplate = gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, gst_caps_ref(caps));
    gst_caps_unref(caps);

    gst_element_class_add_pad_template(elementClass, sinkTemplate);
    gst_element_class_add_pad_template(elementClass, sourceTemplate);
    gst_element_class_set_static_metadata(elementClass, "Qt video preprocess filter", "Filter/Effect/Video",
                                          "Applies brightness, contrast and gamma as one lookup", "Qt CCTV Client");

    videoFilterClass->transform_frame_ip = &transformFrameInPlace;
}

/**
 * @brief      필터 인스턴스를 in-place 변환 모드로 초기화합니다.
 * @param self 초기화할 필터 인스턴스
 */
void gst_qt_preprocess_filter_init(GstQtPreprocessFilter* self) {
    // 설정이 오기 전까지는 항등 표를 둔다. passthrough라 실제로 쓰이지는 않는다
    for (int input = 0; input < 256; ++input) {
        self->lut[static_cast<size_t>(input)] = static_cast<unsigned char>(input);
    }
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), TRUE);
}
}  // namespace

/**
 * @brief  현재 프로세스에 전처리 영상 요소를 한 번만 등록합니다.
 * @return 등록에 성공했거나 이미 등록되어 있으면 true
 */
bool VideoPreprocessFilter::ensureRegistered() {
    static std::once_flag registrationFlag;
    static bool registered = false;

    std::call_once(registrationFlag, []() {
        if (GstElementFactory* existingFactory = gst_element_factory_find(preprocessFactoryName)) {
            gst_object_unref(existingFactory);
            registered = true;
            return;
        }

        registered =
            gst_element_register(nullptr, preprocessFactoryName, GST_RANK_NONE, gst_qt_preprocess_filter_get_type());
    });

    return registered;
}

/**
 * @brief          필터에 현재 전처리 설정을 반영합니다.
 * @param element  qtpreprocess 요소
 * @param settings 적용할 밝기·대비·감마
 *
 * @details 항등 설정이면 passthrough로 내립니다. passthrough가 아니면 GstBaseTransform이
 *          매 프레임 버퍼를 쓰기 가능 상태로 만들고, 버퍼가 쓰기 불가능하면 프레임 전체를
 *          복사합니다. 보정을 끈 채널에서는 그 비용이 전부 낭비입니다.
 */
void VideoPreprocessFilter::setSettings(GstElement* element, const VideoPreprocessingSettings& settings) {
    if (!element || !G_TYPE_CHECK_INSTANCE_TYPE(element, gst_qt_preprocess_filter_get_type())) {
        return;
    }

    auto* filter = reinterpret_cast<GstQtPreprocessFilter*>(element);
    const VideoPreprocessLut lut = buildVideoPreprocessLut(settings);

    GST_OBJECT_LOCK(element);
    filter->lut = lut;
    GST_OBJECT_UNLOCK(element);

    gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(element), isNeutralVideoPreprocessing(settings));
}
