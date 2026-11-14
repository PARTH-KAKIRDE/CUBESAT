#!/usr/bin/env bash
# One-time Raspberry Pi 4B setup for the OBC. Run on the Pi from Software/:
#     bash OBC/scripts/setup_pi.sh
# It installs packages and PRINTS the /boot/firmware/config.txt lines to add;
# it does not edit config.txt itself.
set -euo pipefail

sudo apt-get update
# rpicam-apps: Pi camera (CSI) capture (rpicam-still). On Raspberry Pi OS Lite use rpicam-apps-lite.
sudo apt-get install -y python3-smbus2 python3-gpiozero python3-lgpio i2c-tools rpicam-apps \
    gcc-arm-none-eabi libnewlib-arm-none-eabi openocd gpiod python3-serial python3-pytest

mkdir -p /home/pi/cubesat_data /home/pi/.cubesat

echo
echo "Add these lines to /boot/firmware/config.txt (sudo nano /boot/firmware/config.txt), then reboot:"
cat <<'CFG'
    dtparam=i2c_arm=on
    dtparam=i2c_arm_baudrate=100000
    dtoverlay=i2c3,pins_4_5
    dtparam=watchdog=on
    enable_uart=1
    camera_auto_detect=1
CFG
echo
# Pi UART (GPIO15 RX, pin 10) is used to read the STM32 logs: no login console on it.
sudo raspi-config nonint do_serial_cons 1 || true
sudo raspi-config nonint do_serial_hw 0 || true
echo
echo "Remove any 'dtoverlay=uart3' line: it uses GPIO4/5, the same pins as i2c3."
echo "After reboot check:"
echo "    ls /dev/i2c-1 /dev/i2c-3 ; i2cdetect -y 1 ; i2cdetect -y 3     (expect 29 TSL2591, 40 INA226 on bus 3)"
echo "    rpicam-still --list-cameras"
echo "    python3 OBC/EPS/EPS.py --bus 3      (INA226 bench check: VBUS must read the battery voltage)"
echo "    python3 ICD/python/test_link.py --bus 1 --addr 0x32"
