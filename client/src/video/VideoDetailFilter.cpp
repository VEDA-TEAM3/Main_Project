#include "video/VideoDetailFilter.h"

#include <gst/base/gstbasetransform.h>
#include <gst/video/gstvideofilter.h>

#include <mutex>

#include "video/VideoDetailProcessor.h"

namespace {
constexpr auto detailFactoryName = "qtvideodetail";

struct GstQtVideoDetailFilter {
    GstVideoFilter parent;
    VideoDetailProcessor* processor = nullptr;
};

struct GstQtVideoDetailFilterClass {
    GstVideoFilterClass parentClass;
};

G_DEFINE_TYPE(GstQtVideoDetailFilter, gst_qt_video_detail_filter, GST_TYPE_VIDEO_FILTER)

/**
 * @brief       쓰기 가능한 영상 프레임에 선택된 디테일 보정을 적용합니다.
 * @param filter 영상 디테일 필터 인스턴스
 * @param frame  쓰기 가능한 BGRA 프레임
 * @return      다음 요소로 프레임을 전달하기 위한 흐름 상태
 */
GstFlowReturn transformFrameInPlace(GstVideoFilter* filter, GstVideoFrame* frame) {
    auto* detailFilter = reinterpret_cast<GstQtVideoDetailFilter*>(filter);
    if (detailFilter->processor && frame) {
        detailFilter->processor->apply(*frame);
    }
    return GST_FLOW_OK;
}

/**
 * @brief       애플리케이션 내부 디테일 필터의 caps와 변환 함수를 등록합니다.
 * @param klass 초기화할 필터 클래스
 */
void gst_qt_video_detail_filter_class_init(GstQtVideoDetailFilterClass* klass) {
    auto* elementClass = GST_ELEMENT_CLASS(klass);
    auto* videoFilterClass = GST_VIDEO_FILTER_CLASS(klass);

    GstCaps* caps = gst_caps_from_string("video/x-raw,format=(string)BGRA");
    GstPadTemplate* sinkTemplate = gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, gst_caps_ref(caps));
    GstPadTemplate* sourceTemplate = gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, gst_caps_ref(caps));
    gst_caps_unref(caps);

    gst_element_class_add_pad_template(elementClass, sinkTemplate);
    gst_element_class_add_pad_template(elementClass, sourceTemplate);
    gst_element_class_set_static_metadata(elementClass, "Qt video detail filter", "Filter/Effect/Video",
                                          "Applies lightweight denoise and sharpening to BGRA frames",
                                          "Qt CCTV Client");
    videoFilterClass->transform_frame_ip = &transformFrameInPlace;
}

/**
 * @brief      디테일 필터 인스턴스를 in-place 변환 모드로 초기화합니다.
 * @param self 초기화할 필터 인스턴스
 */
void gst_qt_video_detail_filter_init(GstQtVideoDetailFilter* self) {
    self->processor = nullptr;
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
}
}  // namespace

/**
 * @brief  현재 프로세스에 영상 디테일 필터를 한 번만 등록합니다.
 * @return 등록에 성공했거나 이미 등록되어 있으면 true
 */
bool VideoDetailFilter::ensureRegistered() {
    static std::once_flag registrationFlag;
    static bool registered = false;

    std::call_once(registrationFlag, []() {
        if (GstElementFactory* existingFactory = gst_element_factory_find(detailFactoryName)) {
            gst_object_unref(existingFactory);
            registered = true;
            return;
        }
        registered =
            gst_element_register(nullptr, detailFactoryName, GST_RANK_NONE, gst_qt_video_detail_filter_get_type());
    });
    return registered;
}

/**
 * @brief           생성된 GStreamer 필터에 채널 전용 디테일 처리기를 연결합니다.
 * @param element   qtvideodetail 요소
 * @param processor 연결할 디테일 처리기
 */
void VideoDetailFilter::setProcessor(GstElement* element, VideoDetailProcessor* processor) {
    if (!element || !G_TYPE_CHECK_INSTANCE_TYPE(element, gst_qt_video_detail_filter_get_type())) {
        return;
    }
    auto* filter = reinterpret_cast<GstQtVideoDetailFilter*>(element);
    filter->processor = processor;
}
