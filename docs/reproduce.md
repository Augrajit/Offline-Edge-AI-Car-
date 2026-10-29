# Reproducing the build

From a fresh clone to a car that drives from typed commands, then the evaluation
runs. Replace `<PORT>` with the serial port (`COM5`, `/dev/ttyUSB0`,
`/dev/cu.usbserial-XXXX`, ...).

You need:

- **ESP-IDF v5.5+** for Board A.
- **Arduino IDE** with the **ESP32 core 3.x** for Board B. The ESP-NOW receive
  callback uses the 3.x signature.
- **Python 3.**

The shell commands are for macOS/Linux. On Windows, use `.venv\Scripts\python`
in place of `.venv/bin/python`.

## 1. Host tools and the model

```bash
python3 -m venv .venv
.venv/bin/pip install numpy rich pyserial huggingface_hub sentencepiece
.venv/bin/python fetch_model.py          # model/needle2.cact, 13.7 MB, not committed
```

To use the fine-tuned model instead, build it with
[training/TRAINING.md](../training/TRAINING.md) and flash that file in step 4.

## 2. Car (Board B)

1. Wire it up as in [hardware.md](hardware.md).
2. Open `car/needle_car/needle_car.ino` in the Arduino IDE, select your ESP32
   board, and upload.
3. Open the serial monitor at 115200 baud and reset the board. Write down the
   MAC address it prints:

   ```
   MAC: 2C:BC:BB:0C:31:24
   radio ready (channel 1)
   car ready. BOOT button = go / stop. Wired: M F 2 / T R 90 / S.
   ```

4. **Calibrate it now**, with the wheels off the floor at first. Type plan
   steps straight into the serial monitor, then press BOOT to run them:

   | Type | Expect |
   |---|---|
   | `M F 2` | 2 s forward. If it veers, adjust `LEFT_SCALE` / `RIGHT_SCALE` |
   | `T R 360` | One full turn. If it overshoots or undershoots, adjust `TURN_DEG_PER_SEC` |
   | `S` | Stop |

   Re-upload after each change. The committed values (`TURN_DEG_PER_SEC = 120`,
   `RIGHT_SCALE = 1.07`) fit one particular chassis and battery.

## 3. S3 firmware (Board A)

Put the car's MAC into `CAR_MAC` in `esp32/needle_demo/main/main.c`:

```c
static const uint8_t CAR_MAC[6] = { 0x2C, 0xBC, 0xBB, 0x0C, 0x31, 0x24 };
```

Then build and flash, with ESP-IDF's `export` script already run in this shell:

```bash
cd esp32/needle_demo
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash
cd ../..
```

The tool schema is generated from `tools/led.json` at build time. Edit the JSON,
not the C string.

## 4. Flash the model

The model goes into its own partition at `0x210000`. This takes ~3 min, and
only needs redoing when the model changes:

```bash
esptool.py --chip esp32s3 -p <PORT> -b 921600 write_flash 0x210000 model/needle2.cact
```

## 5. Drive

```bash
.venv/bin/python tools/needle_tui.py --serial <PORT>
```

On first boot the S3 primes the prompt once (a progress bar with an ETA,
about a minute), pulses its LED green three times, and prints `READY`. Then:

1. Type `go forward 2 seconds then turn left 90 degrees`.
2. Watch the reasoning stream, then the `move` and `turn` calls.
3. If the car's serial monitor is open, it prints
   `radio: plan 1 accepted, 2 step(s) waiting`.
4. **Press BOOT on the car.** It runs the plan and prints `DONE`.

`!think 0` in the TUI turns reasoning off: responses are faster, but the
results in this repo were measured with it on.

## 6. Evaluate

The board must be connected and idle, and the TUI closed, since only one
program can hold the port:

```bash
.venv/bin/python tools/eval_serial.py --port <PORT> --file data/test_set.txt --out my_test_set.csv
.venv/bin/python tools/analyze_results.py my_test_set.csv
```

80 commands at ~45 s each takes about an hour. The car doesn't need to be on
for scoring, which uses the S3's `ACT` lines. Other command files:
`data/test_same_style.txt`, `data/guard_check.txt`, `data/baseline.txt`.

Compare against [`results/`](../results/README.md): about 36/80 is expected for
the stock model, and 62/80 for the 10-epoch fine-tune.

## Optional: the host engine

The same engine runs on a PC, for trying the model or checking the engine
without the board:

```bash
cmake -S host -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
.venv/bin/python tools/needle_tui.py --local
```

To check the engine against independent references:

```bash
.venv/bin/python tools/check_engine.py model/needle2.cact ./build/nd_dump
.venv/bin/python tools/check_tokenizer.py model/needle2.cact ./build/nd_dump
./build/nd_gtest
```
