.pragma library

// ---------------------------------------------------------------------------
// 아파트 지하주차장 평면도 한 층의 기하를 만듭니다.
//
// 이 파일이 도면의 **유일한 원본**입니다. C++은 여기서 나온 구역 사각형을 읽어 객체 좌표를
// 옮기므로, 치수를 여기서 바꾸면 지도와 객체가 함께 따라옵니다. 양쪽에 같은 상수를 두면
// 언젠가 한쪽만 고쳐져 객체가 도면 밖에 찍힙니다.
//
// 좌표 단위는 1m = 14. 실척(주차장법 시행규칙)에서 그대로 나온 값이라 도면 비율이 실제와 같습니다.
//  - 주차단위구획 2.5m x 5.0m, 직각주차 차로 6.0m 이상
//
// 세로 배치: 주차열 - 차로 - 주차열 - 주차열 - 차로 - 주차열
//            위·아래 줄은 외벽에 붙고 가운데 두 줄은 등을 맞댑니다.
// 가로 배치: 좌우 6.5m 띠가 설비실이고 그 사이가 홀입니다. 중앙 세로 차로가 가운데 블록만
//            좌우로 가르고, 위·아래 줄은 끊기지 않습니다.
// 동선:      아래 차로가 그대로 진출입로입니다. 왼쪽 설비실 띠를 비워 1층으로 나가고,
//            오른쪽 아래 모서리의 곡선 램프로 B2F로 내려갑니다. 설비실을 띠에 빈틈없이
//            채우면 이 길이 막히므로 방 사이를 반드시 비워 둡니다.
// ---------------------------------------------------------------------------

var METRE = 14.0;

var STALL_W = 2.5 * METRE;
var STALL_D = 5.0 * METRE;
var AISLE = 6.0 * METRE;
var BAND = 6.5 * METRE;
var SIDE_LANE = 3.0 * METRE;

var STALL_ROWS = 4;
var AISLE_ROWS = 2;

var HALL_H = STALL_ROWS * STALL_D + AISLE_ROWS * AISLE;
/// 홀 가로세로비는 커버리지 격자가 정합니다. 4열 x 2행이므로 2:1이어야 셀이 정사각형입니다
var HALL_W = HALL_H * 2.0;

/// 도면 바깥 여백. 위험 테두리 광채가 나갈 자리만 둡니다
var EDGE = 6.0;
var X0 = EDGE;
var Y0 = EDGE;
var HALL_X = X0 + BAND;
var HALL_R = HALL_X + HALL_W;
var X1 = HALL_R + BAND;
var Y1 = Y0 + HALL_H;
var PLAN_W = X1 + EDGE;
var PLAN_H = Y1 + EDGE;
var BUILDING_W = X1 - X0;
var BUILDING_H = Y1 - Y0;

/// 커버리지 격자. 홀이 2:1이라 4열 x 2행이 정확히 정사각형 여덟 칸으로 떨어집니다.
/// 객체는 구역 안의 정사각 영역에만 그려지므로 셀이 정사각형이 아니면 가로·세로 배율이 어긋납니다.
/// 열 수를 바꾸면 HALL_W의 배수도 함께 바꿔야 합니다 (ZONE_COLS / ZONE_ROWS 비율).
var ZONE_COLS = 4;
var ZONE_ROWS = 2;
var ZONE_MAX = ZONE_COLS * ZONE_ROWS;
var ZONE_INSET = 3.0;
/// 객체 영역과 구역 테두리 사이 여백. 아이콘이 테두리를 넘지 않게 둡니다
var OBJECT_MARGIN = 18.0;

var CHANNELS_PER_ZONE = 4;

function rect(x, y, w, h) {
    return { x: x, y: y, w: w, h: h };
}

/** 커버리지 격자에서 물리 CCTV 한 대가 담당하는 정사각형입니다. 홀만 나눕니다. */
function zoneRect(index) {
    var cw = HALL_W / ZONE_COLS;
    var ch = HALL_H / ZONE_ROWS;
    return rect(HALL_X + (index % ZONE_COLS) * cw + ZONE_INSET, Y0 + Math.floor(index / ZONE_COLS) * ch + ZONE_INSET,
                cw - ZONE_INSET * 2, ch - ZONE_INSET * 2);
}

/** 구역 안에서 객체가 그려지는 정사각 영역입니다. C++이 월드 좌표를 여기에 늘려 맞춥니다. */
function objectArea(index) {
    var cell = zoneRect(index);
    var side = Math.min(cell.w, cell.h) - OBJECT_MARGIN * 2;
    return rect(cell.x + cell.w / 2 - side / 2, cell.y + cell.h / 2 - side / 2, side, side);
}

/** 주차 구획 한 줄. stopAtTop은 바퀴턱이 붙는 쪽, 곧 차로 반대쪽입니다. */
function band(x, y, w, stopAtTop) {
    return { x: x, y: y, w: w, h: STALL_D, stopAtTop: stopAtTop };
}

/**
 * 문 기호 하나.
 *
 * hinge에서 leaf 방향으로 문짝을 그리고, 거기서 sweep만큼 호를 돌려 닫힌 자리까지 잇습니다.
 * gap은 벽에서 이 문이 뚫고 지나가는 자리라, 벽 바를 그 만큼 지워야 개구부로 보입니다.
 */
function door(hx, hy, radius, leafDeg, sweepDeg) {
    var a0 = leafDeg * Math.PI / 180;
    var a1 = (leafDeg + sweepDeg) * Math.PI / 180;
    var endX = hx + radius * Math.cos(a1);
    var endY = hy + radius * Math.sin(a1);

    // 개구부는 hinge에서 닫힌 자리까지, 곧 **벽을 따라 놓인 구간**입니다. 호가 지나는 사각형
    // 전체를 지우면 벽이 아니라 방 모서리가 뭉텅 떨어져 나간 것처럼 보이므로, 벽 두께만큼만
    // 가로지르는 얇은 띠로 잡습니다.
    var alongX = Math.abs(endX - hx);
    var alongY = Math.abs(endY - hy);
    var vertical = alongY >= alongX;
    // 벽을 가로지르는 폭. 밝은 심(3.4)과 어두운 테(+1.8), 오른쪽 아래로 밀린 그림자(2.2)까지
    // 덮어야 개구부가 깨끗하게 뚫린다. 중심에서 대칭으로 잡으면 그림자가 남는다
    var before = 3.0;
    var after = 5.5;

    return {
        hx: hx,
        hy: hy,
        r: radius,
        leafX: hx + radius * Math.cos(a0),
        leafY: hy + radius * Math.sin(a0),
        endX: endX,
        endY: endY,
        clockwise: sweepDeg > 0,
        gapX: vertical ? hx - before : Math.min(hx, endX),
        gapY: vertical ? Math.min(hy, endY) : hy - before,
        gapW: vertical ? before + after : alongX,
        gapH: vertical ? alongY : before + after
    };
}

function boxOutline(x, y, w, h) {
    return [x, y, x + w, y, x + w, y + h, x, y + h];
}

/**
 * 방 안에 놓이는 장비 하나.
 *
 * 설비실을 빈 상자로 두면 이름표만 붙은 라벨이 됩니다. 실제 도면은 방마다 그 안에 들어가는
 * 장비를 자리와 개수까지 그리고, 그 기호가 있어야 무슨 방인지 이름표 없이도 읽힙니다.
 * 도면 배율이 작아 기호는 굵게만 잡습니다 — 칸 나눔·대각선·원통, 이 셋으로 전부 표현합니다.
 */
function fixture(x, y, w, h, opts) {
    var o = opts || {};
    return {
        x: x,
        y: y,
        w: w,
        h: h,
        round: o.round === true,        // 원통형(펌프·변압기)
        divisions: o.divisions || 0,    // 긴 축을 나누는 칸 수. 같은 장비가 줄지어 선 한 벌
        cross: o.cross === true,        // 대각 X. 도면에서 승강로 표기
        solid: o.solid === true         // 속을 진하게 채운 장비
    };
}

/**
 * 방 종류별 장비 배치. a는 벽 안쪽에서 이름표 자리를 뺀 나머지 바닥입니다.
 *
 * 치수는 전부 a에 대한 비율이나 상한을 씌운 값이라, 방 높이를 바꿔도 장비가 방 밖으로
 * 튀어나가지 않습니다. 장비 사이는 점검 통로로 비워 둡니다 — 벽에서 벽까지 꽉 채우면
 * 방이 아니라 채워진 덩어리로 보입니다.
 */
function equipment(kind, a) {
    var out = [];
    if (kind === "switchgear") {
        // 배전반 한 벌이 벽을 등지고 늘어서고, 맞은편 벽에 분전반이 붙는다
        out.push(fixture(a.x, a.y, a.w, Math.min(12, a.h * 0.30), { divisions: 5 }));
        out.push(fixture(a.x, a.y + a.h - 9, a.w * 0.45, 9, { divisions: 2 }));
    } else if (kind === "machine") {
        // 급배수 펌프 두 대와 공조기. 방이 길면 아래에 물탱크가 더 들어간다
        var d = Math.min(15, a.h * 0.32);
        out.push(fixture(a.x, a.y, d, d, { round: true }));
        out.push(fixture(a.x, a.y + d + 5, d, d, { round: true }));
        out.push(fixture(a.x + d + 8, a.y, a.w - d - 8, Math.min(26, a.h * 0.55), { divisions: 3 }));
        if (a.h > 80) {
            out.push(fixture(a.x, a.y + a.h - 36, a.w, 36, { divisions: 2 }));
        }
    } else if (kind === "substation") {
        // 변압기 두 대와 수배전반 한 벌. 변압기는 원통이 아니라 상자로 둔다 — 이 배율에서
        // 작은 원을 진하게 채우면 기호가 아니라 바닥에 뚫린 구멍으로 읽힌다
        var t = Math.min(20, a.w * 0.28);
        out.push(fixture(a.x + 2, a.y + 4, t, t, { divisions: 2 }));
        out.push(fixture(a.x + t + 10, a.y + 4, t, t, { divisions: 2 }));
        out.push(fixture(a.x, a.y + a.h - 12, a.w, 12, { divisions: 4 }));
    } else if (kind === "office") {
        // 책상과 의자. 자리가 남는 방에만 벽면 캐비닛까지 붙인다
        out.push(fixture(a.x, a.y + a.h - 11, a.w * 0.58, 11));
        out.push(fixture(a.x + a.w * 0.58 + 4, a.y + a.h - 9.5, 8, 8, { round: true }));
        if (a.h > 22) {
            out.push(fixture(a.x, a.y, a.w * 0.55, 8, { divisions: 2 }));
        }
    } else if (kind === "tank") {
        out.push(fixture(a.x, a.y + 2, a.w, a.h - 4, { divisions: 2 }));
    }
    return out;
}

/** 설비실 한 칸. ratio는 외벽 안쪽 높이에 대한 비율이라 도면 높이를 바꿔도 배치가 유지됩니다. */
function room(side, topRatio, heightRatio, name, opts) {
    var options = opts || {};
    var indent = options.indent || 0;
    var x = side === "left" ? X0 + indent : HALL_R;
    var w = BAND - indent;
    var y = Y0 + BUILDING_H * topRatio;
    var h = BUILDING_H * heightRatio;

    var doors = [];
    // 문은 홀 쪽 벽에 낸다. 왼쪽 띠는 오른쪽 벽, 오른쪽 띠는 왼쪽 벽이 홀에 면한다.
    // 폭은 벽 높이의 3분의 1쯤이다 — 크게 잡으면 개구부가 벽을 반쯤 먹어 방 모서리가
    // 뭉텅 떨어져 나간 것처럼 보인다
    var doorRadius = Math.min(22, h * 0.34);
    if (options.doorAtLeft) {
        doors.push(door(x, options.doorAtBottom ? y + h - 3 : y + 3, doorRadius, 180,
                        options.doorAtBottom ? 90 : -90));
    } else if (side === "left") {
        doors.push(door(x + w, options.doorAtBottom ? y + h - 3 : y + 3, doorRadius, 0,
                        options.doorAtBottom ? -90 : 90));
    } else {
        doors.push(door(x, options.doorAtBottom ? y + h - 3 : y + 3, doorRadius, 180,
                        options.doorAtBottom ? 90 : -90));
    }

    // 계단실은 방을 통째로 계단이 채우지 않는다. 문 쪽 3할은 계단참으로 비워 두고 그 사이를
    // 벽으로 가른다 — 실제 도면이 그렇고, 그래야 문을 열고 들어서는 자리가 읽힌다
    var stairBox = null;
    var landingWall = null;
    if (options.stair === true) {
        var splitX = x + w * 0.70;
        stairBox = rect(x + 9, y + 22, splitX - (x + 9) - 4, h - 30);
        landingWall = { x1: splitX, y1: y + h * 0.20, x2: splitX, y2: y + h };
    }

    // 장비는 이름표 자리를 뺀 나머지 바닥에 놓는다. 장비가 있는 방은 이름표를 방 가운데가
    // 아니라 위로 올린다 — 가운데 두면 이름이 장비 위에 겹쳐 둘 다 읽히지 않는다
    var LABEL_BAND = 21.0;
    var fixtures = equipment(options.kind || "", rect(x + 7, y + LABEL_BAND, w - 14, h - LABEL_BAND - 7));

    return {
        x: x,
        y: y,
        w: w,
        h: h,
        name: name || "",
        outline: boxOutline(x, y, w, h),
        doors: doors,
        fixtures: fixtures,
        labelAtTop: options.stair === true || fixtures.length > 0,
        stair: options.stair === true,
        stairBox: stairBox,
        landingWall: landingWall
    };
}

/**
 * 승강기 코어. 한쪽 위 모서리가 꺾인 실제 도면 형태를 그대로 씁니다.
 *
 * 꺾인 자리와 승강기 칸은 둘 다 **중앙 차로에서 먼 쪽**입니다. 도면에서 두 코어가 서로 등을
 * 돌린 것처럼 보이는 것이 이 비대칭 때문이고, 좌우를 똑같이 그리면 복사해 붙인 티가 납니다.
 */
function core(x, y, w, h, notchOnLeft, cabinCount) {
    var nw = w * 0.18;
    var nh = h * 0.14;
    var outline = notchOnLeft
                      ? [x + nw, y, x + w, y, x + w, y + h, x, y + h, x, y + nh, x + nw, y + nh]
                      : [x, y, x + w - nw, y, x + w - nw, y + nh, x + w, y + nh, x + w, y + h, x, y + h];

    // 승강로는 도면에서 대각 X로 표기한다. 빈 상자로 두면 그냥 작은 방이 하나 더 있는 것처럼
    // 보이고, 이 X 하나가 "여기는 사람이 서는 바닥이 아니라 위아래로 뚫린 구멍"을 말한다
    var shaftW = 16.0;
    var shaftH = 13.0;
    var shaftGap = 3.5;
    var span = cabinCount * shaftW + (cabinCount - 1) * shaftGap;
    var startX = notchOnLeft ? x + 8 : x + w - 8 - span;
    var fixtures = [];
    for (var i = 0; i < cabinCount; ++i) {
        fixtures.push(fixture(startX + i * (shaftW + shaftGap), y + h - 7 - shaftH, shaftW, shaftH,
                              { cross: true }));
    }

    return {
        x: x,
        y: y,
        w: w,
        h: h,
        outline: outline,
        doors: [],
        fixtures: fixtures,
        labelAtTop: false,
        name: "엘리베이터실"
    };
}

var cached = null;

/** 도면 전체를 만듭니다. 값이 고정이라 한 번만 계산하고 이후에는 같은 객체를 돌려줍니다. */
function plan() {
    if (cached !== null) {
        return cached;
    }

    var rowY = [];
    var y = Y0;
    for (var r = 0; r < STALL_ROWS; ++r) {
        rowY.push(y);
        y += STALL_D;
        // 가운데 두 줄은 등을 맞대므로 그 사이에는 차로를 두지 않는다
        if (r === 0 || r === 2) {
            y += AISLE;
        }
    }
    var exitAisleY = rowY[2] + STALL_D;
    var centreX = (HALL_X + HALL_R) / 2;
    var centreRoad = rect(centreX - AISLE / 2, rowY[1], AISLE, rowY[2] + STALL_D - rowY[1]);

    var bands = [];
    bands.push(band(HALL_X, rowY[0], HALL_W, true));
    for (var m = 1; m <= 2; ++m) {
        var left = HALL_X + SIDE_LANE;
        var right = HALL_R - SIDE_LANE;
        var stopAtTop = m === 2;
        bands.push(band(left, rowY[m], centreRoad.x - left, stopAtTop));
        bands.push(band(centreRoad.x + centreRoad.w, rowY[m], right - (centreRoad.x + centreRoad.w), stopAtTop));
    }
    bands.push(band(HALL_X, rowY[3], HALL_W, false));

    // 승강기 코어 둘이 아래 블록 줄(band 3, 4)의 중앙 차로 쪽 끝을 차지한다.
    // 폭은 남는 자리가 주차면으로 딱 떨어지도록 잡았다 — 왼쪽은 5면, 오른쪽은 4면이 남는다.
    // 실제 도면도 두 코어의 크기가 다르므로 굳이 맞추지 않는다
    var coreLeftW = 2.2 * STALL_W;
    var coreRightW = 3.2 * STALL_W;
    var cores = [core(bands[3].x + bands[3].w - coreLeftW, bands[3].y, coreLeftW, bands[3].h, true, 2),
                 core(bands[4].x, bands[4].y, coreRightW, bands[4].h, false, 3)];
    bands[3].w -= coreLeftW;
    bands[4].x += coreRightW;
    bands[4].w -= coreRightW;

    // 구획과 기둥. 기둥은 주차열 경계선 위 7.5m 격자에 선다
    var stalls = [];
    var columns = [];
    for (var b = 0; b < bands.length; ++b) {
        var bd = bands[b];
        var count = Math.floor(bd.w / STALL_W);
        var first = bd.x + (bd.w - count * STALL_W) / 2;
        for (var s = 0; s < count; ++s) {
            stalls.push({ x: first + s * STALL_W, y: bd.y, w: STALL_W, h: bd.h, stopAtTop: bd.stopAtTop });
        }
        for (var c = 0; c <= count; c += 3) {
            var cx = first + Math.min(c, count) * STALL_W;
            columns.push({ x: cx, y: bd.y });
            columns.push({ x: cx, y: bd.y + bd.h });
        }
    }

    // 설비실. 아래 차로가 지나는 높이(0.65~0.85)는 통째로 비워 1층으로 나가는 길을 남긴다
    var rooms = [room("left", 0.010, 0.150, "전기실", { kind: "switchgear", doorAtBottom: true }),
                 room("left", 0.190, 0.160, "계단실 (UP)", { stair: true }),
                 room("left", 0.380, 0.140, "기계실", { kind: "machine" }),
                 room("left", 0.550, 0.100, "관리실",
                      { kind: "office", indent: BAND * 0.30, doorAtLeft: true, doorAtBottom: true }),
                 room("left", 0.855, 0.135, "물탱크실", { kind: "tank" }),
                 room("right", 0.050, 0.340, "기계실", { kind: "machine" }),
                 room("right", 0.440, 0.190, "변전실", { kind: "substation" })];

    // 홀에 면한 복도 벽. 방과 방 사이를 이어 주어야 설비실 띠가 한 덩어리로 읽힌다
    var walls = [];
    function addBandWall(side, indices) {
        var x = side === "left" ? HALL_X : HALL_R;
        var prev = Y0;
        for (var i = 0; i < indices.length; ++i) {
            var rm = rooms[indices[i]];
            walls.push({ x1: x, y1: prev, x2: x, y2: rm.y });
            prev = rm.y + rm.h;
        }
        walls.push({ x1: x, y1: prev, x2: x, y2: Y1 });
    }
    addBandWall("left", [0, 1, 2, 3, 4]);
    addBandWall("right", [5, 6]);

    // 램프. 아래 차로에서 오른쪽 아래 모서리로 꺾여 B2F로 내려간다.
    // 호는 벽·구획과 같은 그리기(PlanWalls)를 타야 두께와 그림자가 도면 나머지와 맞으므로,
    // 여기서 미리 폴리라인으로 펴 둔다. 4분원을 14토막으로 끊으면 눈으로는 호와 구분되지 않는다
    var laneY = exitAisleY + AISLE / 2;
    var rampR = 45;
    var rampHalf = 30;
    var rampCx = HALL_R;
    var rampCy = laneY + rampR;

    // 4분원: 위(진입, 각도 90°)에서 오른쪽(각도 0°)까지 시계 방향으로 돈 뒤 아래 외벽까지 내린다
    function rampPolyline(radius, stopY) {
        var points = [];
        for (var i = 0; i <= 14; ++i) {
            var angle = Math.PI / 2 * (1 - i / 14);
            points.push(rampCx + radius * Math.cos(angle));
            points.push(rampCy - radius * Math.sin(angle));
        }
        points.push(rampCx + radius);
        points.push(stopY);
        return points;
    }

    var ramp = {
        cx: rampCx,
        cy: rampCy,
        r: rampR,
        half: rampHalf,
        exitX: rampCx + rampR,
        bottom: Y1,
        outerWall: rampPolyline(rampR + rampHalf, Y1),
        innerWall: rampPolyline(rampR - rampHalf, Y1),
        centreLine: rampPolyline(rampR, Y1 - 46)
    };

    var arrows = [{ tipX: X0 + 14, tipY: laneY, dx: -1, dy: 0, len: 78 },
                  { tipX: HALL_X + HALL_W * 0.72, tipY: laneY, dx: 1, dy: 0, len: 78 },
                  { tipX: ramp.exitX, tipY: Y1 - 12, dx: 0, dy: 1, len: 46 }];

    // 구역 사각형은 서로 겹쳐 보이지 않도록 여백을 두고 그리지만, 비활성 구역을 덮는 가리개는
    // 여백 없는 원래 칸을 써야 옆 칸과 정확히 맞물립니다. 여백째로 덮으면 칸과 칸 사이에
    // 가려지지 않은 밝은 띠가 남아 도면을 세로로 그어 놓은 것처럼 보입니다.
    var zones = [];
    for (var z = 0; z < ZONE_MAX; ++z) {
        var cell = zoneRect(z);
        zones.push({
            x: cell.x,
            y: cell.y,
            w: cell.w,
            h: cell.h,
            cx: cell.x + cell.w / 2,
            cy: cell.y + cell.h / 2,
            maskX: cell.x - ZONE_INSET,
            maskY: cell.y - ZONE_INSET,
            maskW: cell.w + ZONE_INSET * 2,
            maskH: cell.h + ZONE_INSET * 2
        });
    }

    cached = {
        width: PLAN_W,
        height: PLAN_H,
        building: rect(X0, Y0, BUILDING_W, BUILDING_H),
        hall: rect(HALL_X, Y0, HALL_W, HALL_H),
        stalls: stalls,
        columns: columns,
        rooms: rooms,
        cores: cores,
        walls: walls,
        ramp: ramp,
        arrows: arrows,
        zones: zones,
        laneY: laneY,
        exitLabelX: HALL_X + 36,
        rampLabelX: HALL_R - 34
    };
    return cached;
}

/** C++이 객체 좌표를 옮길 때 쓰는 구역별 정사각 영역입니다. [x,y,w,h] 를 구역 수만큼 이어 붙입니다. */
function objectAreaList(zoneCount) {
    var out = [];
    for (var i = 0; i < zoneCount && i < ZONE_MAX; ++i) {
        var a = objectArea(i);
        out.push(a.x, a.y, a.w, a.h);
    }
    return out;
}
