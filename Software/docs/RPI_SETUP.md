# Raspberry Pi 4B setup

The Pi's SSH and VNC details are kept in `RASPBERRY_PI_GUIDE.md`, which is **never committed** (this repo is public; `.gitignore` blocks it).

All commands below run from the `Software/` folder. On the Pi that folder is `/home/pi/cubesat/Software` (the systemd service expects that path).

## 1. Copy the code and install packages

```bash
# from the PC, with the Pi on the same network (or git clone the repo on the Pi):
scp -r Software pi@<pi-ip>:/home/pi/cubesat/
# on the Pi:
cd /home/pi/cubesat/Software
bash OBC/scripts/setup_pi.sh
```

`setup_pi.sh` installs the Python packages, `i2c-tools`, `rpicam-apps` (Pi camera), the ARM compiler and `openocd`. It also turns off the login console on the UART (used for the STM32 logs). It **prints** the `config.txt` lines to add but does not edit `config.txt` itself.

## 2. /boot/firmware/config.txt

Add:

```
dtparam=i2c_arm=on
dtparam=i2c_arm_baudrate=100000
dtoverlay=i2c3,pins_4_5
dtparam=watchdog=on
enable_uart=1
camera_auto_detect=1
```

Remove any `dtoverlay=uart3` line, because it uses GPIO4/5, the same pins as i2c3. `dtparam=uart2..5=on` lines are not valid dtparams and have no effect; they can go. Reboot, then check:

```bash
ls /dev/i2c-1 /dev/i2c-3
i2cdetect -y 1              # expect 32 (ADCS) and 33 (TT&C) once the boards are connected
i2cdetect -y 3              # expect 29 (TSL2591) and 40 (INA226)
rpicam-still --list-cameras # the Pi camera on CSI
```

`i2cdetect` probes these addresses with an empty write or a 1-byte read. The STM32 firmware ignores the empty write, and the read just returns the first byte of the current response, so scanning is harmless.

## 3. Link test (deliverables 4 and 5)

Run this with the Nucleo dummy slave or a real board:

```bash
python3 ICD/python/test_link.py --bus 1 --addr 0x32     # TT&C: --addr 0x33
```

You should see `15/15 passed`. Test (b) asks you to unplug the slave's SDA wire and plug it back in (`--skip-unplug` skips it).

## 4. Clock stretching

The Pi's hardware I2C controller handles slave clock stretching badly. The STM32 stretches SCL briefly at the start of each read while its interrupt loads the first byte. If `test_link.py` reports errors or read-CRC errors, switch the link to the bit-banged driver, which handles stretching correctly:

```
# config.txt: add this instead of using bus 1 for the STM32s
dtoverlay=i2c-gpio,bus=11,i2c_gpio_sda=23,i2c_gpio_scl=24,i2c_gpio_delay_us=2
```

Then:
- Move the SDA/SCL wires to pins 16 and 18 (see [WIRING.md](WIRING.md)).
- Set `[link] bus = 11` in `OBC/config/obc.toml`.
- Re-run `python3 ICD/python/test_link.py --bus 11`.

## 5. Run the OBC

**Ground-command key (once).** The OBC ignores every ground command until it has the key. Beacons still go out without it. Generate the key on the ground PC with the ground tool's `keygen` (the ground tool is still in the base repo, see [the Software README](../README.md)), then copy it to the Pi:

```bash
# on the Pi, after copying the key file to /tmp/ground_key:
mkdir -p ~/.cubesat && mv /tmp/ground_key ~/.cubesat/ground_key && chmod 600 ~/.cubesat/ground_key
```

The path is set in `OBC/config/obc.toml` (`[ground] key_file`). Never commit the key. If it ever leaks, generate a new one and copy it again.

```bash
python3 -m OBC --config OBC/config/obc.toml scan          # which devices answer
python3 -m OBC --config OBC/config/obc.toml status adcs
python3 -m OBC --config OBC/config/obc.toml tlm adcs 0
python3 -m OBC --config OBC/config/obc.toml run           # main loop, Ctrl+C to stop
```

To run it as a service that starts at boot, restarts on a crash and is watched by the systemd watchdog (restart after a 30 s hang):

```bash
sudo cp OBC/systemd/cubesat-obc.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now cubesat-obc
journalctl -u cubesat-obc -f
```

Data goes to `/home/pi/cubesat_data`:
- `telemetry/telemetry_<day>.jsonl`, one JSON line per second;
- `payload/payload_<day>.jsonl`;
- `images/`;
- `ground_<day>.jsonl` (ground traffic);
- `adcs_gyro_offset.json` (the gyro offset ADCS measured on the bench, re-sent after a reset in orbit).

## 6. Building and flashing the STM32 firmware on the Pi

`setup_pi.sh` installs `gcc-arm-none-eabi` and `openocd`, so the Pi can build and flash the firmware too:

```bash
python3 firmware/build.py arm all
python3 OBC/scripts/swd_flash.py adcs          # no programmer needed: see NO_PROGRAMMER.md
# or with an ST-LINK:
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
        -c "program build/arm/adcs/adcs.elf verify reset exit"
```
