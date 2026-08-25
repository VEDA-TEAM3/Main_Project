#!/usr/bin/env python3
"""CCTV 지면 호모그래피 캘리브레이션 도구 (현장용).

스냅샷에서 바닥 기준점 4개를 클릭하고 실측 미터값을 입력하면, compute-server 의
config.json 에 그대로 붙여넣을 수 있는 블록을 출력한다.

이 도구는 compute-server 의 HomographyTransform 생성자가 수행하는 검증을 **그대로
재현**한다. 여기서 통과하면 서버도 통과하고, 여기서 걸리면 서버도 예외로 죽는다.
현장에서 삼각대를 세워둔 채로 알아내는 것이 목적이다 (돌아가서 재방문하지 않도록).

사용법:
    python calibrate.py snapshot.png --channel 0
    python calibrate.py snapshot.png --channel 0 --k 4.0 --extent 10.0
"""

import argparse
import json
import sys

import cv2
import numpy as np

# --- HomographyTransform.cpp 와 동일한 상수 (바꾸지 말 것) ---
K_MIN_NORMALIZED_DETERMINANT = 1e-12
K_MIN_DENOMINATOR = 1e-9
K_ANCHOR_U = 0.5   # 화면 하단 중앙 -- 아래로 기울어진 CCTV 면 항상 카메라 앞쪽 지면
K_ANCHOR_V = 1.0

# localBounds 는 FOV 마스크가 아니라 '캘리브레이션 오류 감지용' sanity 필터다.
# 빡빡하게 잡으면 정상 검출이 조용히 폐기되므로 여유를 둔다.
BOUNDS_MARGIN = 1.2

POINT_COLOR = (0, 255, 0)
TEXT_COLOR = (0, 255, 255)
MAX_DISPLAY_WIDTH = 1400
MAX_DISPLAY_HEIGHT = 900


class PointPicker:
    """원본 픽셀 좌표를 유지한 채 화면에 맞게 축소해 보여주는 클릭 수집기."""

    def __init__(self, image, window_name="calibrate"):
        self.image = image
        self.window_name = window_name
        self.points = []

        h, w = image.shape[:2]
        # 화면보다 큰 스냅샷은 축소해서 띄우되, 클릭은 반드시 '원본' 좌표로 되돌린다.
        # 축소 배율을 잊으면 H 가 통째로 틀어지는데 예외가 나지 않아 잡기 어렵다.
        self.scale = min(1.0, MAX_DISPLAY_WIDTH / w, MAX_DISPLAY_HEIGHT / h)
        self.display_size = (int(w * self.scale), int(h * self.scale))

    def _on_mouse(self, event, x, y, flags, _param):
        del flags
        if event == cv2.EVENT_LBUTTONDOWN and len(self.points) < 4:
            self.points.append((x / self.scale, y / self.scale))
        elif event == cv2.EVENT_RBUTTONDOWN and self.points:
            self.points.pop()

    def _render(self):
        canvas = cv2.resize(self.image, self.display_size) if self.scale < 1.0 else self.image.copy()
        for index, (px, py) in enumerate(self.points):
            dx, dy = int(px * self.scale), int(py * self.scale)
            cv2.drawMarker(canvas, (dx, dy), POINT_COLOR, cv2.MARKER_CROSS, 24, 2)
            cv2.circle(canvas, (dx, dy), 9, POINT_COLOR, 2)
            cv2.putText(canvas, chr(ord("A") + index), (dx + 14, dy - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.8, POINT_COLOR, 2)
        banner = f"{len(self.points)}/4  L-click=add  R-click=undo  r=reset  ENTER=done  ESC=abort"
        cv2.putText(canvas, banner, (12, 28), cv2.FONT_HERSHEY_SIMPLEX, 0.6, TEXT_COLOR, 2)
        return canvas

    def collect(self):
        """4점을 모아 (4,2) float32 배열로 돌려준다. 중단하면 None."""
        cv2.namedWindow(self.window_name, cv2.WINDOW_AUTOSIZE)
        cv2.setMouseCallback(self.window_name, self._on_mouse)
        try:
            while True:
                cv2.imshow(self.window_name, self._render())
                key = cv2.waitKey(20) & 0xFF
                if key == 27:  # ESC
                    return None
                if key in (13, 10) and len(self.points) == 4:  # ENTER
                    return np.float32(self.points)
                if key == ord("r"):
                    self.points.clear()
        finally:
            cv2.destroyWindow(self.window_name)


def prompt_float(label, default=None):
    """숫자 하나를 받는다. 빈 입력은 기본값, 잘못된 입력은 다시 묻는다."""
    suffix = f" [{default}]" if default is not None else ""
    while True:
        raw = input(f"{label}{suffix}: ").strip()
        if not raw and default is not None:
            return float(default)
        try:
            return float(raw)
        except ValueError:
            print("  숫자를 입력하세요.")


def prompt_ground_truth(count):
    """클릭 순서대로 실측 로컬 좌표(미터)를 받는다.

    로컬 좌표 규약: 원점 = 카메라 바로 아래 바닥점, +y = 카메라 전방,
    +x = 카메라가 보는 방향 기준 오른쪽 (config 의 lateralSign = 1 과 짝).
    """
    print("\n[실측 좌표 입력] 원점=카메라 바로 아래, +y=전방, +x=오른쪽 (단위: m)")
    points = []
    for index in range(count):
        tag = chr(ord("A") + index)
        x = prompt_float(f"  {tag}  x (오른쪽 +)")
        y = prompt_float(f"  {tag}  y (전방 +)")
        points.append((x, y))
    return np.float32(points)


def to_normalized(matrix, width, height):
    """픽셀 좌표계 H 를 정규화 좌표계로 환산 (열 0 에 W, 열 1 에 H)."""
    converted = matrix.copy()
    converted[:, 0] *= width
    converted[:, 1] *= height
    return converted


def verify_like_server(matrix, width, height):
    """HomographyTransform 생성자의 검증을 그대로 재현한다.

    반환: (문제 목록, 정보 목록). 문제 목록이 비어 있지 않으면 서버가 예외로 죽는다.
    """
    problems, notes = [], []

    if not np.all(np.isfinite(matrix)):
        problems.append("행렬에 유한하지 않은 값이 있음")
        return problems, notes

    normalized = to_normalized(matrix, width, height)

    max_abs = np.max(np.abs(normalized))
    if not np.isfinite(max_abs) or max_abs <= 0.0:
        problems.append("행렬 크기가 0 이거나 유한하지 않음")
        return problems, notes
    normalized = normalized / max_abs

    determinant = float(np.linalg.det(normalized))
    if abs(determinant) < K_MIN_NORMALIZED_DETERMINANT:
        problems.append(
            f"특이 행렬 (det={determinant:.3e}) -- 기준점 4개가 한 직선 위에 있는지 확인")
    notes.append(f"정규화 행렬식 det = {determinant:+.6e}")

    anchor = normalized[2, 0] * K_ANCHOR_U + normalized[2, 1] * K_ANCHOR_V + normalized[2, 2]
    if not np.isfinite(anchor) or abs(anchor) <= K_MIN_DENOMINATOR:
        problems.append(
            f"지평선이 화면 하단 중앙을 지남 (분모={anchor:.3e}) -- 카메라를 더 아래로 "
            "기울여 스냅샷을 다시 찍을 것")
    else:
        notes.append(f"하단 중앙 분모 = {anchor:+.6f} (부호는 서버가 자동 정규화)")
        if anchor < 0.0:
            normalized = -normalized

    scale = normalized[2, 2]
    if abs(scale) > 1e-12:
        if np.allclose(normalized / scale, np.eye(3), atol=1e-9):
            problems.append("단위 행렬과 같음 -- 캘리브레이션이 실제로 적용되지 않음")

    return problems, notes


def residuals(matrix, src_px, dst_m):
    """클릭 픽셀을 H 로 되쏘아 실측값과의 잔차(m)를 구한다.

    getPerspectiveTransform 은 4점을 정확히 통과하므로 잔차는 원래 0 에 가깝다.
    0 이 아니면 수치가 불안정하다는 뜻 -- 4점이 너무 좁게 몰렸을 때 나타난다.
    """
    projected = cv2.perspectiveTransform(src_px.reshape(-1, 1, 2), matrix).reshape(-1, 2)
    return projected, np.linalg.norm(projected - dst_m, axis=1)


def compute_bounds(dst_scaled, extent_m, k):
    """localBounds 를 산출한다.

    물리 FOV 한 변(extent)을 기준으로 하되, 실제 클릭한 기준점이 반드시 안에 들어오도록
    넓힌다. 여기가 좁으면 정상 객체가 toLocal() 에서 조용히 폐기되고 파이프라인이 텅 빈다.
    """
    lateral = (extent_m / 2.0) * k * BOUNDS_MARGIN
    forward = (extent_m * (2 ** 0.5) / 2.0) * k * BOUNDS_MARGIN  # 중앙 설치 -> 대각선 코너

    # 클릭한 기준점이 경계 밖이면 그 자체로 모순이다. 무조건 포함시킨다.
    lateral = max(lateral, float(np.max(np.abs(dst_scaled[:, 0]))) * BOUNDS_MARGIN)
    forward = max(forward, float(np.max(dst_scaled[:, 1])) * BOUNDS_MARGIN)

    return {
        "localMinX": -round(lateral, 2),
        "localMaxX": round(lateral, 2),
        "localMinY": 0.0,   # 카메라 뒤쪽에 객체가 나올 수 없다
        "localMaxY": round(forward, 2),
    }


def main():
    parser = argparse.ArgumentParser(
        description="CCTV 지면 호모그래피 캘리브레이션 (compute-server config.json 생성)")
    parser.add_argument("image", help="CCTV 스냅샷 경로 (반드시 180도 플립을 켠 뒤 촬영한 것)")
    parser.add_argument("--channel", type=int, default=0, help="channelId (기본 0)")
    parser.add_argument("--k", type=float, default=None, help="축소 배율 (기본 4.0, 미지정 시 프롬프트)")
    parser.add_argument("--extent", type=float, default=None,
                        help="물리 FOV 한 변 (m, 기본 10.0, 미지정 시 프롬프트)")
    parser.add_argument("--output", help="config 조각을 파일로도 저장할 경로")
    args = parser.parse_args()

    image = cv2.imread(args.image)
    if image is None:
        sys.exit(f"[오류] 이미지를 열 수 없습니다: {args.image}")

    height, width = image.shape[:2]
    print(f"\n스냅샷: {args.image}  ({width} x {height})")
    print("※ 이 해상도가 운영 스트림 해상도와 달라도 됩니다 -- imageWidth/imageHeight 에 "
          "이 값이 기록되고 서버가 정규화 좌표로 환산합니다.\n")
    print("[클릭] 바닥 기준점 4개를 A→B→C→D 순서로 찍으세요.")
    print("  - 테이프 마크의 '바닥 접점'을 찍을 것")
    print("  - 4점이 한 직선 위에 있으면 안 됨 (되도록 큰 사각형)")
    print("  - 카메라 바로 아래(1m 이내)와 화면 최상단(지평선 근처)은 피할 것")

    src_px = PointPicker(image).collect()
    if src_px is None:
        sys.exit("[중단] 사용자가 취소했습니다.")

    print("\n클릭한 픽셀 좌표:")
    for index, (px, py) in enumerate(src_px):
        print(f"  {chr(ord('A') + index)}  ({px:8.2f}, {py:8.2f})")

    dst_physical = prompt_ground_truth(4)

    print()
    k = args.k if args.k is not None else prompt_float("축소 배율 k", 4.0)
    extent = args.extent if args.extent is not None else prompt_float("물리 FOV 한 변 (m)", 10.0)
    if k <= 0.0 or extent <= 0.0:
        sys.exit("[오류] k 와 extent 는 0 보다 커야 합니다.")

    # 실측(물리) 기준 기본 행렬 -> 0/1 행에 k 를 곱해 축소 스케일 주입.
    # 2행(분모)에는 절대 곱하지 않는다. 곱하면 배율이 상쇄돼 아무 효과가 없다.
    base = cv2.getPerspectiveTransform(src_px, dst_physical)
    scaled = base.copy()
    scaled[0, :] *= k
    scaled[1, :] *= k

    dst_scaled = dst_physical * k

    problems, notes = verify_like_server(scaled, width, height)
    projected, errors = residuals(scaled, src_px, dst_scaled)

    print("\n=== 검증 (compute-server 생성자와 동일) ===")
    for note in notes:
        print(f"  [정보] {note}")
    print(f"  [정보] 기준점 재투영 최대 오차 = {float(np.max(errors)):.4f} m (축소 스케일 기준)")
    if float(np.max(errors)) > 0.05:
        print("  [경고] 재투영 오차가 큽니다 -- 4점이 너무 좁게 몰렸을 수 있습니다.")
    for problem in problems:
        print(f"  [실패] {problem}")

    if problems:
        print("\n서버가 이 행렬을 거부합니다. 위 항목을 고치고 다시 실행하세요.")
        sys.exit(1)

    print("\n=== 기준점 확인 (축소 스케일, 단위 m) ===")
    for index in range(4):
        tag = chr(ord("A") + index)
        print(f"  {tag}  입력 ({dst_scaled[index, 0]:+8.2f}, {dst_scaled[index, 1]:+8.2f})"
              f"   재투영 ({projected[index, 0]:+8.2f}, {projected[index, 1]:+8.2f})")

    bounds = compute_bounds(dst_scaled, extent, k)

    snippet = {
        "channelId": args.channel,
        "homography": [round(float(v), 12) for v in scaled.flatten()],
        "homographySpace": "pixel",
        "imageWidth": float(width),
        "imageHeight": float(height),
        "localBoundsEnabled": True,
        **bounds,
        "riskEdgePolicy": "dropBottomTruncated",
    }
    rendered = json.dumps(snippet, indent=2, ensure_ascii=False)

    print(f"\n=== compute-server config.json (channel {args.channel}, k={k}) ===\n")
    print(rendered)
    print(f"\n※ localBounds 는 물리 {extent} m x {extent} m 기준에 여유 {BOUNDS_MARGIN}배를 "
          "적용한 값입니다.")
    print("※ 이 값은 FOV 마스크가 아니라 캘리브레이션 오류 감지용입니다 -- 좁히지 마세요. "
          "좁으면 정상 객체가 조용히 폐기됩니다.")

    if args.output:
        with open(args.output, "w", encoding="utf-8") as handle:
            handle.write(rendered + "\n")
        print(f"\n저장됨: {args.output}")


if __name__ == "__main__":
    main()
