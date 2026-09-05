# Onboard recorder status on the ground station

## Goal

The flight computer prints its recording state to USB every 5 s (`[SD]` line in
`printStatus`): file, lines written, write rate, error count. On the ground the
operator sees one bit — `SD OK` / `SD LOST` — so "the card is mounted" and "the
flight is actually being recorded" are indistinguishable from the console.

Put the recording state on the ground station: which file, how many lines, at
what rate, and how many write errors.

## Constraint that shapes the design

The downlink is full. `MRCC_FlightComputer/src/Radio.cpp` builds one ASCII
packet at 2 Hz into a 256-byte buffer against LoRa's 255-byte payload limit;
observed lengths across `flights/*/raw.log` run 189–204 with a single 227, and
the file's own arithmetic puts the format-width maximum at ~252. There is no
room for three more fields on every packet, and no air-time for them either —
two copies plus `COPY_GAP` already fill 87% of the 500 ms window.

So the recorder fields ride the **status cadence, not every packet** (every 10th
packet = 5 s, matching `STATUS_INTERVAL`), and are **appended only if they fit**
in what the base packet left. A recorder field must never be able to truncate
the flight fields; dropping the block for one tick is free, truncating the tail
(the health block) is not.

Three numeric fields, chosen for what an operator cannot already see:

- `SDF` — log file index (`/FLIGHT%03d.CSV`), so the file is identifiable in flight.
- `SDL` — `logLineCount`. Ground derives the write rate from ΔSDL/Δt, the same
  way `printStatus` derives `logHz`.
- `SDE` — `sdErrorCount`.

Byte count is deliberately left out: it is ~120×lines, and it is the field
worth least per byte on a link with ~20 bytes to spare.

## Affected files

- `firmware/MRCC_FlightComputer/src/Storage.cpp`, `State.h` — expose the log file index.
- `firmware/MRCC_FlightComputer/src/Config.h` — cadence constant, payload limit.
- `firmware/MRCC_FlightComputer/src/Radio.cpp` — append-if-it-fits recorder block.
- `shared/protocol/mrcc.py` — `AUX_FIELDS` gains SDF/SDL/SDE (appended at the end).
- `dashboard/src/hooks/useRecorderStatus.ts` — latch the last report, derive rate.
- `dashboard/src/components/SubsystemHealth.tsx` — render it on the SD row.
- `dashboard/src/components/AuxReadouts.tsx` — hide the raw keys (rendered as state elsewhere).
- `firmware/tests/test_recorder_downlink.py`, `backend/tests.py` — tests.

## Verification

- `python3 firmware/tests/test_recorder_downlink.py`
- `backend/.venv/bin/python -m backend.tests`
- `cd dashboard && npm run build && npm run lint`
- Replay a synthetic MRCC session carrying the new fields and read the panel in
  the browser.
- Re-run TX_Doctor test 3 on the bench: packet length moved, so the duty-cycle
  figure quoted in `MRCC_GroundStation.ino` moved with it.
