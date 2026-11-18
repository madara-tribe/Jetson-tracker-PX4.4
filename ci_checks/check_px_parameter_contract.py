#!/usr/bin/env python3
"""Check the ROS2 launch-to-C++ parameter contract for the PX system.

The checker is intentionally independent of ROS2 so it can run in a normal
GitHub Actions runner.  It verifies that launch-exposed runtime parameters and
C++ ``declare_parameter`` definitions remain synchronized.
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
from collections import defaultdict
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_LAUNCH_PATH = Path("SeamlessTrack/src/px3_launch/launch/px3_system_launch.py")
DEFAULT_CPP_SOURCES: dict[str, tuple[Path, ...]] = {
    "px2": (
        Path("SeamlessTrack/src/px2/src/inference.cpp"),
        Path("SeamlessTrack/src/px2/include/inference.h"),
    ),
    "px3": (
        Path("SeamlessTrack/src/px3/src/px3_servo_node.cpp"),
        Path("SeamlessTrack/src/px3/include/px3/px3_servo_node.h"),
    ),
    "watch_node": (
        Path("SeamlessTrack/src/px3/src/watch_node.cpp"),
        Path("SeamlessTrack/src/px3/include/px3/watch_node.h"),
    ),
}
DEFAULT_IGNORED_LAUNCH_ARGUMENTS = frozenset({"px2_pkg", "px2_exec", "watch_pkg", "watch_exec"})

_CPP_PARAMETER_RE = re.compile(
    r"declare_parameter\s*<\s*(?P<cpp_type>[^>]+?)\s*>\s*"
    r"\(\s*\"(?P<name>[^\"]+)\"\s*,\s*"
    r"(?P<default>\"(?:\\.|[^\"\\])*\"|true|false|"
    r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)\s*\)",
    re.MULTILINE,
)


@dataclass(frozen=True)
class CppParameter:
    node: str
    name: str
    cpp_type: str
    default: object
    raw_default: str
    path: Path
    line: int


@dataclass(frozen=True)
class LaunchArgument:
    name: str
    default: object | None
    raw_default: str | None
    line: int


@dataclass(frozen=True)
class LaunchBinding:
    node: str
    parameter: str
    launch_argument: str | None
    launch_default: object | None
    raw_launch_default: str | None
    line: int


@dataclass(frozen=True)
class Issue:
    code: str
    message: str
    severity: str = "error"


@dataclass
class ContractResult:
    launch_path: Path
    cpp_sources: dict[str, list[Path]]
    launch_arguments: dict[str, LaunchArgument] = field(default_factory=dict)
    launch_bindings: list[LaunchBinding] = field(default_factory=list)
    cpp_parameters: dict[str, dict[str, CppParameter]] = field(default_factory=dict)
    issues: list[Issue] = field(default_factory=list)
    ignored_launch_arguments: set[str] = field(default_factory=set)
    allowed_embedded_routes: dict[tuple[str, str], set[str]] = field(default_factory=dict)

    @property
    def errors(self) -> list[Issue]:
        return [issue for issue in self.issues if issue.severity == "error"]

    @property
    def passed(self) -> bool:
        return not self.errors


class ContractParseError(RuntimeError):
    """Raised when an input file cannot be parsed into the expected contract."""


def _function_name(node: ast.AST) -> str | None:
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        return node.attr
    return None


def _ast_literal(node: ast.AST | None) -> tuple[object | None, str | None]:
    if node is None:
        return None, None
    try:
        value = ast.literal_eval(node)
    except (ValueError, TypeError):
        return None, ast.unparse(node)
    return _coerce_text_default(value), repr(value)


def _coerce_text_default(value: object) -> object:
    """Coerce ROS launch textual scalar defaults to comparable Python values."""
    if not isinstance(value, str):
        return value
    lowered = value.strip().lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    if re.fullmatch(r"[-+]?\d+", value.strip()):
        return int(value.strip())
    if re.fullmatch(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?", value.strip()):
        return float(value.strip())
    return value


def _cpp_default(raw: str) -> object:
    if raw == "true":
        return True
    if raw == "false":
        return False
    if raw.startswith('"'):
        return ast.literal_eval(raw)
    if any(ch in raw.lower() for ch in (".", "e")):
        return float(raw)
    return int(raw)


def _line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _strip_cpp_comments(text: str) -> str:
    """Remove C++ comments while preserving strings and line numbers."""
    result: list[str] = []
    i = 0
    in_string = False
    in_char = False
    in_line_comment = False
    in_block_comment = False
    escape = False

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if in_line_comment:
            if ch == "\n":
                result.append(ch)
                in_line_comment = False
            else:
                result.append(" ")
            i += 1
            continue

        if in_block_comment:
            if ch == "*" and nxt == "/":
                result.extend("  ")
                i += 2
                in_block_comment = False
            else:
                result.append("\n" if ch == "\n" else " ")
                i += 1
            continue

        if in_string:
            result.append(ch)
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            result.append(ch)
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == "'":
                in_char = False
            i += 1
            continue

        if ch == "/" and nxt == "/":
            result.extend("  ")
            i += 2
            in_line_comment = True
            continue

        if ch == "/" and nxt == "*":
            result.extend("  ")
            i += 2
            in_block_comment = True
            continue

        if ch == '"':
            in_string = True
        elif ch == "'":
            in_char = True
        result.append(ch)
        i += 1

    return "".join(result)


def extract_cpp_parameters(
    node: str, paths: Sequence[Path]
) -> tuple[dict[str, CppParameter], list[Issue]]:
    parameters: dict[str, CppParameter] = {}
    issues: list[Issue] = []
    for path in paths:
        if not path.exists():
            raise ContractParseError(f"C++ input file not found: {path}")
        text = path.read_text(encoding="utf-8", errors="replace")
        search_text = _strip_cpp_comments(text)
        for match in _CPP_PARAMETER_RE.finditer(search_text):
            name = match.group("name")
            param = CppParameter(
                node=node,
                name=name,
                cpp_type=match.group("cpp_type").strip(),
                default=_cpp_default(match.group("default")),
                raw_default=match.group("default"),
                path=path,
                line=_line_number(search_text, match.start()),
            )
            if name in parameters:
                previous = parameters[name]
                issues.append(
                    Issue(
                        "DUPLICATE_CPP_PARAMETER",
                        f"`{node}.{name}` is declared more than once: "
                        f"`{previous.path}:{previous.line}` and `{path}:{param.line}`.",
                    )
                )
            else:
                parameters[name] = param
    return parameters, issues


def _resolve_launch_configuration(expr: ast.AST) -> str | None:
    """Return the LaunchConfiguration argument referenced by an expression."""
    if isinstance(expr, ast.Call):
        name = _function_name(expr.func)
        if name == "LaunchConfiguration" and expr.args:
            value, _ = _ast_literal(expr.args[0])
            return value if isinstance(value, str) else None
        if name == "ParameterValue" and expr.args:
            return _resolve_launch_configuration(expr.args[0])
    return None


def extract_launch_contract(path: Path) -> tuple[dict[str, LaunchArgument], list[LaunchBinding]]:
    if not path.exists():
        raise ContractParseError(f"Launch file not found: {path}")
    text = path.read_text(encoding="utf-8", errors="replace")
    try:
        tree = ast.parse(text, filename=str(path))
    except SyntaxError as exc:
        raise ContractParseError(f"Unable to parse launch file `{path}`: {exc}") from exc

    arguments: dict[str, LaunchArgument] = {}
    bindings: list[LaunchBinding] = []

    for call in (node for node in ast.walk(tree) if isinstance(node, ast.Call)):
        if _function_name(call.func) != "DeclareLaunchArgument" or not call.args:
            continue
        name, _ = _ast_literal(call.args[0])
        if not isinstance(name, str):
            continue
        default_node = next((kw.value for kw in call.keywords if kw.arg == "default_value"), None)
        default, raw_default = _ast_literal(default_node)
        arguments[name] = LaunchArgument(name, default, raw_default, call.lineno)

    for call in (node for node in ast.walk(tree) if isinstance(node, ast.Call)):
        if _function_name(call.func) != "Node":
            continue
        name_node = next((kw.value for kw in call.keywords if kw.arg == "name"), None)
        ros_node, _ = _ast_literal(name_node)
        node_name = ros_node if isinstance(ros_node, str) else "<dynamic-node-name>"
        parameters_node = next((kw.value for kw in call.keywords if kw.arg == "parameters"), None)
        if not isinstance(parameters_node, (ast.List, ast.Tuple)):
            continue
        for block in parameters_node.elts:
            if not isinstance(block, ast.Dict):
                continue
            for key_node, value_node in zip(block.keys, block.values, strict=True):
                key, _ = _ast_literal(key_node)
                if not isinstance(key, str):
                    continue
                argument_name = _resolve_launch_configuration(value_node)
                argument = arguments.get(argument_name) if argument_name else None
                bindings.append(
                    LaunchBinding(
                        node=node_name,
                        parameter=key,
                        launch_argument=argument_name,
                        launch_default=argument.default if argument else None,
                        raw_launch_default=argument.raw_default if argument else None,
                        line=getattr(key_node, "lineno", call.lineno),
                    )
                )
    return arguments, bindings


def _fmt(value: object | None) -> str:
    if isinstance(value, str):
        return f'`"{value}"`'
    if value is None:
        return "`<dynamic/unknown>`"
    return f"`{value!r}`"


def analyze_contract(
    launch_path: Path,
    cpp_sources: Mapping[str, Sequence[Path]],
    *,
    ignored_launch_arguments: Iterable[str] = DEFAULT_IGNORED_LAUNCH_ARGUMENTS,
    allow_default_overrides: Iterable[str] = (),
    allow_embedded_routes: Mapping[tuple[str, str], Iterable[str]] | None = None,
) -> ContractResult:
    ignored = set(ignored_launch_arguments)
    allowed_overrides = set(allow_default_overrides)
    embedded_routes = {
        route: set(parameter_names)
        for route, parameter_names in (allow_embedded_routes or {}).items()
    }
    result = ContractResult(
        launch_path=launch_path,
        cpp_sources={node: list(paths) for node, paths in cpp_sources.items()},
        ignored_launch_arguments=ignored,
        allowed_embedded_routes=embedded_routes,
    )
    result.launch_arguments, result.launch_bindings = extract_launch_contract(launch_path)

    for node, paths in cpp_sources.items():
        params, issues = extract_cpp_parameters(node, paths)
        result.cpp_parameters[node] = params
        result.issues.extend(issues)

    binding_by_node: dict[str, dict[str, LaunchBinding]] = defaultdict(dict)
    for binding in result.launch_bindings:
        existing = binding_by_node[binding.node].get(binding.parameter)
        if existing:
            result.issues.append(
                Issue(
                    "DUPLICATE_LAUNCH_BINDING",
                    f"`{binding.node}.{binding.parameter}` is configured more than once in the launch file.",
                )
            )
        binding_by_node[binding.node][binding.parameter] = binding
        if binding.launch_argument is None:
            result.issues.append(
                Issue(
                    "DYNAMIC_LAUNCH_BINDING",
                    f"`{binding.node}.{binding.parameter}` does not map to a recognizable "
                    "`LaunchConfiguration(...)`; it cannot be checked deterministically.",
                )
            )

    bound_arguments = {b.launch_argument for b in result.launch_bindings if b.launch_argument}
    for arg in sorted(set(result.launch_arguments) - bound_arguments - ignored):
        result.issues.append(
            Issue(
                "UNUSED_LAUNCH_ARGUMENT",
                f"Launch argument `{arg}` is declared but not passed to any ROS node parameter.",
            )
        )

    cpp_unique_names = {name for params in result.cpp_parameters.values() for name in params}
    launch_unique_names = {binding.parameter for binding in result.launch_bindings}
    if len(launch_unique_names) != len(cpp_unique_names):
        result.issues.append(
            Issue(
                "UNIQUE_PARAMETER_COUNT_MISMATCH",
                f"Launch routes {len(launch_unique_names)} unique runtime parameter(s), "
                f"but C++ declares {len(cpp_unique_names)} unique parameter name(s).",
            )
        )
    missing_unique = sorted(cpp_unique_names - launch_unique_names)
    extra_unique = sorted(launch_unique_names - cpp_unique_names)
    if missing_unique:
        result.issues.append(
            Issue(
                "MISSING_PUBLIC_PARAMETERS",
                "C++ parameter name(s) not routed by launch: "
                + ", ".join(f"`{name}`" for name in missing_unique)
                + ".",
            )
        )
    if extra_unique:
        result.issues.append(
            Issue(
                "UNKNOWN_LAUNCH_PARAMETERS",
                "Launch routes parameter name(s) not declared in C++: "
                + ", ".join(f"`{name}`" for name in extra_unique)
                + ".",
            )
        )

    def embedded_binding_for_owner(
        owner_node: str, parameter: str
    ) -> tuple[str, LaunchBinding] | None:
        """Resolve a launch binding routed through a host for one embedded node."""
        for (host_node, embedded_node), allowed_parameters in embedded_routes.items():
            if embedded_node != owner_node or parameter not in allowed_parameters:
                continue
            host_binding = binding_by_node.get(host_node, {}).get(parameter)
            if host_binding is not None:
                return host_node, host_binding
        return None

    def embedded_owner_for_host_binding(host_node: str, parameter: str) -> str | None:
        """Return the allowed embedded owner for an otherwise wrong-node binding."""
        for (configured_host, embedded_node), allowed_parameters in embedded_routes.items():
            if configured_host != host_node or parameter not in allowed_parameters:
                continue
            if parameter in result.cpp_parameters.get(embedded_node, {}):
                return embedded_node
        return None

    reported_embedded_routes: set[tuple[str, str, str]] = set()
    for node, declared in result.cpp_parameters.items():
        routed = binding_by_node.get(node, {})
        for name, cpp_param in declared.items():
            binding = routed.get(name)
            if binding is None:
                delegated = embedded_binding_for_owner(node, name)
                if delegated is None:
                    result.issues.append(
                        Issue(
                            "PARAMETER_NOT_ROUTED_TO_OWNER",
                            f"`{node}.{name}` is declared in C++ but launch does not configure it on node `{node}`.",
                        )
                    )
                    continue
                host_node, binding = delegated
                route_key = (host_node, node, name)
                if route_key not in reported_embedded_routes:
                    result.issues.append(
                        Issue(
                            "ALLOWED_EMBEDDED_PARAMETER_ROUTE",
                            f"Launch route `{host_node}.{name}` is accepted for embedded C++ node "
                            f"`{node}` by explicit CI configuration.",
                            severity="info",
                        )
                    )
                    reported_embedded_routes.add(route_key)
            key = f"{node}.{name}"
            if key not in allowed_overrides and binding.launch_default != cpp_param.default:
                result.issues.append(
                    Issue(
                        "DEFAULT_VALUE_MISMATCH",
                        f"`{key}` default differs: launch={_fmt(binding.launch_default)}, "
                        f"C++={_fmt(cpp_param.default)} "
                        f"(`{cpp_param.path}:{cpp_param.line}`).",
                    )
                )
        for name in sorted(set(routed) - set(declared)):
            embedded_owner = embedded_owner_for_host_binding(node, name)
            if embedded_owner is not None:
                continue
            result.issues.append(
                Issue(
                    "PARAMETER_ROUTED_TO_WRONG_NODE",
                    f"Launch configures `{node}.{name}`, but `{name}` is not declared by C++ node `{node}`.",
                )
            )

    for node in sorted(set(binding_by_node) - set(result.cpp_parameters)):
        result.issues.append(
            Issue(
                "UNKNOWN_LAUNCH_NODE",
                f"Launch configures node `{node}`, but no C++ parameter source set was configured for it.",
            )
        )

    return result


def render_markdown(result: ContractResult) -> str:
    launch_names = {binding.parameter for binding in result.launch_bindings}
    cpp_names = {name for params in result.cpp_parameters.values() for name in params}
    status = "✅ PASS" if result.passed else "❌ FAIL"
    rows = [
        ("Overall", status, f"{len(result.errors)} blocking issue(s)"),
        (
            "Unique runtime parameter count",
            "✅" if len(launch_names) == len(cpp_names) else "❌",
            f"launch={len(launch_names)}, C++={len(cpp_names)}",
        ),
        (
            "Launch arguments",
            "ℹ️",
            f"declared={len(result.launch_arguments)}, ignored launch-only={len(result.ignored_launch_arguments)}",
        ),
        (
            "Launch runtime bindings",
            "ℹ️",
            f"bindings={len(result.launch_bindings)}",
        ),
    ]
    for node, parameters in sorted(result.cpp_parameters.items()):
        routed = sum(1 for binding in result.launch_bindings if binding.node == node)
        rows.append(
            (f"Node `{node}`", "ℹ️", f"C++ declared={len(parameters)}, launch routed={routed}")
        )

    lines = [
        "## PX Parameter Contract Check",
        "",
        f"- Launch file: `{result.launch_path}`",
        "",
        "| Check | Result | Detail |",
        "|---|---:|---|",
    ]
    for check, check_status, detail in rows:
        lines.append(f"| {check} | {check_status} | {detail} |")

    if result.allowed_embedded_routes:
        lines.extend(
            [
                "",
                "### Allowed embedded parameter routes",
                "",
                "| Host node | Embedded node | Allowed parameter names |",
                "|---|---|---|",
            ]
        )
        for (host_node, embedded_node), parameters in sorted(
            result.allowed_embedded_routes.items()
        ):
            names = ", ".join(f"`{name}`" for name in sorted(parameters))
            lines.append(f"| `{host_node}` | `{embedded_node}` | {names} |")

    if result.issues:
        lines.extend(
            ["", "### Issues / Notes", "", "| Code | Severity | Detail |", "|---|---:|---|"]
        )
        for issue in result.issues:
            icon = "❌" if issue.severity == "error" else ("ℹ️" if issue.severity == "info" else "⚠️")
            lines.append(f"| `{issue.code}` | {icon} | {issue.message} |")
    else:
        lines.extend(["", "No parameter-contract inconsistencies were detected."])

    lines.extend(
        [
            "",
            "### Checked inputs",
            "",
            f"- `{result.launch_path}`",
        ]
    )
    for node, paths in sorted(result.cpp_sources.items()):
        for path in paths:
            lines.append(f"- `{path}` (`{node}`)")
    lines.append("")
    return "\n".join(lines)


def _parse_cpp_node_arg(value: str, root: Path) -> tuple[str, list[Path]]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("--cpp-node must be formatted as NODE=PATH[,PATH...]")
    node, paths_text = value.split("=", 1)
    paths = [root / part.strip() for part in paths_text.split(",") if part.strip()]
    if not node.strip() or not paths:
        raise argparse.ArgumentTypeError("--cpp-node must include a node and at least one path")
    return node.strip(), paths


def _parse_embedded_route_arg(value: str) -> tuple[tuple[str, str], set[str]]:
    if "=" not in value or ":" not in value.split("=", 1)[0]:
        raise argparse.ArgumentTypeError(
            "--allow-embedded-route must be formatted as HOST:EMBEDDED=PARAM[,PARAM...]"
        )
    nodes_text, parameters_text = value.split("=", 1)
    host_node, embedded_node = (part.strip() for part in nodes_text.split(":", 1))
    parameters = {part.strip() for part in parameters_text.split(",") if part.strip()}
    if not host_node or not embedded_node or not parameters:
        raise argparse.ArgumentTypeError(
            "--allow-embedded-route must include host node, embedded node, and parameter names"
        )
    return (host_node, embedded_node), parameters


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root", type=Path, default=Path("."), help="Repository root directory."
    )
    parser.add_argument("--launch-file", type=Path, default=DEFAULT_LAUNCH_PATH)
    parser.add_argument(
        "--cpp-node",
        action="append",
        default=[],
        metavar="NODE=PATH[,PATH...]",
        help="Override C++ input paths for one node. May be supplied multiple times.",
    )
    parser.add_argument(
        "--ignore-launch-argument",
        action="append",
        default=[],
        help="Additional launch-only argument not expected to map to a ROS parameter.",
    )
    parser.add_argument(
        "--allow-default-override",
        action="append",
        default=[],
        metavar="NODE.PARAMETER",
        help="Permit an intentional launch/C++ default difference for one routed parameter.",
    )
    parser.add_argument(
        "--allow-embedded-route",
        action="append",
        default=[],
        metavar="HOST:EMBEDDED=PARAM[,PARAM...]",
        help=(
            "Permit selected parameters for an embedded node to be configured on its host node; "
            "default-value checking still applies to the embedded declaration."
        ),
    )
    parser.add_argument("--report", type=Path, help="Write a Markdown report to this path.")
    args = parser.parse_args(argv)

    root = args.repo_root.resolve()
    launch_path = root / args.launch_file
    if args.cpp_node:
        cpp_sources: dict[str, list[Path]] = {}
        for spec in args.cpp_node:
            node, paths = _parse_cpp_node_arg(spec, root)
            cpp_sources[node] = paths
    else:
        cpp_sources = {
            node: [root / relative for relative in paths]
            for node, paths in DEFAULT_CPP_SOURCES.items()
        }
    ignored = DEFAULT_IGNORED_LAUNCH_ARGUMENTS | set(args.ignore_launch_argument)
    embedded_routes: dict[tuple[str, str], set[str]] = {}
    for value in args.allow_embedded_route:
        try:
            route, parameter_names = _parse_embedded_route_arg(value)
        except argparse.ArgumentTypeError as exc:
            parser.error(str(exc))
        embedded_routes.setdefault(route, set()).update(parameter_names)

    try:
        result = analyze_contract(
            launch_path,
            cpp_sources,
            ignored_launch_arguments=ignored,
            allow_default_overrides=args.allow_default_override,
            allow_embedded_routes=embedded_routes,
        )
    except ContractParseError as exc:
        print(f"PX parameter contract check failed to parse inputs: {exc}", file=sys.stderr)
        return 2

    report = render_markdown(result)
    print(report)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(report, encoding="utf-8")
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
