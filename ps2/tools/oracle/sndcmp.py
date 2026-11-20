#!/usr/bin/env python3
"""Compare two recordings of the same emulated run, numerically.

    sndcmp.py reference.wav test.wav [--plot out.png]

For checking the fast sound path (SPU2 voices) against the accurate DSP:
the oracle writes both (--wav / --fast-wav). The two cannot match sample
for sample (different interpolation, re-encoded samples, no echo), so
this compares what should survive: loudness over time and the spectrum
over time. Prints

  level      overall RMS of test relative to reference, dB
  envelope   correlation of the 20 ms RMS envelopes
  spectrum   mean correlation, per 46 ms frame, of the log energy in
             third-octave bands (frames where the reference is silent
             are skipped), and the 10th percentile of it
  silent     frames where one side plays and the other is silent

16-bit WAVs of the same rate, covering the same emulated time; the test
is stretched to the reference's length (under 1%) and shifted by up to
100 ms to line up.
"""
import sys
import wave

import numpy as np


def load(path):
    with wave.open(path, "rb") as w:
        rate, ch = w.getframerate(), w.getnchannels()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    return rate, data.reshape(-1, ch).mean(axis=1)


def envelope(x, win):
    n = len(x) // win
    return np.sqrt((x[: n * win].reshape(n, win) ** 2).mean(axis=1))


def bands(x, rate, size=2048, hop=1024):
    n = (len(x) - size) // hop
    idx = np.arange(size)[None, :] + hop * np.arange(n)[:, None]
    spec = np.abs(np.fft.rfft(x[idx] * np.hanning(size), axis=1)) ** 2
    freqs = np.fft.rfftfreq(size, 1.0 / rate)
    edges = 100.0 * 2 ** (np.arange(0, 23) / 3.0)  # 100 Hz .. 16 kHz
    out = np.stack([spec[:, (freqs >= lo) & (freqs < hi)].sum(axis=1) for lo, hi in zip(edges[:-1], edges[1:])], axis=1)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.exit(__doc__)
    rate, ref = load(args[0])
    rate2, test = load(args[1])
    if rate != rate2:
        sys.exit("sample rates differ")
    # Both cover the same emulated time, but the accurate path resamples
    # with a slightly different rate ratio than the SPU2's fixed 3/2:
    # stretch the test onto the reference's time base (under 1%).
    if len(ref) != len(test) and abs(len(test) / len(ref) - 1) < 0.01:
        test = np.interp(np.linspace(0, len(test) - 1, len(ref)), np.arange(len(test)), test)
    n = min(len(ref), len(test))
    ref, test = ref[:n], test[:n]

    # Line up on the 1 ms envelopes.
    w = rate // 1000
    e1, e2 = envelope(ref, w), envelope(test, w)
    best, shift = -2.0, 0
    for s in range(-100, 101):
        a, b = (e1[s:], e2[: len(e2) - s]) if s >= 0 else (e1[:s], e2[-s:])
        if a.std() == 0 or b.std() == 0:
            continue
        c = np.corrcoef(a, b)[0, 1]
        if c > best:
            best, shift = c, s
    if shift > 0:
        ref, test = ref[shift * w :], test[: len(test) - shift * w]
    elif shift < 0:
        ref, test = ref[: len(ref) + shift * w], test[-shift * w :]

    rms_ref, rms_test = np.sqrt((ref**2).mean()), np.sqrt((test**2).mean())
    level = 20 * np.log10(rms_test / rms_ref) if rms_ref and rms_test else float("nan")
    w20 = rate // 50
    env_ref, env_test = envelope(ref, w20), envelope(test, w20)
    env_corr = np.corrcoef(env_ref, env_test)[0, 1] if env_ref.std() and env_test.std() else float("nan")

    b_ref, b_test = bands(ref, rate), bands(test, rate)
    floor = 1e-4 * b_ref.sum(axis=1).max() / b_ref.shape[1]
    loud_ref = b_ref.sum(axis=1) > floor * b_ref.shape[1] * 10
    loud_test = b_test.sum(axis=1) > floor * b_ref.shape[1] * 10
    l_ref, l_test = np.log10(b_ref + floor), np.log10(b_test + floor)
    corr = []
    for i in np.nonzero(loud_ref & loud_test)[0]:
        if l_ref[i].std() and l_test[i].std():
            corr.append(np.corrcoef(l_ref[i], l_test[i])[0, 1])
    corr = np.array(corr) if corr else np.array([float("nan")])

    print(f"shift      {shift} ms (test {'late' if shift < 0 else 'early' if shift > 0 else 'aligned'})")
    print(f"level      {level:+.1f} dB")
    print(f"envelope   {env_corr:.3f}")
    print(f"spectrum   {corr.mean():.3f} mean, {np.percentile(corr, 10):.3f} 10th percentile ({len(corr)} frames)")
    print(f"silent     test only: {int((loud_test & ~loud_ref).sum())}, reference only: {int((loud_ref & ~loud_test).sum())}"
          f" of {len(loud_ref)} frames")

    plot = [a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--plot=")]
    if plot:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        t = np.arange(len(env_ref)) / 50.0
        fig, ax = plt.subplots(3, 1, figsize=(14, 9), sharex=True)
        ax[0].plot(t, env_ref, label="reference", lw=0.8)
        ax[0].plot(t, env_test, label="test", lw=0.8)
        ax[0].legend()
        ax[0].set_ylabel("RMS (20 ms)")
        ext = [0, len(l_ref) * 1024.0 / rate, 0, l_ref.shape[1]]
        ax[1].imshow(l_ref.T, aspect="auto", origin="lower", extent=ext)
        ax[1].set_ylabel("reference bands")
        ax[2].imshow(l_test.T, aspect="auto", origin="lower", extent=ext)
        ax[2].set_ylabel("test bands")
        ax[2].set_xlabel("seconds")
        fig.tight_layout()
        fig.savefig(plot[0], dpi=80)


if __name__ == "__main__":
    main()
