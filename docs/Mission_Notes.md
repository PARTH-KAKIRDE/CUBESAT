# Mission Notes (early design notes)

> Early working notes for the mission. Some of these decisions were later revised.
> For example, the TT&C notes below say "fixed antenna / RF switch", but the current design is in
> [Electronics/TTC/TTC_Design_Record.md](Electronics/TTC/TTC_Design_Record.md).

### **2U SAT: 10 x 10 x 20 cm; 4kgs**

- Use R-Pi + STM32 combination:
- COPyCubed's existing open source flight software

## CubeSat Subsystems

### 1. Structure and Mechanical (STR)

The physical chassis, frame, rails, fasteners, and deployable mechanisms. Everything that gives the satellite its shape and holds all other subsystems together. Includes separation switches, deployment spring mechanisms, and any hinged or deployable panels.

### 2. Electrical Power System (EPS)

Solar panels, batteries, charge controller, power distribution, load switches, and protection circuits. Responsible for generating, storing, and distributing power to all other subsystems. Includes power monitoring — voltages, currents, state of charge.

### 3. Onboard Computer (OBC)

The main flight computer running mission logic, scheduling, data management, and coordinating all other subsystems. In this mission: RPi CM4. Sometimes called the C&DH — Command and Data Handling.

### 4. Attitude Determination and Control (ADCS)

Sensors (magnetometer, gyroscope, sun sensors, star tracker) for determining orientation, and actuators (magnetorquers, reaction wheels, thrusters) for controlling it. Can be a standalone board or integrated into the OBC.

### 5. Telemetry, Tracking and Command (TT&C)

The communications subsystem. Radio transceiver, antenna, and the protocol stack for receiving commands from and sending data to the ground station. Sometimes split into a separate transmitter and receiver.

### 6. Payload (PL)

The science or mission instruments. In this mission: optical camera, solar flux sensor, magnetometer (science use), and Geiger counter. Everything that constitutes the actual mission purpose rather than spacecraft housekeeping.

### 7. Thermal Control (TCS)

Passive and active management of temperatures across the satellite. Includes thermal coatings, surface finishes, thermal interface materials, insulation blankets, and heaters. Often not a standalone board but a design consideration applied across all subsystems.

### 8. Flight Software (FSW)

Sometimes listed separately from OBC hardware as its own subsystem. Includes the RTOS or operating system, device drivers, ADCS algorithms, fault detection and response, telemetry and command handling, and mission scheduling.

---

### Additional Subsystems (Mission Dependent)

- **Propulsion (PROP)** — cold gas thrusters, electrospray, or resistojet for orbit maintenance or deorbit.
- **Inter-Satellite Link (ISL)** — a separate radio for satellite-to-satellite communication in a constellation.
- **Data Storage (DSS)** — dedicated storage hardware and data management software for data-intensive missions.
- **Mechanisms (MECH)** — deployable booms, antenna deployment systems, camera covers, and other moving parts.

---

### Mission Subsystem Mapping

| Subsystem | Implementation |
| --- | --- |
| Structure | Custom aluminum chassis or commercial CubeSat frame |
| EPS | Custom STM32-managed EPS board |
| OBC | RPi CM4 |
| ADCS | STM32 + magnetometer + IMU + magnetorquers + reaction wheels |
| TT&C | UHF transceiver on RPi UART |
| Payload | Optical camera + solar flux sensor + magnetometer + Geiger counter |
| Thermal | Passive coatings + battery heater |
| FSW | Linux PREEMPT-RT (RPi) + FreeRTOS (STM32) |

## Final Subsystem:

### The system is organized into five subsystems:

**Mechanical** — chassis, PCB stack, deployable mechanisms, payload mounting, harness routing, mass and center of mass management.

**EPS** — solar panels, battery, charge controller with MPPT, switched power distribution rails, current and voltage monitoring, fault protection.

**ADCS** — RM3100 magnetometer, ICM-42688 IMU, sun sensors, GPS receiver for determination. Magnetorquers for detumbling and coarse control, reaction wheels for precise pointing.

**Payload** — optical camera, solar flux sensor, magnetometer science data, Geiger counter if included. Data management, compression, and storage on RPi.

**Comms (TT&C)** — UHF transceiver, deployable antenna, AX.25 link layer, CCSDS-inspired packet structure, Yamcs ground software, SatNogs ground station network.

## Software Stack

**STM32 (FreeRTOS in C):**

- EPS management, power monitoring, load switching
- ADCS sensor fusion and control laws
- Watchdog and fault detection
- Inter-processor communication protocol with RPi over UART

**RPi (Linux PREEMPT-RT + Python):**

- State machine mission software (safe mode, nominal, science, downlink)
- Payload scheduling and camera control
- Data storage and compression
- Communications management

**Ground (Yamcs + SatNogs):**

- Yamcs for mission control, telemetry display, and commanding
- SatNogs for ground station network access

## **PAYLOAD:**

1. Optical Camera
2. Solar Flux Sensor
3. Magnetometer (Place in a boom or away from power electronics to avoid interference from Reaction wheel currents, Magnetorquer currents, Radio transmissions)
4. UV Sensor

### EPS:

Solar Cell:

Incidence Angle Effect:

**Is= Io cos(qi)**

Surface area of the array's surface from your power generation requirement is:

**A = P reqd / Io cos(qi) * n**

- Required power to generate based off your power budget:  P_reqd
- Efficiency of the solar cells you've chosen: n
- Solar irradiance at your distance from the sun: I_o
- Incidence angle across all solar arrays: q_i (q_i = 0 degrees is full face towards sun)

On the solar input side, a simple Schottky diode is the easiest implementation — low cost, zero firmware, and adequate for prototype validation even though it has a small forward voltage drop. For the battery side, a P-channel MOSFET ideal diode circuit is cleaner but adds design effort. For a prototype, the Schottky on solar input is sufficient.

### ADCS:

#### Types of attitude control:

1. Passive control
2. Active control

#### Attitude control modes:

1. Detumbling control (after the separation from the launch vehicle or release from the ISS)
    1. Active control:
    Generate magnetic moment by means of magnetic torquers to interact with the Earth's magnetic field to actively slow down the rotational rate.
    2. Passive Control:
    Utilize permanent magnets and magnetic hysteresis dumpers to passively slow down
    the rotational rate.
2. Gravity gradient control (Passive Control)
    - Satellites with long shapes and spread mass distribution experience a gravity gradient torque such that the longitudinal direction points toward the Earth.
    - Cameras, antennas, and sensors can be pointed toward the Earth without additional electrical power for the attitude control.
    - Pointing accuracy is relatively low.
    - Can be combined with active attitude control with some attitude control actuators, such as magnetic torquers and reaction wheels.
3. Spin-stabilization control
4. 3-axis control (Pointing control): Inertial, Nadir, Target, Velocity Direction, etc.
    - Attitude control actuators, such as magnetic torquers and reaction wheels, are used for active 3-axis control.
    - Reaction wheels can realize agile and stable attitude control.
    - Disturbance torques acting on the satellite gradually accumulate as angular momentum stored in the reaction wheels. Reaction wheels cannot be operated for a long time without desaturation using magnetic torquers.
    - Satellite attitude shall be determined precisely by means of a combination of attitude determination sensors.

## TT&C:

Frequencies for educational and student-built satellites generally fall into **IARU** coordinated amateur bands. The 434–438 MHz (UHF) and 144–146 MHz (VHF) bands are commonly used on a secondary, non-interference basis.

Survey of modulation and coding schemes for application in CubeSat systems research paper  proposes OQPSK as the most appropriate modulation scheme for CubeSats due to the good BER performance and bandwidth efficiency.

FPGA is used for TT&C

#### Data Collection:

**Measurements and status of health**

- Power functions
- Telemetry functions
- Telecommand functions
- Attitude control functions
- Propulsion functions
- Structure functions
- Antenna functions
- Tracking functions
- Payload functions
- Miscellaneous functions
- Measurements:
    - Acceleration, velocity, displacement
    - Angular rate, angular position
    - Pressure
    - Temperature
    - Density
    - Resistance
    - Voltage, current
    - Intensity
    - Electric field, magnetic field

437MHz Frequency (needs to be registered with Wireless Planning and Consultancy India Pvt.ltd)

λ = (3 × 10⁸ m/s) / (437 × 10⁶ Hz) ≈ **0.686 m**, or about **68.6 cm**

Using the more precise value c = 299,792,458 m/s gives 0.6860 m, so the result holds either way.

For antenna work, a quarter-wave element would be about 17.2 cm, and a half-wave dipole about 34.3 cm (in practice slightly shorter, roughly 5% less, due to the velocity factor and end effects).

### PCB Design

0805 smd footprint of components can be used for direct soldering

#### Layout:

**Top panel (zenith-facing, away from Earth):**

- Magnetometer
- Solar flux sensor
- UV sensor
- 1 of 6 sun sensors

**Between top panel and PCB stack:**

- Battery

**PCB 1 — Payload:**

- STM32F446
- Handles solar flux + UV sensor readings via I2C (UV/VEML6075 confirmed I2C; solar flux — confirm ADC vs I2C per exact part) and/or ADC
- Camera does not route through this board — CSI direct to RPi
- Separate I2C peripheral instance for local sensors vs. RPi-bus communication
- Decoupling caps on ADC/I2C supply pins (precaution near EPS)

**PCB 2 — EPS:**

- No MCU
- Chain: Solar Array → Reverse Polarity Protection → MPPT → Battery Charger → Battery + Protection Circuit → DC Bus → INA219 (+ shunt) → Buck Converters → Fuses → Manual Switch → Load
- INA219 → I2C → RPi directly

**PCB 3 — ADCS:**

- STM32F446
- Gyro sensor — I2C
- Magnetic torquer + driver — GPIO/PWM
- Sun sensors (×6) — direct analog to STM32 ADC pins
- Magnetometer (physically on top panel) — I2C, logically read/owned here
- Firmware: magnetometer-torquer gating (measure → actuate → settle → measure)
- Separate I2C peripheral instance for local sensors vs. RPi-bus communication

**PCB 4 — TT&C:**

- STM32F446
- RFM98W/SX1278 module — SPI (breakout, header-mounted, integrated PA)
- RF switch — GPIO
- Balun
- Fixed (non-deployable) antenna

**Bottom panel (nadir-facing, Earth-facing):**

- Camera, CSI direct to RPi
- Fixed antenna

**Between nadir panel and PCB 4:**

- OBC (RPi 3A+)

**Side panels:**

- Solar arrays (multi-face)
- Remaining 5 sun sensors (1 per face)

**System-wide bus architecture:**

- RPi = I2C master to: INA219 (EPS), and each STM32's dedicated "RPi-facing" I2C peripheral (Payload, ADCS, TT&C) — 4 total I2C endpoints on one shared master bus, each with a unique slave address
- Each STM32 additionally runs its own separate local I2C bus for its own sensors (gyro on ADCS, UV/solar flux on Payload) — isolated from the RPi bus
- ADCS's sun sensors and TT&C's RF switch are GPIO/ADC only, not bus traffic
- No Earth sensor, no reaction wheels, no RCS, no GPS, no deployable antenna, no STM32 on EPS
- All three MCU boards standardized on STM32F446

### Confirmed ADCS Component List

- **MCU**: STM32F446RET6
- **Magnetometer**: MMC5983MA (I2C/SPI, 3.3V)
- **IMU/Gyro**: MPU-6050 (I2C, 3.3V tolerant)
- **Sun Sensor**: BPW34 photodiodes (analog, one per face, six total)
- **MTQ Driver**: DRV8833 (pending confirmation)

### I2C Pin Options for STM32F446RET6

**I2C1:**

- SCL → PB6 (AF4) or PB8 (AF4)
- SDA → PB7 (AF4) or PB9 (AF4)

**I2C2:**

- SCL → PB10 (AF4) or PF1 (AF4)
- SDA → PB11 (AF4) or PF0 (AF4)

**I2C3:**

- SCL → PA8 (AF4)
- SDA → PC9 (AF4)

Pick one peripheral and use both its SCL and SDA pins together.

### Timer PWM Output Pins

**TIM1 (Advanced, 4 channels):**

- CH1 → PA8 (AF1)
- CH2 → PA9 (AF1)
- CH3 → PA10 (AF1)
- CH4 → PA11 (AF1)

**TIM2 (General, 4 channels):**

- CH1 → PA0 (AF1) or PA5 (AF1) or PA15 (AF1)
- CH2 → PA1 (AF1) or PB3 (AF1)
- CH3 → PA2 (AF1) or PB10 (AF1)
- CH4 → PA3 (AF1) or PB11 (AF1)

**TIM3 (General, 4 channels):**

- CH1 → PA6 (AF2) or PB4 (AF2) or PC6 (AF2)
- CH2 → PA7 (AF2) or PB5 (AF2) or PC7 (AF2)
- CH3 → PB0 (AF2) or PC8 (AF2)
- CH4 → PB1 (AF2) or PC9 (AF2)

**TIM4 (General, 4 channels):**

- CH1 → PB6 (AF2) or PD12 (AF2)
- CH2 → PB7 (AF2) or PD13 (AF2)
- CH3 → PB8 (AF2) or PD14 (AF2)
- CH4 → PB9 (AF2) or PD15 (AF2)

**TIM5 (General, 4 channels):**

- CH1 → PA0 (AF2)
- CH2 → PA1 (AF2)
- CH3 → PA2 (AF2)
- CH4 → PA3 (AF2)

**TIM9 (General, 2 channels):**

- CH1 → PA2 (AF3) or PE5 (AF3)
- CH2 → PA3 (AF3) or PE6 (AF3)

**TIM10 (General, 1 channel):**

- CH1 → PB8 (AF3)

**TIM11 (General, 1 channel):**

- CH1 → PB9 (AF3)

### Pins to Avoid

**Already assigned:**

- PB6, PB7 — I2C1 (MPU-6050 SCL/SDA)
- PA0–PA5 — ADC sun sensors
- PA8, PA9 — TIM1 CH1/CH2 (DRV3 PWM per earlier suggestion — verify if you're still following that)
- PA15 — MPU-6050 INT
- PB0, PB1 — confirmed earlier for ~SLEEP/~FAULT of another DRV or ADC backup

**Avoid entirely:**

- PA13, PA14 — SWDIO and SWCLK (debug interface, never use these as GPIO)
- PB3 — SWO (debug trace, avoid unless debug is disabled)
- PC14, PC15 — OSC32 crystal pins if you plan to use RTC
- PH0, PH1 — HSE crystal pins if using external oscillator

### COMPONENTS

ADCS:

1. MCU - STM32 f446 - 12 * 12 * 1.6 mm
2. Magnetometer - MMC5983MA - 3 * 3 * 1 mm
3. Gyro - MPU6050 - 4.8 * 4.8 * 1 mm + berg connectors
4. Sun Sensor - BPW34 - 5.4 * 4.3 * 3.2 mm (CONNECTED IN REVERSE BIAS WITH TIA)
5. MTQ Driver -DRV8833 - 5.1 * 6.6 *1.2 mm

PAYLOAD:

1. STM32F446 - 12×12×1.6mm
2. UV Sensor - VEML6075 - 4.08×2.35×0.7mm
3. Solar Flux Sensor - TSL2591 -     2.0×2.0×0.75mm
4. camera - WebCam
5. Connector - JST-PH 4-pin connector (×2 — one to top-panel sensors, one to RPi)

#### **Moment of inertia (I)** — depends on satellite mass and geometry:

For a 2U CubeSat (10×10×20cm, ~2kg estimated):

<u>**m_target = 0.001 × 10 = 0.01 A·m² per axis**</u>

### ORBIT:

Polar Orbit with altitude of around 600km LEO.

Earth's magnetic field is N-S and at Equator it is weakest. Mapping Magnetic field wont give full coverage around earth and readings will be of the weakest part of the magnetic field only rather than the complete magnetic field.

LEO is best for photographing earth and magnetic field follows inverse square law so going farther reduces the strength of the field. Also at around 600km atmospheric drag and space debris is comparitively less.
