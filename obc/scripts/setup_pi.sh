#!/usr/bin/env bash
# One-time Raspberry Pi 4B setup for the OBC. Run on the Pi from code/obc:
#     bash scripts/setup_pi.sh
# It installs packages and PRINTS the /boot/firmware/config.txt lines to add;
# it does not edit config.txt itself (see docs/RPI_SETUP.md).
set -euo pipefail

sudo apt-get update
sudo apt-get install -y python3-smbus2 python3-gpiozero python3-lgpio i2c-tools fswebcam \
    gcc-arm-none-eabi libnewlib-arm-none-eabi openocd gpiod python3-serial python3-pytest

mkdir -p /home/pi/cubesat_data

echo
echo "Add these lines to /boot/firmware/config.txt (sudo nano /boot/firmware/config.txt), then reboot:"
cat <<'EOF'
    dtparam=i2c_arm=on
    dtparam=i2c_arm_baudrate=100000
    dtoverlay=i2c3,pins_4_5
    dtparam=watchdog=on
    enable_uart=1
EOF
echo
# Pi UART (GPIO15 RX, pin 10) is used to read the STM32 logs: no login console on it.
sudo raspi-config nonint do_serial_cons 1 || true
sudo raspi-config nonint do_serial_hw 0 || true
echo
echo "Remove any 'dtoverlay=uart3' line: it uses GPIO4/5, the same pins as i2c3."
echo "After reboot check:  ls /dev/i2c-1 /dev/i2c-3   and   i2cdetect -y 1 ; i2cdetect -y 3"
echo
echo "Then: python3 hw_tests/link_test.py --bus 1 --addr 0x32"
