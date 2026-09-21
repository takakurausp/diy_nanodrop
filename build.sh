#!/bin/bash
# Direct compile of nanodrop.ino with lgt8f core (bypasses PlatformIO pkg mgr)
set -e
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FW=~/.platformio/packages/framework-arduino-avr-lgt8f
BIN=$FW/../toolchain-atmelavr/bin
BUILD=$SCRIPT_DIR/build_lgt8f
rm -rf $BUILD/obj
mkdir -p $BUILD/obj

# nanodrop.ino と main.cpp を同期（二重管理のリスク低減）
cp -f "$SCRIPT_DIR/nanodrop.ino" "$SCRIPT_DIR/main.cpp"
echo "Synced nanodrop.ino -> main.cpp for build"

CORE=$FW/cores/lgt8f
VAR=$FW/variants/lgt8fx8p
LIB=$FW/libraries
INC="-I$CORE -I$VAR -I$LIB/Wire -I$LIB/SPI/src -I$LIB/E2PROM -I$SCRIPT_DIR/libraries/Adafruit_GFX -I$SCRIPT_DIR/libraries/Adafruit_SSD1306 -I$SCRIPT_DIR/libraries/Adafruit_BusIO"
COMMON="-mmcu=atmega328p -Os -Wall -ffunction-sections -fdata-sections -flto -fpermissive -fno-exceptions -fno-threadsafe-statics"
DEF="-DAVR_LARDU_328E -DARDUINO_LGT8F328P -DF_CPU=16000000L -DARDUINO=10808 -DARDUINO_ARCH_AVR ${USE_OLED:+-DUSE_OLED=$USE_OLED}"

echo "=== Compiling core .cpp ==="
for f in $CORE/*.cpp; do
  b=$(basename "$f" .cpp)
  echo "  $b.cpp"
  $BIN/avr-g++ $COMMON $DEF $INC -c "$f" -o $BUILD/obj/$b.o || { echo "FAIL core $b"; exit 1; }
done

echo "=== Compiling core .c files ==="
for f in $CORE/*.c; do
  b=$(basename "$f" .c)
  echo "  $b.c"
  $BIN/avr-gcc $COMMON $DEF $INC -c "$f" -o $BUILD/obj/$b.o || { echo "FAIL corec $b"; exit 1; }
done

echo "=== Compiling Wire lib ==="
$BIN/avr-g++ $COMMON $DEF $INC -c $LIB/Wire/Wire.cpp -o $BUILD/obj/Wire.o || { echo "FAIL Wire"; exit 1; }
$BIN/avr-gcc $COMMON $DEF $INC -I$LIB/Wire/utility -c $LIB/Wire/utility/twi.c -o $BUILD/obj/twi.o || { echo "FAIL twi"; exit 1; }

echo "=== Compiling SPI lib (needed by BusIO) ==="
$BIN/avr-g++ $COMMON $DEF $INC -c $LIB/SPI/src/SPI.cpp -o $BUILD/obj/SPI.o || { echo "FAIL SPI"; exit 1; }

echo "=== Compiling E2PROM lib (LGT8F real EEPROM) ==="
$BIN/avr-g++ $COMMON $DEF $INC -c $LIB/E2PROM/EEPROM.cpp -o $BUILD/obj/eeprom.o || { echo "FAIL eeprom"; exit 1; }

echo "=== Compiling GFX + SSD1306 libs (OLED) ==="
GFX=$SCRIPT_DIR/libraries/Adafruit_GFX
SSD=$SCRIPT_DIR/libraries/Adafruit_SSD1306
BUSIO=$SCRIPT_DIR/libraries/Adafruit_BusIO
$BIN/avr-g++ $COMMON $DEF $INC -c $BUSIO/Adafruit_I2CDevice.cpp -o $BUILD/obj/i2cdev.o || { echo "FAIL i2cdev"; exit 1; }
$BIN/avr-g++ $COMMON $DEF $INC -c $GFX/Adafruit_GFX.cpp -o $BUILD/obj/gfx.o || { echo "FAIL gfx"; exit 1; }
$BIN/avr-g++ $COMMON $DEF $INC -c $SSD/Adafruit_SSD1306.cpp -o $BUILD/obj/ssd1306.o || { echo "FAIL ssd1306"; exit 1; }

echo "=== Compiling sketch ==="
$BIN/avr-g++ $COMMON $DEF $INC -c $SCRIPT_DIR/main.cpp -o $BUILD/obj/sketch_main.o || { echo "FAIL sketch"; exit 1; }

echo "=== Linking ==="
$BIN/avr-g++ $COMMON $DEF $BUILD/obj/*.o -lm -Wl,--gc-sections -flto -fuse-linker-plugin -o $BUILD/nanodrop.elf || { echo "FAIL link"; exit 1; }

echo "=== Size ==="
$BIN/avr-size --format=avr $BUILD/nanodrop.elf
