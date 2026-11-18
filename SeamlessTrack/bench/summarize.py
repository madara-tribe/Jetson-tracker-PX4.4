#!/usr/bin/env python3
"""
summarize.py — bench/results/<TAG>/ の CSV を集計して summary.md を作る（標準ライブラリのみ）

  python3 bench/summarize.py bench/results/gpu_day1 --baseline bench/results/cpu_base

- offline_*.csv : bench_yolo の結果（同じ画像で N 回）
- live_*.csv    : launch_px.sh 実行中の 1 フレーム 1 行（DETECT 中のみ）。
                  モード切替直後の 1 フレーム（first_frame=1）は除外する
- tegrastats.log: あれば CPU / GPU 使用率・温度・電力の平均を出す
--baseline を付けると、同じ種類（offline / live）の cpu の値と比べた速度倍率を出す。
"""

import argparse
import csv
import glob
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


def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    kind = "live" if os.path.basename(path).startswith("live_") else "offline"
    if kind == "live":
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
