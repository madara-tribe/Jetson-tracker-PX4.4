#!/usr/bin/env bash
# ============================================================
#  ci_checks/fix_ci.sh — GitHub Actions の CI エラーをまとめて直す
#
#  直すもの:
#    1. cpp-ci          : .clang-format が無い → 追加し、C++ を clang-format 14 で整形
#    2. px-parameter-contract : px3.pir_pin が許可リストに無い → ワークフローに追加
#                         （F07 で消すはずだった max_step_deg の除外も削除）
#    3. .gitignore が無い → 追加（engine / onnx / .venv などを git に入れない）
#  最後に ./ci_checks/run_local.sh で、CI と同じチェックを手元で実行する。
#
#  使い方（リポジトリのどこからでも）:
#    ./ci_checks/fix_ci.sh
#  必要: uv（clang-format 14 は uvx で自動的に取ってくる）
# ============================================================
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

CF_VERSION="14.0.6" # CI（ubuntu-22.04 の apt）と同じ版
if command -v clang-format >/dev/null && clang-format --version | grep -q "version 14\."; then
  CF=(clang-format)
else
  CF=(uvx --from "clang-format==${CF_VERSION}" clang-format)
fi
echo "== clang-format: $("${CF[@]}" --version)"

# ---------- 1. .clang-format ----------
if [[ ! -f .clang-format ]]; then
  echo "== .clang-format を追加"
  cat > .clang-format <<'EOF'
---
Language: Cpp
BasedOnStyle: LLVM
Standard: c++17
IndentWidth: 2
TabWidth: 2
UseTab: Never
ColumnLimit: 100
BreakBeforeBraces: Attach
PointerAlignment: Left
ReferenceAlignment: Left
DerivePointerAlignment: false
SortIncludes: CaseSensitive
IncludeBlocks: Preserve
AllowShortFunctionsOnASingleLine: Empty
AllowShortIfStatementsOnASingleLine: Never
AllowShortLoopsOnASingleLine: false
...
EOF
fi

# ---------- 2. C++ の整形 ----------
mapfile -t CPP < <(git ls-files SeamlessTrack/src | grep -E '\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$')
# まだ git add していない新しい C++ ファイルも対象にする
mapfile -t NEW < <(git ls-files --others --exclude-standard SeamlessTrack/src |
  grep -E '\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$' || true)
CPP+=("${NEW[@]}")
echo "== C++ を整形（${#CPP[@]} ファイル）"
"${CF[@]}" -i "${CPP[@]}"
git diff --stat -- "${CPP[@]}" || true

# ---------- 3. px-parameter-contract ワークフロー ----------
WF=.github/workflows/px-parameter-contract.yml
echo "== ${WF} を修正"
python3 - "$WF" <<'PY'
import re
import sys

path = sys.argv[1]
s = open(path, encoding="utf-8").read()
orig = s
# pir_pin を許可リストに追加（px2 から px3 へ渡す 5 つ目のパラメータ。F08）
s = re.sub(
    r'--allow-embedded-route "px2:px3=([^"]*)"',
    lambda m: m.group(0)
    if "pir_pin" in m.group(1).split(",")
    else f'--allow-embedded-route "px2:px3={m.group(1)},pir_pin"',
    s,
)
# max_step_deg の除外を削除（F07 で既定値を 3.0 に揃えたので不要）
s = re.sub(r"\n[ \t]*--allow-default-override px2\.max_step_deg \\", "", s)
# ヘッダーの古い説明を更新
s = s.replace(
    "#        max_step_deg は調整で頻繁に変えるため、既定値の差を許可（チェック対象外）。",
    "#        px3 のパラメータは px2 経由で渡す（serial_port, baudrate, x/y_center_deg, pir_pin）。",
)
if s != orig:
    open(path, "w", encoding="utf-8").write(s)
    print("  updated")
else:
    print("  no change")
PY
grep -n "allow-" "$WF"

# ---------- 4. .gitignore ----------
if [[ ! -f .gitignore ]]; then
  echo "== .gitignore を追加"
  cat > .gitignore <<'EOF'
# ---- ROS 2 / colcon（コンテナ内で作られる）----
SeamlessTrack/build/
SeamlessTrack/install/
SeamlessTrack/log/

# ---- 重いファイル（ローカルに置いて cp する。README「重いファイル」参照）----
SeamlessTrack/src/px2/weights/*.onnx
SeamlessTrack/engines/*
!SeamlessTrack/engines/.gitkeep
SeamlessTrack/trt_cache/
Docker/third_party/*
!Docker/third_party/.gitkeep

# ---- 計測結果・実行時の出力 ----
SeamlessTrack/bench/results/
SeamlessTrack/frames/
SeamlessTrack/src/px2/data/*.png

# ---- Python / ツール ----
__pycache__/
*.pyc
.venv/
.ruff_cache/
.pytest_cache/

# ---- OS ----
.DS_Store
__MACOSX/
EOF
fi
mkdir -p SeamlessTrack/engines && touch SeamlessTrack/engines/.gitkeep

# ---------- 5. CI と同じチェック ----------
echo "== CI と同じチェックを実行"
PATH="$(dirname "$(command -v uv)"):$PATH"
if ! clang-format --version 2>/dev/null | grep -q "version 14\."; then
  # run_local.sh は clang-format を直接呼ぶので、14 を一時的に PATH の先頭に置く
  TMPBIN="$(mktemp -d)"
  printf '#!/usr/bin/env bash\nexec uvx --from clang-format==%s clang-format "$@"\n' "$CF_VERSION" \
    > "$TMPBIN/clang-format"
  chmod +x "$TMPBIN/clang-format"
  PATH="$TMPBIN:$PATH"
fi
"$ROOT/ci_checks/run_local.sh"

echo
echo "次のコマンドでコミットする:"
echo "  git add -A .clang-format .gitignore .github ci_checks SeamlessTrack/src SeamlessTrack/engines/.gitkeep"
echo "  git commit -m 'Fix CI: clang-format, pir_pin route, gitignore'"
