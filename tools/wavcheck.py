#!/usr/bin/env python3
"""Check a WAV file recorded from NovaOS (QEMU -audiodev wav,...): list the notes in it.

    tools/wavcheck.py out.wav                       # format, level, notes (start, length, pitch)
    tools/wavcheck.py out.wav --expect 440          # exit 1 unless a ~440 Hz note is heard
    tools/wavcheck.py out.wav --expect 523,659,784 --tolerance 2
    tools/wavcheck.py out.wav --peaks 0.5 1.5       # strongest frequencies in 0.5..1.5 s (chords)

Notes are found with a sliding window (default 20 ms, half overlap): the FFT peak of each
window (parabolic interpolation), windows with the same pitch within 1.5 % merged into one
note. The window mean is removed first, so the constant level QEMU's PC speaker idles at
counts as silence. Needs numpy.
"""
import argparse
import sys
import wave

import numpy as np


def load(path):
    w = wave.open(path, "rb")
    ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    if sw == 2:
        a = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    elif sw == 1:
        a = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128) * 256
    else:
        sys.exit("unsupported sample width %d" % sw)
    return a.reshape(-1, ch), rate, ch, sw


def peak_freq(b, rate, nfft, fmin=40.0):
    sp = np.abs(np.fft.rfft(b * np.hanning(len(b)), nfft))
    lo = int(fmin * nfft / rate)
    k = lo + int(np.argmax(sp[lo:]))
    if 0 < k < len(sp) - 1:
        a_, b_, c_ = np.log(sp[k - 1] + 1e-9), np.log(sp[k] + 1e-9), np.log(sp[k + 1] + 1e-9)
        d = a_ - 2 * b_ + c_
        p = 0.5 * (a_ - c_) / d if d else 0.0
    else:
        p = 0.0
    return (k + p) * rate / nfft


def notes(x, rate, win_ms, min_level):
    win = max(64, int(rate * win_ms / 1000))
    hop = win // 2
    nfft = 1 << 15
    res = []
    for s in range(0, len(x) - win, hop):
        b = x[s:s + win]
        b = b - b.mean()
        if np.sqrt((b ** 2).mean()) < min_level:
            res.append((s, 0.0))
        else:
            res.append((s, peak_freq(b, rate, nfft)))
    runs = []
    for s, f in res:
        if runs and ((f == 0 and runs[-1][2] == 0) or (f and runs[-1][2] and abs(f / runs[-1][2] - 1) < 0.015)):
            runs[-1][1] = s + hop
            runs[-1][3].append(f)
        else:
            runs.append([s, s + hop, f, [f]])
    out = []
    for s0, s1, f, fs in runs:
        if (s1 - s0) * 1000 / rate < win_ms:
            continue
        out.append((s0 / rate, (s1 - s0) / rate, float(np.median(fs)) if f else 0.0))
    return out


def peaks(x, rate, t0, t1, count):
    seg = x[int(t0 * rate):int(t1 * rate)]
    seg = seg - seg.mean()
    nfft = 1 << 18
    sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), nfft))
    f = np.arange(len(sp)) * rate / nfft
    idx = [k for k in range(2, len(sp) - 2) if sp[k] > sp[k - 1] and sp[k] >= sp[k + 1] and f[k] > 40]
    idx.sort(key=lambda k: -sp[k])
    top = sp[idx[0]] if idx else 1.0
    return [(f[k], 20 * np.log10(sp[k] / top)) for k in sorted(idx[:count], key=lambda k: f[k])]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--window", type=float, default=20, help="analysis window in ms (default 20)")
    ap.add_argument("--min-level", type=float, default=200, help="RMS below this is silence (default 200)")
    ap.add_argument("--expect", help="comma-separated frequencies (Hz) that must be heard as notes")
    ap.add_argument("--tolerance", type=float, default=3.0, help="--expect tolerance in percent (default 3)")
    ap.add_argument("--peaks", nargs=2, type=float, metavar=("START", "END"),
                    help="list the strongest frequencies between START and END seconds")
    ap.add_argument("--count", type=int, default=8, help="number of --peaks (default 8)")
    a = ap.parse_args()

    data, rate, ch, sw = load(a.wav)
    x = data[:, 0]
    print("%s: %d Hz, %d channel%s, %d-bit, %.2f s" % (a.wav, rate, ch, "" if ch == 1 else "s", sw * 8, len(x) / rate))
    if not len(x):
        print("empty recording (the guest never started the audio stream)")
        return 1
    xc = x - x.mean()
    print("peak %d, rms %.0f" % (np.abs(x).max(), np.sqrt((xc ** 2).mean())))
    found = notes(x, rate, a.window, a.min_level)
    print("notes:")
    for t, d, f in found:
        print("  %8.3f s  %7.1f ms  %s" % (t, d * 1000, ("%8.2f Hz" % f) if f else "   silence"))
    if a.peaks:
        print("strongest frequencies %.3f..%.3f s:" % tuple(a.peaks))
        for f, db in peaks(x, rate, a.peaks[0], a.peaks[1], a.count):
            print("  %8.2f Hz  %6.1f dB" % (f, db))
    if a.expect:
        ok = True
        for want in [float(v) for v in a.expect.split(",") if v]:
            hits = [f for _, _, f in found if f and abs(f / want - 1) * 100 <= a.tolerance]
            print("expect %.2f Hz: %s" % (want, ("heard %.2f Hz" % hits[0]) if hits else "NOT HEARD"))
            ok = ok and bool(hits)
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
