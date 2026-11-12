# Bring-up with only the Raspberry Pi and the boards

No ST-LINK, Nucleo, Tag-Connect cable or antenna is needed. The Pi does the programming (SWD bit-bang over GPIO), reads the logs (its UART) and acts as the OBC. You need a soldering iron, thin wire (30 AWG wire-wrap or single strands from a ribbon cable) and a multimeter.

## Step 0: get the Pi back on the network

It was not reachable at 10.101.243.47. Connect a screen and keyboard or plug in Ethernet, then run `hostname -I` to find its IP. Then:

```bash
scp -r C:\Users\abhijit\Desktop\FinalCS\CUBESAT\code pi@<ip>:/home/pi/cubesat/code
ssh pi@<ip>
cd /home/pi/cubesat/code/obc && bash scripts/setup_pi.sh     # then edit config.txt as it says, reboot
```

## Step 1: checks with the power OFF (every board)

With the multimeter on resistance:

| Measure | ADCS | TT&C | Expect |
|---|---|---|---|
| 3.3 V to GND | TP4 to J16 pad 5 | TP2 to TP3 | **not** close to 0 Ω. It rises slowly as the capacitors charge; anything over about 100 Ω is fine. |
| 5 V to GND | TP1 to GND | – | not close to 0 Ω |

Under a magnifier, look for solder bridges on the 0.5 mm-pitch STM32 pins (U1 on ADCS, U3 on TT&C) and on the DRV8833s. A bridge on 3.3 V can damage the Pi's 3.3 V rail in step 3.

## Step 2: solder the SWD wires (pads are the same on J16 / J4)

```
Tag-Connect footprint, top view:    1 3.3V     2 SWDIO
                                    3 NRST     4 SWCLK
                                    5 GND      6 SWO
```

Only **SWDIO** and **SWCLK** must go on the small Tag-Connect pads. The other wires have bigger places to go:

| Signal | ADCS | TT&C | Pi header pin |
|---|---|---|---|
| SWDIO | J16 pad 2 | J4 pad 2 | **13** (GPIO27) |
| SWCLK | J16 pad 4 | J4 pad 4 | **22** (GPIO25) |
| NRST | J16 pad 3, or C1 (the NRST side) | J4 pad 3, or C15 | **15** (GPIO22) |
| GND | J16 pad 5 / any GND | TP3 | **20** |
| 3.3 V (power for flashing) | TP4 | TP2 | **17** (3V3) |
| SWO (ADCS log) | J16 pad 6 | – | **10** (GPIO15 RX) |
| UART TX (TT&C log) | – | J6 pin 2 | **10** (GPIO15 RX) |

Keep the wires short (under 20 cm) and glue them down so they don't rip the pads off.

**Power:** for flashing and I2C tests, the Pi's 3.3 V pin can power **one** STM32 board at a time. Don't also connect the EPS power to that board. The ADCS coils need 5 V (J12/J13); leave the coils **unconnected** for now. With no 5 V, ADCS may report DRV faults and go SAFE. That is expected.

## Step 3: talk to the chip

```bash
cd /home/pi/cubesat/code
python3 obc/scripts/swd_flash.py probe
```

You should see `STM32F4xx ... device id = 0x...421` (the F446) and a flash size of 512 KiB.

If it fails, check the wiring, check that the board is powered (3.3 V on TP4/TP2), swap SWDIO and SWCLK if unsure, and try `erase`.

## Step 4: flash and watch the log

```bash
python3 firmware/build.py arm all            # or copy code/build/ from the PC
python3 obc/scripts/swd_flash.py adcs
python3 obc/scripts/swo_log.py               # ADCS: expect "ADCS fw 0.2.0 ... mag=1 imu=1"
```

For TT&C, use the **receive-only** build until an antenna (or a 50 Ω dummy load) is on J3:

```bash
python3 obc/scripts/swd_flash.py build/arm/ttc_notx/ttc_notx.elf
python3 obc/scripts/swo_log.py --raw         # expect "TX INHIBITED build" and "radio=1"
```

**Never flash the normal `ttc` build without an antenna.** It beacons every 30 s, and transmitting into no antenna can damage the RFM98W.

## Step 5: the I2C link (your friend's tests, on real hardware)

Wire ADCS J11 (or TT&C J5) to Pi pin 3 (SDA), pin 5 (SCL) and pin 6 (GND):

```bash
cd obc
python3 hw_tests/link_test.py --bus 1 --addr 0x32     # ADCS (TT&C: --addr 0x33)
python3 -m cubesat_obc status adcs
python3 -m cubesat_obc tlm adcs
```

If the 10,000-PING test shows errors, use the i2c-gpio fallback (RPI_SETUP.md section 4).

## Step 6: sensors and the full stack

- **ADCS sensors:**
  - Move and turn the board: `tlm adcs 0` should show `gyro_dps` changing and `mag_uT` around 25–65 µT in total.
  - Then do the axis check in BRINGUP.md section 2.
- **Both boards on the bus:** run `python3 -m cubesat_obc --config config/obc.toml run`. The EPS INA226 and TSL2591 simply report as missing until they are connected.

## What still needs extra hardware

| Test | Needs |
|---|---|
| Radio to the ground | 433 MHz antenna + the LilyGO ground station |
| Coils (detumble for real) | EPS 5 V (or a 5 V supply) + the coils on J1–J3 |
| EPS, low battery, pyro | 2S battery or bench supply; dummy load for the pyro |
| Camera / light sensor | USB webcam / TSL2591 module |
