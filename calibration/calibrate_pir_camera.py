#!/usr/bin/env python3
# ============================================================
#  calibrate_pir_camera.py
#
#  PIR カメラ (Night/Pattern-C) の intrinsic キャリブレーション。
#  udev セットアップ後（/dev/video/pir_camera が存在する状態）に実行する。
#
#  チェスボードを各方向・各距離で撮影し、cv2.calibrateCamera で
#  カメラ行列 K と歪み係数 D を求めて calib_result.yaml に保存する。
#
#  操作:
#    c   : 現フレームをサンプルとして採用（コーナー検出成功時のみ）
#    d   : 直前のサンプルを削除
#    q   : 撮影終了 → キャリブレーション実行 → YAML 保存
#    ESC : 中断（保存しない）
#
#  --- IR カメラ特有の注意 ---
#  intrinsic（レンズ幾何）は照明条件に依存しない。よって夜間 IR 環境
#  ではなく、コーナー検出が安定する明るい環境で印刷チェスボードを
#  撮影してキャリブレーションし、得られた K/D を夜間運用に使えばよい。
#  IR 照明下で直接行う必要はない（むしろ検出が不安定になりやすい）。
# ============================================================

import datetime
import os

import cv2
import numpy as np

# ==================== Config ====================
CAMERA_PATH = "/dev/video/pir_camera"  # udev symlink（CAP_V4L2 で明示オープン）

# 運用解像度に合わせる。C++ 側は calib 解像度 -> active 解像度へスケールするが、
# 同じ 1920x1080 で撮っておくのが最も素直。
WIDTH, HEIGHT, FPS = 1920, 1080, 30

# チェスボードの「内側」コーナー数 (cols, rows)。
# 例: 10x7 マスのボードなら内側コーナーは (9, 6)。
CHESSBOARD = (9, 6)
SQUARE_SIZE_MM = 25.0  # 1 マスの実寸 [mm]（物理的に測って入れる）

MIN_SHOTS = 15  # キャリブレーションに必要な最小サンプル数（多いほど安定）

OUT_DIR = "/home/hagi/Downloads/place/here"
OUT_YAML = os.path.join(OUT_DIR, "calib_result.yaml")
SAVE_SHOTS = True  # 採用フレームを png 保存するか（再現・検証用）
SHOT_DIR = os.path.join(OUT_DIR, "calib_shots")


def build_object_points():
    """チェスボード 1 枚分の 3D 座標（Z=0 平面、単位 mm）。"""
    objp = np.zeros((CHESSBOARD[0] * CHESSBOARD[1], 3), np.float32)
    objp[:, :2] = np.mgrid[0 : CHESSBOARD[0], 0 : CHESSBOARD[1]].T.reshape(-1, 2)
    objp *= SQUARE_SIZE_MM
    return objp


def open_camera():
    cap = cv2.VideoCapture(CAMERA_PATH, cv2.CAP_V4L2)
    if not cap.isOpened():
        raise RuntimeError(f"Failed to open {CAMERA_PATH}")

    # MJPG で開かないと高解像度の fps が出ない個体が多い
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, WIDTH)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, HEIGHT)
    cap.set(cv2.CAP_PROP_FPS, FPS)

    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv2.CAP_PROP_FPS)
    fourcc = int(cap.get(cv2.CAP_PROP_FOURCC))
    cc = "".join([chr((fourcc >> (8 * i)) & 0xFF) for i in range(4)])
    print(f"Opened {CAMERA_PATH}: {w}x{h} @ {fps:.1f} FOURCC={cc}")
    return cap, w, h


def save_yaml(K, D, img_w, img_h, rms):
    """C++ 側 vision::loadCalibrationFile が読む calib_result.yaml を出力。
    フィールド名がローダ実装と一致しない場合は調整すること（下の注記参照）。"""
    fs = cv2.FileStorage(OUT_YAML, cv2.FILE_STORAGE_WRITE)
    fs.write("image_width", img_w)
    fs.write("image_height", img_h)
    fs.write("camera_matrix", K)
    fs.write("distortion_coefficients", D)
    fs.write("avg_reprojection_error", float(rms))
    fs.write("calibration_date", datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
    fs.release()
    print(f"Saved calibration -> {OUT_YAML}")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    if SAVE_SHOTS:
        os.makedirs(SHOT_DIR, exist_ok=True)

    objp = build_object_points()
    objpoints = []  # 3D 点（各サンプル）
    imgpoints = []  # 2D 検出コーナー（各サンプル）

    cap, img_w, img_h = open_camera()
    cv2.namedWindow("calib", cv2.WINDOW_NORMAL)

    # cornerSubPix 終了条件
    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)
    find_flags = cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE

    print("操作: [c]採用  [d]直前削除  [q]終了して計算  [ESC]中断")

    try:
        while cap.isOpened():
            ret, frame = cap.read()
            if not ret:
                print("Read failed (device busy / format not accepted)")
                break

            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            found, corners = cv2.findChessboardCorners(gray, CHESSBOARD, find_flags)

            vis = frame.copy()
            if found:
                corners_refined = cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), criteria)
                cv2.drawChessboardCorners(vis, CHESSBOARD, corners_refined, found)
                status, color = "DETECTED (press c)", (0, 255, 0)
            else:
                corners_refined = None
                status, color = "no board", (0, 0, 255)

            cv2.putText(
                vis,
                f"{status}  shots={len(objpoints)}/{MIN_SHOTS}",
                (20, 50),
                cv2.FONT_HERSHEY_SIMPLEX,
                1.2,
                color,
                2,
                cv2.LINE_AA,
            )
            cv2.imshow("calib", vis)
            key = cv2.waitKey(1) & 0xFF

            if key == 27:  # ESC
                print("Aborted. No file written.")
                return

            if key == ord("q"):
                break

            if key == ord("d") and objpoints:
                objpoints.pop()
                imgpoints.pop()
                print(f"Removed last sample. shots={len(objpoints)}")

            if key == ord("c"):
                if not found:
                    print("Board not detected; sample not taken.")
                    continue
                objpoints.append(objp.copy())
                imgpoints.append(corners_refined)
                print(f"Captured sample {len(objpoints)}")
                if SAVE_SHOTS:
                    ts = datetime.datetime.now().strftime("%Y_%m_%d_%H_%M_%S")
                    cv2.imwrite(os.path.join(SHOT_DIR, f"calib_{ts}.png"), frame)
    finally:
        cap.release()
        cv2.destroyAllWindows()

    # ==================== Calibration ====================
    if len(objpoints) < MIN_SHOTS:
        print(f"サンプル不足: {len(objpoints)} < {MIN_SHOTS}。計算を中止します。")
        return

    print(f"Calibrating with {len(objpoints)} samples...")
    rms, K, D, rvecs, tvecs = cv2.calibrateCamera(objpoints, imgpoints, (img_w, img_h), None, None)

    # 各ビューの再投影誤差（外れサンプル発見に有用）
    total_err = 0.0
    for i in range(len(objpoints)):
        proj, _ = cv2.projectPoints(objpoints[i], rvecs[i], tvecs[i], K, D)
        err = cv2.norm(imgpoints[i], proj, cv2.NORM_L2) / len(proj)
        total_err += err
    mean_err = total_err / len(objpoints)

    print("=== Calibration result ===")
    print(f"RMS (cv2.calibrateCamera) = {rms:.4f}")
    print(f"mean reprojection error   = {mean_err:.4f} px")
    print("Camera matrix K:\n", K)
    print("Distortion D:\n", D.ravel())
    if mean_err > 1.0:
        print(
            "WARNING: 再投影誤差が大きい(>1px)。撮影枚数・角度・ボード平面性を見直すと改善します。"
        )

    save_yaml(K, D, img_w, img_h, rms)


if __name__ == "__main__":
    main()
