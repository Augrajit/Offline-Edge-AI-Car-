# Results

Each CSV was produced on the board with `tools/eval_serial.py` and can be
broken down with `tools/analyze_results.py <file>.csv`.

Columns: `command, expected, got, result, confidence, seconds, error`.
A row passes only when every tool call matches exactly (action, direction
and amount).

| File | Model | Command file | Score |
|---|---|---|---|
| `before_tuning_testset.csv` | stock Needle 2 | `data/test_set.txt` | **36/80 (45%)** |
| `after_10epoch.csv` | fine-tuned, 10 epochs | `data/test_set.txt` | **62/80 (78%)** |
| `after_10epoch_samestyle.csv` | fine-tuned, 10 epochs | `data/test_same_style.txt` | **55/60 (92%)** |
| `after_1epoch_20.csv` | fine-tuned, 1 epoch | first 20 of `data/test_set.txt` | **13/20** |
| `guard_check.csv` | fine-tuned, 10 epochs | `data/guard_check.txt` | **5/6** |
| `baseline_results.csv` | stock Needle 2 | `data/baseline.txt` | **7/10** |

For comparison on the same first 20 commands of `test_set.txt`: stock 9/20,
1 epoch 13/20, 10 epochs 15/20.

`test_set.txt` uses phrasings that never appear in training, so it measures
generalisation. `test_same_style.txt` uses unseen sentences built from the
training templates, so the gap between the two (78% vs 92%) is mostly
phrasing, not task difficulty.

The one guard-check failure is `turn left 45 degrees right away`, which came
back with no calls instead of `turn left 45`.

Timing (the `seconds` column, reasoning on) is 11–108 s per command, with a
median of about 43 s; longer commands produce longer reasoning.
