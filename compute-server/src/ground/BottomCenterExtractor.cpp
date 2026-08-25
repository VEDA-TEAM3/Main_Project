#include "ground/BottomCenterExtractor.h"

domain::ImagePoint BottomCenterExtractor::extract(const domain::NormBox& box) {
    return domain::ImagePoint{(box.l + box.r) * 0.5, box.b};
}