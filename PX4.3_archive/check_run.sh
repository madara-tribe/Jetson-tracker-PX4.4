## c++ format check
clang-format --dry-run --Werror \
  $(git ls-files SeamlessTrack/src | grep -E '\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$')

## auto-correct c++ format
clang-format -i \
  $(git ls-files SeamlessTrack/src | grep -E '\.(c|cc|cpp|cxx|h|hh|hpp|hxx)$')
clang-format -i SeamlessTrack/src/px3/src/watch_node.cpp
clang-format -i SeamlessTrack/src/px3/include/px3/watch_node.h
clang-format -i SeamlessTrack/src/px3/src/px3_servo_node.cpp
## python format /lint/unit test 
uv sync --frozen

uv run ruff format calibration ci_checks SeamlessTrack/src/px3_launch

uv run ruff check --fix calibration ci_checks SeamlessTrack/src/px3_launch

uv run python -m unittest discover \
  -s ci_checks/tests \
  -p "test_*.py" \
  -v

## after modification do CI check by hand
uv run ruff format --check calibration ci_checks SeamlessTrack/src/px3_launch
uv run ruff check calibration ci_checks SeamlessTrack/src/px3_launch

## cide check on local
python3 -m unittest discover -s ci_checks/tests -p "test_*.py" -v

python3 ci_checks/check_px_parameter_contract.py \
  --repo-root . \
  --allow-embedded-route "px2:px3=serial_port,baudrate,x_center_deg,y_center_deg" \
  --report /tmp/px-parameter-contract-report.md
