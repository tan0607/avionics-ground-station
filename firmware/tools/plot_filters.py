#!/usr/bin/env python3
"""
Before / after figures for the ICM20948 filter chain.

Works on either input, because both carry the same
column names:

  * a real card log   FLIGHT001.CSV
  * ./replay output   demo_filtered.csv   (+ --truth demo_raw.csv)

  python3 plot_filters.py --input FLIGHT001.CSV --out figures
"""

import argparse
import os
import sys

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ---- palette: categorical slots in fixed order ----
INK       = "#0b0b0b"
INK_2     = "#52514e"
INK_MUTED = "#8a8983"
SURFACE   = "#fcfcfb"
GRID      = "#e4e3de"

S1 = "#2a78d6"   # slot 1  blue
S2 = "#eb6834"   # slot 2  orange
S3 = "#1baf7a"   # slot 3  aqua
S4 = "#eda100"   # slot 4  yellow
GATE      = "#f0efec"

GYRO_CAL_SAMPLES = 300   # overridden from the CLI

RAW_KW  = dict(color=S1, lw=0.7, alpha=0.85, zorder=2)
FILT_KW = dict(color=S2, lw=1.8, zorder=4, solid_capstyle="round")
TRUE_KW = dict(color=INK_MUTED, lw=1.4, ls=(0, (5, 3)), zorder=3)


def style(ax, title=None, sub=None, xlabel=None, ylabel=None):
    ax.set_facecolor(SURFACE)
    ax.grid(True, color=GRID, lw=0.7, zorder=0)
    ax.set_axisbelow(True)

    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)

    ax.tick_params(colors=INK_2, labelsize=9, length=0)

    if title:
        ax.set_title(title, loc="left", color=INK, fontsize=12,
                     fontweight="bold", pad=22 if sub else 8)
    if sub:
        ax.text(0, 1.012, sub, transform=ax.transAxes, color=INK_2,
                fontsize=9.5, va="bottom")
    if xlabel:
        ax.set_xlabel(xlabel, color=INK_2, fontsize=9.5)
    if ylabel:
        ax.set_ylabel(ylabel, color=INK_2, fontsize=9.5)


def legend(ax, **kw):
    lg = ax.legend(frameon=False, fontsize=9.5, labelcolor=INK_2, **kw)
    return lg


def shade_gate(ax, d):
    """Grey the stretches where the accelerometer was gated off."""
    if "ATR" not in d:
        return

    gated = d["ATR"].to_numpy() == 0
    if not gated.any():
        return

    t = d["T"].to_numpy()
    edges = np.diff(gated.astype(int))
    starts = list(np.where(edges == 1)[0] + 1)
    stops = list(np.where(edges == -1)[0] + 1)

    if gated[0]:
        starts.insert(0, 0)
    if gated[-1]:
        stops.append(len(gated) - 1)

    for a, b in zip(starts, stops):
        ax.axvspan(t[a], t[b], color=GATE, lw=0, zorder=1)


def headroom(ax, frac=0.20):
    """Open space at the top so the legend never sits on the data."""
    lo, hi = ax.get_ylim()
    ax.set_ylim(lo, hi + (hi - lo) * frac)


def robust_ylim(ax, series, frac=0.20, include_zero=False):
    """Scale to the signal, not to the spikes.

    A couple of 300 deg/s glitches would otherwise stretch the
    axis until the thing the panel exists to show - a 1.3 deg/s
    bias offset - is a flat line on zero.
    """
    vals = np.concatenate([np.asarray(x, dtype=float) for x in series])
    vals = vals[np.isfinite(vals)]
    if vals.size == 0:
        return

    lo, hi = np.percentile(vals, [0.5, 99.5])
    if include_zero:
        lo, hi = min(lo, 0.0), max(hi, 0.0)

    span = max(hi - lo, 1e-6)
    ax.set_ylim(lo - 0.15 * span, hi + (0.15 + frac) * span)


def break_wraps(t, deg, limit=180.0):
    """Lift the pen where an angle wraps 360 -> 0.

    Without this the line is drawn straight down the page on
    every wrap and the plot fills with vertical bars.
    """
    y = np.asarray(deg, dtype=float).copy()
    jump = np.abs(np.diff(y)) > limit
    y[1:][jump] = np.nan
    return t, y


def newfig(nrows=1, h=6.0):
    fig, axes = plt.subplots(nrows, 1, figsize=(11, h))
    fig.patch.set_facecolor(SURFACE)
    return fig, (axes if nrows > 1 else [axes])


def save(fig, out, name):
    p = os.path.join(out, name)
    fig.tight_layout()
    fig.savefig(p, dpi=200, facecolor=SURFACE)
    plt.close(fig)
    print("  wrote", p)


# =====================================================
# FIGURES
# =====================================================

def fig_accel(d, out, boost):
    fig, ax = newfig(2, 7.4)

    ax[0].plot(d["T"], d["AZ"], label="AZ raw", **RAW_KW)
    ax[0].plot(d["T"], d["FAZ"], label="AZ filtered", **FILT_KW)
    style(ax[0],
          "Axial acceleration, before and after filtering",
          "3-sample median, then a 1st-order low pass at the Config.h cutoff",
          ylabel="m/s$^2$")
    headroom(ax[0])
    legend(ax[0], loc="upper right", ncol=2)

    lo, hi = boost
    m = (d["T"] >= lo) & (d["T"] <= hi)
    ax[1].plot(d["T"][m], d["AZ"][m], label="AZ raw", **RAW_KW)
    ax[1].plot(d["T"][m], d["FAZ"][m], label="AZ filtered", **FILT_KW)
    style(ax[1], None,
          f"Motor burn, {lo:.1f}-{hi:.1f} s - where the vibration lives",
          xlabel="time (s)", ylabel="m/s$^2$")
    headroom(ax[1])
    legend(ax[1], loc="upper right", ncol=2)

    save(fig, out, "01_axial_accel.png")


def fig_gyro(d, out, quiet):
    fig, ax = newfig(2, 7.4)

    ax[0].plot(d["T"], d["GY"], label="GY raw", **RAW_KW)
    ax[0].plot(d["T"], d["FGY"], label="GY filtered + de-biased", **FILT_KW)
    style(ax[0],
          "Pitch rate, before and after filtering",
          "This channel drives the Kalman prediction step. Spikes and the "
          "ejection transient run off the top of the scale.",
          ylabel="deg/s")
    robust_ylim(ax[0], [d["GY"], d["FGY"]])
    legend(ax[0], loc="upper right", ncol=2)

    lo, hi = quiet
    m = (d["T"] >= lo) & (d["T"] <= hi)
    ax[1].axhline(0, color=INK_MUTED, lw=1.0, ls=(0, (4, 3)), zorder=3)
    ax[1].plot(d["T"][m], d["GY"][m], label="GY raw", **RAW_KW)
    ax[1].plot(d["T"][m], d["FGY"][m], label="GY filtered + de-biased", **FILT_KW)
    style(ax[1], None,
          "On the pad, still. The raw trace sits BELOW zero - that offset "
          "is the bias. The step onto zero is the calibration finishing.",
          xlabel="time (s)", ylabel="deg/s")
    # Scaled to the bias, not to the glitches - otherwise the
    # offset this panel exists to show is a flat line on zero.
    robust_ylim(ax[1], [d["GY"][m], d["FGY"][m]], include_zero=True)
    legend(ax[1], loc="upper right", ncol=2)

    save(fig, out, "02_gyro.png")


def fig_attitude(d, out, truth):
    fig, ax = newfig(2, 7.6)

    for i, (axis, raw, kal, tcol, name) in enumerate([
        (ax[0], "PA", "PK", "TRUE_PITCH", "Pitch"),
        (ax[1], "RA", "RK", "TRUE_ROLL",  "Roll"),
    ]):
        shade_gate(axis, d)
        axis.plot(d["T"], d[raw], label=f"{name}, accelerometer only (raw)", **RAW_KW)

        if truth is not None and tcol in truth:
            axis.plot(d["T"], truth[tcol], label="true attitude", **TRUE_KW)

        axis.plot(d["T"], d[kal], label=f"{name}, Kalman", **FILT_KW)

        style(axis,
              f"{name}: before and after sensor fusion",
              ("Shaded = the accelerometer was gated off and the Kalman was "
               "running on the gyro alone")
              if i == 0 else
              "Spikes off the top of the scale are single bad samples - "
              "the median filter is what removes them",
              xlabel="time (s)" if i == 1 else None,
              ylabel="deg")
        headroom(axis, 0.16)
        legend(axis, loc="upper left", ncol=3)

    save(fig, out, "03_attitude.png")


def fig_stages(d, out, truth):
    """All four stages on one axis - the cumulative story."""
    fig, ax = newfig(1, 5.4)
    a = ax[0]

    shade_gate(a, d)

    a.plot(d["T"], d["PA"], label="1. accelerometer only", color=S1,
           lw=0.7, alpha=0.75, zorder=2)
    a.plot(d["T"], d["PL"], label="2. + low pass", color=S3, lw=1.3, zorder=3)
    a.plot(d["T"], d["PC"], label="3. complementary", color=S4, lw=1.5, zorder=4)
    a.plot(d["T"], d["PK"], label="4. Kalman", color=S2, lw=2.0, zorder=6)

    if truth is not None and "TRUE_PITCH" in truth:
        a.plot(d["T"], truth["TRUE_PITCH"], label="true", **TRUE_KW)

    style(a, "Pitch, one stage at a time",
          "Each stage is the previous one plus one idea. Stages 1 and 2 "
          "cannot survive the shaded region; 3 and 4 can.",
          xlabel="time (s)", ylabel="deg")
    headroom(a, 0.16)
    legend(a, loc="upper left", ncol=5)

    save(fig, out, "04_stages.png")


def fig_error_bars(d, out, truth):
    if truth is None:
        return None

    methods = [("accelerometer only", "PA", "RA", S1),
               ("+ low pass",         "PL", "RL", S3),
               ("complementary",      "PC", "RC", S4),
               ("Kalman",             "PK", "RK", S2)]

    def rms(a, b):
        return float(np.sqrt(np.mean((np.asarray(a) - np.asarray(b)) ** 2)))

    rows = [(name, rms(d[p], truth["TRUE_PITCH"]), rms(d[r], truth["TRUE_ROLL"]), c)
            for name, p, r, c in methods]

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.4))
    fig.patch.set_facecolor(SURFACE)

    for ax, idx, label in ((axes[0], 1, "Pitch"), (axes[1], 2, "Roll")):
        names = [r[0] for r in rows]
        vals = [r[idx] for r in rows]
        cols = [r[3] for r in rows]
        y = np.arange(len(rows))[::-1]

        ax.barh(y, vals, height=0.62, color=cols, zorder=3)

        for yy, v in zip(y, vals):
            ax.text(v + max(vals) * 0.02, yy, f"{v:.2f}", va="center",
                    color=INK, fontsize=10, fontweight="bold", zorder=4)

        ax.set_yticks(y)
        ax.set_yticklabels(names)
        ax.set_xlim(0, max(vals) * 1.22)
        style(ax, f"{label} RMS error", None, xlabel="degrees")
        ax.grid(axis="y", visible=False)

    fig.suptitle("Attitude error against truth, whole flight",
                 x=0.008, ha="left", color=INK, fontsize=12.5,
                 fontweight="bold", y=0.995)

    save(fig, out, "05_error.png")
    return rows


def fig_spectrum(d, out, boost, fc):
    m = (d["T"] >= boost[0]) & (d["T"] <= boost[1])
    if m.sum() < 64:
        m = slice(None)

    t = d["T"].to_numpy()
    fs = 1.0 / np.median(np.diff(t))

    fig, ax = newfig(1, 5.2)
    a = ax[0]

    for col, lab, kw in (("AZ", "raw", dict(color=S1, lw=1.2, alpha=0.9)),
                         ("FAZ", "filtered", dict(color=S2, lw=2.0))):
        x = d[col].to_numpy()[m]
        x = x - x.mean()
        w = np.hanning(len(x))
        sp = np.abs(np.fft.rfft(x * w)) / len(x)
        fr = np.fft.rfftfreq(len(x), 1.0 / fs)
        a.semilogy(fr[1:], sp[1:], label=lab, **kw)

    a.axvline(fc, color=INK_MUTED, lw=1.2, ls=(0, (4, 3)), zorder=5)
    a.annotate(f" cutoff {fc:.0f} Hz", xy=(fc, 0.88), xycoords=("data", "axes fraction"),
               color=INK_2, fontsize=9.5, va="center")

    style(a, "Where the noise went",
          f"Spectrum of axial acceleration during the burn, "
          f"sampled at {fs:.0f} Hz",
          xlabel="frequency (Hz)", ylabel="amplitude (m/s$^2$)")
    legend(a, loc="upper right", ncol=2)

    save(fig, out, "06_spectrum.png")


def fig_heading(d, out):
    if "FHDG" not in d:
        return

    fig, ax = newfig(1, 4.8)
    a = ax[0]

    t = d["T"].to_numpy()
    a.plot(*break_wraps(t, d["HDG"]), label="heading, atan2(my, mx) raw", **RAW_KW)
    a.plot(*break_wraps(t, d["FHDG"]), label="heading, tilt compensated", **FILT_KW)

    style(a, "Heading, before and after tilt compensation",
          "The raw form ignores the magnetometer's own axis order and "
          "assumes the board is level. Both are wrong in flight.",
          xlabel="time (s)", ylabel="deg")
    # If the flight actually crosses the 0/360 wrap, the full
    # compass has to be on screen. If it does not, zooming in
    # is the only way the difference between the two traces is
    # visible at all.
    wraps = (np.abs(np.diff(d["HDG"].to_numpy())) > 180).any() or \
            (np.abs(np.diff(d["FHDG"].to_numpy())) > 180).any()

    if wraps:
        a.set_ylim(0, 360)
        a.set_yticks([0, 90, 180, 270, 360])
        headroom(a, 0.16)
    else:
        robust_ylim(a, [d["HDG"], d["FHDG"]], frac=0.35)
    legend(a, loc="upper right", ncol=2)

    save(fig, out, "07_heading.png")


def robust_sigma(x):
    """MAD-based sigma.

    Plain std() on this data measures whether the channel
    happened to catch a spike, not how well the low pass
    works. The median absolute deviation ignores the
    outliers, which is the point - de-spiking is the median
    filter's job and it is reported separately below.
    """
    v = np.asarray(x, dtype=float)
    v = v[np.isfinite(v)]
    if v.size == 0:
        return float("nan")
    return 1.4826 * float(np.median(np.abs(v - np.median(v))))


def draw_table(ax, data, cols, widths, title):
    ax.axis("off")
    tbl = ax.table(cellText=data, colLabels=cols, cellLoc="left",
                   colLoc="left", loc="upper left", colWidths=widths)
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(10)
    tbl.scale(1, 1.45)

    for (r, c), cell in tbl.get_celld().items():
        cell.set_edgecolor(GRID)
        cell.set_linewidth(0.8)
        cell.get_text().set_color(INK if r else INK_2)
        cell.set_facecolor(SURFACE)
        if r == 0:
            cell.get_text().set_fontweight("bold")

    ax.set_title(title, loc="left", color=INK, fontsize=11.5,
                 fontweight="bold", pad=14)


def fig_table(d, out, quiet, rows):
    """The numbers. Deliberately a table and not a chart."""
    m = (d["T"] >= quiet[0]) & (d["T"] <= quiet[1])
    q = d[m]

    chans = [("AX", "FAX", "accel X", "m/s²"), ("AY", "FAY", "accel Y", "m/s²"),
             ("AZ", "FAZ", "accel Z", "m/s²"), ("GX", "FGX", "gyro X", "deg/s"),
             ("GY", "FGY", "gyro Y", "deg/s"), ("GZ", "FGZ", "gyro Z", "deg/s")]

    data = []
    spikes = 0
    for raw, filt, name, unit in chans:
        a = robust_sigma(q[raw])
        b = robust_sigma(q[filt])
        sig = robust_sigma(q[raw])
        spikes += int((np.abs(q[raw] - np.median(q[raw])) > 8 * sig).sum())
        data.append([name, unit, f"{a:.3f}", f"{b:.3f}",
                     f"{100 * (1 - b / a):.0f}%" if a > 0 else "-"])

    n_noise = len(data) + 1
    n_att = (len(rows) + 1) if rows else 0

    if rows:
        fig, axes = plt.subplots(
            2, 1, figsize=(11, 1.1 + 0.40 * (n_noise + n_att)),
            gridspec_kw=dict(height_ratios=[n_noise + 0.5, n_att]))
    else:
        fig, ax0 = plt.subplots(figsize=(11, 1.2 + 0.42 * n_noise))
        axes = [ax0]

    fig.patch.set_facecolor(SURFACE)

    draw_table(
        axes[0], data,
        ["channel", "unit", "σ raw", "σ filtered", "reduction"],
        [0.20, 0.12, 0.16, 0.18, 0.16],
        f"Broadband noise on the pad, {quiet[0]:.0f}-{quiet[1]:.0f} s "
        "(still, so every wiggle is noise)")

    axes[0].text(0, -0.06,
                 "σ is a robust (MAD) estimate, so one glitch cannot dominate "
                 f"it. Separately, {spikes} single-sample spikes were present "
                 "in this window\nand removed by the median stage — that is a "
                 "different filter doing a different job.",
                 transform=axes[0].transAxes, color=INK_2, fontsize=9,
                 va="top")

    if rows:
        att = [[name, f"{p:.2f}", f"{r:.2f}"] for name, p, r, _ in rows]
        draw_table(axes[1], att,
                   ["method", "pitch RMS error (deg)", "roll RMS error (deg)"],
                   [0.26, 0.26, 0.26],
                   "Attitude error against truth, whole flight")

    save(fig, out, "08_results_table.png")


# =====================================================

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True,
                    help="replay output, or a FLIGHTnnn.CSV off the card")
    ap.add_argument("--truth", default=None,
                    help="demo_raw.csv, for the TRUE_* columns")
    ap.add_argument("--out", default="figures")
    ap.add_argument("--fc", type=float, default=12.0,
                    help="accel low pass cutoff, to draw on the spectrum")
    ap.add_argument("--cal-samples", type=int, default=300,
                    help="GYRO_CAL_SAMPLES, so the quiet window can start "
                         "after the calibration finishes")
    args = ap.parse_args()

    global GYRO_CAL_SAMPLES
    GYRO_CAL_SAMPLES = args.cal_samples

    d = pd.read_csv(args.input, comment="#")
    d.columns = [c.strip() for c in d.columns]

    need = ["T", "AZ", "FAZ", "PA", "PK", "RA", "RK"]
    missing = [c for c in need if c not in d.columns]
    if missing:
        sys.exit(f"input is missing {missing} - is this a filtered log?")

    truth = None
    if args.truth:
        truth = pd.read_csv(args.truth, comment="#")
        if len(truth) != len(d):
            n = min(len(truth), len(d))
            truth, d = truth.iloc[:n].reset_index(drop=True), d.iloc[:n].reset_index(drop=True)

    os.makedirs(args.out, exist_ok=True)

    # Pick the windows from the data itself so this works on
    # a real flight, where nothing is labelled.
    t = d["T"].to_numpy()
    if "ANRM" in d:
        # A rolling mean, not the single largest sample. The
        # biggest number in the file is the ejection charge,
        # which lasts 50 ms; the burn is what we want and it
        # is the only thing that stays high for a second.
        hz = 1.0 / max(1e-6, float(np.median(np.diff(t))))
        w = max(3, int(0.6 * hz))
        smooth = pd.Series(d["ANRM"]).rolling(w, center=True, min_periods=1).mean()
        i_boost = int(np.argmax(smooth.to_numpy()))
    else:
        i_boost = len(d) // 2

    boost = (max(t[0], t[i_boost] - 1.5), min(t[-1], t[i_boost] + 2.5))

    # The quiet window must start AFTER the gyro calibration has
    # finished. Straddling it makes the filtered trace bimodal -
    # one level before the bias is applied, another after - which
    # inflates its spread and can read as the filter making the
    # channel noisier. It is not; the window was just wrong.
    hz_col = float(np.median(d["IHZ"])) if "IHZ" in d else 0.0
    hz_est = hz_col if hz_col > 1 else (1.0 / max(1e-6, float(np.median(np.diff(t)))))
    cal_done = t[0] + GYRO_CAL_SAMPLES / hz_est + 1.0

    q_start = min(cal_done, t[i_boost] - 2.0)
    q_end = max(q_start + 1.0, min(t[i_boost] - 1.0, q_start + 10.0))
    quiet = (q_start, q_end)

    print(f"reading {args.input}  ({len(d)} rows, {t[-1] - t[0]:.1f} s)")
    print(f"  quiet window {quiet[0]:.1f}-{quiet[1]:.1f} s   "
          f"boost window {boost[0]:.1f}-{boost[1]:.1f} s")

    fig_accel(d, args.out, boost)
    fig_gyro(d, args.out, quiet)
    fig_attitude(d, args.out, truth)
    fig_stages(d, args.out, truth)
    rows = fig_error_bars(d, args.out, truth)
    fig_spectrum(d, args.out, boost, args.fc)
    fig_heading(d, args.out)
    fig_table(d, args.out, quiet, rows)

    # ---- the same numbers as text, for pasting into a report ----
    q = d[(d["T"] >= quiet[0]) & (d["T"] <= quiet[1])]
    print(f"\nbroadband noise on the pad, {quiet[0]:.1f}-{quiet[1]:.1f} s")
    print("(robust MAD sigma, so a stray spike cannot dominate it)")
    for raw, filt in [("AX", "FAX"), ("AY", "FAY"), ("AZ", "FAZ"),
                      ("GX", "FGX"), ("GY", "FGY"), ("GZ", "FGZ")]:
        a, b = robust_sigma(q[raw]), robust_sigma(q[filt])
        print(f"  {raw:>3} {a:8.3f} -> {b:7.3f}   {100 * (1 - b / a):5.1f}% lower")

    if rows:
        print("\nattitude RMS error vs truth (deg):")
        print(f"  {'method':<22}{'pitch':>8}{'roll':>8}")
        for name, p, r, _ in rows:
            print(f"  {name:<22}{p:8.2f}{r:8.2f}")

    if "ATR" in d:
        print(f"\naccelerometer gated off for "
              f"{100 * (1 - float(d['ATR'].mean())):.1f}% of the flight")


if __name__ == "__main__":
    main()
