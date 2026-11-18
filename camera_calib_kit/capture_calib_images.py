#!/usr/bin/env python3
"""
capture_calib_images.py — キャリブレーション用チェッカーボード画像の撮影

用途:
    IRカメラ(夜間用)・RGBカメラ(BRIO等)どちらでも共通して使えるキャリブレーション
    画像収集ツール。チェッカーボードをカメラに向けて動かしながら、コーナーが
    検出できたフレームだけを 's' キー(または --auto)で captures/ 以下に保存する。

    保存されたフォルダは、そのまま run_camera_calibration.py の --images に渡せる。

前提:
    - チェッカーボードのサイズ(内側コーナー数)を実物に合わせて --cols / --rows で指定する
      例: 10x7マスの市松模様なら、内側コーナーは 9x6 → --cols 9 --rows 6
    - IRカメラの場合、印刷したチェッカーボードにIR照明(内蔵IR LEDや外付け照明)が
      きちんと当たっていないとコーナー検出が安定しない。低照度環境では
      --gray-only や露光調整(下記オプション)を試すこと。

使い方:
    # RGBカメラ(day, /dev/video0) を手動保存モードで撮影
    python capture_calib_images.py --device /dev/video0 --cols 9 --rows 6 \
        --square-size-mm 25 --out captures_rgb

    # IRカメラ(night, /dev/video2) を自動保存モードで撮影(2秒間隔)
    python capture_calib_images.py --device /dev/video2 --cols 9 --rows 6 \
        --square-size-mm 25 --out captures_ir --auto --auto-interval 2.0

操作:
    's'  : 現フレームを保存(コーナー検出できていなくても保存はできるが非推奨)
    'q'  : 終了
    (--auto 指定時は、コーナー検出でき次第 --auto-interval 秒おきに自動保存)

出力:
    <out>/img_YYYYMMDD_HHMMSS_NNNN.png  という命名で連番保存
    (run_camera_calibration.py 側は、この命名規則である必要はなく単純にフォルダ内の
     画像を全部読み込むだけなので、命名規則を変えても問題ない)
"""

from __future__ import annotations

import argparse
import time
from datetime import datetime
from pathlib import Path

import cv2


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Checkerboard capture for camera calibration")
    p.add_argument(
        "--device",
        default="/dev/video0",
        help="camera device path or index (e.g. /dev/video0, 0, 2)",
    )
    p.add_argument(
        "--cols", type=int, required=True, help="inner corners per row (checkerboard width - 1)"
    )
    p.add_argument(
        "--rows", type=int, required=True, help="inner corners per column (checkerboard height - 1)"
    )
    p.add_argument(
        "--square-size-mm",
        type=float,
        default=25.0,
        help="checkerboard square size in mm (metadata only, not used for capture)",
    )
    p.add_argument("--out", default="captures", help="output directory for saved images")
    p.add_argument(
        "--width", type=int, default=1280, help="capture width to request from the camera"
    )
    p.add_argument(
        "--height", type=int, default=720, help="capture height to request from the camera"
    )
    p.add_argument(
        "--fourcc", default=None, help="optional FOURCC to force (e.g. MJPG, YUYV, GREY)"
    )
    p.add_argument(
        "--gray-only",
        action="store_true",
        help="request single-channel capture (some IR cams need this)",
    )
    p.add_argument(
        "--auto",
        action="store_true",
        help="auto-save whenever a fresh checkerboard pose is detected",
    )
    p.add_argument(
        "--auto-interval", type=float, default=1.5, help="minimum seconds between auto-saves"
    )
    p.add_argument(
        "--min-images", type=int, default=15, help="just a reminder printed at exit; not enforced"
    )
    return p.parse_args()


def open_camera(
    device: str, width: int, height: int, fourcc: str | None, gray_only: bool
) -> cv2.VideoCapture:
    # device may be an index ("0") or a path ("/dev/video2")
    dev: str | int = int(device) if device.isdigit() else device
    cap = cv2.VideoCapture(dev, cv2.CAP_V4L2) if isinstance(dev, str) else cv2.VideoCapture(dev)
    if not cap.isOpened():
        # fall back to default backend if V4L2 open failed (e.g. non-Linux)
        cap = cv2.VideoCapture(dev)
    if not cap.isOpened():
        raise RuntimeError(f"could not open camera device: {device}")

    if fourcc:
        cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*fourcc))
    if gray_only:
        cap.set(cv2.CAP_PROP_CONVERT_RGB, 0)

    cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
    return cap


def main() -> None:
    args = parse_args()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    pattern_size = (args.cols, args.rows)
    cap = open_camera(args.device, args.width, args.height, args.fourcc, args.gray_only)

    actual_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    actual_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    print(
        f"Opened {args.device}  requested={args.width}x{args.height}  actual={actual_w}x{actual_h}"
    )
    print(f"Checkerboard inner corners: {pattern_size[0]} x {pattern_size[1]}")
    print("Press 's' to save, 'q' to quit." + (" (--auto is ON)" if args.auto else ""))

    saved = 0
    last_auto_save_t = 0.0
    win = "capture_calib_images (s=save, q=quit)"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)

    flags = cv2.CALIB_CB_ADAPTIVE_THRESH | cv2.CALIB_CB_NORMALIZE_IMAGE | cv2.CALIB_CB_FAST_CHECK

    try:
        while True:
            ok, frame = cap.read()
            if not ok:
                print("WARNING: frame grab failed, retrying...")
                time.sleep(0.1)
                continue

            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY) if frame.ndim == 3 else frame
            found, corners = cv2.findChessboardCorners(gray, pattern_size, flags=flags)

            display = frame.copy() if frame.ndim == 3 else cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)
            if found:
                cv2.drawChessboardCorners(display, pattern_size, corners, found)

            status = f"saved={saved}  detected={'YES' if found else 'no'}"
            cv2.putText(
                display,
                status,
                (10, 30),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.8,
                (0, 255, 0) if found else (0, 0, 255),
                2,
                cv2.LINE_AA,
            )
            cv2.imshow(win, display)

            key = cv2.waitKey(1) & 0xFF

            do_save = False
            if key == ord("s"):
                do_save = True
            elif (
                args.auto and found and (time.monotonic() - last_auto_save_t) >= args.auto_interval
            ):
                do_save = True
                last_auto_save_t = time.monotonic()

            if do_save:
                saved += 1
                ts = datetime.now().strftime("%Y%m%d_%H%M%S")
                fname = out_dir / f"img_{ts}_{saved:04d}.png"
                cv2.imwrite(str(fname), frame)
                print(
                    f"  saved: {fname}  (checkerboard {'found' if found else 'NOT found — recommend retake'})"
                )

            if key == ord("q"):
                break
    finally:
        cap.release()
        cv2.destroyAllWindows()

    print(f"\nDone. {saved} image(s) saved to {out_dir}/")
    if saved < args.min_images:
        print(
            f"NOTE: fewer than {args.min_images} images saved — consider capturing more "
            f"(different angles/distances/corners of the frame) before running calibration."
        )
    print(
        f"\nNext step:\n"
        f"  python run_camera_calibration.py --images {out_dir} --cols {args.cols} --rows {args.rows} "
        f"--square-size-mm {args.square_size_mm} --out weights/calib_result_ir.yaml"
    )


if __name__ == "__main__":
    main()
