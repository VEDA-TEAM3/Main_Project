#include "risk/ThresholdRiskPolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

ThresholdRiskPolicy::ThresholdRiskPolicy(std::shared_ptr<IDistanceMetric> metric, double warningDist, double dangerDist)
    : metric_(std::move(metric)), warningDist_(warningDist), dangerDist_(dangerDist) {}

domain::RiskEvaluation ThresholdRiskPolicy::evaluate(domain::WorldFrame& frame) {
    domain::RiskEvaluation eval;
    eval.timestamp = frame.timestamp;
    eval.overallLevel = veda::RiskLevel::None;

    if (frame.objects.size() < 2) {
        return eval;
    }

    for (auto& obj : frame.objects) {
        obj.nearestDist = std::numeric_limits<double>::max();
        obj.riskLevel = veda::RiskLevel::None;
    }

    std::vector<domain::WorldObject*> sortedObjs;
    sortedObjs.reserve(frame.objects.size());
    for (auto& obj : frame.objects) {
        sortedObjs.push_back(&obj);
    }

    std::sort(sortedObjs.begin(), sortedObjs.end(),
              [](const domain::WorldObject* a, const domain::WorldObject* b) { return a->pos.x < b->pos.x; });

    for (size_t i = 0; i < sortedObjs.size(); ++i) {
        for (size_t j = i + 1; j < sortedObjs.size(); ++j) {
            double dx = sortedObjs[j]->pos.x - sortedObjs[i]->pos.x;

            if (dx > warningDist_) {
                break;
            }

            double dy = std::abs(sortedObjs[j]->pos.y - sortedObjs[i]->pos.y);
            if (dy > warningDist_) {
                continue;
            }

            double dist = metric_->calculate(sortedObjs[i]->pos, sortedObjs[j]->pos);

            if (dist < sortedObjs[i]->nearestDist) {
                sortedObjs[i]->nearestDist = dist;
                sortedObjs[i]->nearestObj = sortedObjs[j]->gid;
            }

            if (dist < sortedObjs[j]->nearestDist) {
                sortedObjs[j]->nearestDist = dist;
                sortedObjs[j]->nearestObj = sortedObjs[i]->gid;
            }
        }
    }

    for (auto& obj : frame.objects) {
        if (obj.nearestDist <= dangerDist_) {
            obj.riskLevel = veda::RiskLevel::Danger;
            eval.overallLevel = veda::RiskLevel::Danger;
        } else if (obj.nearestDist <= warningDist_) {
            obj.riskLevel = veda::RiskLevel::Warning;
            if (eval.overallLevel != veda::RiskLevel::Danger) {
                eval.overallLevel = veda::RiskLevel::Warning;
            }
        }
    }

    return eval;
}