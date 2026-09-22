#!/bin/bash
# Build/upload the ESP32 + 2.8" ST7789 LCD sketch with PlatformIO.
#
# The Arduino sketch source of truth is nanodrop.ino. It is copied to main.cpp
# (PlatformIO compiles main.cpp, see src_dir/build_src_filter in platformio.ini).
set -e
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# nanodrop.ino と main.cpp を同期（二重管理のリスク低減）
cp -f "$SCRIPT_DIR/nanodrop.ino" "$SCRIPT_DIR/main.cpp"
echo "Synced nanodrop.ino -> main.cpp for build"

if [ "$1" = "upload" ]; then
  pio run -t upload
elif [ "$1" = "monitor" ]; then
  pio device monitor
else
  pio run
fi
