/**
 * @file    verify_pipeline.cpp
 * @brief   control-server 좌표 변환 / zone 배정 / 위험 판정 / 채널 융합 검증 하네스
 *
 * @details
 * AffineLocalToWorldTransform, SpatialZoneMapper, ThresholdRiskPolicy, ConcatFuser 를
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
#include "zone/SpatialZoneMapper.h"

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

/// @brief 변환기 입력용 프레임 (카메라 '로컬' 좌표)
veda::TopViewFrame makeLocalFrame(veda::ChannelId ch, double localX, double localY,
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

/// @brief 융합기 입력용 관측치 프레임 (이미 '월드' 좌표 -- 타입이 달라 로컬과 섞일 수 없음)
domain::ObservationFrame makeFrame(veda::ChannelId ch, double worldX, double worldY,
                                   veda::ObjectClass cls = veda::ObjectClass::Vehicle) {
    domain::ObservationFrame f;
    f.ch = ch;
    f.ts = 1000;
    f.objects.push_back(domain::WorldObservation{1, cls, domain::WorldPoint{worldX, worldY}});
    return f;
}

/// @brief 로컬 좌표 하나를 월드 좌표로 변환한 결과
/// @note  이제 반환 타입 자체가 domain::WorldPoint 라 '로컬인지 월드인지' 헷갈릴 여지가 없음
domain::WorldPoint transformOne(const CameraCalibration& cal, double localX, double localY) {
    AffineLocalToWorldTransform transform({cal});
    const std::vector<veda::TopViewFrame> in{makeLocalFrame(cal.channelId, localX, localY)};
    std::vector<domain::ObservationFrame> out;
    transform.transform(in, out);
    return out.at(0).objects.at(0).pos;
}

}  // namespace

int main() {
    // 크래시가 나도 그 직전까지의 결과가 보이도록 stdout 버퍼링을 끈다.
    // (출력이 파이프/파일로 리다이렉트되면 완전 버퍼링이라, 도중에 죽으면 결과가 통째로 사라짐)
    std::setvbuf(stdout, nullptr, _IONBF, 0);

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
        const std::vector<veda::TopViewFrame> in{makeLocalFrame(3, 7.0, 11.0)};  // ch3 은 캘리브 없음

        AffineLocalToWorldTransform dropping({makeCal(0, 0.0, 0.0, 0.0, 1)});
        std::vector<domain::ObservationFrame> out;
        dropping.transform(in, out);
        check(out.at(0).objects.empty(), "캘리브 없는 채널의 객체는 폐기됨 (기본값)");

        // 명시적으로 끈 경우에만 예전처럼 통과
        AffineLocalToWorldTransform passing({makeCal(0, 0.0, 0.0, 0.0, 1)}, /*dropUncalibrated=*/false);
        std::vector<domain::ObservationFrame> out2;
        passing.transform(in, out2);
        check(!out2.at(0).objects.empty() && near(out2.at(0).objects[0].pos.x, 7.0),
              "dropUncalibrated=false 면 예전처럼 로컬 좌표가 그대로 통과");
    }

    // -----------------------------------------------------------------------
    section("5) SpatialZoneMapper — 공간 경계 상자(AABB) 배정");
    // -----------------------------------------------------------------------
    {
        // 전역 도면 위 서로 떨어진 네 구역 (원점 각도로는 구분 불가능한 배치).
        // 특히 zone0(동쪽 100m)과 zone2(서쪽 100m)는 각기 다른 교차로를 흉내낸다.
        std::vector<SpatialZone> zones{
            {0, 90.0, 110.0, -10.0, 10.0},     // 동쪽 구역 (x≈100)
            {1, -10.0, 10.0, 90.0, 110.0},     // 북쪽 구역 (y≈100)
            {2, -110.0, -90.0, -10.0, 10.0},   // 서쪽 구역 (x≈-100)
            {3, -10.0, 10.0, -110.0, -90.0},   // 남쪽 구역 (y≈-100)
        };
        SpatialZoneMapper mapper(zones);

        struct ZCase {
            double x, y;
            int expect;
            const char* name;
        };
        const ZCase zcases[] = {
            {100.0, 0.0, 0, "동쪽 상자 안 -> zone 0"},
            {0.0, 100.0, 1, "북쪽 상자 안 -> zone 1"},
            {-100.0, 0.0, 2, "서쪽 상자 안 -> zone 2 (angle 방식이면 zone0 과 못 나눔)"},
            {0.0, -100.0, 3, "남쪽 상자 안 -> zone 3"},
            {90.0, -10.0, 0, "경계(모서리) 포함 -> zone 0"},
            {0.0, 0.0, -1, "어느 상자에도 안 듦(원점) -> -1"},
            {500.0, 500.0, -1, "도면 밖 -> -1"},
        };
        for (const auto& c : zcases) {
            domain::WorldFrame frame;
            domain::WorldObject o;
            o.pos.x = c.x;
            o.pos.y = c.y;
            frame.objects.push_back(o);
            mapper.assign(frame);
            std::printf("      (%7.1f,%7.1f) -> zone %d (기대 %d)\n", c.x, c.y, frame.objects[0].zoneId, c.expect);
            check(frame.objects[0].zoneId == c.expect, c.name);
        }

        // first-match wins: 겹치는 상자는 선언 순서상 먼저가 이김
        std::vector<SpatialZone> overlap{
            {5, 0.0, 20.0, 0.0, 20.0},   // 먼저 선언
            {6, 10.0, 30.0, 10.0, 30.0}, // (15,15) 에서 겹침
        };
        SpatialZoneMapper overlapMapper(overlap);
        domain::WorldFrame f2;
        domain::WorldObject o2;
        o2.pos.x = 15.0;
        o2.pos.y = 15.0;
        f2.objects.push_back(o2);
        overlapMapper.assign(f2);
        check(f2.objects[0].zoneId == 5, "겹치는 구역은 first-match(먼저 선언한 zone 5)");

        // 빈 zones -> 전부 미배정
        SpatialZoneMapper emptyMapper(std::vector<SpatialZone>{});
        domain::WorldFrame f3;
        domain::WorldObject o3;
        o3.pos.x = 100.0;
        o3.pos.y = 0.0;
        f3.objects.push_back(o3);
        emptyMapper.assign(f3);
        check(f3.objects[0].zoneId == -1, "zones 가 비면 모든 객체 zoneId=-1 (안전한 실패)");
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
    section("7) ThresholdRiskPolicy — 차량 없으면 무조건 None (Rule 1)");
    // -----------------------------------------------------------------------
    // 위험은 오직 차량 기준으로만 판정한다. 사람만 있으면 아무리 붙어 있어도 None.
    {
        auto metric = std::make_shared<EuclideanMetric>();

        domain::WorldFrame frame;
        domain::WorldObject human;
        human.gid = 1;
        human.cls = veda::ObjectClass::Human;
        human.zoneId = 2;
        frame.objects.push_back(human);

        domain::WorldObject human2;
        human2.gid = 2;
        human2.cls = veda::ObjectClass::Human;
        human2.pos.x = 0.3;  // 사람끼리 danger 거리 안이라도 무시
        human2.zoneId = 2;
        frame.objects.push_back(human2);

        RiskConfig risk;
        ThresholdRiskPolicy policy(metric, risk, 4);
        const auto eval = policy.evaluate(frame);
        check(eval.zoneLevels[2].level == veda::RiskLevel::None, "차량이 없으면 사람만 있어도 zone 은 None (Rule 1)");
        check(frame.level == veda::RiskLevel::None, "  └ 프레임 전체 위험도도 None");
        check(frame.objects[0].riskLevel == veda::RiskLevel::None && frame.objects[1].riskLevel == veda::RiskLevel::None,
              "  └ 사람 객체 자체도 None 유지 (사람-사람 거리 계산 안 함)");
    }

    // -----------------------------------------------------------------------
    section("7b) Rule 4/5 — 프레임 위험도 = 채널별(차량) 위험도의 max (UI==HW 단일 소스)");
    // -----------------------------------------------------------------------
    {
        auto metric = std::make_shared<EuclideanMetric>();
        RiskConfig risk;
        ThresholdRiskPolicy policy(metric, risk, 4);

        domain::WorldFrame frame;
        // zone1: 차량-사람 1.0m -> Danger
        domain::WorldObject v1;
        v1.gid = 1;
        v1.cls = veda::ObjectClass::Vehicle;
        v1.zoneId = 1;
        frame.objects.push_back(v1);
        domain::WorldObject h1;
        h1.gid = 2;
        h1.cls = veda::ObjectClass::Human;
        h1.pos.x = 1.0;
        h1.zoneId = 1;
        frame.objects.push_back(h1);
        // zone3: 멀리 떨어진 곳의 차량-사람 4.0m -> Warning
        domain::WorldObject v2;
        v2.gid = 3;
        v2.cls = veda::ObjectClass::Vehicle;
        v2.pos.x = 100.0;
        v2.zoneId = 3;
        frame.objects.push_back(v2);
        domain::WorldObject h2;
        h2.gid = 4;
        h2.cls = veda::ObjectClass::Human;
        h2.pos.x = 104.0;
        h2.zoneId = 3;
        frame.objects.push_back(h2);

        const auto eval = policy.evaluate(frame);
        check(eval.zoneLevels[1].level == veda::RiskLevel::Danger, "zone1(차량-사람 1.0m) = Danger");
        check(eval.zoneLevels[3].level == veda::RiskLevel::Warning, "zone3(차량-사람 4.0m) = Warning");

        veda::RiskLevel maxZone = veda::RiskLevel::None;
        for (const auto& z : eval.zoneLevels) {
            if (z.level > maxZone)
                maxZone = z.level;
        }
        check(frame.level == maxZone, "프레임 위험도 = 채널별 max (UI RiskFrame.level 이 읽는 단일 소스)");
        check(frame.level == veda::RiskLevel::Danger, "  └ = Danger");

        // Rule 3: 최근접 사람에게도 레벨 전파 (표시용)
        check(frame.objects[1].riskLevel == veda::RiskLevel::Danger, "  └ zone1 최근접 사람에게 Danger 전파 (Rule 3)");
    }

    // -----------------------------------------------------------------------
    section("8) ConcatFuser — 채널 간 중복 병합");
    // -----------------------------------------------------------------------
    {
        auto metric = std::make_shared<EuclideanMetric>();
        ConcatFuser fuser(metric, /*dedupMergeDistance=*/1.0);

        // 같은 차량을 ch0/ch1 이 각각 0.4m 차이로 관측
        std::vector<domain::ObservationFrame> frames{makeFrame(0, 10.0, 20.0), makeFrame(1, 10.3, 20.3)};
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
        std::vector<domain::ObservationFrame> far{makeFrame(0, 10.0, 20.0), makeFrame(1, 13.0, 20.0)};
        check(fuser2.fuse(far).objects.size() == 2, "임계 거리 밖이면 별개 객체로 유지");

        // 클래스가 다르면 병합하지 않음
        ConcatFuser fuser3(metric, 1.0);
        std::vector<domain::ObservationFrame> mixed{makeFrame(0, 10.0, 20.0, veda::ObjectClass::Vehicle),
                                              makeFrame(1, 10.1, 20.1, veda::ObjectClass::Human)};
        check(fuser3.fuse(mixed).objects.size() == 2, "클래스가 다르면 병합하지 않음");

        // 같은 채널 안의 두 객체는 병합 대상이 아님 (한 카메라가 본 서로 다른 실체)
        ConcatFuser fuser4(metric, 1.0);
        domain::ObservationFrame sameCh = makeFrame(0, 10.0, 20.0);
        sameCh.objects.push_back(
            domain::WorldObservation{2, veda::ObjectClass::Vehicle, domain::WorldPoint{10.2, 20.2}});
        std::vector<domain::ObservationFrame> one{sameCh};
        check(fuser4.fuse(one).objects.size() == 2, "같은 채널 내 객체끼리는 병합하지 않음");
    }

    // -----------------------------------------------------------------------
    section("9) ConcatFuser — 프레임 간 GlobalId 안정성 (CT-8)");
    // -----------------------------------------------------------------------
    {
        auto metric = std::make_shared<EuclideanMetric>();

        // 추적 켬: 조금씩 움직이는 같은 실체는 gid 를 유지해야 함
        ConcatFuser tracking(metric, /*dedup=*/1.0, /*trackMaxDistance=*/2.0);
        std::vector<domain::ObservationFrame> f1{makeFrame(0, 10.0, 20.0)};
        const veda::GlobalId gid1 = tracking.fuse(f1).objects.at(0).gid;

        std::vector<domain::ObservationFrame> f2{makeFrame(0, 10.5, 20.4)};  // 0.64m 이동
        const veda::GlobalId gid2 = tracking.fuse(f2).objects.at(0).gid;

        std::vector<domain::ObservationFrame> f3{makeFrame(0, 11.0, 20.8)};
        const veda::GlobalId gid3 = tracking.fuse(f3).objects.at(0).gid;
        std::printf("      gid 추이: %lld -> %lld -> %lld\n", static_cast<long long>(gid1),
                    static_cast<long long>(gid2), static_cast<long long>(gid3));
        check(gid1 == gid2 && gid2 == gid3, "조금씩 움직이는 같은 객체는 gid 유지");

        // 순간이동하면 다른 실체로 보고 새 gid
        std::vector<domain::ObservationFrame> jump{makeFrame(0, 40.0, 20.0)};
        check(tracking.fuse(jump).objects.at(0).gid != gid3, "trackMaxDistance 밖으로 뛰면 새 gid");

        // 빈 윈도우 1개 정도는 유예 안에 들어가므로 추적이 끊기지 않음
        std::vector<domain::ObservationFrame> empty{domain::ObservationFrame{}};
        tracking.fuse(empty);
        std::vector<domain::ObservationFrame> after{makeFrame(0, 40.0, 20.0)};
        check(tracking.fuse(after).objects.at(0).gid != gid3,
              "빈 윈도우 이후에도 (jump 로 이미 gid3 와는 다른 실체이므로) gid3 는 아님");

        // 감지 유예(kMaxMissedWindows=5): 순간적으로 몇 윈도우 놓쳐도 추적 유지
        ConcatFuser grace(metric, /*dedup=*/1.0, /*trackMaxDistance=*/2.0);
        std::vector<domain::ObservationFrame> g1{makeFrame(0, 10.0, 20.0)};
        const veda::GlobalId gGid = grace.fuse(g1).objects.at(0).gid;

        std::vector<domain::ObservationFrame> gEmpty{domain::ObservationFrame{}};
        for (int i = 0; i < 5; ++i)
            grace.fuse(gEmpty);  // 유예 범위 안에서 5윈도우 연속으로 놓침

        std::vector<domain::ObservationFrame> gBack{makeFrame(0, 10.0, 20.0)};
        check(grace.fuse(gBack).objects.at(0).gid == gGid, "5윈도우 안에 다시 잡히면 gid 유지 (유예)");

        for (int i = 0; i < 6; ++i)
            grace.fuse(gEmpty);  // 유예(5)를 넘겨 6윈도우 연속으로 놓침
        check(grace.fuse(gBack).objects.at(0).gid != gGid, "유예를 넘겨 오래 비면 추적을 끊고 새 gid");

        // 추적 끔(기본값): 예전처럼 매 프레임 새 gid
        ConcatFuser noTrack(metric, 1.0);
        std::vector<domain::ObservationFrame> n1{makeFrame(0, 10.0, 20.0)};
        std::vector<domain::ObservationFrame> n2{makeFrame(0, 10.0, 20.0)};
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

        domain::ObservationFrame ch0 = makeFrame(0, 10.0, 20.0);
        ch0.objects.push_back(
            domain::WorldObservation{2, veda::ObjectClass::Vehicle, domain::WorldPoint{10.4, 20.0}});

        std::vector<domain::ObservationFrame> frames{ch0, makeFrame(1, 10.2, 20.0)};
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
