#!/usr/bin/env python3
"""Compare two hr_demo frames.csv runs (e.g. inches Z-up vs metres Y-up).

Both files are in inches Z-up. Prints, per man, the largest gap between the two runs
over the first 2 s and over the whole run, and the final pelvis gap.
"""
import sys, csv, math

NAMES = ["stunned", "gut shot", "chest shot", "knockdown", "headshot", "blast", "leg shot"]
P = 21

def load(path):
    rows = []
    with open(path) as f:
        for r in csv.reader(f):
            rows.append((int(r[0]), [float(x) for x in r[1:]]))
    return rows

a, b = load(sys.argv[1]), load(sys.argv[2])
n = min(len(a), len(b))
men = len(a[0][1]) // (P * 3)
print("largest gap between the two runs (inches)")
print("%-11s %8s %8s %10s" % ("", "to 2 s", "all", "end pelvis"))
worst = 0
for m in range(men):
    early = full = 0.0
    for i in range(n):
        t, ra = a[i]; _, rb = b[i]
        for k in range(P):
            o = (m * P + k) * 3
            d = math.dist(ra[o:o + 3], rb[o:o + 3])
            full = max(full, d)
            if t <= 2000:
                early = max(early, d)
    o = m * P * 3
    end = math.dist(a[n - 1][1][o:o + 3], b[n - 1][1][o:o + 3])
    worst = max(worst, full)
    print("%-11s %8.2f %8.2f %10.2f" % (NAMES[m] if m < len(NAMES) else m, early, full, end))
