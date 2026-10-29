# Fine-tuning Needle 2 for the car

Stock Needle 2 scores 36/80 on the held-out robot commands. A LoRA adapter
trained on 1,200 generated examples raises that to 62/80. This page covers how
the data and the model were made.

## 1. Data

```bash
python tools/make_dataset.py          # writes data/train.jsonl and data/test_set.txt
python tools/make_same_style_test.py  # writes data/test_same_style.txt
```

`make_dataset.py` is deterministic (default `--seed 7`). With its defaults it
reproduces the committed `data/train.jsonl` and `data/test_set.txt` byte for
byte. It reads the tool schema from `tools/led.json` and refuses any schema that
isn't exactly `move` + `turn`.

The rules it teaches match the firmware:

- A command that contains a turn with no direction gets `answers: []`, so the
  whole command is refused rather than partly run.
- `seconds` and `degrees` may be left out; the firmware then uses 2 s / 90°.
- Off-topic text gets `answers: []`.
- A command chains up to 3 steps (`--max-steps`).

**Training and test sets use different phrasing templates** (`TRAIN` and
`TEST` in the script), and no test sentence appears in training. So
`test_set.txt` measures generalisation to new wording.
`test_same_style.txt` holds 60 new sentences built from the *training*
templates; it shows how much of the remaining error comes from phrasing.

Each line of `train.jsonl` has the `query`, `tools`, `answers` format that
`needle finetune` expects:

```json
{"query": "Can you take a 30 degree turn to the left",
 "tools": [...move and turn schema...],
 "answers": [{"name": "turn", "arguments": {"direction": "left", "degrees": 30}}],
 ...}
```

## 2. Train a LoRA adapter

The fine-tune uses the `needle` CLI from Cactus Compute's `cactus-needle`
package (version 2.0.15, on JAX 0.10.2). Install it in its own venv, kept apart
from the lightweight `.venv` used by the board tools:

```bash
python -m venv .venv-train
.venv-train/bin/pip install cactus-needle
```

Then train:

```bash
needle finetune data/train.jsonl --epochs 10 --out checkpoints/needle_lora.pkl
```

Without `--checkpoint`, the base Needle 2 checkpoint is downloaded from Hugging
Face. Pass `--checkpoint checkpoints/needle2.pkl` to reuse a local copy.

Two runs were evaluated, at 1 epoch and at 10 epochs. The other
hyperparameters (learning rate, batch size, LoRA rank) weren't recorded; see
`needle finetune --help` for the defaults.

## 3. Export to `.cact` and flash

Merge the adapter into the base checkpoint and export:

```bash
needle build checkpoints/needle2.pkl --lora checkpoints/needle_lora.pkl --out my_needle.cact
```

The output is the same size as the stock model (13.7 MB). It goes into the same
flash partition, in place of `model/needle2.cact`:

```bash
esptool.py --chip esp32s3 -p <PORT> -b 921600 write_flash 0x210000 my_needle.cact
```

The firmware doesn't need rebuilding. `.cact` and `.pkl` files are gitignored.

## 4. Results

| Run | First 20 of `test_set.txt` | Full `test_set.txt` | `test_same_style.txt` |
|---|---|---|---|
| Stock | 9/20 | 36/80 | — |
| 1 epoch | 13/20 | — | — |
| 10 epochs | 15/20 | **62/80** | **55/60** |

The CSVs are in [`results/`](../results/README.md). To see where a run fails:

```bash
python tools/analyze_results.py results/after_10epoch.csv
```
