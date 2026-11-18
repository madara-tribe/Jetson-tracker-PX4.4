"""test_sim_hil_param_contract.py — シミュレーション⇔実機 パラメータ契約チェック

目的:
    simulation/data/params/control_params.yaml に転記された制御パラメータが、
    実機コード側の「正データ」からズレていないかを機械的に検証する。

正データの参照元（優先順位）:
    1. launch時のオーバーライドスクリプト
       - Pattern-B: SeamlessTrack/run.sh
       - Pattern-C: SeamlessTrack/launch_px.sh
       `ros2 launch ... key:=value` の形で明示的にオーバーライドされている値。
       最優先。実運用値そのもの。
       ファイル名がブランチによって異なるため、存在する方を自動検出する。
    2. SeamlessTrack/src/px3_launch/launch/px3_system_launch.py
       - DeclareLaunchArgument の default_value。
         オーバーライドスクリプト側で指定されていないキーはここが実効値になる。
    3. SeamlessTrack/src/px2/include/inference.h
       - static constexpr で定義された定数（kYawDeadZoneDeg 等）。
         launchパラメータ化されていない値はここが正データ。

Pattern-BとPattern-Cでは「どのキーがオーバーライドスクリプト側にあり、
どのキーがlaunchデフォルトのままか」が異なる（例: kp_yaw は Pattern-Bでは
launchデフォルトのみ、Pattern-Cではrun.sh相当のスクリプトで明示指定）。
そのため個別に2つのマッピング表を持つのではなく、
「launchデフォルト」に「オーバーライドスクリプトの値」を上書きした
"実効値(effective value)" を1つ作り、それをyamlと突き合わせる。
この設計により、キーがどちらのファイルに書かれていてもブランチ間で
テストコード自体は無変更で動く。

実行方法:
    python -m unittest ci_checks/tests/test_sim_hil_param_contract.py -v

CIでの位置づけ:
    axis_pd_controller.hpp / px3_servo_node.h / inference.h や
    launchファイル、simulation/data/params/control_params.yaml のいずれかが
    変更された時にのみ sim-hil.yaml から呼ばれる想定（path filter側で制御）。
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path
from typing import Any

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]

# ブランチによってファイル名が異なる（Pattern-B: run.sh / Pattern-C: launch_px.sh）
# ため、存在する方を自動検出する。
OVERRIDE_SCRIPT_CANDIDATES = [
    REPO_ROOT / "SeamlessTrack" / "run.sh",
    REPO_ROOT / "SeamlessTrack" / "launch_px.sh",
]
LAUNCH_PY_PATH = (
    REPO_ROOT / "SeamlessTrack" / "src" / "px3_launch" / "launch" / "px3_system_launch.py"
)
INFERENCE_H_PATH = REPO_ROOT / "SeamlessTrack" / "src" / "px2" / "include" / "inference.h"
CONTROL_PARAMS_YAML_PATH = REPO_ROOT / "simulation" / "data" / "params" / "control_params.yaml"

FLOAT_RE = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"


def find_override_script() -> Path:
    for candidate in OVERRIDE_SCRIPT_CANDIDATES:
        if candidate.exists():
            return candidate
    names = ", ".join(str(c.relative_to(REPO_ROOT)) for c in OVERRIDE_SCRIPT_CANDIDATES)
    raise FileNotFoundError(f"override script not found. tried: {names}")


# ---------------------------------------------------------------------------
# パーサ群（regexベース。ROS2非依存でCIランナー上で完結させる）
# ---------------------------------------------------------------------------
def parse_key_value_overrides(path: Path) -> dict[str, float]:
    """`ros2 launch ... key:=value` から key -> float の辞書を作る。

    1行に複数の `key:=value` が並ぶケース（例: `xmax:=150.0 xmin:=30.0 \\`）
    があるため、行単位ではなくファイル全体に対して非アンカーで探索する。
    run.sh (Pattern-B) / launch_px.sh (Pattern-C) のどちらの書式にも対応する。
    """
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(rf"(\w+):=({FLOAT_RE})")
    return {name: float(value) for name, value in pattern.findall(text)}


def parse_launch_defaults(path: Path) -> dict[str, float]:
    r"""DeclareLaunchArgument("name", default_value="1.23") から name -> float。

    数値に変換できない default_value（文字列パスやboolなど）はスキップする。
    """
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        r'DeclareLaunchArgument\(\s*"(?P<name>\w+)"\s*,\s*default_value\s*=\s*'
        r'"(?P<default>[^"]*)"'
    )
    result: dict[str, float] = {}
    for m in pattern.finditer(text):
        raw = m.group("default")
        try:
            result[m.group("name")] = float(raw)
        except ValueError:
            continue  # 数値以外のデフォルト値（パス等）は対象外
    return result


def parse_inference_h_constants(path: Path) -> dict[str, float]:
    """`static constexpr <type> kName = value;` から kName -> float。"""
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        rf"static\s+constexpr\s+\w+\s+(?P<name>k\w+)\s*=\s*(?P<value>{FLOAT_RE})\s*;"
    )
    return {m.group("name"): float(m.group("value")) for m in pattern.finditer(text)}


def load_control_params_yaml(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as f:
        return yaml.safe_load(f)


def get_nested(d: dict[str, Any], dotted_key: str) -> Any:
    node: Any = d
    for part in dotted_key.split("."):
        node = node[part]
    return node


# ---------------------------------------------------------------------------
# 突き合わせマッピング: (yamlのdotted key, launchパラメータ名)
#   実効値 = launchデフォルト値をオーバーライドスクリプトの値で上書きしたもの。
#   どちらのファイルにキーが書かれているかはブランチによって異なってよい。
# ---------------------------------------------------------------------------
CONTROL_PARAM_MAPPING: list[tuple[str, str]] = [
    ("axes.yaw.center_deg", "x_center_deg"),
    ("axes.pitch.center_deg", "y_center_deg"),
    ("axes.yaw.max_deg", "xmax"),
    ("axes.yaw.min_deg", "xmin"),
    ("axes.pitch.max_deg", "ymax"),
    ("axes.pitch.min_deg", "ymin"),
    ("future_params.lost_max_frames", "lost_max_frames"),
    ("future_params.watch_fps", "watch_fps"),
    ("future_params.active_fps", "active_fps"),
    ("future_params.max_step_deg", "max_step_deg"),
    ("future_params.pd_gains.kp_yaw", "kp_yaw"),
    ("future_params.pd_gains.kd_yaw", "kd_yaw"),
    ("future_params.pd_gains.kp_pitch", "kp_pitch"),
    ("future_params.pd_gains.kd_pitch", "kd_pitch"),
    ("future_params.conf_threshold", "conf_threshold"),
]

INFERENCE_H_MAPPING: list[tuple[str, str]] = [
    ("future_params.dead_zone_deg.yaw", "kYawDeadZoneDeg"),
    ("future_params.dead_zone_deg.pitch", "kPitchDeadZoneDeg"),
]

TOLERANCE = 1e-6


class SimHilParamContractTest(unittest.TestCase):
    """simulation/data/params/control_params.yaml と実機の正データを突き合わせる。"""

    @classmethod
    def setUpClass(cls) -> None:
        cls.override_script_path = find_override_script()

        for path in (LAUNCH_PY_PATH, INFERENCE_H_PATH, CONTROL_PARAMS_YAML_PATH):
            if not path.exists():
                raise unittest.SkipTest(f"required file not found: {path}")

        launch_defaults = parse_launch_defaults(LAUNCH_PY_PATH)
        overrides = parse_key_value_overrides(cls.override_script_path)

        # 実効値 = デフォルトにオーバーライドを上書き（オーバーライド優先）
        cls.effective_params: dict[str, float] = {**launch_defaults, **overrides}
        cls.overridden_keys = set(overrides.keys())

        cls.inference_constants = parse_inference_h_constants(INFERENCE_H_PATH)
        cls.yaml_params = load_control_params_yaml(CONTROL_PARAMS_YAML_PATH)

    def _assert_matches(
        self,
        mapping: list[tuple[str, str]],
        source: dict[str, float],
        source_label: str,
        *,
        annotate_origin: bool = False,
    ) -> None:
        mismatches: list[str] = []
        missing: list[str] = []

        for yaml_key, source_key in mapping:
            if source_key not in source:
                missing.append(f"{source_label} に '{source_key}' が見つからない")
                continue
            try:
                yaml_value = get_nested(self.yaml_params, yaml_key)
            except KeyError:
                missing.append(f"control_params.yaml に '{yaml_key}' が見つからない")
                continue

            source_value = source[source_key]
            if abs(float(yaml_value) - source_value) > TOLERANCE:
                origin = ""
                if annotate_origin:
                    is_override = source_key in self.overridden_keys
                    origin = f" [由来: {'override script' if is_override else 'launch default'}]"
                mismatches.append(
                    f"{yaml_key} = {yaml_value} (yaml) != {source_value} "
                    f"({source_label}.{source_key}){origin}"
                )

        failures = missing + mismatches
        if failures:
            detail = "\n  - " + "\n  - ".join(failures)
            self.fail(f"{source_label} とのパラメータ不一致:{detail}")

    def test_effective_control_params_match_yaml(self) -> None:
        """launchデフォルト+オーバーライドの実効値と yaml の一致を確認する（最優先の正データ）。"""
        self._assert_matches(
            CONTROL_PARAM_MAPPING,
            self.effective_params,
            f"effective params ({self.override_script_path.name} + px3_system_launch.py defaults)",
            annotate_origin=True,
        )

    def test_inference_h_constants_match_yaml(self) -> None:
        """launchパラメータ化されていない定数（デッドゾーン等）と yaml の一致を確認する。"""
        self._assert_matches(INFERENCE_H_MAPPING, self.inference_constants, "inference.h")

    def test_yaw_pitch_range_is_physically_valid(self) -> None:
        """min_deg < center_deg < max_deg が両軸で成立することを確認する（契約以前の健全性チェック）。"""
        for axis in ("yaw", "pitch"):
            axis_params = self.yaml_params["axes"][axis]
            min_deg = axis_params["min_deg"]
            center_deg = axis_params["center_deg"]
            max_deg = axis_params["max_deg"]
            self.assertLess(min_deg, center_deg, f"{axis}: min_deg must be < center_deg")
            self.assertLess(center_deg, max_deg, f"{axis}: center_deg must be < max_deg")


if __name__ == "__main__":
    unittest.main()
