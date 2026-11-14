# Wiring: Raspberry Pi 4B ↔ the boards

All boards are 3.3 V logic. Connect **GND first** on every link.

The I2C addresses and buses are defined in `ICD/cubesat_icd.h` (section 1 of `ICD/ICD.md`); the pin tables below come from the KiCad boards.

## Pi 40-pin header

| Pi pin | GPIO | Goes to | Notes |
|---|---|---|---|
| 3 | GPIO2 SDA1 | ADCS J11-1 **and** TT&C J5-1 (SDA) | `/dev/i2c-1`, STM32 link |
| 5 | GPIO3 SCL1 | ADCS J11-2 **and** TT&C J5-2 (SCL) | |
| 6, 9 | GND | ADCS J11-3, TT&C J5-3 | |
| 7 | GPIO4 (i2c3 SDA) | EPS J6-3 (SDA), TSL2591 SDA, VEML6075 SDA | `/dev/i2c-3`, sensor bus |
| 29 | GPIO5 (i2c3 SCL) | EPS J6-4 (SCL), TSL2591 SCL, VEML6075 SCL | |
| 11 | GPIO17 | EPS J6-5 PYRO_FIRE | Pulled down at boot. Keep `[pyro] enabled = false` until it has been tested. |
| 2 / 4 | 5V | EPS J6-1 (+5V) | Only if the Pi is powered from the EPS. This bypasses the Pi's USB-C protection. |
| 14 | GND | EPS J6-2 | |
| 10 | GPIO15 (UART RX) | ADCS SWO or TT&C UART TX | STM32 logs, read with `OBC/scripts/swo_log.py` |
| 13, 15, 22 | GPIO27, 22, 25 | SWDIO, NRST, SWCLK | Only for flashing from the Pi (NO_PROGRAMMER.md) |
| CSI | – | Pi camera | Ribbon cable to the camera connector |

Sensor bus addresses: INA226 0x40, TSL2591 0x29, VEML6075 0x10. The VEML6075 is switched off in software until `ICD_ADDR_VEML6075` is uncommented in the ICD. The magnetometer (MMC5983MA) is **not** on the Pi: it sits on the ADCS board's own I2C bus and reaches the Pi only through ADCS telemetry.

**Pull-ups:**
- STM32 link: the Pi has 1.8k on GPIO2/3, and ADCS and TT&C each add 4.7k. About 1.1k in total is fine at 100 kHz.
- Sensor bus: there are no Pi pull-ups on GPIO4/5. Most INA226, TSL2591 and VEML6075 breakout modules have their own. If `i2cdetect -y 3` shows nothing, add 4.7k to 3.3 V on each line.

**INA226 module:** wire its **VBUS pin to IN−**, or the bus voltage reads 0 (see the bring-up checklist in `OBC/README.md`).

**Fallback if the link test shows errors** (the Pi clock-stretching problem): move the ADCS/TT&C SDA wire to **pin 16 (GPIO23)** and SCL to **pin 18 (GPIO24)**, then use the bit-banged bus 11 (see [RPI_SETUP.md](RPI_SETUP.md) section 4).

## ADCS sun sensors (BPW34)

| ADCS connector | Channel | Face (placeholder, `ADCS/board_config.h`) |
|---|---|---|
| J4 | SS1 (PA0) | +X |
| J5 | SS2 (PA1) | −X |
| J6 | SS3 (PA2) | +Y |
| J7 | SS4 (PA3) | −Y |
| J8 | SS5 (PA4) | +Z |
| J9 | SS6 (PA5) | −Z |

Each connector takes one BPW34 photodiode (pin 2 = GNDA), on every face: with no pointing control any face can face the Sun, and a bare face would read as eclipse. Earth albedo is rejected by the lit (albedo) threshold. The board's MCP6001 stage turns its current into a voltage (680 Ω), so the reading is proportional to the light on that face. Check the diode's polarity on the bench: reversed, the reading stays at 0 in light. Not to be confused with the payload's TSL2591, which is on the Pi's sensor bus.

## Nucleo-F446RE dummy slave (link test before the real boards)

| Nucleo | Pi |
|---|---|
| D7 = PA8 (I2C3 SCL) | pin 5 (GPIO3) |
| CN10 pin 1 = PC9 (I2C3 SDA) | pin 3 (GPIO2) |
| GND | pin 6 |

These are the same STM32 pins as on ADCS and TT&C, so the same driver is tested. Its log goes to the ST-LINK virtual COM port at 115200.

## TT&C debug UART (J6 "OBC UART")

| J6 pin | Signal | USB-serial adapter (3.3 V!) or Pi |
|---|---|---|
| 1 | TT&C RX (PA10, via 1k) | adapter TX |
| 2 | TT&C TX (PA9, via 1k) | adapter RX, or Pi pin 10 |
| 3 | GND | GND |

115200 8N1.

## TT&C mission header (J2): burn wire

| J2 pin | Signal | Firmware assumption (`firmware/ttc/ttc_app.h`) |
|---|---|---|
| 1 | DEPLOY_SW (PC0, 10k pull-up) | High = antenna released (switch open) |
| 2 | RBF_IN (PC1, 10k pull-up) | High = remove-before-flight pin **out** (burn allowed) |
| 3 | BURN_EN (PC2, 100k pull-down) | Logic output to the external burn-wire MOSFET |
| 4 | GND | |

If your switches work the other way round, flip `TTC_DEPLOYED_LEVEL` / `TTC_RBF_REMOVED_LEVEL`.

## Debug probes

- **ADCS:** J16 Tag-Connect TC2030 → ST-LINK (SWDIO, SWCLK, NRST, SWO).
- **TT&C:** J4 Tag-Connect TC2030 → ST-LINK.
- A Nucleo's on-board ST-LINK can program both: remove the two CN2 jumpers and wire CN4 to the Tag-Connect.
- No probe at all: the Pi can flash both boards itself, see [NO_PROGRAMMER.md](NO_PROGRAMMER.md).
