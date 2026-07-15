#include "metric/EuclideanMetric.h"

#include <cmath>

double EuclideanMetric::calculate(const domain::WorldPoint& p1, const domain::WorldPoint& p2) const {
    double dx = p1.x - p2.x;
    double dy = p1.y - p2.y;

    return std::hypot(dx, dy);
}