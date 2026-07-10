#include "trans/HomographyTrans.h"

#include <cmath>

bool HomographyTrans::calibrate(const std::vector<cv::Point2f>& srcPts, const std::vector<cv::Point2f>& dstPts) {
    H = cv::findHomography(srcPts, dstPts);
    return !H.empty();
}

void HomographyTrans::setCameraPosition(double camera_x, double camera_y) {
    camera_x_ = camera_x;
    camera_y_ = camera_y;
}

WorldPoint HomographyTrans::transform(double x, double y) const {
    WorldPoint wp;
    if (H.empty())
        return wp;

    std::vector<cv::Point2f> src = {cv::Point2f((float)x, (float)y)};
    std::vector<cv::Point2f> dst;
    cv::perspectiveTransform(src, dst, H);

    wp.x = dst[0].x;
    wp.y = dst[0].y;
    double dx = wp.x - camera_x_, dy = wp.y - camera_y_;
    wp.distance = std::sqrt(dx * dx + dy * dy);
    return wp;
}

std::shared_ptr<ITrans> createTrans() { return std::make_shared<HomographyTrans>(); }