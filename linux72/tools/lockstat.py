import re, sys
n = 0; L = R = 0; Lf = Rf = 0
for ln in open(sys.argv[1]):
    m = re.search(r"LEFT tracked (\d+) found (\d+).*RIGHT tracked (\d+) found (\d+)", ln)
    if not m: continue
    n += 1; a, b, c, d = map(int, m.groups())
    L += a > 0; R += c > 0; Lf += b; Rf += d
print("seconds", n, "| LEFT tracked in %d s (%.0f%%), found %d | RIGHT tracked in %d s (%.0f%%), found %d" % (L, 100.0 * L / max(1, n), Lf, R, 100.0 * R / max(1, n), Rf))
