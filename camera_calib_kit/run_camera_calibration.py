#!/usr/bin/env python3
"""
run_camera_calibration.py — チェッカーボード画像からカメラ内部パラメータを算出し、
                             calib_result.yaml を出力する。

出力フォーマットは px2/include/vision/camera_calibration.hpp の
loadCalibrationFile() がそのまま読める OpenCV FileStorage 形式で、
既存の SeamlessTrack/src/px2/weights/calib_result.yaml (day/RGBカメラ用) と
バイト単位で互換のレイアウトになる:

    camera_matrix: !!opencv-matrix (3x3, dt:d)
    dist_coeffs:   !!opencv-matrix (1x5, dt:d)  # k1,k2,p1,p2,k3
    image_width / image_height
    rms_reprojection_error
    per_view_errors: !!opencv-matrix (Nx1, dt:f)
    num_images_used
    used_images: [ "captures_ir/img_....png", ... ]

読み込み側(loadCalibrationFile)が実際に使うのは camera_matrix / dist_coeffs /
image_width / image_height の4つだけ。それ以外(rms_reprojection_error 等)は
記録用メタデータとして既存ファイルの慣習に合わせて残している。

使い方:
    python run_camera_calibration.py \
        --images captures_ir \
        --cols 9 --rows 6 --square-size-mm 25 \
        --out weights/calib_result_ir.yaml

    # 実機の runtime 解像度(px2の active_width/active_height)と撮影解像度が違う場合、
    # loadCalibrationFile() 側で fx,fy,cx,cy をスケーリングしてくれるので、
    # ここでは撮影時の実解像度をそのまま image_width/image_height として書けばよい。

注意:
    - --cols / --rows は「内側コーナー数」。10x7マスのチェッカーボードなら 9x6。
    - RMS再投影誤差(rms_reprojection_error)が 1.0px を超える場合は撮影条件を見直すこと
      (day/RGBカメラの実績値は 0.27px 程度)。
    - IRカメラで撮影した画像がグレースケール1chの場合もそのまま動作する。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import cv2
import numpy as np


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Checkerboard camera calibration -> calib_result.yaml")
    p.add_argument(
        "--images", required=True, help="directory containing checkerboard images (png/jpg)"
    )
    p.add_argument("--cols", type=int, required=True, help="inner corners per row")
    p.add_argument("--rows", type=int, required=True, help="inner corners per column")
    p.add_argument(
        "--square-size-mm", type=float, required=True, help="checkerboard square edge length in mm"
    )
    p.add_argument("--out", default="calib_result.yaml", help="output YAML path")
    p.add_argument(
        "--glob",
        default="*.png",
        help="glob pattern for image files within --images (comma-separated for multiple, e.g. '*.png,*.jpg')",
    )
    p.add_argument(
        "--max-rms",
        type=float,
        default=1.0,
        help="warn if final RMS reprojection error (px) exceeds this",
    )
    return p.parse_args()


def find_images(images_dir: Path, glob_patterns: str) -> list[Path]:
    files: list[Path] = []
    for pat in glob_patterns.split(","):
        files.extend(sorted(images_dir.glob(pat.strip())))
    # de-dupe while preserving order
    seen = set()
    unique = []
    for f in files:
        if f not in seen:
            seen.add(f)
            unique.append(f)
    return unique


def main() -> None:
    args = parse_args()
    images_dir = Path(args.images)
    if not images_dir.is_dir():
        sys.exit(f"ERROR: images directory not found: {images_dir}")

    image_files = find_images(images_dir, args.glob)
    if not image_files:
        sys.exit(f"ERROR: no images matching '{args.glob}' found in {images_dir}")

    pattern_size = (args.cols, args.rows)
    square_size = args.square_size_mm

    # 3D object points for one checkerboard view, in the board's own coordinate
    # frame (Z=0 plane), scaled by the real square size.
    objp = np.zeros((pattern_size[0] * pattern_size[1], 3), np.float32)
    objp[:, :2] = np.mgrid[0 : pattern_size[0], 0 : pattern_size[1]].T.reshape(-1, 2)
    objp *= square_size

    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-4)

    flags = cv2.CALIB_CB_ADAPTIVE_THRESH | cv2.CALIB_CB_NORMALIZE_IMAGE

    objpoints: list[np.ndarray] = []
    imgpoints: list[np.ndarray] = []
    used_images: list[str] = []
    image_size: tuple[int, int] | None = None

    print(f"Scanning {len(image_files)} image(s) in {images_dir}/ ...")
    for f in image_files:
        img = cv2.imread(str(f))
        if img is None:
            print(f"  [skip] could not read: {f.name}")
            continue

        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY) if img.ndim == 3 else img
        h, w = gray.shape[:2]
        if image_size is None:
            image_size = (w, h)
        elif (w, h) != image_size:
            print(f"  [skip] size mismatch {w}x{h} != {image_size[0]}x{image_size[1]}: {f.name}")
            continue

        found, corners = cv2.findChessboardCorners(gray, pattern_size, flags=flags)
        if not found:
            print(f"  [skip] checkerboard not found: {f.name}")
            continue

        corners_refined = cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), criteria)
        objpoints.append(objp)
        imgpoints.append(corners_refined)
        used_images.append(f"{images_dir.name}/{f.name}")
        print(f"  [ok]   {f.name}")

    n_used = len(objpoints)
    if n_used < 5:
        sys.exit(
            f"ERROR: only {n_used} usable image(s) with detected checkerboard "
            f"(need at least ~10-15 for a reliable calibration). Capture more images."
        )

    assert image_size is not None
    print(f"\nRunning cv2.calibrateCamera() with {n_used} image(s), image_size={image_size} ...")

    rms, K, D, rvecs, tvecs = cv2.calibrateCamera(objpoints, imgpoints, image_size, None, None)

    # per-view reprojection error (matches the "per_view_errors" field convention
    # used in the existing day-camera calib_result.yaml)
    per_view_errors = []
    for i in range(n_used):
        proj, _ = cv2.projectPoints(objpoints[i], rvecs[i], tvecs[i], K, D)
        err = cv2.norm(imgpoints[i], proj, cv2.NORM_L2) / len(proj)
        per_view_errors.append(err)
    per_view_errors = np.array(per_view_errors, dtype=np.float32).reshape(-1, 1)

    print(f"\nRMS reprojection error: {rms:.4f} px")
    if rms > args.max_rms:
        print(
            f"WARNING: RMS error exceeds --max-rms={args.max_rms}px. "
            f"Consider recapturing with more varied angles/coverage of the frame, "
            f"and make sure the board was flat and well lit."
        )

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    fs = cv2.FileStorage(str(out_path), cv2.FILE_STORAGE_WRITE)
    fs.write("camera_matrix", K)
    fs.write("dist_coeffs", D)
    fs.write("image_width", image_size[0])
    fs.write("image_height", image_size[1])
    fs.write("rms_reprojection_error", float(rms))
    fs.write("per_view_errors", per_view_errors)
    fs.write("num_images_used", n_used)
    fs.startWriteStruct("used_images", cv2.FileNode_SEQ)
    for name in used_images:
        fs.write("", name)
    fs.endWriteStruct()
    fs.release()

    print(f"\nWrote {out_path}")
    print(
        f"  camera_matrix (fx,fy,cx,cy) = "
        f"{K[0, 0]:.4f}, {K[1, 1]:.4f}, {K[0, 2]:.4f}, {K[1, 2]:.4f}"
    )
    print(f"  dist_coeffs (k1,k2,p1,p2,k3) = {', '.join(f'{v:.6f}' for v in D.ravel())}")
    print(f"  image_size = {image_size[0]} x {image_size[1]}")
    print(f"  images used = {n_used} / {len(image_files)}")


if __name__ == "__main__":
    main()
