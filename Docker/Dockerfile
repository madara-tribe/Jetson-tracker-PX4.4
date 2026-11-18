# ============================================================
#  Docker/Dockerfile — PX4.4 (Jetson Orin Nano)
#    Host : JetPack 6.2.x (L4T r36.4.x / r36.5), Ubuntu 22.04, aarch64
#    Image: L4T JetPack r36.4.0 (CUDA / cuDNN / TensorRT 10.3 included)
#           + ROS 2 Humble (apt) + Ubuntu OpenCV 4.5.4 (+contrib)
#           + ONNX Runtime 1.14.1 aarch64 (CPU)
#
#  推論: PROVIDER=cpu → ONNX Runtime CPU / PROVIDER=trt → TensorRT（ベースに同梱）
#
#  ONNX Runtime の入手:
#    microsoft/onnxruntime が GitHub から消えているため、ダウンロードは失敗する。
#    Docker/third_party/onnxruntime-1.14.1-aarch64.tgz を置けば、そちらを使う。
#    （既存イメージから取り出す: ../scripts/export_ort_from_image.sh）
# ============================================================
ARG BASE_IMAGE=nvcr.io/nvidia/l4t-jetpack:r36.4.0
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# ---------- base tools ----------
RUN apt-get update && apt-get install -y --no-install-recommends \
        sudo curl wget ca-certificates gnupg2 lsb-release locales tzdata \
        build-essential cmake git pkg-config unzip tmux vim gdb \
        python3 python3-pip python3-dev \
        udev v4l-utils psmisc file bash-completion \
    && locale-gen en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*
ENV LANG=en_US.UTF-8 LANGUAGE=en_US:en LC_ALL=en_US.UTF-8

# ---------- OpenCV: use Ubuntu's 4.5.4 (has contrib/tracking) ----------
# NVIDIA's apt repo ships its own OpenCV without contrib. Block it so
# px2 (needs 'tracking' component) and cv_bridge use the same OpenCV.
RUN printf 'Package: *opencv*\nPin: origin repo.download.nvidia.com\nPin-Priority: -1\n' \
        > /etc/apt/preferences.d/no-nvidia-opencv \
    && (apt-get purge -y '*opencv*' || true) \
    && apt-get update && apt-get install -y --no-install-recommends \
        libopencv-dev libopencv-contrib-dev \
    && rm -rf /var/lib/apt/lists/*

# ---------- ROS 2 Humble (official ros2-apt-source method) ----------
ENV ROS_DISTRO=humble
RUN ROS_APT_SOURCE_VERSION=$(curl -s https://api.github.com/repos/ros-infrastructure/ros-apt-source/releases/latest \
        | grep -F '"tag_name"' | awk -F'"' '{print $4}') \
    && curl -L -o /tmp/ros2-apt-source.deb \
        "https://github.com/ros-infrastructure/ros-apt-source/releases/download/${ROS_APT_SOURCE_VERSION}/ros2-apt-source_${ROS_APT_SOURCE_VERSION}.$(. /etc/os-release && echo $VERSION_CODENAME)_all.deb" \
    && apt-get update && apt-get install -y /tmp/ros2-apt-source.deb \
    && apt-get update && apt-get install -y --no-install-recommends \
        ros-humble-ros-base \
        ros-humble-cv-bridge \
        ros-humble-image-transport \
        python3-colcon-common-extensions \
        python3-rosdep \
    && rm -f /tmp/ros2-apt-source.deb \
    && rm -rf /var/lib/apt/lists/*

# ---------- ONNX Runtime 1.14.1 (aarch64, CPU) ----------
# px2 includes <onnxruntime/core/session/onnxruntime_cxx_api.h>,
# so headers are placed in that tree.
# 1) Docker/third_party/onnxruntime-${ORT_VERSION}-aarch64.tgz があれば使う
# 2) 無ければ GitHub からダウンロード（現在は 404 で失敗する）
ARG ORT_VERSION=1.14.1
COPY third_party/ /tmp/third_party/
RUN set -e; \
    LOCAL=/tmp/third_party/onnxruntime-${ORT_VERSION}-aarch64.tgz; \
    if [ -s "$LOCAL" ]; then \
      echo "ONNX Runtime: using $LOCAL"; tar xzf "$LOCAL" -C /usr/local; \
    else \
      echo "ONNX Runtime: downloading from GitHub"; cd /tmp; \
      wget -q https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-aarch64-${ORT_VERSION}.tgz; \
      tar xzf onnxruntime-linux-aarch64-${ORT_VERSION}.tgz; \
      mkdir -p /usr/local/include/onnxruntime/core/session; \
      cp -r onnxruntime-linux-aarch64-${ORT_VERSION}/include/* /usr/local/include/onnxruntime/core/session/; \
      cp -P onnxruntime-linux-aarch64-${ORT_VERSION}/lib/libonnxruntime.so* /usr/local/lib/; \
      rm -rf /tmp/onnxruntime-linux-aarch64-${ORT_VERSION}*; \
    fi; \
    ldconfig; \
    test -f /usr/local/include/onnxruntime/core/session/onnxruntime_cxx_api.h; \
    ls /usr/local/lib/libonnxruntime.so*; \
    rm -rf /tmp/third_party

# ---------- user (UID/GID match host hagi; UID 0 from "sudo make" falls back to 1000) ----------
ARG USER_NAME=developer
ARG USER_UID=1000
ARG USER_GID=1000
RUN UID_=${USER_UID}; GID_=${USER_GID}; \
    if [ "$UID_" = "0" ]; then UID_=1000; fi; if [ "$GID_" = "0" ]; then GID_=1000; fi; \
    (getent group $GID_ || groupadd -g $GID_ ${USER_NAME}) \
    && useradd -m -u $UID_ -g $GID_ -s /bin/bash ${USER_NAME} \
    && usermod -aG video,dialout ${USER_NAME} \
    && echo "${USER_NAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USER_NAME} \
    && echo "source /opt/ros/${ROS_DISTRO}/setup.bash" >> /home/${USER_NAME}/.bashrc \
    && echo '[ -f /ros2_ws/work/install/setup.bash ] && source /ros2_ws/work/install/setup.bash' >> /home/${USER_NAME}/.bashrc

COPY ros_entrypoint.sh /ros_entrypoint.sh
RUN chmod +x /ros_entrypoint.sh

USER ${USER_NAME}
ENV HOME=/home/${USER_NAME}
WORKDIR /ros2_ws/work
ENTRYPOINT ["/ros_entrypoint.sh"]
CMD ["/bin/bash"]
