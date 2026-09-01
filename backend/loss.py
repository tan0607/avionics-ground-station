"""Packet-loss tracking from the 16-bit sequence counter.

Loss % = how many sequence numbers went missing between the frames we actually
decoded. This used to be the ONLY link-quality signal available, because the E32
had no RSSI output; the SX1278 that replaced it reports RSSI and SNR per packet
(see mrcc.LinkQuality), so seq gaps are now one signal of two rather than all
there is.

`seq` is a uint16 (0..65535) that increments once per transmitted packet and wraps.
So the forward distance between two received seqs is `(seq - last) & 0xFFFF`.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass
class LossStats:
    received: int          # frames we decoded and counted in the running stream
    lost: int              # seq numbers inferred missing (from gaps)
    duplicates: int        # repeated seq (delta == 0)
    resets: int            # stream re-baselines (restart / huge jump)
    expected: int          # received + lost
    fraction: float        # lost / expected, 0.0 when nothing expected yet


class LossTracker:
    """Feed decoded seq numbers in arrival order; read cumulative loss stats.

    `RESET_GAP` is the policy knob: a forward jump larger than this is treated as
    a stream restart (flight computer/bridge reboot -> seq counter jumps or resets)
    rather than a genuine burst loss, so we re-baseline instead of inventing tens
    of thousands of "lost" packets. Tune it to your longest plausible real
    dropout: at the link's measured 2 Hz (session.PACKET_DESC), 1000 packets is
    ~8 minutes of blackout.

    `duplicates` is not a rounding detail on this link -- the transmitter sends
    every packet TWICE, so roughly half of all decoded frames land here with
    delta == 0. They are counted and then excluded from `expected`, which is why
    a healthy link reads 0.0% loss rather than 50%.
    """

    RESET_GAP = 1000

    def __init__(self) -> None:
        self.received = 0
        self.lost = 0
        self.duplicates = 0
        self.resets = 0
        self.last_seq: int | None = None

    def observe(self, seq: int) -> int:
        """Record one decoded frame's seq. Returns how many packets this gap
        implies were lost (0 in the normal +1 case).

        Policy, wrap-safe via `(seq - last) & 0xFFFF`:
          - first frame          -> baseline, no loss
          - delta == 1           -> in order, no loss
          - delta == 0           -> duplicate seq (count, don't expect it)
          - delta  > RESET_GAP   -> restart/rebaseline, don't invent loss
          - 1 < delta <= RESET_GAP -> (delta - 1) packets lost
        """
        if self.last_seq is None:
            self.last_seq = seq
            self.received += 1
            return 0

        delta = (seq - self.last_seq) & 0xFFFF
        self.last_seq = seq

        if delta == 0:
            self.duplicates += 1
            return 0

        if delta > self.RESET_GAP:
            self.resets += 1
            self.received += 1
            return 0

        missing = delta - 1
        self.lost += missing
        self.received += 1
        return missing

    @property
    def expected(self) -> int:
        return self.received + self.lost

    @property
    def fraction(self) -> float:
        exp = self.expected
        return self.lost / exp if exp else 0.0

    def stats(self) -> LossStats:
        return LossStats(
            received=self.received,
            lost=self.lost,
            duplicates=self.duplicates,
            resets=self.resets,
            expected=self.expected,
            fraction=self.fraction,
        )
