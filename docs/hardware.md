# Hardware

Two boards. **Board A** (the ESP32-S3) runs the model. **Board B** (any classic
ESP32 dev board) drives the motors. They talk over ESP-NOW radio by default, or
over a 3-wire UART.

## Board A: ESP32-S3

This board is specific: the model needs the flash and PSRAM listed here.

| | |
|---|---|
| Chip | ESP32-S3 (Xtensa LX7, dual core), run at 240 MHz |
| Flash | **16 MB**: the model alone takes 13.1 MB |
| PSRAM | **8 MB octal** (an "N16R8"-class module) |
| Console | the **UART bridge** USB port, not the native USB port |

A board with 2 MB PSRAM or quad PSRAM will not work: the KV cache alone is
3.5 MB. If something stays silent or boot-loops, see
[troubleshooting.md](troubleshooting.md).

## Board B: the car

| Part | Notes |
|---|---|
| ESP32 dev board | Classic ESP32 with Wi-Fi (for ESP-NOW) and a BOOT button on GPIO0 |
| TB6612FNG dual H-bridge | Drives both motors |
| Two-wheel chassis with DC gear motors | Differential drive: the car turns by spinning its wheels in opposite directions |
| Motor battery | Use whatever suits your motors. It goes to the TB6612FNG's VM pin, not the ESP32 |

### Pin map (car side)

Set in [`car/needle_car/needle_car.ino`](../car/needle_car/needle_car.ino):

| ESP32 pin | Connects to |
|---|---|
| GPIO32 | TB6612FNG PWMA (left motor speed) |
| GPIO18 | AIN1 |
| GPIO25 | AIN2 |
| GPIO14 | PWMB (right motor speed) |
| GPIO26 | BIN1 |
| GPIO27 | BIN2 |
| 3V3 | STBY (or a spare GPIO; set `PIN_STBY`) and VCC |
| GND | TB6612FNG GND, battery GND |
| GPIO0 (BOOT) | Built-in button: **GO** when a plan waits, **STOP** while one runs |

Motor outputs AO1/AO2 go to the left motor and BO1/BO2 to the right. If a wheel spins the
wrong way, set `INVERT_LEFT` or `INVERT_RIGHT` instead of rewiring.

### Wired link (optional)

The firmware uses ESP-NOW unless `LINK_ESPNOW` is set to `0` in `main.c`. For
the wired link:

| ESP32-S3 (Board A) | ESP32 (Board B) |
|---|---|
| GPIO17 (TX) | GPIO16 (RX2) |
| GPIO18 (RX) | GPIO17 (TX2), optional: the car never replies over UART |
| GND | GND |

Both sides run at 115200 baud, 8N1.

## Safety

The car is the last line of defence, so it enforces its own limits whatever it
receives:

- **Nothing moves until you press BOOT.** A received plan waits for up to 60 s,
  then is dropped (`REQUIRE_CONFIRM`, `WAIT_TIMEOUT_MS`).
- Pressing BOOT while the car is moving stops it immediately, and so does
  sending `S` over USB serial.
- Each move lasts 0.5–10 s and each turn is 1–360°. A plan has at most 8 steps.
- The motors are braked at power-on.
