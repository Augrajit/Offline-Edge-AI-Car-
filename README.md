# Needle 2 robot car on an ESP32-S3

Plain-English driving commands, turned into robot moves by a 45M-parameter
language model running entirely on a microcontroller. No network, no cloud.

```
"go forward 4 seconds, then turn right 90 degrees"
        │  typed over USB serial
        ▼
ESP32-S3 (Board A) ── Needle 2, grammar-constrained ──► move(forward, 4)
                                                        turn(right, 90)
        │  ESP-NOW radio (or 3-wire UART)
        ▼
ESP32 car (Board B) ── waits for the BOOT button ──► TB6612FNG ──► motors
```

This is a fork of [andrisgauracs/needle-2-esp32](https://github.com/andrisgauracs/needle-2-esp32),
which runs Needle 2 on an ESP32-S3 to control the onboard RGB LED. The C99
inference engine is unchanged. This fork adds:

- **`move` and `turn` tools** in place of `set_led`, and a firmware check that
  refuses a turn whose direction isn't in the request.
- **A second board that drives the car.** The S3 sends it the whole plan as one
  ESP-NOW packet, or line by line over a wired UART. The car runs the plan only
  after you press its button, and it enforces its own safety limits.
- **A LoRA fine-tune of Needle 2** for robot commands, with the dataset
  generator, test sets, evaluation scripts and the results from the board.

<video src="project.mp4" controls muted width="100%"></video>

**[Watch the demo video (project.mp4)](project.mp4)**: the car driven by typed
commands. Each command takes ~40 s on the board.

## Results

Measured on the board with [`tools/eval_serial.py`](tools/eval_serial.py).
A command passes only if every call matches exactly: action, direction and amount.

| Test set | Stock Needle 2 | Fine-tuned (10 epochs) |
|---|---|---|
| Held-out phrasings ([`test_set.txt`](data/test_set.txt)) | 36/80 (45%) | **62/80 (78%)** |
| Unseen sentences, training style ([`test_same_style.txt`](data/test_same_style.txt)) | — | **55/60 (92%)** |
| Guard check ([`guard_check.txt`](data/guard_check.txt)) | — | 5/6 |

Full CSVs and a per-file breakdown are in [`results/`](results/README.md).
[`training/TRAINING.md`](training/TRAINING.md) covers how the fine-tune was made.

## What the car understands

Two tools, defined in [`tools/led.json`](tools/led.json). The file keeps its
upstream name because the build reads it from that path.

| Tool | Arguments | Default if left out | Clamped to |
|---|---|---|---|
| `move` | `direction`: forward / backward, `seconds` | forward, 2 s | 0.5–10 s |
| `turn` | `direction`: left / right, `degrees` | 90° | 1–360° |

- A command can chain up to **8 steps**. Longer plans are rejected.
- **A turn without a direction is refused**: "turn 45 degrees" does nothing. If
  the model picks a direction that doesn't appear in the request, the firmware
  blocks the whole command. "right away" and "right now" don't count as a
  direction.
- **Anything off-topic produces no action.**

## Getting started

You need an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM, an ESP32 dev board
for the car, a TB6612FNG motor driver and a two-motor chassis.

1. [docs/hardware.md](docs/hardware.md): parts, wiring and pin map.
2. [docs/reproduce.md](docs/reproduce.md): build and flash both boards, pair
   them, then run the evaluation.
3. [docs/troubleshooting.md](docs/troubleshooting.md): if something stays
   silent or runs slowly.

## Layout

```
engine/                 portable C99 inference engine (upstream, unchanged)
host/                   CLI harness and grammar tests (upstream)
esp32/needle_demo/      S3 firmware: model, move/turn tools, car link
car/needle_car/         Arduino sketch for the car board
tools/                  TUI, schema, dataset + eval scripts, engine checks
data/                   training data and test sets
results/                evaluation CSVs from the board
training/               how the fine-tune was made
docs/                   hardware, reproduction, troubleshooting
```

## Limitations

- **Slow:** 11–108 s per command (median ~43 s) with reasoning on. The
  workload is compute-bound at 1.87 tokens/s.
- **Timed, not measured:** the car has no encoders. Turn angles come from a
  calibrated rate (`TURN_DEG_PER_SEC`), which drifts with battery voltage and
  floor surface.
- **Phrasing still matters:** 92% on training-style sentences drops to 78% on
  new phrasings.
- **Small prompt:** the 256-token window leaves room for two or three tools.

## License and credits

Apache License 2.0; see [LICENSE](LICENSE). Files changed from upstream carry a
change notice at the top. [ATTRIBUTION.md](ATTRIBUTION.md) lists what came from
where.

- The ESP32 port and inference engine are by
  [Andris Gauracs](https://github.com/andrisgauracs/needle-2-esp32), Apache 2.0.
- Needle 2 and its weights are by
  [Cactus Compute](https://github.com/cactus-compute/needle), Apache 2.0.
