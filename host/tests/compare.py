#!/usr/bin/env python3
"""Compare engine answers with the original DOS program (greedy decoding).

usage: compare.py questions.txt ref.txt float.txt precise.txt
                  [--min-precise 0.95] [--fast fast.txt --min-fast 0.88]
                  [--same-as-precise file.txt ...]

The float kernel must match the original exactly. The precise kernel (the
watch default) and the fast kernel must agree on at least the given share of
the questions; differences are listed for inspection. Files passed with
--same-as-precise (e.g. the in-place weight run) must equal precise.txt
line for line.
"""
import sys

args = sys.argv[1:]


def take(flag, default=None, many=False):
    vals = []
    while flag in args:
        i = args.index(flag)
        vals.append(args[i + 1])
        del args[i:i + 2]
    if many:
        return vals
    return vals[-1] if vals else default


min_precise = float(take("--min-precise", take("--min-int8", "0.95")))
fast_file = take("--fast")
min_fast = float(take("--min-fast", "0.88"))
same = take("--same-as-precise", many=True)
qf, rf, ff, pf = args


def lines(path):
    with open(path) as f:
        return [l.rstrip("\n") for l in f]


with open(qf) as f:
    q = [l.rstrip("\n") for l in f if l.strip()]
ref, flt, pre = lines(rf), lines(ff), lines(pf)
assert len(q) == len(ref) == len(flt) == len(pre), (len(q), len(ref), len(flt), len(pre))
ok = True


def report(name, got, minimum):
    global ok
    n = sum(a == b for a, b in zip(ref, got))
    print(f"{name:8} kernel : {n}/{len(q)} identical to the DOS program ({100.0 * n / len(q):.1f}%)")
    for qq, a, b in zip(q, ref, got):
        if a != b:
            print(f"    Q: {qq}\n       dos : {a}\n       {name:5}: {b}")
    if n < minimum * len(q):
        print(f"  -> below the required {100 * minimum:.0f}%")
        ok = False


fl_ok = sum(a == b for a, b in zip(ref, flt))
print(f"float    kernel : {fl_ok}/{len(q)} identical to the DOS program")
if fl_ok != len(q):
    ok = False
report("precise", pre, min_precise)
if fast_file:
    report("fast", lines(fast_file), min_fast)
for s in same:
    if lines(s) != pre:
        print(f"{s}: differs from the precise run")
        ok = False
    else:
        print(f"{s}: identical to the precise run")
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
