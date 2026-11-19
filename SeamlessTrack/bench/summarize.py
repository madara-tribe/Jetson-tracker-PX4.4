#!/usr/bin/env python3
"""
summarize.py — bench/results/<TAG>/ の CSV を集計して summary.md を作る（標準ライブラリのみ）

  python3 bench/summarize.py bench/results/gpu_day1 --baseline bench/results/cpu_base

- offline_*.csv : bench_yolo の結果（同じ画像で N 回）
- live_*.csv    : launch_px.sh 実行中の 1 フレーム 1 行（DETECT 中のみ）。
                  時間の集計では、DETECT に入った最初の 1 フレーム（first_frame=1）を除外する
                  （loop_ms に WATCH の待ち時間が入るため）。
                  追従の集計では除外しない（検出できなかったフレームも数える）
- tegrastats.log: あれば CPU / GPU 使用率・温度・電力の平均を出す
- live_*.csv に誤差の列（detected, yaw_err_deg, ... 2026-11-19 追加）があれば、
  追従の数字（検出率、誤差の平均と 95% 値、指令の変化回数、取りこぼしの回数、
  WATCH に戻った回数、フレームの古さ）も出す。
  誤差は検出できたフレームだけで計算するので、必ず検出率と一緒に見ること。
  --distance-m で、度を cm に直すときの距離を指定する（既定 5 m）
--baseline を付けると、同じ種類（offline / live）の cpu の値と比べた速度倍率を出す。
"""

import argparse
import csv
import glob
import math
import os
import re
import statistics


def pct(values, p):
    if not values:
        return float("nan")
    v = sorted(values)
    k = (len(v) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def load(path, keep_first_frame=False):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    kind = "live" if os.path.basename(path).startswith("live_") else "offline"
    if kind == "live" and not keep_first_frame:
        rows = [r for r in rows if r.get("first_frame", "0") != "1"]
    label = os.path.basename(path)[len(kind) + 1 : -4]
    return kind, label, rows


def col(rows, name):
    out = []
    for r in rows:
        try:
            out.append(float(r[name]))
        except (KeyError, ValueError):
            pass
    return out


def stats_for(kind, rows):
    stages = (
        ["pre_ms", "infer_ms", "gpu_ms", "post_ms", "total_ms"]
        if kind == "offline"
        else ["capture_ms", "pre_ms", "infer_ms", "post_ms", "control_ms", "loop_ms"]
    )
    s = {"n": len(rows)}
    for st in stages:
        v = col(rows, st)
        s[st] = (statistics.mean(v) if v else float("nan"), pct(v, 50), pct(v, 95))
    key = "total_ms" if kind == "offline" else "loop_ms"
    m = s[key][0]
    s["fps"] = 1000.0 / m if m and m == m else float("nan")
    if kind == "live":
        nd = col(rows, "n_det")
        s["det_rate"] = (sum(1 for x in nd if x > 0) / len(nd)) if nd else float("nan")
    s["provider"] = rows[0].get("provider", "?") if rows else "?"
    s["fp16"] = rows[0].get("fp16", "?") if rows else "?"
    return s


def tracking_for(rows, distance_m):
    """追従の数字。誤差の列が無い古い CSV では None を返す。

    rows は first_frame=1 の行も含む、すべての行。
    """
    if not rows or "yaw_err_deg" not in rows[0]:
        return None
    det = [r for r in rows if r.get("detected") == "1"]
    yaw = [abs(x) for x in col(det, "yaw_err_deg")]
    pitch = [abs(x) for x in col(det, "pitch_err_deg")]
    # 時間は、DETECT に入った最初のフレームを除いて数える（loop_ms に WATCH の待ちが入るため）
    timed = [r for r in rows if r.get("first_frame", "0") != "1"]
    seconds = sum(col(timed, "loop_ms")) / 1000.0

    has_session = "session_id" in rows[0]

    def same_session(a, b):
        return (not has_session) or a.get("session_id") == b.get("session_id")

    # サーボに実際に送られるのは四捨五入した整数。その値が変わった回数を数える（横と縦）
    def changes(name):
        vals = [round(x) for x in col(rows, name)]
        return sum(1 for a, b in zip(vals, vals[1:], strict=False) if a != b)

    # 取りこぼし: 検出あり → 検出なし に変わった回数（1 フレームだけでも 1 回）。
    # WATCH に戻った回数とは別物
    drops = sum(
        1
        for a, b in zip(rows, rows[1:], strict=False)
        if same_session(a, b) and a.get("detected") == "1" and b.get("detected") != "1"
    )
    # WATCH に戻った回数: to_watch 列（2026-11-19 r2 で追加）。無ければ None
    to_watch = int(sum(col(rows, "to_watch"))) if "to_watch" in rows[0] else None
    sessions = len({r.get("session_id") for r in rows}) if has_session else None
    age = col(timed, "frame_age_ms")

    def cm(deg):
        return math.tan(math.radians(deg)) * distance_m * 100.0

    def mean(v):
        return statistics.mean(v) if v else float("nan")

    def per_s(n):
        return (n / seconds) if seconds > 0 else float("nan")

    return {
        "n": len(rows),
        "n_det": len(det),
        "det_rate": len(det) / len(rows),
        "hz": per_s(len(timed)),
        "yaw_mean": mean(yaw),
        "yaw_p95": pct(yaw, 95),
        "yaw_mean_cm": cm(mean(yaw)) if yaw else float("nan"),
        "yaw_p95_cm": cm(pct(yaw, 95)) if yaw else float("nan"),
        "pitch_mean": mean(pitch),
        "changes_x_per_s": per_s(changes("cmd_x_deg")),
        "changes_y_per_s": per_s(changes("cmd_y_deg")),
        "drops": drops,
        "to_watch": to_watch,
        "sessions": sessions,
        "age_mean": mean(age) if age else None,
        "age_p95": pct(age, 95) if age else None,
    }


def collect_tracking(d, distance_m):
    res = {}
    for p in sorted(glob.glob(os.path.join(d, "live_*.csv"))):
        _, label, rows = load(p, keep_first_frame=True)
        t = tracking_for(rows, distance_m)
        if t:
            res[label] = t
    return res


def collect(d):
    res = {}
    for p in sorted(glob.glob(os.path.join(d, "*.csv"))):
        kind, label, rows = load(p)
        if rows:
            res[(kind, label)] = stats_for(kind, rows)
    return res


def tegrastats(d):
    p = os.path.join(d, "tegrastats.log")
    if not os.path.exists(p):
        return None
    cpu, gpu, vin, temp = [], [], [], []
    for line in open(p, errors="replace"):
        m = re.search(r"CPU \[([^\]]*)\]", line)
        if m:
            vals = [int(x) for x in re.findall(r"(\d+)%@", m.group(1))]
            if vals:
                cpu.append(statistics.mean(vals))
        m = re.search(r"GR3D_FREQ (\d+)%", line)
        if m:
            gpu.append(int(m.group(1)))
        m = re.search(r"VDD_IN (\d+)mW", line)
        if m:
            vin.append(int(m.group(1)))
        m = re.search(r"tj@([\d.]+)C", line)
        if m:
            temp.append(float(m.group(1)))

    def f(v):
        return f"{statistics.mean(v):.1f}" if v else "-"

    def mx(v):
        return f"{max(v):.1f}" if v else "-"

    return (
        f"| CPU 平均 % | GPU 平均 % | 電力 VDD_IN 平均 mW | 温度 tj 平均 / 最大 °C | サンプル数 |\n"
        f"|---|---|---|---|---|\n| {f(cpu)} | {f(gpu)} | {f(vin)} | {f(temp)} / {mx(temp)} | {len(cpu)} |\n"
    )


def fmt(t):
    if t[0] != t[0] or t[0] == 0.0:  # nan（列が無い）または 0（TensorRT 以外の gpu 列）
        return "-"
    return f"{t[0]:.2f} / {t[1]:.2f} / {t[2]:.2f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--baseline", help="Step 1 の結果フォルダ（cpu の値と比べる）")
    ap.add_argument(
        "--distance-m", type=float, default=5.0, help="誤差を cm に直すときの距離（既定 5 m）"
    )
    a = ap.parse_args()

    res = collect(a.dir)
    base = collect(a.baseline) if a.baseline else {}
    # 基準: baseline フォルダの cpu、無ければ同じフォルダの cpu
    ref = {}
    for src in (res, base):  # baseline があればそちらを優先
        for (k, lbl), v in src.items():
            if lbl == "cpu":
                ref[k] = v

    out = [
        f"# 計測結果: {os.path.basename(os.path.abspath(a.dir))}\n",
        "値は 平均 / p50 / p95（ms）。速度倍率は CPU の infer 平均 ÷ この行の infer 平均。\n",
        "gpu は infer のうち GPU 計算だけの時間（TensorRT 直接版のみ。残りは CPU⇔GPU のコピー）。\n",
    ]

    for kind, title, stages in [
        (
            "offline",
            "## オフライン（同じ画像で繰り返し推論）",
            ["pre_ms", "infer_ms", "gpu_ms", "post_ms", "total_ms"],
        ),
        (
            "live",
            "## 実機（launch_px.sh 実行中、DETECT 中のフレーム）",
            ["capture_ms", "pre_ms", "infer_ms", "post_ms", "control_ms", "loop_ms"],
        ),
    ]:
        items = [(lbl, s) for (k, lbl), s in res.items() if k == kind]
        if (kind, "cpu") in base:  # 基準の CPU も表に並べる
            items.insert(0, ("cpu（基準）", base[(kind, "cpu")]))
        if not items:
            continue
        out.append(title + "\n")
        head = (
            ["label", "provider", "fp16", "n"]
            + [s.replace("_ms", "") for s in stages]
            + ["FPS", "infer 倍率"]
        )
        if kind == "live":
            head.insert(-1, "検出率")
        out.append("| " + " | ".join(head) + " |")
        out.append("|" + "---|" * len(head))
        for lbl, s in items:
            r = ref.get(kind)
            speed = (
                (r["infer_ms"][0] / s["infer_ms"][0]) if r and s["infer_ms"][0] else float("nan")
            )
            cells = [lbl, s["provider"], s["fp16"], str(s["n"])] + [fmt(s[st]) for st in stages]
            cells.append(f"{s['fps']:.1f}")
            if kind == "live":
                cells.append(f"{s['det_rate'] * 100:.0f}%")
            cells.append(f"x{speed:.2f}" if speed == speed else "-")
            out.append("| " + " | ".join(cells) + " |")
        out.append("")

    track = collect_tracking(a.dir, a.distance_m)
    if track:
        out.append(
            f"## 追従（実機、検出できたフレームの誤差。cm は {a.distance_m:g} m 先での値）\n"
        )
        head = [
            "label",
            "n",
            "検出率",
            "ループ Hz",
            "横の誤差 平均 度 (cm)",
            "横の誤差 95% 度 (cm)",
            "縦の誤差 平均 度",
            "指令変化 / 秒（横 / 縦）",
            "取りこぼし 回",
            "DETECT に入った 回",
            "WATCH に戻った 回",
            "フレームの古さ ms（平均 / 95%）",
        ]
        out.append("| " + " | ".join(head) + " |")
        out.append("|" + "---|" * len(head))

        def opt(v):
            return "-" if v is None else str(v)

        for lbl, t in track.items():
            age = "-" if t["age_mean"] is None else f"{t['age_mean']:.1f} / {t['age_p95']:.1f}"
            out.append(
                f"| {lbl} | {t['n']} | {t['det_rate'] * 100:.0f}% | {t['hz']:.1f} "
                f"| {t['yaw_mean']:.2f} ({t['yaw_mean_cm']:.0f}) "
                f"| {t['yaw_p95']:.2f} ({t['yaw_p95_cm']:.0f}) "
                f"| {t['pitch_mean']:.2f} "
                f"| {t['changes_x_per_s']:.1f} / {t['changes_y_per_s']:.1f} "
                f"| {t['drops']} | {opt(t['sessions'])} | {opt(t['to_watch'])} | {age} |"
            )
        out.append("")
        out.append(
            "誤差は、検出できたフレームだけで計算している。見失っている間のずれは入らないので、"
            "検出率と一緒に見ること。「取りこぼし」は 1 フレームだけ検出が途切れた場合も 1 回と数える。"
            "「フレームの古さ」は、撮ってから処理を始めるまでの時間（LATEST_FRAME=true のとき）。\n"
        )

    if base:
        out.append(f"基準（CPU）: {a.baseline}\n")
    ts = tegrastats(a.dir)
    if ts:
        out.append("## Jetson の状態（tegrastats）\n")
        out.append(ts)
    text = "\n".join(out)
    print(text)
    with open(os.path.join(a.dir, "summary.md"), "w") as f:
        f.write(text + "\n")


if __name__ == "__main__":
    main()
