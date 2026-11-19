from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parents[1] / "check_px_parameter_contract.py"
SPEC = importlib.util.spec_from_file_location("px_contract", MODULE_PATH)
assert SPEC and SPEC.loader
px_contract = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = px_contract
SPEC.loader.exec_module(px_contract)


class PxParameterContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write(self, relative: str, content: str) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def run_contract(self, launch: str, cpp_sources: dict[str, str], **kwargs):
        launch_path = self.write("system_launch.py", launch)
        sources = {
            node: [self.write(f"{node}.cpp", content)] for node, content in cpp_sources.items()
        }
        return px_contract.analyze_contract(launch_path, sources, **kwargs)

    def test_aligned_contract_passes(self) -> None:
        result = self.run_contract(
            """
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    arg_device = DeclareLaunchArgument("device_path", default_value="/dev/video0")
    arg_width = DeclareLaunchArgument("watch_width", default_value="320")
    node = Node(name="px2", parameters=[{
        "device_path": LaunchConfiguration("device_path"),
        "watch_width": LaunchConfiguration("watch_width"),
    }])
""",
            {
                "px2": """
void build() {
  declare_parameter<std::string>("device_path", "/dev/video0");
  declare_parameter<int>("watch_width", 320);
}
"""
            },
        )
        self.assertTrue(result.passed, px_contract.render_markdown(result))

    def test_alias_from_camera_path_to_device_path_is_resolved_by_binding(self) -> None:
        result = self.run_contract(
            """
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    camera = DeclareLaunchArgument("camera_path", default_value="/dev/video0")
    node = Node(name="px2", parameters=[{"device_path": LaunchConfiguration("camera_path")}])
""",
            {
                "px2": 'void build() { declare_parameter<std::string>("device_path", "/dev/video0"); }'
            },
        )
        self.assertTrue(result.passed, px_contract.render_markdown(result))

    def test_missing_runtime_parameter_fails_count_and_coverage(self) -> None:
        result = self.run_contract(
            """
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    width = DeclareLaunchArgument("watch_width", default_value="320")
    node = Node(name="px2", parameters=[{"watch_width": LaunchConfiguration("watch_width")}])
""",
            {
                "px2": """
void build() {
  declare_parameter<int>("watch_width", 320);
  declare_parameter<int>("active_width", 1920);
}
"""
            },
        )
        codes = {issue.code for issue in result.errors}
        self.assertIn("UNIQUE_PARAMETER_COUNT_MISMATCH", codes)
        self.assertIn("MISSING_PUBLIC_PARAMETERS", codes)
        self.assertIn("PARAMETER_NOT_ROUTED_TO_OWNER", codes)

    def test_parameter_routed_to_wrong_node_fails(self) -> None:
        result = self.run_contract(
            """
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    serial = DeclareLaunchArgument("serial_port", default_value="/dev/ttyACM0")
    node = Node(name="px2", parameters=[{"serial_port": LaunchConfiguration("serial_port")}])
""",
            {
                "px2": "void build() {}",
                "px3": 'void build() { declare_parameter<std::string>("serial_port", "/dev/ttyACM0"); }',
            },
        )
        codes = {issue.code for issue in result.errors}
        self.assertIn("PARAMETER_ROUTED_TO_WRONG_NODE", codes)
        self.assertIn("PARAMETER_NOT_ROUTED_TO_OWNER", codes)

    def test_targeted_embedded_route_allows_serial_port_on_px2_for_px3(self) -> None:
        result = self.run_contract(
            """
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    serial = DeclareLaunchArgument("serial_port", default_value="/dev/ttyACM0")
    node = Node(name="px2", parameters=[{"serial_port": LaunchConfiguration("serial_port")}])
""",
            {
                "px2": "void build() {}",
                "px3": 'void build() { declare_parameter<std::string>("serial_port", "/dev/ttyACM0"); }',
            },
            allow_embedded_routes={("px2", "px3"): {"serial_port"}},
        )
        codes = {issue.code for issue in result.errors}
        self.assertNotIn("PARAMETER_ROUTED_TO_WRONG_NODE", codes)
        self.assertNotIn("PARAMETER_NOT_ROUTED_TO_OWNER", codes)
        self.assertTrue(result.passed, px_contract.render_markdown(result))

    def test_targeted_embedded_route_still_checks_px3_default_value(self) -> None:
        result = self.run_contract(
            """
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    center = DeclareLaunchArgument("y_center_deg", default_value="90.0")
    node = Node(name="px2", parameters=[{"y_center_deg": LaunchConfiguration("y_center_deg")}])
""",
            {
                "px2": 'void build() { declare_parameter<double>("y_center_deg", 90.0); }',
                "px3": 'void build() { declare_parameter<double>("y_center_deg", 60.0); }',
            },
            allow_embedded_routes={("px2", "px3"): {"y_center_deg"}},
        )
        errors = [
            issue.message for issue in result.errors if issue.code == "DEFAULT_VALUE_MISMATCH"
        ]
        self.assertTrue(any("px3.y_center_deg" in message for message in errors))
        self.assertNotIn("PARAMETER_NOT_ROUTED_TO_OWNER", {issue.code for issue in result.errors})

    def test_default_mismatch_can_be_explicitly_allowed(self) -> None:
        launch_path = self.write(
            "system_launch.py",
            """
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    angle = DeclareLaunchArgument("xmax", default_value="350.0")
    node = Node(name="px2", parameters=[{"xmax": LaunchConfiguration("xmax")}])
""",
        )
        source = self.write("px2.cpp", 'void build() { declare_parameter<double>("xmax", 180.0); }')
        failed = px_contract.analyze_contract(launch_path, {"px2": [source]})
        self.assertIn("DEFAULT_VALUE_MISMATCH", {issue.code for issue in failed.errors})
        permitted = px_contract.analyze_contract(
            launch_path,
            {"px2": [source]},
            allow_default_overrides={"px2.xmax"},
        )
        self.assertTrue(permitted.passed, px_contract.render_markdown(permitted))

    def test_cpp_parameters_inside_comments_are_ignored(self) -> None:
        result = self.run_contract(
            """
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    width = DeclareLaunchArgument("watch_width", default_value="320")
    node = Node(name="px2", parameters=[{"watch_width": LaunchConfiguration("watch_width")}])
""",
            {
                "px2": """
void build() {
  // declare_parameter<int>("motion_diff_thresh", 25);
  /*
   declare_parameter<double>("motion_ratio_thresh", 0.01);
  */
  const char *url = "http://example.test/not_a_comment";
  (void)url;
  declare_parameter<int>("watch_width", 320);
}
"""
            },
        )
        self.assertTrue(result.passed, px_contract.render_markdown(result))

    def test_default_ignored_launch_arguments_include_watch_pkg_exec(self) -> None:
        self.assertIn("watch_pkg", px_contract.DEFAULT_IGNORED_LAUNCH_ARGUMENTS)
        self.assertIn("watch_exec", px_contract.DEFAULT_IGNORED_LAUNCH_ARGUMENTS)

    def test_default_cpp_sources_include_watch_node(self) -> None:
        self.assertIn("watch_node", px_contract.DEFAULT_CPP_SOURCES)

    def test_watch_node_parameter_is_checked_when_source_is_registered(self) -> None:
        result = self.run_contract(
            """
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.parameter_descriptions import ParameterValue

def generate_launch_description():
    debounce = DeclareLaunchArgument("pir_debounce_ms", default_value="200")
    node = Node(name="watch_node", parameters=[{
        "pir_debounce_ms": ParameterValue(LaunchConfiguration("pir_debounce_ms"), value_type=int),
    }])
""",
            {
                "watch_node": """
void build() {
  declare_parameter<int>("pir_debounce_ms", 200);
}
"""
            },
        )
        self.assertTrue(result.passed, px_contract.render_markdown(result))

    def test_named_constant_default_is_resolved(self) -> None:
        launch = """
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    pin = DeclareLaunchArgument("pir_pin", default_value="2")
    node = Node(name="px3", parameters=[{
        "pir_pin": ParameterValue(LaunchConfiguration("pir_pin"), value_type=int),
    }])
"""
        result = self.run_contract(
            launch,
            {
                "px3": """
inline constexpr int PIR_PIN = 2; // SB612B OUT
void build() {
  const int pir_pin = declare_parameter<int>("pir_pin", PIR_PIN);
}
"""
            },
        )
        self.assertTrue(result.passed, px_contract.render_markdown(result))

        mismatch = self.run_contract(
            launch,
            {
                "px3": """
inline constexpr int PIR_PIN = 3;
void build() { declare_parameter<int>("pir_pin", PIR_PIN); }
"""
            },
        )
        self.assertIn("DEFAULT_VALUE_MISMATCH", [issue.code for issue in mismatch.issues])

    def test_unresolved_named_default_is_reported(self) -> None:
        result = self.run_contract(
            """
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    pin = DeclareLaunchArgument("pir_pin", default_value="2")
    node = Node(name="px3", parameters=[{
        "pir_pin": ParameterValue(LaunchConfiguration("pir_pin"), value_type=int),
    }])
""",
            {"px3": 'void build() { declare_parameter<int>("pir_pin", SOME_PIN); }'},
        )
        self.assertFalse(result.passed)
        self.assertIn("UNRESOLVED_CPP_DEFAULT", [issue.code for issue in result.issues])


if __name__ == "__main__":
    unittest.main()
