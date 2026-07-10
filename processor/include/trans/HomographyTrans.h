#pragma once

#include <opencv2/opencv.hpp>
#include <vector>

#include "interfaces/ITrans.h"

class HomographyTrans : public ITrans {
public:
    bool calibrate(const std::vector<cv::Point2f>& srcPts, const std::vector<cv::Point2f>& dstPts);

    void setCameraPosition(double camera_x, double camera_y);

    WorldPoint transform(double x, double y) const override;

private:
    cv::Mat H;
    double camera_x_ = 0.0;
    double camera_y_ = 0.0;
};