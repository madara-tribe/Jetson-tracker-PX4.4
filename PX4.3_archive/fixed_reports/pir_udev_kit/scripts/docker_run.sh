#!/bin/bash
# Docker launch script for PX System Night/Pattern-C (PIR camera)
# Resolves the host udev symlink to the real capture node and maps it into
# the container under the SAME name the launch file expects (/dev/video/pir_camera).
ARDUINO_DEV="/dev/ttyACM0"
UDEV_SYMLINK="/dev/video/pir_camera"

# Resolve symlink -> real node (run setup_udev.sh first)
if [[ -L "$UDEV_SYMLINK" ]]; then
  PIR_DEV=$(readlink -f "$UDEV_SYMLINK")
  echo "INFO: Resolved PIR_DEV=${PIR_DEV} from symlink ${UDEV_SYMLINK}"
elif [[ -e "$UDEV_SYMLINK" ]]; then
  PIR_DEV="$UDEV_SYMLINK"
  echo "INFO: UDEV_SYMLINK is a real device node: ${PIR_DEV}"
else
  echo "ERROR: ${UDEV_SYMLINK} not found. Run udev setup first:"
  echo "  sudo cp 99-pir_camera.rules /etc/udev/rules.d/"
  echo "  sudo udevadm control --reload-rules && sudo udevadm trigger"
  exit 1
fi

if [[ ! -e "$PIR_DEV" ]]; then
  echo "ERROR: Resolved device '${PIR_DEV}' not found on host. Check camera connection."
  exit 1
fi

if [[ ! -e "$ARDUINO_DEV" ]]; then
  echo "ERROR: ${ARDUINO_DEV} not found on host."
  exit 1
fi

xhost +local:docker

docker run -it --rm \
  --net=host \
  --privileged \
  --env="DISPLAY=$DISPLAY" \
  --volume="/tmp/.X11-unix:/tmp/.X11-unix:rw" \
  --device="${PIR_DEV}:/dev/video/pir_camera" \
  --group-add video \
  -v /home/hagi/Downloads/place/pC_place/:/ros2_ws \
  test:latest
