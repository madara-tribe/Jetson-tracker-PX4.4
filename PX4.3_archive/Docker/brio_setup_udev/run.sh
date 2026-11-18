#!/bin/bash
ARDUINO_DEV="/dev/ttyACM0"

# udev symlink から実キャプチャノードを解決する
# run.sh 実行時点で brio100-setup@.service が完了している前提
UDEV_SYMLINK="/dev/video/brio100"

if [[ -L "$UDEV_SYMLINK" ]]; then
  BRIO_DEV=$(readlink -f "$UDEV_SYMLINK")
  echo "INFO: Resolved BRIO_DEV=${BRIO_DEV} from symlink ${UDEV_SYMLINK}"
elif [[ -e "$UDEV_SYMLINK" ]]; then
  BRIO_DEV="$UDEV_SYMLINK"
  echo "INFO: UDEV_SYMLINK is a real device node: ${BRIO_DEV}"
else
  echo "ERROR: ${UDEV_SYMLINK} not found. Run udev setup first:"
  echo "  sudo cp 99-brio100.rules /etc/udev/rules.d/"
  echo "  sudo udevadm control --reload-rules && sudo udevadm trigger"
  exit 1
fi

if [[ ! -e "$BRIO_DEV" ]]; then
  echo "ERROR: Resolved device '${BRIO_DEV}' not found on host. Check camera connection."
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
  --device="${BRIO_DEV}:/dev/video/brio100" \
  --group-add video \
  -v /home/hagi/Downloads/place/:/ros2_ws \
  test:latest
