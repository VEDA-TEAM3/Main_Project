#include "video/BlurVideoFilter.h"

#include <gst/base/gstbasetransform.h>
#include <gst/video/gstvideofilter.h>

#include <mutex>

#include "video/BlurProcessor.h"

namespace {
constexpr auto blurFactoryName = "qtblur";

struct GstQtBlurFilter {
    GstVideoFilter parent;
    BlurProcessor* processor = nullptr;
};

struct GstQtBlurFilterClass {
    GstVideoFilterClass parentClass;
};

G_DEFINE_TYPE(GstQtBlurFilter, gst_qt_blur_filter, GST_TYPE_VIDEO_FILTER)

/**
 * @brief       GStreamer가 제공한 쓰기 가능한 프레임에 블러를 적용합니다.
 * @param filter 블러 필터 인스턴스
 * @param frame  쓰기 가능한 BGRA 영상 프레임
 * @return      다음 요소로 프레임을 전달하기 위한 흐름 상태
 */
GstFlowReturn transformFrameInPlace(GstVideoFilter* filter, GstVideoFrame* frame) {
    auto* blurFilter = reinterpret_cast<GstQtBlurFilter*>(filter);

    if (blurFilter->processor && frame) {
        blurFilter->processor->apply(*frame);
    }

    return GST_FLOW_OK;
}

/**
 * @brief       애플리케이션 내부 블러 필터 타입의 caps와 변환 함수를 등록합니다.
 * @param klass 초기화할 필터 클래스
 */
void gst_qt_blur_filter_class_init(GstQtBlurFilterClass* klass) {
    auto* elementClass = GST_ELEMENT_CLASS(klass);
    auto* videoFilterClass = GST_VIDEO_FILTER_CLASS(klass);

    GstCaps* caps = gst_caps_from_string("video/x-raw,format=(string)BGRA");
    GstPadTemplate* sinkTemplate = gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, gst_caps_ref(caps));
    GstPadTemplate* sourceTemplate = gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, gst_caps_ref(caps));
    gst_caps_unref(caps);

    gst_element_class_add_pad_template(elementClass, sinkTemplate);
    gst_element_class_add_pad_template(elementClass, sourceTemplate);
    gst_element_class_set_static_metadata(elementClass, "Qt blur filter", "Filter/Effect/Video",
                                          "Applies MQTT blur regions to writable BGRA frames", "Qt CCTV Client");

    videoFilterClass->transform_frame_ip = &transformFrameInPlace;
}

/**
 * @brief      블러 필터 인스턴스를 in-place 변환 모드로 초기화합니다.
 * @param self 초기화할 필터 인스턴스
 */
void gst_qt_blur_filter_init(GstQtBlurFilter* self) {
    self->processor = nullptr;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
}
}  // namespace

/**
 * @brief  현재 프로세스에 블러 영상 요소를 한 번만 등록합니다.
 * @return 등록에 성공했거나 이미 등록되어 있으면 true
 */
bool BlurVideoFilter::ensureRegistered() {
    static std::once_flag registrationFlag;
    static bool registered = false;

    std::call_once(registrationFlag, []() {
        if (GstElementFactory* existingFactory = gst_element_factory_find(blurFactoryName)) {
            gst_object_unref(existingFactory);
            registered = true;
            return;
        }

        registered = gst_element_register(nullptr, blurFactoryName, GST_RANK_NONE, gst_qt_blur_filter_get_type());
    });

    return registered;
}

/**
 * @brief           생성된 GStreamer 필터에 채널 전용 블러 처리기를 연결합니다.
 * @param element   qtblur 요소
 * @param processor 연결할 블러 처리기
 */
void BlurVideoFilter::setProcessor(GstElement* element, BlurProcessor* processor) {
    if (!element || !G_TYPE_CHECK_INSTANCE_TYPE(element, gst_qt_blur_filter_get_type())) {
        return;
    }

    auto* filter = reinterpret_cast<GstQtBlurFilter*>(element);
    filter->processor = processor;
}
