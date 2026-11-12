# ICD v0.1 deliverables

| File | What |
|---|---|
| `ICD.md` | The interface control document |
| `icd.h` | Shared header: addresses, command IDs, status codes, modes, flags, telemetry offsets |
| `icd_crc.c/.h` | CRC-16/CCITT-FALSE for the STM32s (C) |
| `stm32_dummy/icd_slave.c/.h` | Dummy slave protocol logic (PING, GET_VERSION), no HAL, host-testable |
| `stm32_dummy/icd_slave_it.c` | STM32F4 HAL I2C slave interrupt glue |
| `cm4/icd_link.py` | CM4 master: CRC, framing, transactions, COMM_FAULT tracking. Reads constants from `icd.h` |
| `cm4/test_link.py` | Tests (a), (b), (c) against the dummy slave |

## Dummy slave on the Nucleo-F446RE

Uses I2C3 on the same pins as the ADCS board, so the glue carries over unchanged.

**CubeMX**
- I2C3: mode I2C, Standard mode 100 kHz, Clock No Stretch Mode **Disabled**,
  7-bit primary address, Primary slave address **100** (= 0x64 = 0x32 << 1; HAL wants the shifted value).
  Use 102 (0x66) to impersonate TT&C.
- Pins: PA8 = I2C3_SCL (Arduino D7), PC9 = I2C3_SDA (morpho CN10 pin 1).
- NVIC: enable **I2C3 event** and **I2C3 error** interrupts.

**Project**: add `icd.h`, `icd_crc.c/.h`, `stm32_dummy/icd_slave.c/.h`, `stm32_dummy/icd_slave_it.c`, and add `icd/` and `icd/stm32_dummy/` to the include path. In `main.c`:

```c
/* USER CODE BEGIN Includes */
#include "icd_slave.h"
/* USER CODE BEGIN 2 */
icd_slave_init(&hi2c3);
/* in while (1): */
icd_slave_poll();
```

**Wiring to the CM4**: SDA (GPIO2) to PC9, SCL (GPIO3) to PA8, GND to GND.
The bus needs pull-ups to 3.3 V. Raspberry Pi boards have 1.8 kΩ on GPIO2/3; on the CM4 carrier
check the schematic, and add 4.7 kΩ if there are none.

## CM4

`/boot/firmware/config.txt` (older OS: `/boot/config.txt`):

```
dtparam=i2c_arm=on
dtparam=i2c_arm_baudrate=100000
```

Then reboot and run:

```
i2cdetect -y 1                 # the slave should show at 32
cd icd/cm4
python3 test_link.py ping
python3 test_link.py badcrc    # test (a)
python3 test_link.py unplug    # test (b): follow the prompts
python3 test_link.py soak      # test (c): 10,000 PINGs, must be 0 errors
```

Use `--addr 0x33` for TT&C and `--bus N` for another bus. Each run does the CRC self-test first.

### If test (c) drops frames

The STM32 briefly stretches SCL at every address match, and the Raspberry Pi hardware I2C
controller has known clock-stretching problems. Switch that bus to the bit-banged driver, which handles stretching:

```
dtparam=i2c_arm=off
dtoverlay=i2c-gpio,bus=3,i2c_gpio_sda=2,i2c_gpio_scl=3,i2c_gpio_delay_us=5
```

`i2c_gpio_delay_us=5` gives about 100 kHz. The bus appears as `/dev/i2c-3`, so run with `--bus 3`. Rerun test (c).

## Host check (no hardware)

The slave protocol logic and both CRCs were cross-checked on a PC with gcc: CRC 0x29B1 on both
sides, every bad-frame case returns the right status, and 2,000 random frames all got well-formed
responses. The HAL glue was type-checked against stub HAL headers only; it still needs to be built
in STM32CubeIDE and run on the Nucleo.
