import json
import os
import random
import sys

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)
import make_dataset as m

seen = set()
with open(os.path.join(_here, "..", "data", "train.jsonl"), encoding="utf-8") as f:
    for line in f:
        seen.add(json.loads(line)["query"].lower())

rows = m.build(random.Random(99), m.TRAIN, 60, 3, seen)
with open(os.path.join(_here, "..", "data", "test_same_style.txt"), "w", encoding="utf-8") as f:
    f.write("# command ==> expected (training style, unseen sentences)\n")
    for r in rows:
        f.write("%s ==> %s\n" % (r["query"], r["expected"]))
print("wrote %d commands" % len(rows))
