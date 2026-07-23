/**
 * @file    verify_pipeline.cpp
 * @brief   control-server 좌표 변환 / zone 배정 / 위험 판정 / 채널 융합 검증 하네스
 *
 * @details
 * AffineLocalToWorldTransform, AngleZoneMapper, ThresholdRiskPolicy, ConcatFuser 를
 * 실제 소스 그대로 링크해서 돌린다. MQTT/UART 없이 순수 계산 경로만 검증하므로
 * libmosquitto 의존이 없다.
 *
 * @par [ 빌드 & 실행 ]
 * @code
 * cmake -B build && cmake --build build --target verify-pipeline
 * ./build/verify-pipeline
 * @endcode
 *
 * @note [ 좌표계 규약 — 이 하네스가 고정하려는 핵심 ]
 * compute-server 가 보내는 TopViewObject::pos 는 '카메라 로컬' 좌표다.
 * (타입 이름이 veda::WorldPoint 라서 오해하기 쉽지만, ILocalToWorldTransform.h 가
 *  명시하듯 연산 서버는 도면을 모른다)
 *  - localY = 카메라 전방 거리 (m)
 *  - localX = 좌우 오프셋 (m), 부호는 CameraCalibration::lateralSign 이 정함
 * 이 규약이 어긋나면 zone 배정과 거리 판정이 통째로 틀어지므로 여기서 고정한다.
 *
 * @note 종료 코드 0 = 전부 통과. 실패 개수가 있으면 1.
 */

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/AppConfig.h"
#include "fuse/ConcatFuser.h"
#include "metric/EuclideanMetric.h"
#include "risk/ThresholdRiskPolicy.h"
#include "transform/AffineLocalToWorldTransform.h"
#include "zone/AngleZoneMapper.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what) {
    (ok ? g_pass : g_fail)++;
    std::printf("%s%s\n", ok ? "  [PASS] " : "  [FAIL] ", what.c_str());
}

void section(const std::string& title) { std::printf("\n=== %s ===\n", title.c_str()); }

bool near(double a, double b, double tol = 1e-9) { return std::abs(a - b) < tol; }

/// @brief 테스트 대상이 남기는 로그를 삼켜 결과 출력이 묻히지 않게 함
class LogSilencer {
public:
    LogSilencer() : out_(std::cout.rdbuf(nullptr)), err_(std::cerr.rdbuf(nullptr)) {}
    ~LogSilencer() {
        std::cout.rdbuf(out_);
        std::cerr.rdbuf(err_);
    }
    LogSilencer(const LogSilencer&) = delete;
    LogSilencer& operator=(const LogSilencer&) = delete;

private:
    std::streambuf* out_;
    std::streambuf* err_;
};

CameraCalibration makeCal(veda::ChannelId ch, double x, double y, double facingDeg, int lateralSign) {
    CameraCalibration c;
    c.channelId = ch;
    c.cameraPosX = x;
    c.cameraPosY = y;
    c.facingAngleDeg = facingDeg;
    c.lateralSign = lateralSign;
    return c;
}

veda::TopViewFrame makeFrame(veda::ChannelId ch, double localX, double localY,
                             veda::ObjectClass cls = veda::ObjectClass::Vehicle) {
    veda::TopViewFrame f;
    f.ch = ch;
    f.ts = 1000;
    veda::TopViewObject o;
    o.id = 1;
    o.cls = cls;
    o.pos = veda::LocalPoint{localX, localY};
    f.objects.push_back(o);
    return f;
}

/// @brief 로컬 좌표 하나를 변환한 결과 (in-place 변환이라 타입은 LocalPoint 지만 값은 월드 좌표)
veda::LocalPoint transformOne(const CameraCalibration& cal, double localX, double localY) {
    AffineLocalToWorldTransform transform({cal});
    std::vector<veda::TopViewFrame> frames{makeFrame(cal.channelId, localX, localY)};
    transform.transform(frames);
    return frames[0].objects[0].pos;
}

}  // namespace

int main() {
    LogSilencer silencer;

    // -----------------------------------------------------------------------
    section("1) AffineLocalToWorldTransform — 나침반 방위 규약");
    // -----------------------------------------------------------------------
    // facingAngleDeg: 북(도면 위쪽)=0, 시계방향 증가. 카메라는 원점에 설치.
    {
        struct Case {
            double facing;
            const char* name;
            double expectX, expectY;  ///< 전방 10m 지점의 기대 월드 좌표
        };
        const Case cases[] = {
            {0.0, "북(0도) 전방 10m -> +y", 0.0, 10.0},
            {90.0, "동(90도) 전방 10m -> +x", 10.0, 0.0},
            {180.0, "남(180도) 전방 10m -> -y", 0.0, -10.0},
            {270.0, "서(270도) 전방 10m -> -x", -10.0, 0.0},
        };
        for (const auto& c : cases) {
            const auto w = transformOne(makeCal(0, 0.0, 0.0, c.facing, 1), 0.0, 10.0);
            std::printf("      facing=%5.1f도  local(0,10) -> world(%6.2f, %6.2f)\n", c.facing, w.x, w.y);
            check(near(w.x, c.expectX, 1e-9) && near(w.y, c.expectY, 1e-9), c.name);
        }
    }

    // -----------------------------------------------------------------------
    section("2) lateralSign — 로컬 +x 가 전방 기준 어느 쪽인가");
    // -----------------------------------------------------------------------
    {
        // 북쪽을 보는 카메라: 전방 = +y. 오른쪽 = 동 = +x
        const auto right = transformOne(makeCal(0, 0.0, 0.0, 0.0, +1), 5.0, 10.0);
        std::printf("      lateralSign=+1  local(5,10) -> world(%6.2f, %6.2f)\n", right.x, right.y);
        check(near(right.x, 5.0) && near(right.y, 10.0), "lateralSign=+1 -> 로컬 +x 가 전방의 오른쪽(동)");

        const auto left = transformOne(makeCal(0, 0.0, 0.0, 0.0, -1), 5.0, 10.0);
        std::printf("      lateralSign=-1  local(5,10) -> world(%6.2f, %6.2f)\n", left.x, left.y);
        check(near(left.x, -5.0) && near(left.y, 10.0), "lateralSign=-1 -> 로컬 +x 가 전방의 왼쪽(서)");

        // 동쪽을 보는 카메라의 오른쪽은 남(-y)
        const auto eastRight = transformOne(makeCal(0, 0.0, 0.0, 90.0, +1), 5.0, 10.0);
        std::printf("      facing=90, lateralSign=+1  local(5,10) -> world(%6.2f, %6.2f)\n", eastRight.x, eastRight.y);
        check(near(eastRight.x, 10.0) && near(eastRight.y, -5.0), "동향 카메라의 오른쪽은 남(-y)");
    }

    // -----------------------------------------------------------------------
    section("3) 카메라 설치 위치 오프셋");
    // -----------------------------------------------------------------------
    {
        const auto w = transformOne(makeCal(0, 3.0, -4.0, 0.0, +1), 0.0, 10.0);
        std::printf("      cameraPos(3,-4) + 전방 10m -> world(%6.2f, %6.2f)\n", w.x, w.y);
        check(near(w.x, 3.0) && near(w.y, 6.0), "cameraPosX/Y 가 그대로 더해짐");
    }

    // -----------------------------------------------------------------------
    section("4) 캘리브레이션 누락 채널 (CT-7)");
    // -----------------------------------------------------------------------
    {
        // 기본값(dropUncalibrated=true): 조용히 틀린 좌표를 흘리느니 버린다
        AffineLocalToWorldTransform dropping({makeCal(0, 0.0, 0.0, 0.0, 1)});
        std::vector<veda::TopViewFrame> frames{makeFrame(3, 7.0, 11.0)};  // ch3 은 캘리브 없음
        dropping.transform(frames);
        check(frames[0].objects.empty(), "캘리브 없는 채널의 객체는 폐기됨 (기본값)");

        // 명시적으로 끈 경우에만 예전처럼 통과
        AffineLocalToWorldTransform passing({makeCal(0, 0.0, 0.0, 0.0, 1)}, /*dropUncalibrated=*/false);
        std::vector<veda::TopViewFrame> frames2{makeFrame(3, 7.0, 11.0)};
        passing.transform(frames2);
        check(!frames2[0].objects.empty() && near(frames2[0].objects[0].pos.x, 7.0),
              "dropUncalibrated=false 면 예전처럼 로컬 좌표가 그대로 통과");
    }

    // -----------------------------------------------------------------------
    section("5) AngleZoneMapper — 각도 구간 배정");
    // -----------------------------------------------------------------------
    {
        std::vector<ZoneBoundary> boundaries;
        for (int i = 0; i < 4; ++i) {
            ZoneBoundary b;
            b.channelId = i;
            b.angleMinDeg = i * 90.0;
            b.angleMaxDeg = (i + 1) * 90.0;
            boundaries.push_back(b);
        }
        AngleZoneMapper mapper(boundaries);

        domain::WorldFrame frame;
        // 각 90도 구간의 한가운데(45/135/225/315도)를 대표점으로 -- 경계에 걸리지 않도록
        const double positions[4][2] = {{10, 10}, {-10, 10}, {-10, -10}, {10, -10}};
        const int expectedZone[4] = {0, 1, 2, 3};
        for (int i = 0; i < 4; ++i) {
            domain::WorldObject o;
            o.gid = static_cast<veda::GlobalId>(i + 1);
            o.pos.x = positions[i][0];
            o.pos.y = positions[i][1];
            frame.objects.push_back(o);
        }
        mapper.assign(frame);
        bool allOk = true;
        for (int i = 0; i < 4; ++i) {
            const double deg = std::atan2(positions[i][1], positions[i][0]) * 180.0 / 3.14159265358979323846;
            std::printf("      (%5.1f,%5.1f) angle=%7.2f도 -> zone %d (기대 %d)\n", positions[i][0], positions[i][1],
                        deg, frame.objects[i].zoneId, expectedZone[i]);
            allOk = allOk && frame.objects[i].zoneId == expectedZone[i];
        }
        check(allOk, "4분면이 각각 zone 0~3 으로 배정됨");

        // 어떤 구간에도 안 걸리는 경우
        std::vector<ZoneBoundary> partial{boundaries[0]};  // 0~90도만
        AngleZoneMapper partialMapper(partial);
        domain::WorldFrame f2;
        domain::WorldObject o2;
        o2.pos.x = -10;
        o2.pos.y = -10;  // 225도
        f2.objects.push_back(o2);
        partialMapper.assign(f2);
        check(f2.objects[0].zoneId == -1, "구간 밖 객체는 zoneId=-1 (미배정)");
    }

    // -----------------------------------------------------------------------
    section("6) ThresholdRiskPolicy — 거리 임계값");
    // -----------------------------------------------------------------------
    {
        auto metric = std::make_shared<EuclideanMetric>();
        RiskConfig risk;  // warning=5.0, danger=2.0 기본값
        ThresholdRiskPolicy policy(metric, risk, /*channelCount=*/4);

        struct Case {
            double dist;
            veda::RiskLevel expect;
            const char* name;
        };
        const Case cases[] = {
            {1.5, veda::RiskLevel::Danger, "1.5m (< danger 2.0) -> Danger"},
            {2.0, veda::RiskLevel::Danger, "2.0m (경계, <=) -> Danger"},
            {3.0, veda::RiskLevel::Warning, "3.0m (danger~warning) -> Warning"},
            {5.0, veda::RiskLevel::Warning, "5.0m (경계, <=) -> Warning"},
            {7.0, veda::RiskLevel::None, "7.0m (> warning 5.0) -> None"},
        };
        for (const auto& c : cases) {
            domain::WorldFrame frame;
            domain::WorldObject vehicle;
            vehicle.gid = 1;
            vehicle.cls = veda::ObjectClass::Vehicle;
            vehicle.zoneId = 0;
            frame.objects.push_back(vehicle);

            domain::WorldObject human;
            human.gid = 2;
            human.cls = veda::ObjectClass::Human;
            human.pos.x = c.dist;
            human.zoneId = 0;
            frame.objects.push_back(human);

            const auto eval = policy.evaluate(frame);
            check(frame.objects[0].riskLevel == c.expect, c.name);
            if (c.expect != veda::RiskLevel::None) {
                check(eval.zoneLevels[0].level == c.expect, std::string("  └ zone 0 에도 집계됨"));
                check(frame.objects[1].riskLevel == c.expect, std::string("  └ 최근접 사람에게도 전파됨"));
            }
        }
    }

    // -----------------------------------------------------------------------
    section("7) ThresholdRiskPolicy — 차량 없이 사람만 있을 때 (CT-6)");
    // -----------------------------------------------------------------------
    // Contract.h: `Warning = 사람 감지 or 거리 warningDistance 이내`
    {
        auto metric = std::make_shared<EuclideanMetric>();

        domain::WorldFrame frame;
        domain::WorldObject human;
        human.gid = 1;
        human.cls = veda::ObjectClass::Human;
        human.zoneId = 2;
        frame.objects.push_back(human);

        RiskConfig risk;  // warnOnHumanPresence 기본 true
        ThresholdRiskPolicy policy(metric, risk, 4);
        const auto eval = policy.evaluate(frame);
        check(eval.zoneLevels[2].level == veda::RiskLevel::Warning,
              "차량이 없어도 사람이 있으면 그 zone 은 Warning (계약 정의와 일치)");
        check(eval.zoneLevels[0].level == veda::RiskLevel::None, "  └ 사람이 없는 zone 은 None 유지");

        RiskConfig off = risk;
        off.warnOnHumanPresence = false;
        ThresholdRiskPolicy policyOff(metric, off, 4);
        domain::WorldFrame frame2 = frame;
        check(policyOff.evaluate(frame2).zoneLevels[2].level == veda::RiskLevel::None,
              "warnOnHumanPresence=false 면 예전처럼 None");
    }

    // -----------------------------------------------------------------------
    section("7b) 사람 존재 Warning 이 Danger 를 깎지 않는가 (CT-6)");
    // -----------------------------------------------------------------------
    {
        auto metric = std::make_shared<EuclideanMetric>();
        RiskConfig risk;
        ThresholdRiskPolicy policy(metric, risk, 4);

        domain::WorldFrame frame;
        domain::WorldObject vehicle;
        vehicle.gid = 1;
        vehicle.cls = veda::ObjectClass::Vehicle;
        vehicle.zoneId = 1;
        frame.objects.push_back(vehicle);

        domain::WorldObject human;
        human.gid = 2;
        human.cls = veda::ObjectClass::Human;
        human.pos.x = 1.0;  // danger(2.0m) 이내
        human.zoneId = 1;
        frame.objects.push_back(human);

        const auto eval = policy.evaluate(frame);
        check(eval.zoneLevels[1].level == veda::RiskLevel::Danger,
              "이미 Danger 인 zone 이 사람 존재 Warning 으로 깎이지 않음");
    }

    // -----------------------------------------------------------------------
    section("8) ConcatFuser — 채널 간 중복 병합");
    // -----------------------------------------------------------------------
    {
        auto metric = std::make_shared<EuclideanMetric>();
        ConcatFuser fuser(metric, /*dedupMergeDistance=*/1.0);

        // 같은 차량을 ch0/ch1 이 각각 0.4m 차이로 관측
        std::vector<veda::TopViewFrame> frames{makeFrame(0, 10.0, 20.0), makeFrame(1, 10.3, 20.3)};
        const auto world = fuser.fuse(frames);
        std::printf("      후보 2개 -> 객체 %zu개, 위치(%.2f, %.2f)\n", world.objects.size(),
                    world.objects.empty() ? 0.0 : world.objects[0].pos.x,
                    world.objects.empty() ? 0.0 : world.objects[0].pos.y);
        check(world.objects.size() == 1, "임계 거리 내 동일 클래스는 1개로 병합");
        if (world.objects.size() == 1) {
            check(near(world.objects[0].pos.x, 10.15, 1e-9) && near(world.objects[0].pos.y, 20.15, 1e-9),
                  "  └ 병합 위치는 두 관측의 평균");
            check(world.objects[0].sourceChannels.size() == 2, "  └ sourceChannels 에 두 채널이 기록됨");
        }

        // 임계 거리 밖이면 병합하지 않음
        ConcatFuser fuser2(metric, 1.0);
        std::vector<veda::TopViewFrame> far{makeFrame(0, 10.0, 20.0), makeFrame(1, 13.0, 20.0)};
        check(fuser2.fuse(far).objects.size() == 2, "임계 거리 밖이면 별개 객체로 유지");

        // 클래스가 다르면 병합하지 않음
        ConcatFuser fuser3(metric, 1.0);
        std::vector<veda::TopViewFrame> mixed{makeFrame(0, 10.0, 20.0, veda::ObjectClass::Vehicle),
                                              makeFrame(1, 10.1, 20.1, veda::ObjectClass::Human)};
        check(fuser3.fuse(mixed).objects.size() == 2, "클래스가 다르면 병합하지 않음");

        // 같은 채널 안의 두 객체는 병합 대상이 아님 (한 카메라가 본 서로 다른 실체)
        ConcatFuser fuser4(metric, 1.0);
        veda::TopViewFrame sameCh = makeFrame(0, 10.0, 20.0);
        veda::TopViewObject second;
        second.id = 2;
        second.cls = veda::ObjectClass::Vehicle;
        second.pos = veda::LocalPoint{10.2, 20.2};
        sameCh.objects.push_back(second);
        std::vector<veda::TopViewFrame> one{sameCh};
        check(fuser4.fuse(one).objects.size() == 2, "같은 채널 내 객체끼리는 병합하지 않음");
    }

    // -----------------------------------------------------------------------
    section("9) ConcatFuser — 프레임 간 GlobalId 안정성 (CT-8)");
    // -----------------------------------------------------------------------
    {
        auto metric = std::make_shared<EuclideanMetric>();

        // 추적 켬: 조금씩 움직이는 같은 실체는 gid 를 유지해야 함
        ConcatFuser tracking(metric, /*dedup=*/1.0, /*trackMaxDistance=*/2.0);
        std::vector<veda::TopViewFrame> f1{makeFrame(0, 10.0, 20.0)};
        const veda::GlobalId gid1 = tracking.fuse(f1).objects.at(0).gid;

        std::vector<veda::TopViewFrame> f2{makeFrame(0, 10.5, 20.4)};  // 0.64m 이동
        const veda::GlobalId gid2 = tracking.fuse(f2).objects.at(0).gid;

        std::vector<veda::TopViewFrame> f3{makeFrame(0, 11.0, 20.8)};
        const veda::GlobalId gid3 = tracking.fuse(f3).objects.at(0).gid;
        std::printf("      gid 추이: %lld -> %lld -> %lld\n", static_cast<long long>(gid1),
                    static_cast<long long>(gid2), static_cast<long long>(gid3));
        check(gid1 == gid2 && gid2 == gid3, "조금씩 움직이는 같은 객체는 gid 유지");

        // 순간이동하면 다른 실체로 보고 새 gid
        std::vector<veda::TopViewFrame> jump{makeFrame(0, 40.0, 20.0)};
        check(tracking.fuse(jump).objects.at(0).gid != gid3, "trackMaxDistance 밖으로 뛰면 새 gid");

        // 빈 윈도우가 지나가면 추적을 끊음
        std::vector<veda::TopViewFrame> empty{veda::TopViewFrame{}};
        tracking.fuse(empty);
        std::vector<veda::TopViewFrame> after{makeFrame(0, 40.0, 20.0)};
        check(tracking.fuse(after).objects.at(0).gid != gid3, "빈 윈도우 이후에는 예전 gid 를 물려주지 않음");

        // 추적 끔(기본값): 예전처럼 매 프레임 새 gid
        ConcatFuser noTrack(metric, 1.0);
        std::vector<veda::TopViewFrame> n1{makeFrame(0, 10.0, 20.0)};
        std::vector<veda::TopViewFrame> n2{makeFrame(0, 10.0, 20.0)};
        check(noTrack.fuse(n1).objects.at(0).gid != noTrack.fuse(n2).objects.at(0).gid,
              "trackMaxDistance=0 이면 매 프레임 새 gid (예전 동작)");
    }

    // -----------------------------------------------------------------------
    section("10) ConcatFuser — 같은 채널 중복 제약이 유지되는가 (CT-8 최적화 회귀)");
    // -----------------------------------------------------------------------
    {
        // ch0 에 두 객체, ch1 에 하나. ch1 객체가 두 ch0 객체 모두와 임계 거리 안이지만
        // 한 클러스터에 ch0 가 두 번 들어가면 안 되므로 하나와만 병합되어야 함
        auto metric = std::make_shared<EuclideanMetric>();
        ConcatFuser fuser(metric, 1.0);

        veda::TopViewFrame ch0 = makeFrame(0, 10.0, 20.0);
        veda::TopViewObject second;
        second.id = 2;
        second.cls = veda::ObjectClass::Vehicle;
        second.pos = veda::LocalPoint{10.4, 20.0};
        ch0.objects.push_back(second);

        std::vector<veda::TopViewFrame> frames{ch0, makeFrame(1, 10.2, 20.0)};
        const auto world = fuser.fuse(frames);
        std::printf("      ch0 2개 + ch1 1개 -> 객체 %zu개\n", world.objects.size());
        check(world.objects.size() == 2, "ch0 객체 2개가 하나로 뭉개지지 않음 (채널 비트마스크 제약)");

        std::size_t maxSources = 0;
        for (const auto& o : world.objects)
            maxSources = std::max(maxSources, o.sourceChannels.size());
        check(maxSources == 2, "  └ 병합된 클러스터의 sourceChannels 는 서로 다른 2채널");
    }

    section("결과");
    std::printf("PASS %d / FAIL %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
