pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import "Theme.js" as Theme

// 지도 위의 실시간 객체 하나. C++이 평평한 배열 하나로 넘겨 주는 값을 자리로 읽습니다.
// 배열에 QVariantMap을 담으면 그 목록이 소멸하는 자리에서 heap이 깨지므로 순서로만 주고받습니다
// (CLAUDE.md의 DeviceStatusPanel 항목 참고).
//
//   fields: 0 id · 1 아이콘 · 2 x · 3 y · 4 크기 · 5 회전 · 6 투명도 · 7 이름표색 · 8 경로색
//   trailFields: 0 id · 1.. 경로 좌표
Item {
    id: root

    required property var fields
    required property var trailFields
    property bool showTrail: true

    readonly property string objectId: fields[0]
    readonly property real posX: fields[2]
    readonly property real posY: fields[3]
    readonly property real markerSize: fields[4]

    anchors.fill: parent
    opacity: fields[6]

    // 이동 경로. 점이 둘 미만이면 그릴 것이 없습니다
    readonly property var trailPoints: {
        var points = [];
        if (trailFields[0] !== root.objectId) {
            return points;
        }
        for (var i = 1; i + 1 < trailFields.length; i += 2) {
            points.push(Qt.point(trailFields[i], trailFields[i + 1]));
        }
        return points;
    }

    // dashPattern은 선 굵기의 배수라 [4,4]는 칠한 만큼 그대로 비웁니다. 그러면 실제 궤적 길이는
    // 그대로인데 눈에는 절반만 남아 경로가 짧아 보입니다. 칠한 쪽을 두 배로 두어 예전
    // QPainter의 Qt::DashLine(4:2)과 같은 밀도로 맞춥니다.
    // 여기는 CurveRenderer를 쓰지 않습니다. PathPolyline은 곡선이 하나도 없어 곡선 셰이더로
    // 얻을 것이 없는데, 궤적 좌표는 C++이 매 갱신마다 새로 넘기므로 Shape 문서가 말하는
    // "path element 속성이 바뀌면 해당 path를 매번 다시 삼각분할한다"에 그대로 걸립니다.
    // 파선 + 둥근 캡/조인은 그 재생성이 가장 비싼 조합이라, 객체가 잡히는 순간부터 지도가
    // 초당 20번 이 비용을 물고 그만큼 영상 렌더와 GPU·GUI 스레드를 나눠 쓰게 됩니다.
    Shape {
        anchors.fill: parent
        visible: root.showTrail && root.trailPoints.length > 1
        opacity: 0.7
        asynchronous: true

        ShapePath {
            strokeColor: root.fields[8]
            strokeWidth: 2.4
            strokeStyle: ShapePath.DashLine
            dashPattern: [4, 2]
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            PathPolyline {
                path: root.trailPoints
            }
        }
    }

    // 원본이 500px인데 화면에서는 stage 배율까지 먹어 50px 남짓으로 줄어듭니다. 이만큼
    // 줄이면 `smooth`(선형 보간)는 원본 픽셀을 띄엄띄엄 집어 와서 가장자리가 지글거리고,
    // 아이콘이 **회전까지 하므로** 그 지글거림이 프레임마다 자리를 옮겨 더 눈에 띕니다.
    // mipmap을 켜면 미리 줄여 둔 단계에서 골라 쓰므로 배율이 얼마든 매끄럽게 내려갑니다
    // (설정 아이콘이 같은 이유로 이미 쓰고 있습니다 — TopBarForm.ui.qml).
    Image {
        x: root.posX - width / 2
        y: root.posY - height / 2
        width: root.markerSize
        height: root.markerSize
        source: root.fields[1]
        sourceSize: Qt.size(root.markerSize * 2, root.markerSize * 2)
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        rotation: root.fields[5]
    }

    Text {
        x: root.posX + 14
        y: root.posY - 26
        text: root.objectId
        color: root.fields[7]
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.DemiBold
        style: Text.Outline
        styleColor: Qt.rgba(0, 0, 0, 0.65)
    }
}
