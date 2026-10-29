#!/usr/bin/env python3
"""Break down an eval_serial.py CSV: where did the model go wrong?"""
import csv
import sys
from collections import Counter


def calls(s):
    s = (s or "").strip()
    return [] if s in ("", "none") else [c.strip() for c in s.split("|")]


def category(r):
    exp, got = calls(r["expected"]), calls(r["got"])
    if r["result"] == "PASS":
        return "correct"
    if not exp:
        return "should refuse but acted"
    if not got:
        return "valid command refused"
    if len(got) < len(exp):
        return "dropped a call"
    if len(got) > len(exp):
        return "extra call"
    for e, g in zip(exp, got):
        if e.split()[:2] != g.split()[:2]:
            return "wrong action or direction"
    return "wrong amount"


def conf(r):
    try:
        return float(r["confidence"])
    except (TypeError, ValueError):
        return None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "before_tuning_testset.csv"
    rows = [r for r in csv.DictReader(open(path, encoding="utf-8")) if r["expected"] != ""]
    n = len(rows)
    ok = sum(r["result"] == "PASS" for r in rows)
    print("%s: %d/%d correct (%.0f%%)\n" % (path, ok, n, 100.0 * ok / n))
    print("By outcome:")
    for k, v in Counter(category(r) for r in rows).most_common():
        print("  %-28s %3d" % (k, v))
    print("\nBy expected number of calls:")
    by = {}
    for r in rows:
        k = len(calls(r["expected"]))
        t = by.setdefault(k, [0, 0])
        t[1] += 1
        t[0] += r["result"] == "PASS"
    for k in sorted(by):
        label = "refuse (0 calls)" if k == 0 else "%d call(s)" % k
        print("  %-18s %2d/%2d correct" % (label, by[k][0], by[k][1]))
    print("\nConfidence (cutoff 0.5):")
    for name, keep in (("correct", lambda r: r["result"] == "PASS"),
                       ("wrong", lambda r: r["result"] != "PASS")):
        cs = [conf(r) for r in rows if keep(r)]
        cs = [c for c in cs if c is not None]
        hi = sum(c >= 0.5 for c in cs)
        print("  %-8s %2d at or above 0.5, %2d below" % (name, hi, len(cs) - hi))


main()
