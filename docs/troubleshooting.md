# Troubleshooting

## The S3 (Board A)

Three settings cause nearly every problem with this board. All three fail
*silently*, so check them first. `sdkconfig.defaults` already sets PSRAM and CPU
speed correctly and leaves the console at its default, so these problems
usually mean a stale `sdkconfig` or a different board.

**No serial output, but the board is clearly running.** The console is on the
UART bridge port, not USB-Serial/JTAG. Leave `CONFIG_ESP_CONSOLE_*` at its
default. With `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` the board runs but prints
nothing, while ROM boot messages still appear on the same port, which makes it
look like a crash.

**Boot loop with no error message.** The PSRAM line mode is wrong. 8 MB parts are
*octal*:

```
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
```

Quad mode gives `E quad_psram: PSRAM chip is not connected, or wrong PSRAM
line mode`, but only once the console works, which is why these two failures
together are hard to unpick.

**It runs, but about 1.5× too slowly.** ESP-IDF defaults the ESP32-S3 to
160 MHz. `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` is the documented maximum, not
an overclock.

After changing any of these, delete `esp32/needle_demo/sdkconfig` (or run
`idf.py fullclean`) so the defaults are applied again.

**Want a timing breakdown?** Uncomment the `ND_PROFILE` line in
`esp32/needle_demo/components/needle/CMakeLists.txt`. The board then prints
per-phase milliseconds at boot.

**`idf.py monitor` fails in a script.** It needs a TTY. Use
`python tools/mon.py <PORT> [seconds]` to read the console instead.

## The car link

The S3 prints these lines on its console. `tools/mon.py` shows them; the TUI
hides most of them.

| You see | Meaning | Fix |
|---|---|---|
| `EVT car sent ...` but never `EVT car accepted` | The packet left the S3, but the car didn't accept it | Check `CAR_MAC` against the `MAC:` line the car prints at boot. Make sure the car printed `radio ready (channel 1)` |
| `EVT car busy id=N` | The car was still running the previous plan | Wait for it to finish, or press BOOT to stop it |
| `EVT car rejected id=N` | The car couldn't parse the plan | Make sure both boards run firmware from the same commit |
| `ERR plan_too_long (max 8 steps)` | The command produced more than 8 steps | Split it into shorter commands |
| `ERR car_send_failed` | The ESP-NOW send failed locally | Reset the S3. Check that the radio came up (`EVT radio_up` at boot) |
| `EVT guard turn direction not in request` | The model chose a turn direction that isn't in your text | Working as intended: say "left" or "right" explicitly |
| `ACT none` | Nothing to do: off-topic text, or a turn with no direction | Rephrase with an explicit action, direction and amount |

**The car prints `plan accepted` but doesn't move.** Press BOOT: it never
starts on its own. A waiting plan is dropped after 60 s
(`not confirmed in time`). To run plans without the button, set
`REQUIRE_CONFIRM = false`, but only with the wheels off the floor first.

**Turns are the wrong size, or the car veers.** Movement is timed, not
measured, so recalibrate `TURN_DEG_PER_SEC`, `LEFT_SCALE` and `RIGHT_SCALE`
(see [reproduce.md](reproduce.md#2-car-board-b)). Weak batteries make every
turn short.

**A wheel spins backwards.** Set `INVERT_LEFT` or `INVERT_RIGHT` in the
sketch.

**The radio doesn't work at all.** Switch to the wired link: set
`LINK_ESPNOW 0` in `main.c` and wire it as in
[hardware.md](hardware.md#wired-link-optional). In wired mode the car collects
lines that arrive within 1.5 s of each other into one plan, then waits for BOOT
as usual.

## The model

**A correct-looking command does nothing.** Check the phrasing against
[`results/`](../results/README.md). The fine-tuned model gets 92% on
training-style sentences but 78% on new phrasings. Stating each step with its
direction and amount ("turn left 45 degrees") is the most reliable.

**Reasoning is slow.** `!think 0` skips it. The results in this repo were
measured with reasoning on.
