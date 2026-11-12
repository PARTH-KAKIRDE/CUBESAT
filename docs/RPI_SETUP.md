# Raspberry Pi 4B setup

The SSH and VNC details are in `../RASPBERRY_PI_GUIDE.md`.

## 1. Copy the code and install packages

```bash
# from this PC (PowerShell), with the Pi on the same Wi-Fi:
scp -r C:\Users\abhijit\Desktop\FinalCS\CUBESAT\code pi@<pi-ip>:/home/pi/cubesat/code
# on the Pi:
cd /home/pi/cubesat/code/obc
bash scripts/setup_pi.sh
```

## 2. /boot/firmware/config.txt

Add:

```
dtparam=i2c_arm=on
dtparam=i2c_arm_baudrate=100000
dtoverlay=i2c3,pins_4_5
dtparam=watchdog=on
```

Remove any `dtoverlay=uart3` line, because it uses GPIO4/5. The earlier `dtparam=uart2..5=on` lines are not valid dtparams and have no effect; they can go. Reboot, then check:

```bash
ls /dev/i2c-1 /dev/i2c-3
i2cdetect -y 1     # expect 32 (ADCS) and 33 (TT&C) once the boards are connected
i2cdetect -y 3     # expect 29 (TSL2591) and 40 (INA226)
```

`i2cdetect` probes these addresses with an empty write or a 1-byte read. The STM32 firmware ignores the empty write, and the read just returns the first byte of the current response, so scanning is harmless.

## 3. Link test (deliverables 4 and 5)

Run this with the Nucleo dummy slave or a real board:

```bash
cd /home/pi/cubesat/code/obc
python3 hw_tests/link_test.py --bus 1 --addr 0x32
```

You should see `9/9 passed`. Test (b) asks you to unplug SDA and plug it back in.

## 4. Clock stretching

The Pi's hardware I2C controller handles slave clock stretching badly. The STM32 stretches SCL briefly at the start of each read while its interrupt loads the first byte. If `link_test.py` reports errors or read-CRC errors, switch the link to the bit-banged driver, which handles stretching correctly:

```
# config.txt: add this instead of using bus 1 for the STM32s
dtoverlay=i2c-gpio,bus=11,i2c_gpio_sda=23,i2c_gpio_scl=24,i2c_gpio_delay_us=2
```

Then:
- Move the SDA/SCL wires to pins 16 and 18 (see WIRING.md).
- Set `[link] bus = 11` in `obc/config/obc.toml`.
- Re-run `python3 hw_tests/link_test.py --bus 11`.

## 5. Run the OBC

**Ground-command key (once).** The OBC ignores every ground command until it has the key. On the ground PC:

```bash
python ground/gs_tool.py keygen          # writes ground/.ground_key (secret, git-ignored)
scp ground/.ground_key pi@<pi-address>:/tmp/ground_key
```

On the Pi:

```bash
mkdir -p ~/.cubesat && mv /tmp/ground_key ~/.cubesat/ground_key && chmod 600 ~/.cubesat/ground_key
```

The path is set in `config/obc.toml` (`[ground] key_file`). If the key is ever leaked, run `keygen --force` and copy the new file to the Pi.

```bash
python3 -m cubesat_obc --config config/obc.toml scan     # who answers
python3 -m cubesat_obc --config config/obc.toml status adcs
python3 -m cubesat_obc --config config/obc.toml tlm adcs
python3 -m cubesat_obc --config config/obc.toml run      # main loop, Ctrl+C to stop
```

To run it as a service that starts at boot, restarts on a crash and is watched by the systemd watchdog:

```bash
sudo cp systemd/cubesat-obc.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now cubesat-obc
journalctl -u cubesat-obc -f
```

Telemetry is written to `/home/pi/cubesat_data/telemetry/telemetry_YYYY-MM-DD.jsonl`, one JSON line per second. Ground traffic goes to `ground_*.jsonl` and images to `images/`.

## 6. Building and flashing the STM32 firmware on the Pi

`setup_pi.sh` installs `gcc-arm-none-eabi` and `openocd`, so the Pi can build and flash the firmware too:

```bash
cd /home/pi/cubesat/code
python3 firmware/build.py arm all
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
        -c "program build/arm/adcs/adcs.elf verify reset exit"
```
