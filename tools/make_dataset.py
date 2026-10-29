#!/usr/bin/env python3
"""Make robot-command data for Needle 2 (tools: move, turn).

Rules taught (they match the firmware):
  * turn needs a direction. A command containing a turn with no direction
    gets answers [] (the whole command is refused).
  * seconds / degrees may be left out; the firmware then uses 2 s / 90 deg.
  * off-topic text gets answers [].
Train and test use different phrasings, so test scores measure generalisation.
"""
import argparse
import json
import os
import random
from collections import Counter

DEF_SECS, DEF_DEG = 2, 90
SEC_VALUES = [1, 2, 3, 4, 5, 6, 7, 8, 10, 0.5, 1.5, 2.5, 3.5]
DEG_VALUES = [15, 30, 45, 60, 90, 120, 135, 180, 270, 360]
SEC_WORDS = {1: "one", 2: "two", 3: "three", 4: "four", 5: "five",
             6: "six", 7: "seven", 8: "eight", 9: "nine", 10: "ten"}
DEG_WORDS = {30: "thirty", 45: "forty five", 60: "sixty", 90: "ninety",
             180: "one hundred eighty"}

TRAIN = {
    "fwd_amt": ["go forward for {t}", "move forward {t}", "drive ahead for {t}",
                "go straight for {t}", "forward {t}", "run forward for {t}",
                "move ahead {t}"],
    "fwd_no": ["go forward", "move forward", "go straight", "drive forward",
               "go ahead", "go straight ahead"],
    "bwd_amt": ["go backward for {t}", "move back {t}", "reverse for {t}",
                "back up for {t}", "drive backward {t}"],
    "bwd_no": ["go backward", "reverse", "back up", "move back"],
    "turn_amt": ["turn {d} {a} degrees", "rotate {d} {a} degrees",
                 "take a {a} degree turn to the {d}", "make a {a} degree {d} turn",
                 "spin {d} {a} degrees", "turn to the {d} by {a} degrees",
                 "turn {a} degrees {d}"],
    "turn_no": ["turn {d}", "turn to the {d}", "make a {d} turn", "rotate {d}",
                "take a {d} turn"],
    "turn_nodir": ["turn {a} degrees", "rotate {a} degrees", "take a {a} degree turn",
                   "make a {a} degree turn", "spin {a} degrees", "turn",
                   "make a turn", "rotate"],
    "chat": ["what is the weather", "hello", "tell me a joke", "what time is it",
             "how are you", "play some music", "turn on the light", "who are you",
             "open the door", "what is two plus two", "good morning", "thank you",
             "set an alarm for seven", "call my friend"],
    "connect": [" then ", ", then ", " and then ", ", ", " and ", ". Then "],
    "prefix": ["", "", "", "please ", "can you ", "robot "],
    "suffix": ["", "", "", "", " please", " now"],
}

TEST = {
    "fwd_amt": ["advance for {t}", "proceed forward {t}", "head forward for {t}",
                "roll forward for {t}", "keep going forward {t}",
                "drive forward for {t}"],
    "fwd_no": ["advance", "proceed forward", "head forward", "roll forward"],
    "bwd_amt": ["retreat for {t}", "go in reverse for {t}", "roll backward {t}",
                "move in reverse for {t}", "head backward for {t}"],
    "bwd_no": ["retreat", "go in reverse", "roll backward", "head backward"],
    "turn_amt": ["pivot {d} {a} degrees", "swing {d} {a} degrees",
                 "veer {d} by {a} degrees", "do a {a} degree turn {d}",
                 "rotate {a} degrees to the {d}", "turn {d} through {a} degrees"],
    "turn_no": ["pivot {d}", "swing {d}", "veer {d}", "do a {d} turn"],
    "turn_nodir": ["pivot {a} degrees", "swing {a} degrees", "do a {a} degree turn",
                   "pivot", "veer"],
    "chat": ["what is the capital of france", "hi there", "sing a song",
             "how is the weather today", "switch on the lamp", "what is your name",
             "i am hungry", "can you hear me", "good night"],
    "connect": [" and after that ", " followed by ", "; then ", ", afterwards ",
                " next "],
    "prefix": ["", "", "robot, please ", "i need you to ", "could you ",
               "hey robot ", "now "],
    "suffix": ["", "", " right away", " thanks"],
}


def g(x):
    return "%g" % x


def jnum(x):
    return int(x) if float(x).is_integer() else float(x)


def time_text(rnd, s):
    n = g(s)
    if float(s).is_integer() and int(s) in SEC_WORDS and rnd.random() < 0.25:
        n = SEC_WORDS[int(s)]
    elif rnd.random() < 0.1:
        return n + "s"
    if s == 1:
        return n + " " + rnd.choice(["second", "sec"])
    return n + " " + rnd.choice(["seconds", "seconds", "secs", "sec"])


def deg_text(rnd, d):
    if d in DEG_WORDS and rnd.random() < 0.25:
        return DEG_WORDS[d]
    return str(d)


def make_step(rnd, T, kind):
    if kind == "move":
        fwd = rnd.random() < 0.65
        direction = "forward" if fwd else "backward"
        if rnd.random() < 0.25:
            amt = None
            phrase = rnd.choice(T["fwd_no" if fwd else "bwd_no"])
        else:
            amt = rnd.choice(SEC_VALUES)
            tpl = rnd.choice(T["fwd_amt" if fwd else "bwd_amt"])
            phrase = tpl.format(t=time_text(rnd, amt))
        return {"kind": "move", "dir": direction, "amt": amt, "phrase": phrase}
    if kind == "turn":
        direction = rnd.choice(["left", "right"])
        if rnd.random() < 0.25:
            amt = None
            phrase = rnd.choice(T["turn_no"]).format(d=direction)
        else:
            amt = rnd.choice(DEG_VALUES)
            phrase = rnd.choice(T["turn_amt"]).format(d=direction, a=deg_text(rnd, amt))
        return {"kind": "turn", "dir": direction, "amt": amt, "phrase": phrase}
    amt = rnd.choice(DEG_VALUES)
    phrase = rnd.choice(T["turn_nodir"]).format(a=deg_text(rnd, amt))
    return {"kind": "turn_nodir", "dir": None, "amt": None, "phrase": phrase}


def random_step(rnd, T):
    return make_step(rnd, T, "move" if rnd.random() < 0.55 else "turn")


def make_valid_seq(rnd, T, max_steps):
    n = rnd.choices(range(1, max_steps + 1), weights=[30, 40, 30, 10][:max_steps])[0]
    steps = []
    while len(steps) < n:
        s = random_step(rnd, T)
        if steps and s["phrase"] == steps[-1]["phrase"]:
            continue
        steps.append(s)
    return steps


def casing(rnd, text):
    r = rnd.random()
    if r < 0.25:
        return text[0].upper() + text[1:]
    if r < 0.40:
        return text[0].upper() + text[1:] + "."
    return text


def compose(rnd, T, phrases):
    text = phrases[0]
    for p in phrases[1:]:
        text += rnd.choice(T["connect"]) + p
    text = rnd.choice(T["prefix"]) + text + rnd.choice(T["suffix"])
    return casing(rnd, text.strip())


def answers_for(steps):
    out = []
    for s in steps:
        if s["kind"] == "move":
            a = {"direction": s["dir"]}
            if s["amt"] is not None:
                a["seconds"] = jnum(s["amt"])
            out.append({"name": "move", "arguments": a})
        else:
            a = {"direction": s["dir"]}
            if s["amt"] is not None:
                a["degrees"] = int(s["amt"])
            out.append({"name": "turn", "arguments": a})
    return out


def reasoning_for(steps):
    parts = []
    for s in steps:
        r = "'%s' -> direction '%s'" % (s["phrase"], s["dir"])
        if s["amt"] is not None:
            r += ", %s %s" % ("seconds" if s["kind"] == "move" else "degrees", g(s["amt"]))
        parts.append(r)
    return "; ".join(parts)


def expected_for(steps):
    parts = []
    for s in steps:
        if s["kind"] == "move":
            parts.append("move %s %s" % (s["dir"], g(s["amt"] if s["amt"] is not None else DEF_SECS)))
        else:
            parts.append("turn %s %d" % (s["dir"], s["amt"] if s["amt"] is not None else DEF_DEG))
    return " | ".join(parts)


def make_example(rnd, T, max_steps):
    r = rnd.random()
    if r < 0.78:
        steps = make_valid_seq(rnd, T, max_steps)
        return {"kind": "valid", "query": compose(rnd, T, [s["phrase"] for s in steps]),
                "answers": answers_for(steps), "reasoning": reasoning_for(steps),
                "expected": expected_for(steps)}
    if r < 0.89:
        k = rnd.choices([0, 1, 2], weights=[50, 30, 20])[0]
        k = min(k, max_steps - 1)
        steps = [random_step(rnd, T) for _ in range(k)]
        bad = make_step(rnd, T, "turn_nodir")
        steps.insert(rnd.randint(0, len(steps)), bad)
        return {"kind": "refuse_turn", "query": compose(rnd, T, [s["phrase"] for s in steps]),
                "answers": [],
                "reasoning": "'%s' -> the turn has no direction, so no call" % bad["phrase"],
                "expected": "none"}
    text = rnd.choice(T["chat"])
    return {"kind": "chat", "query": casing(rnd, text), "answers": [],
            "reasoning": "no tool matches '%s'" % text, "expected": "none"}


def build(rnd, T, n, max_steps, banned=()):
    seen, out, tries = set(), [], 0
    while len(out) < n and tries < n * 60:
        tries += 1
        ex = make_example(rnd, T, max_steps)
        q = ex["query"].lower()
        if q in seen or q in banned:
            continue
        seen.add(q)
        out.append(ex)
    return out


def to_line(ex, tools, stringify):
    row = {"query": ex["query"],
           "tools": json.dumps(tools, separators=(",", ":")) if stringify else tools,
           "answers": json.dumps(ex["answers"], separators=(",", ":")) if stringify else ex["answers"],
           "reasoning": ex["reasoning"]}
    return json.dumps(row)


def summary(name, rows):
    c = Counter(r["kind"] for r in rows)
    print("%s: %d examples (%s)" % (name, len(rows),
          ", ".join("%s=%d" % kv for kv in sorted(c.items()))))


def main():
    ap = argparse.ArgumentParser()
    _here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--tools", default=os.path.join(_here, "led.json"))
    ap.add_argument("--outdir", default=os.path.join(_here, "..", "data"))
    ap.add_argument("--train", type=int, default=1200)
    ap.add_argument("--test", type=int, default=80)
    ap.add_argument("--max-steps", type=int, default=3)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--stringify", action="store_true",
                    help="write tools/answers as JSON strings (older format)")
    a = ap.parse_args()

    with open(a.tools, encoding="utf-8-sig") as f:
        tools = json.load(f)
    names = sorted(t["name"] for t in tools)
    if names != ["move", "turn"]:
        raise SystemExit("tools file must define exactly move and turn, found %s" % names)

    os.makedirs(a.outdir, exist_ok=True)
    train = build(random.Random(a.seed), TRAIN, a.train, a.max_steps)
    banned = {e["query"].lower() for e in train}
    test = build(random.Random(a.seed + 1), TEST, a.test, a.max_steps, banned)

    with open(os.path.join(a.outdir, "train.jsonl"), "w", encoding="utf-8") as f:
        for ex in train:
            f.write(to_line(ex, tools, a.stringify) + "\n")
    with open(os.path.join(a.outdir, "test_set.txt"), "w", encoding="utf-8") as f:
        f.write("# command ==> expected (held-out phrasing)\n")
        for ex in test:
            f.write("%s ==> %s\n" % (ex["query"], ex["expected"]))

    summary("train", train)
    summary("test ", test)
    print("longest train query: %d characters" % max(len(e["query"]) for e in train))
    print("\nSample training examples:")
    for ex in train[:8]:
        print("  %s\n      -> %s" % (ex["query"], ex["expected"]))
    print("\nWrote %s" % a.outdir)


if __name__ == "__main__":
    main()
