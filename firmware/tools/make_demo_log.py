#!/usr/bin/env python3
"""
Synthesise a RAW ICM20948 log for a small rocket flight.

This exists so the before/after figures can be produced
TODAY, before the first real flight. It writes only raw
sensor channels - exactly what the ICM20948 would hand
over - plus the true attitude, which a real flight can
never give you. Everything filtered is then produced by
./replay, which links the actual flight firmware.

Frame: +Z out the nose (see Filters.h, MOUNTING).
Output: T,AX,AY,AZ,GX,GY,GZ,MX,MY,MZ,TRUE_ROLL,TRUE_PITCH,PHASE
"""

import argparse
import numpy as np

G = 9.80665

# Flight timeline, seconds
T_PAD_END   = 10.0
T_BURNOUT   = 11.7
T_APOGEE    = 19.0
T_END       = 45.0

# What the sensor gets wrong even when nothing is moving
GYRO_BIAS   = np.array([0.85, -1.30, 0.42])    # deg/s
ACC_NOISE   = 0.16      # m/s2 rms, broadband
GYRO_NOISE  = 0.55      # deg/s rms
MAG_NOISE   = 0.45      # uT rms

# Airframe vibration under thrust
VIB_TONE_HZ = 32.0
VIB_ACC     = 14.0      # m/s2 peak at the tone
VIB_GYR     = 26.0      # deg/s peak

# Earth field, near-equatorial, accelerometer frame at rest
MAG_NORTH   = 40.0      # uT
MAG_DOWN    =  5.0      # uT

# Bearing of the nose, degrees. Deliberately NOT 0: a rocket
# pointing at magnetic north puts the heading exactly on the
# 0/360 wrap, where it flips between 359.9 and 0.1 every
# sample and the plot becomes a solid block of ink. Real
# flights hit this too, which is why the plotter breaks the
# line at wraps as well.
MAG_BEARING = 40.0


def attitude(t):
    """True roll and pitch in degrees, and the axial spin rate."""
    roll  = np.zeros_like(t)
    pitch = np.zeros_like(t)
    spin  = np.zeros_like(t)

    # ---- on the rail: 3 deg off vertical, dead still ----
    pad = t < T_PAD_END
    pitch[pad] = 3.0
    roll[pad]  = 1.0

    # ---- boost: holds the rail angle, spins up on the fins ----
    boost = (t >= T_PAD_END) & (t < T_BURNOUT)
    tb = t[boost] - T_PAD_END
    pitch[boost] = 3.0 + 1.5 * tb
    roll[boost]  = 1.0 + 0.8 * np.sin(2 * np.pi * 1.1 * tb)
    spin[boost]  = 260.0 * (tb / (T_BURNOUT - T_PAD_END))

    # ---- coast: the gravity turn, and this is where the
    #      accelerometer stops being able to see down ----
    coast = (t >= T_BURNOUT) & (t < T_APOGEE)
    tc = (t[coast] - T_BURNOUT) / (T_APOGEE - T_BURNOUT)
    pitch[coast] = 5.6 + 19.0 * tc**1.6
    roll[coast]  = 1.0 + 2.0 * np.sin(2 * np.pi * 0.7 * (t[coast] - T_BURNOUT))
    spin[coast]  = 260.0 * np.exp(-1.1 * (t[coast] - T_BURNOUT))

    # ---- under the parachute: swinging on the shroud lines ----
    desc = t >= T_APOGEE
    td = t[desc] - T_APOGEE
    settle = np.exp(-0.25 * td)
    pitch[desc] = 24.6 * settle * np.cos(2 * np.pi * 0.45 * td) + 4.0
    roll[desc]  = 22.0 * settle * np.sin(2 * np.pi * 0.38 * td)
    spin[desc]  = 30.0 * np.exp(-0.4 * td)

    return roll, pitch, spin


def axial_specific_force(t):
    """Thrust and drag along the body Z axis, m/s2."""
    f = np.zeros_like(t)

    boost = (t >= T_PAD_END) & (t < T_BURNOUT)
    tb = (t[boost] - T_PAD_END) / (T_BURNOUT - T_PAD_END)
    # Fast ramp, long tail - a normal single-grain motor
    f[boost] = 78.0 * np.minimum(1.0, tb / 0.12) * (1.0 - 0.35 * tb)

    coast = (t >= T_BURNOUT) & (t < T_APOGEE)
    v = np.maximum(0.0, 1.0 - (t[coast] - T_BURNOUT) / (T_APOGEE - T_BURNOUT))
    f[coast] = -11.0 * v**2          # drag, decays as it slows

    desc = t >= T_APOGEE
    f[desc] = -1.5 * np.exp(-0.8 * (t[desc] - T_APOGEE))

    return f


def phase_of(t):
    p = np.full(t.shape, 0)
    p[(t >= T_PAD_END) & (t < T_BURNOUT)] = 2
    p[(t >= T_BURNOUT) & (t < T_APOGEE)]  = 3
    p[t >= T_APOGEE]                      = 5
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="demo_raw.csv")
    ap.add_argument("--hz", type=float, default=100.0,
                    help="IMU sample rate (initIMU sets ~100)")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)

    n = int(T_END * args.hz)
    # Real loops jitter. The low pass derives alpha from the
    # measured dt, so the log must jitter too or that is
    # never exercised.
    t = np.arange(n) / args.hz + rng.normal(0, 0.0008, n)
    t = np.maximum.accumulate(t)
    t -= t[0]

    roll, pitch, spin = attitude(t)
    fz_axial = axial_specific_force(t)

    r = np.radians(roll)
    p = np.radians(pitch)

    # Gravity resolved into the body frame, written so that
    # atan2 recovers roll and pitch exactly - the same pair
    # of formulas Filters.cpp uses.
    ax = -np.sin(p) * G
    ay =  np.sin(r) * np.cos(p) * G
    az =  np.cos(r) * np.cos(p) * G

    az = az + fz_axial

    # Body rates: the derivative of the true attitude
    dt = np.gradient(t)
    gx = np.gradient(roll)  / dt
    gy = np.gradient(pitch) / dt
    gz = spin.copy()

    # ---- vibration, only while the motor is burning ----
    burn = ((t >= T_PAD_END) & (t < T_BURNOUT)).astype(float)
    tail = np.exp(-2.5 * np.maximum(0.0, t - T_BURNOUT)) * (t >= T_BURNOUT)
    env  = burn + 0.45 * tail

    tone = np.sin(2 * np.pi * VIB_TONE_HZ * t)
    tone2 = np.sin(2 * np.pi * (VIB_TONE_HZ * 1.7) * t + 0.9)

    ax += env * VIB_ACC * (0.55 * tone + 0.30 * tone2 + 0.5 * rng.normal(0, 1, n))
    ay += env * VIB_ACC * (0.50 * tone2 + 0.30 * tone + 0.5 * rng.normal(0, 1, n))
    az += env * VIB_ACC * (1.00 * tone + 0.45 * tone2 + 0.6 * rng.normal(0, 1, n))

    gx += env * VIB_GYR * (0.6 * tone2 + 0.5 * rng.normal(0, 1, n))
    gy += env * VIB_GYR * (0.6 * tone  + 0.5 * rng.normal(0, 1, n))
    gz += env * VIB_GYR * (0.4 * tone  + 0.5 * rng.normal(0, 1, n))

    # ---- ejection charge: one hard bang at apogee ----
    kick = np.argmax(t >= T_APOGEE)
    for k in range(kick, min(kick + 6, n)):
        w = np.exp(-(k - kick) / 2.0)
        ax[k] += 55 * w * rng.normal(); ay[k] += 55 * w * rng.normal()
        az[k] += 70 * w * rng.normal()
        gx[k] += 340 * w * rng.normal(); gy[k] += 340 * w * rng.normal()
        gz[k] += 300 * w * rng.normal()

    # ---- broadband sensor noise and the gyro zero offset ----
    ax += rng.normal(0, ACC_NOISE, n)
    ay += rng.normal(0, ACC_NOISE, n)
    az += rng.normal(0, ACC_NOISE, n)

    gx += GYRO_BIAS[0] + rng.normal(0, GYRO_NOISE, n)
    gy += GYRO_BIAS[1] + rng.normal(0, GYRO_NOISE, n)
    gz += GYRO_BIAS[2] + rng.normal(0, GYRO_NOISE, n)

    # ---- single-sample spikes: I2C glitches and EMI.
    # These are what the median filter is there for.
    n_spike = int(T_END / 1.4)
    for idx in rng.choice(n, n_spike, replace=False):
        ch = rng.integers(0, 6)
        amp = rng.choice([-1.0, 1.0]) * rng.uniform(25, 60)
        [ax, ay, az][ch % 3][idx] += amp if ch < 3 else 0.0
        if ch >= 3:
            [gx, gy, gz][ch - 3][idx] += amp * 6.0

    # ---- magnetometer ----
    # Field in the accelerometer frame, then swapped into
    # the AK09916's own axes, because that is what the
    # firmware reads and has to undo.
    hb = np.radians(MAG_BEARING)
    bx_w, by_w, bz_w = (MAG_NORTH * np.cos(hb),
                        MAG_NORTH * np.sin(hb),
                        MAG_DOWN)

    amx = bx_w * np.cos(p) + bz_w * np.sin(p)
    amy = (bx_w * np.sin(r) * np.sin(p) + by_w * np.cos(r)
           - bz_w * np.sin(r) * np.cos(p))
    amz = (-bx_w * np.cos(r) * np.sin(p) + by_w * np.sin(r)
           + bz_w * np.cos(r) * np.cos(p))

    mx =  amy + rng.normal(0, MAG_NOISE, n)
    my =  amx + rng.normal(0, MAG_NOISE, n)
    mz = -amz + rng.normal(0, MAG_NOISE, n)

    phase = phase_of(t)

    with open(args.out, "w") as f:
        f.write("T,AX,AY,AZ,GX,GY,GZ,MX,MY,MZ,TRUE_ROLL,TRUE_PITCH,PHASE\n")
        for i in range(n):
            f.write(
                f"{t[i]:.4f},{ax[i]:.4f},{ay[i]:.4f},{az[i]:.4f},"
                f"{gx[i]:.3f},{gy[i]:.3f},{gz[i]:.3f},"
                f"{mx[i]:.3f},{my[i]:.3f},{mz[i]:.3f},"
                f"{roll[i]:.4f},{pitch[i]:.4f},{phase[i]}\n"
            )

    print(f"{args.out}: {n} samples, {t[-1]:.1f} s at ~{args.hz:.0f} Hz")
    print(f"  pad 0-{T_PAD_END:.0f}s  boost {T_PAD_END:.0f}-{T_BURNOUT:.1f}s  "
          f"coast -{T_APOGEE:.0f}s  descent -{T_END:.0f}s")


if __name__ == "__main__":
    main()
