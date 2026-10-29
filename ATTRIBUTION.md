# Attribution

This project is licensed under the Apache License 2.0 (see [LICENSE](LICENSE)).
It builds on the two works below, which are also Apache 2.0.

## Upstream: needle-2-esp32

<https://github.com/andrisgauracs/needle-2-esp32>, by Andris Gauracs.

That project contains the C99 inference engine, the ESP32-S3 firmware and the
host tools that this fork starts from.

**Used unchanged:** `engine/`, `host/`, `esp32/led_probe/`, `esp32/psram_test/`,
`esp32/needle_demo/` (apart from the files listed below), `tools/check_engine.py`,
`tools/check_tokenizer.py`, `tools/ref_forward.py`, `tools/mon.py`, `LICENSE`.

**Modified.** The source files (`main.c`, `main/CMakeLists.txt`,
`needle_tui.py`) also carry a change notice at the top:

| File | Change |
|---|---|
| `esp32/needle_demo/main/main.c` | `move`/`turn` tools in place of `set_led`; ESP-NOW and UART link to the car; turn-direction guard; `MAX_NEW` 96 → 160 |
| `esp32/needle_demo/main/CMakeLists.txt` | `REQUIRES` for Wi-Fi, netif, UART and NVS |
| `esp32/needle_demo/components/needle/CMakeLists.txt` | Recreated; the upstream `.gitignore` excluded it |
| `tools/led.json` | `move`/`turn` schema |
| `tools/needle_tui.py` | Waits for `EVT turn_end`, so multi-step plans finish before the next prompt |
| `.gitignore` | `needle/` → `/needle/`, plus rules for model and checkpoint files |
| `README.md` | Rewritten for the robot car |

`tools/led_original.json` is the upstream schema, kept for reference.

## Needle 2 model

<https://github.com/cactus-compute/needle>, by Cactus Compute.

The model weights (`needle2.cact`, downloaded by `fetch_model.py` and not
included in this repository) and the `cactus-needle` Python package used for
fine-tuning both come from Cactus Compute. A fine-tuned `.cact` built with
[training/TRAINING.md](training/TRAINING.md) is a derivative of their weights.

## New in this fork

`fetch_model.py` (the download step from the upstream README, as a script),
`car/needle_car/needle_car.ino`, `tools/make_dataset.py`,
`tools/make_same_style_test.py`, `tools/eval_serial.py`,
`tools/analyze_results.py`, everything in `data/`, `results/`, `training/` and
`docs/`, the demo video (hosted on GitHub, linked from the README), and this
file.

The upstream LED demo GIF (`needle_demo.gif`) has been removed; it is still in
the upstream repository.

Copyright 2026 Augrajit. Licensed under the Apache License, Version 2.0.
