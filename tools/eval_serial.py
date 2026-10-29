#!/usr/bin/env python3
"""Send commands to the board, log results to CSV."""
import argparse, csv, re, sys, time
import serial


def norm(line):
    body = line[4:].strip()
    kv = dict(t.split("=", 1) for t in body.split() if "=" in t)
    try:
        if body.startswith("move"):
            return "move %s %g" % (kv.get("dir", "?"), float(kv.get("seconds", 0)))
        if body.startswith("turn"):
            return "turn %s %d" % (kv.get("dir", "?"), int(float(kv.get("degrees", 0))))
    except ValueError:
        pass
    return body


def rd(ser):
    return ser.readline().decode("utf-8", "replace").strip()


def wait_ready(ser):
    booting = False
    end = time.time() + 6
    while time.time() < end:
        line = rd(ser)
        if "EVT priming" in line or "EVT mapped" in line:
            booting = True
    if not booting:
        return True
    print("Board is starting up; waiting for READY (up to 4 min)...")
    end = time.time() + 240
    while time.time() < end:
        if "READY - type a request" in rd(ser):
            return True
    return False


def run_cmd(ser, cmd, timeout=300):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    ser.flush()
    acts, conf, ms, err = [], None, None, None
    t0 = time.time()
    while time.time() - t0 < timeout:
        line = rd(ser)
        if not line:
            continue
        if line.startswith("ACT "):
            acts.append(norm(line))
        elif line.startswith("CONF "):
            try:
                conf = float(line[5:])
            except ValueError:
                pass
        elif line.startswith("EVT done"):
            m = re.search(r"ms=(\d+)", line)
            if m:
                ms = int(m.group(1))
        elif line.startswith("ERR "):
            err = line[4:]
        elif line.startswith("EVT turn_end"):
            return acts, conf, ms, err
    return acts, conf, ms, "timeout"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--file", required=True)
    ap.add_argument("--out", default="results.csv")
    ap.add_argument("--no-think", action="store_true")
    a = ap.parse_args()

    items = []
    for raw in open(a.file, encoding="utf-8"):
        raw = raw.strip()
        if not raw or raw.startswith("#"):
            continue
        cmd, _, exp = raw.partition("==>")
        items.append((cmd.strip(), exp.strip()))

    ser = serial.Serial(a.port, 115200, timeout=0.5)
    ser.setDTR(True)
    ser.setRTS(False)
    if not wait_ready(ser):
        print("Board never became ready.")
        return 1
    ser.write(b"!think 0\n" if a.no_think else b"!think 1\n")
    time.sleep(1)

    npass = nscored = 0
    with open(a.out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["command", "expected", "got", "result", "confidence", "seconds", "error"])
        for i, (cmd, exp) in enumerate(items, 1):
            acts, conf, ms, err = run_cmd(ser, cmd)
            got = " | ".join(acts) if acts else "none"
            res = ""
            if exp:
                nscored += 1
                res = "PASS" if got == exp else "FAIL"
                npass += res == "PASS"
            secs = "%.1f" % (ms / 1000.0) if ms else ""
            w.writerow([cmd, exp, got, res, conf if conf is not None else "", secs, err or ""])
            f.flush()
            print("[%d/%d] %-4s %5ss conf=%s  %s -> %s" % (i, len(items), res, secs, conf, cmd, got))
    if nscored:
        print("Score: %d/%d" % (npass, nscored))
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
