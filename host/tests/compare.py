#!/usr/bin/env python3
"""Compare engine answers with the original DOS program (greedy decoding).

usage: compare.py questions.txt ref.txt float.txt int8.txt [--min-int8 0.90]

The float kernel must match the original exactly. The int8 kernel
(quantised activations, used on the watch) must agree on at least
--min-int8 of the questions; differences are listed for inspection.
"""
import sys

args = sys.argv[1:]
min_int8 = 0.90
if "--min-int8" in args:
    i = args.index("--min-int8")
    min_int8 = float(args[i + 1])
    del args[i:i + 2]
qf, rf, ff, nf = args
q = [l.rstrip("\n") for l in open(qf) if l.strip()]
ref = [l.rstrip("\n") for l in open(rf)]
flt = [l.rstrip("\n") for l in open(ff)]
i8 = [l.rstrip("\n") for l in open(nf)]
assert len(q) == len(ref) == len(flt) == len(i8), (len(q), len(ref), len(flt), len(i8))

fl_ok = sum(a == b for a, b in zip(ref, flt))
i8_ok = sum(a == b for a, b in zip(ref, i8))
print(f"float kernel : {fl_ok}/{len(q)} identical to the DOS program")
print(f"int8  kernel : {i8_ok}/{len(q)} identical ({100.0 * i8_ok / len(q):.1f}%)")
for qq, a, b in zip(q, ref, i8):
    if a != b:
        print(f"  Q: {qq}\n     dos : {a}\n     int8: {b}")
ok = fl_ok == len(q) and i8_ok >= min_int8 * len(q)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
