#!/usr/bin/env bash
# ============================================================
#  ci_checks/run_local.sh — GitHub Actions と同じチェックを手元で実行する
#    ./ci_checks/run_local.sh          チェックだけ
#    ./ci_checks/run_local.sh --fix    整形を自動で直してからチェック
#  必要: uv、clang-format（CI と同じ 14 を推奨: pip install clang-format==14.0.6）
# ============================================================
set -euo pipefail
cd "$(dirname "$0")/.."
PY_DIRS=(calibration ci_checks SeamlessTrack/src/px3_launch SeamlessTrack/bench)
mapfile -t CPP < <(git ls-files SeamlessTrack/src 2>/dev/null | grep -E '\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$' \
  || find SeamlessTrack/src -regex '.*\.\(c\|cc\|cpp\|cxx\|h\|hh\|hpp\|hxx\)$')

uv sync --frozen

if [[ "${1:-}" == "--fix" ]]; then
  clang-format -i "${CPP[@]}"
  uv run ruff format "${PY_DIRS[@]}"
  uv run ruff check --fix "${PY_DIRS[@]}"
fi

echo "== cpp-ci: clang-format";      clang-format --dry-run --Werror "${CPP[@]}"
echo "== python-lint: ruff format";  uv run ruff format --check "${PY_DIRS[@]}"
echo "== python-lint: ruff check";   uv run ruff check "${PY_DIRS[@]}"
echo "== unit tests";                uv run python -m unittest discover -s ci_checks/tests -p "test_*.py" -v
echo "== px-parameter-contract"
uv run python ci_checks/check_px_parameter_contract.py \
  --repo-root . \
  --allow-embedded-route "px2:px3=serial_port,baudrate,x_center_deg,y_center_deg,pir_pin" \
  --allow-default-override px2.device_path \
  --report /tmp/px-parameter-contract-report.md
echo "ALL CHECKS PASSED"
