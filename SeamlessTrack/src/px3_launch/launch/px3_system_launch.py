from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, SetEnvironmentVariable
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue


def generate_launch_description():
    color = SetEnvironmentVariable("RCUTILS_COLORIZED_OUTPUT", "1")

    # px2 (px3 is embedded inside px2 process)
    arg_px2_pkg = DeclareLaunchArgument("px2_pkg", default_value="px2")
    arg_px2_exec = DeclareLaunchArgument("px2_exec", default_value="px2")

    # [修正] watch_pkg: px_pir_cpp → px3
    # watch_node.cpp は px3 パッケージに移動済みのため
    arg_watch_pkg = DeclareLaunchArgument("watch_pkg", default_value="px3")
    arg_watch_exec = DeclareLaunchArgument("watch_exec", default_value="watch_node")

    # camera
    arg_camera = DeclareLaunchArgument("camera_path", default_value="/dev/video0")

    # px2 params
    arg_watch_f = DeclareLaunchArgument("watch_fps", default_value="10.0")
    arg_lost = DeclareLaunchArgument("lost_max_frames", default_value="15")
    arg_save = DeclareLaunchArgument("save_frames", default_value="false")
    arg_conf = DeclareLaunchArgument("conf_threshold", default_value="0.25")
    # arg_prepro_use = DeclareLaunchArgument("preprocess_enable", default_value="true")
    arg_active_w = DeclareLaunchArgument("active_width", default_value="1920")
    arg_active_h = DeclareLaunchArgument("active_height", default_value="1080")
    arg_active_f = DeclareLaunchArgument("active_fps", default_value="30.0")

    # inference backend (Jetson GPU)
    arg_ort_provider = DeclareLaunchArgument("ort_provider", default_value="tensorrt")
    arg_trt_fp16 = DeclareLaunchArgument("trt_fp16", default_value="true")
    arg_trt_cache_dir = DeclareLaunchArgument("trt_cache_dir", default_value="/tmp/px_trt_cache")
    arg_timing_csv = DeclareLaunchArgument("timing_csv", default_value="")
    arg_trt_engine = DeclareLaunchArgument("trt_engine", default_value="")

    arg_xmax = DeclareLaunchArgument("xmax", default_value="180.0")
    arg_xmin = DeclareLaunchArgument("xmin", default_value="0.0")
    arg_ymax = DeclareLaunchArgument("ymax", default_value="90.0")
    arg_ymin = DeclareLaunchArgument("ymin", default_value="20.0")

    arg_x_center_deg = DeclareLaunchArgument("x_center_deg", default_value="90.0")
    arg_y_center_deg = DeclareLaunchArgument("y_center_deg", default_value="90.0")

    # embedded px3 params
    arg_serial = DeclareLaunchArgument("serial_port", default_value="/dev/ttyACM0")
    arg_baudrate = DeclareLaunchArgument("baudrate", default_value="115200")

    # watch_node params
    arg_pir_debounce = DeclareLaunchArgument("pir_debounce_ms", default_value="200")

    # PD controller gains and step limit
    arg_max_step = DeclareLaunchArgument("max_step_deg", default_value="3.0")
    arg_kp_yaw = DeclareLaunchArgument("kp_yaw", default_value="0.3")
    arg_kd_yaw = DeclareLaunchArgument("kd_yaw", default_value="0.2")
    arg_kp_pitch = DeclareLaunchArgument("kp_pitch", default_value="0.3")
    arg_kd_pitch = DeclareLaunchArgument("kd_pitch", default_value="0.2")

    # ----------------------------------------------------------
    #  px2 node (px3 embedded inside)
    # ----------------------------------------------------------
    px2_node = Node(
        package=LaunchConfiguration("px2_pkg"),
        executable=LaunchConfiguration("px2_exec"),
        name="px2",
        output="screen",
        emulate_tty=True,
        parameters=[
            {
                "device_path": LaunchConfiguration("camera_path"),
                "lost_max_frames": ParameterValue(
                    LaunchConfiguration("lost_max_frames"), value_type=int
                ),
                "save_frames": ParameterValue(LaunchConfiguration("save_frames"), value_type=bool),
                "conf_threshold": ParameterValue(
                    LaunchConfiguration("conf_threshold"), value_type=float
                ),
                # "preprocess_enable": ParameterValue(
                #    LaunchConfiguration("preprocess_enable"), value_type=bool
                # ),
                "xmax": ParameterValue(LaunchConfiguration("xmax"), value_type=float),
                "xmin": ParameterValue(LaunchConfiguration("xmin"), value_type=float),
                "ymax": ParameterValue(LaunchConfiguration("ymax"), value_type=float),
                "ymin": ParameterValue(LaunchConfiguration("ymin"), value_type=float),
                "x_center_deg": ParameterValue(
                    LaunchConfiguration("x_center_deg"), value_type=float
                ),
                "y_center_deg": ParameterValue(
                    LaunchConfiguration("y_center_deg"), value_type=float
                ),
                "max_step_deg": ParameterValue(
                    LaunchConfiguration("max_step_deg"), value_type=float
                ),
                "kp_yaw": ParameterValue(LaunchConfiguration("kp_yaw"), value_type=float),
                "kd_yaw": ParameterValue(LaunchConfiguration("kd_yaw"), value_type=float),
                "kp_pitch": ParameterValue(LaunchConfiguration("kp_pitch"), value_type=float),
                "kd_pitch": ParameterValue(LaunchConfiguration("kd_pitch"), value_type=float),
                "serial_port": LaunchConfiguration("serial_port"),
                "baudrate": ParameterValue(LaunchConfiguration("baudrate"), value_type=int),
                "watch_fps": ParameterValue(LaunchConfiguration("watch_fps"), value_type=float),
                "active_width": ParameterValue(LaunchConfiguration("active_width"), value_type=int),
                "active_height": ParameterValue(
                    LaunchConfiguration("active_height"), value_type=int
                ),
                "active_fps": ParameterValue(LaunchConfiguration("active_fps"), value_type=float),
                "ort_provider": LaunchConfiguration("ort_provider"),
                "trt_fp16": ParameterValue(LaunchConfiguration("trt_fp16"), value_type=bool),
                "trt_cache_dir": LaunchConfiguration("trt_cache_dir"),
                "timing_csv": ParameterValue(LaunchConfiguration("timing_csv"), value_type=str),
                "trt_engine": ParameterValue(LaunchConfiguration("trt_engine"), value_type=str),
            }
        ],
    )

    # ----------------------------------------------------------
    #  watch_node
    #  役割: /pir/state(from px3) → /px_wakeup(to px2) の仲介
    #  px2 node とは別プロセスで起動する
    #  [修正] package = px3 (watch_node.cpp は px3 パッケージに所属)
    # ----------------------------------------------------------
    watch_node = Node(
        package=LaunchConfiguration("watch_pkg"),
        executable=LaunchConfiguration("watch_exec"),
        name="watch_node",
        output="screen",
        emulate_tty=True,
        parameters=[
            {
                "pir_debounce_ms": ParameterValue(
                    LaunchConfiguration("pir_debounce_ms"), value_type=int
                ),
            }
        ],
    )

    return LaunchDescription(
        [
            color,
            # pkg / exec
            arg_px2_pkg,
            arg_px2_exec,
            arg_watch_pkg,
            arg_watch_exec,
            # camera
            arg_camera,
            # px2 params
            arg_watch_f,
            arg_lost,
            arg_save,
            arg_conf,
            # arg_prepro_use,
            arg_active_w,
            arg_active_h,
            arg_active_f,
            arg_ort_provider,
            arg_trt_fp16,
            arg_trt_cache_dir,
            arg_timing_csv,
            arg_trt_engine,
            arg_xmax,
            arg_xmin,
            arg_ymax,
            arg_ymin,
            arg_x_center_deg,
            arg_y_center_deg,
            arg_max_step,
            arg_kp_yaw,
            arg_kd_yaw,
            arg_kp_pitch,
            arg_kd_pitch,
            # px3 params
            arg_serial,
            arg_baudrate,
            # watch_node params
            arg_pir_debounce,
            # nodes
            px2_node,
            watch_node,
        ]
    )
