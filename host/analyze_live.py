#!/usr/bin/env python3
"""Verify a --live render: the 8-note C-major scale must appear at the scheduled
times with the correct pitches. Proves live MIDI bytes reached the SWP30 through
the H8 SCI. Exits non-zero on failure. Stdlib only (wave/struct/math)."""
import sys, wave, struct, math

NOTES   = [60, 62, 64, 65, 67, 69, 71, 72]            # C major scale
ONSETS  = [5.0, 5.5, 6.0, 6.5, 7.0, 7.5, 8.0, 8.5]    # scheduled (s)
EXPECT  = [440.0 * 2 ** ((m - 69) / 12) for m in NOTES]

def main(path):
    w = wave.open(path, "rb")
    ch, fr, n = w.getnchannels(), w.getframerate(), w.getnframes()
    samp = struct.unpack("<%dh" % (n * ch), w.readframes(n))
    mono = lambda i: (samp[i * ch] + samp[i * ch + 1]) * 0.5

    # --- energy envelope + attack detection (20 ms windows) ---
    win = int(fr * 0.02)
    env = []
    for i in range(0, n - win, win):
        s = sum(mono(i + k) ** 2 for k in range(win))
        env.append((i / fr, math.sqrt(s / win)))
    peak = max(e for _, e in env)
    floor = 0.04 * peak
    attacks, last = [], -9
    for j in range(3, len(env)):
        t, e = env[j]; eprev = env[j - 3][1]
        if e > floor and e > 2.2 * max(eprev, floor * 0.5) and (t - last) > 0.2:
            attacks.append(round(t, 2)); last = t

    # --- autocorrelation pitch around each scheduled onset ---
    def pitch(t0):
        a, L = int((t0 + 0.04) * fr), int(0.06 * fr)
        x = [mono(a + k) for k in range(L)]
        m = sum(x) / L; x = [v - m for v in x]
        best = (0, 0)
        for lag in range(int(fr / 700), int(fr / 200)):
            s = sum(x[k] * x[k + lag] for k in range(L - lag))
            if s > best[0]: best = (s, lag)
        return fr / best[1] if best[1] else 0

    ok = 0
    print("note  expected(Hz)  measured(Hz)")
    for m, e, t in zip(NOTES, EXPECT, ONSETS):
        f = pitch(t); good = e and abs(f - e) / e < 0.03; ok += good
        print(f"  {m}    {e:7.1f}       {f:7.1f}   {'OK' if good else 'FAIL'}")

    # each scheduled onset must have a detected attack within 60 ms
    matched = sum(any(abs(a - t) < 0.06 for a in attacks) for t in ONSETS)
    print(f"attacks detected: {attacks}")
    print(f"onsets matched: {matched}/8   pitches matched: {ok}/8   peak RMS: {peak:.0f}")

    fail = peak < 50 or matched < 8 or ok < 8
    print("RESULT:", "FAIL" if fail else "PASS")
    return 1 if fail else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/live1.wav"))
