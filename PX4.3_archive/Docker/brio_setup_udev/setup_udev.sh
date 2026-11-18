#!/bin/bash
set -e

echo "=== Brio 100 udev Setup Script ==="

# Check if running as root or with sudo
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run with sudo"
   exit 1
fi

USER_NAME=$(logname 2>/dev/null || echo "$SUDO_USER")
echo "Setting up for user: $USER_NAME"

# Add user to dialout and video groups
usermod -aG dialout "$USER_NAME"
usermod -aG video "$USER_NAME"
echo "✓ Added $USER_NAME to dialout and video groups"

# Remove old systemd service (causes FOURCC conflict)
if [[ -f /etc/systemd/system/brio100-setup@.service ]]; then
    echo "Removing old brio100-setup@.service (causes MJPG/YUYV conflict)..."
    rm -f /etc/systemd/system/brio100-setup@.service
    systemctl daemon-reload
    echo "✓ Removed old systemd service"
fi

# Install new udev rules
cp 99-brio100.rules /etc/udev/rules.d/
echo "✓ Installed 99-brio100.rules"

# Reload udev
udevadm control --reload-rules
systemctl daemon-reload
echo "✓ Reloaded udev rules"

# Trigger video4linux devices
udevadm trigger --subsystem-match=video4linux
echo "✓ Triggered video4linux subsystem"

# Wait for udev to settle
sleep 2

# Verify symlink
echo ""
echo "=== Verification ==="
if [[ -L /dev/video/brio100 ]]; then
    RESOLVED=$(readlink -f /dev/video/brio100)
    echo "✓ Symlink exists: /dev/video/brio100 -> $RESOLVED"
    
    # Check if it's a capture node
    if v4l2-ctl -d "$RESOLVED" --all 2>&1 | grep -q "Video Capture"; then
        echo "✓ $RESOLVED is a capture node"
    else
        echo "✗ WARNING: $RESOLVED may not be a capture node!"
    fi
    
    # Show current format
    echo ""
    echo "Current camera format:"
    v4l2-ctl -d "$RESOLVED" --get-fmt-video
else
    echo "✗ Symlink /dev/video/brio100 not found!"
    echo ""
    echo "Available video devices:"
    v4l2-ctl --list-devices
    exit 1
fi

echo ""
echo "=== Setup Complete ==="
echo "You may need to log out and back in for group changes to take effect."
echo "Or run: newgrp video"
