"""Offline model of the DrainEstimator implemented in src/main.cpp.

No C++ compiler is available on this host, so the estimator arithmetic is
replicated here statement-for-statement and driven with synthetic Li-Ion
discharge profiles. This validates the algorithm and its edge cases; it does
NOT validate that main.cpp compiles.
"""

CAPACITY = 1024
SMOOTH_LEN = 9

GAP_RESET_MS = 90_000
RATE_WINDOW_MS = 3_600_000
MIN_RATE_ELAPSED_MS = 900_000
MV_FALLBACK_MIN_ELAPSED_MS = 300_000
MIN_DROP_PCT = 1.0
MV_FALLBACK_MIN_DROP_MV = 8.0

BATT_MIN_MV = 3050
BATT_MAX_MV = 4190

POLL_MS = 5000


class Estimator:
    def __init__(self):
        self.history = []
        self.last_sample_ms = 0
        self.has_last = False
        self.last_elapsed_ms = 0
        self.rates = []
        self.estimate = None

    def clear(self):
        self.history = []
        self.has_last = False
        self.last_elapsed_ms = 0
        self.rates = []

    # mirrors DrainEstimator::Update for the Live+Discharging path only
    def update(self, now_ms, pct, mv, charging=False, live=True):
        if charging:
            self.clear()
            self.estimate = None
            return self.estimate
        if not live:
            return self.estimate

        if self.has_last and (now_ms - self.last_sample_ms) > GAP_RESET_MS:
            self.clear()

        self.history.append((now_ms, pct, mv))
        if len(self.history) > CAPACITY:
            self.history.pop(0)
        self.last_sample_ms = now_ms
        self.has_last = True

        rate = self._window_rate()
        if rate is None:
            self.estimate = None
            return None

        self._push_rate(rate)
        smoothed = self._median_rate()
        self.estimate = {
            "rate_per_min": smoothed,
            "minutes_left": (pct / smoothed) if smoothed > 0 else 0.0,
        }
        return self.estimate

    def _window_rate(self):
        if len(self.history) < 2:
            return None
        newest = self.history[-1]
        idx = len(self.history) - 1
        while idx > 0 and (newest[0] - self.history[idx - 1][0]) <= RATE_WINDOW_MS:
            idx -= 1
        oldest = self.history[idx]

        elapsed_ms = newest[0] - oldest[0]
        self.last_elapsed_ms = elapsed_ms
        if elapsed_ms == 0:
            return None
        elapsed_min = elapsed_ms / 60000.0

        if elapsed_ms >= MIN_RATE_ELAPSED_MS:
            drop = float(oldest[1]) - float(newest[1])
            if drop >= MIN_DROP_PCT:
                return drop / elapsed_min

        if elapsed_ms >= MV_FALLBACK_MIN_ELAPSED_MS:
            drop_mv = float(oldest[2]) - float(newest[2])
            if drop_mv >= MV_FALLBACK_MIN_DROP_MV:
                span = float(BATT_MAX_MV - BATT_MIN_MV)
                return (drop_mv / span) * 100.0 / elapsed_min

        return None

    def _push_rate(self, rate):
        self.rates.append(rate)
        if len(self.rates) > SMOOTH_LEN:
            self.rates.pop(0)

    def _median_rate(self):
        if not self.rates:
            return 0.0
        s = sorted(self.rates)
        return s[len(s) // 2]


def mv_for_pct(pct):
    """Firmware's inverse map: pct = (mv-3050)*100/1140."""
    return BATT_MIN_MV + pct * (BATT_MAX_MV - BATT_MIN_MV) / 100.0


def simulate(total_minutes, run_minutes, noise_mv=0.0, quantize=True):
    est = Estimator()
    true_rate = 100.0 / total_minutes  # %/min
    rows = []
    t = 0
    while t <= run_minutes * 60_000:
        minutes = t / 60_000.0
        pct_exact = 100.0 - true_rate * minutes
        pct_exact = max(0.0, min(100.0, pct_exact))
        # deterministic ripple so the sim is reproducible, not random
        mv = mv_for_pct(pct_exact) + noise_mv * (((t // POLL_MS) % 5) - 2) / 2.0
        pct = int(pct_exact) if quantize else pct_exact
        e = est.update(t, pct, mv)
        if e and int(minutes) % 30 == 0 and (t % 60_000) == 0:
            rows.append((minutes, pct, e["rate_per_min"], e["minutes_left"],
                         total_minutes - minutes))
        t += POLL_MS
    return true_rate, rows


print("=== Scenario 1: flat 6 h discharge (100 % -> 0 %), 1 mV ripple ===")
rate, rows = simulate(360, 300, noise_mv=1.0)
print(f"true rate = {rate:.4f} %/min ({rate*60:.2f} %/h)")
print(f"{'t(min)':>7} {'pct':>4} {'est %/min':>10} {'est left':>9} {'true left':>10} {'err':>8}")
for m, pct, r, left, true_left in rows:
    print(f"{m:7.0f} {pct:4d} {r:10.4f} {left:9.0f} {true_left:10.0f} "
          f"{left - true_left:+8.0f}")

print()
print("=== Scenario 2: early window (first 20 min) ===")
rate, rows = simulate(300, 20, noise_mv=1.0)
for m, pct, r, left, true_left in rows:
    print(f"{m:7.0f} {pct:4d} {r:10.4f} {left:9.0f} {true_left:10.0f} "
          f"{left - true_left:+8.0f}")

print()
print("=== Scenario 3: heavy load 90 min total ===")
rate, rows = simulate(90, 60, noise_mv=1.0)
for m, pct, r, left, true_left in rows:
    print(f"{m:7.0f} {pct:4d} {r:10.4f} {left:9.0f} {true_left:10.0f} "
          f"{left - true_left:+8.0f}")

print()
print("=== Scenario 4: 2 mV quantisation noise resume after 45 min ===")
rate, rows = simulate(300, 45, noise_mv=2.0)
for m, pct, r, left, true_left in rows:
    print(f"{m:7.0f} {pct:4d} {r:10.4f} {left:9.0f} {true_left:10.0f} "
          f"{left - true_left:+8.0f}")

print()
print("=== Edge case A: charging clears the window ===")
est = Estimator()
t = 0
for i in range(240):  # 20 min discharge
    pct = int(100 - (100 / 360.0) * (t / 60000.0))
    est.update(t, pct, mv_for_pct(pct))
    t += POLL_MS
print(f"after 20 min discharge: samples={len(est.history)} "
      f"estimate={'yes' if est.estimate else 'no'}")
est.update(t, 60, mv_for_pct(60), charging=True)
print(f"after charge event:      samples={len(est.history)} "
      f"estimate={'yes' if est.estimate else 'no'}")

print()
print("=== Edge case B: 100 s sampling gap clears the window ===")
est = Estimator()
t = 0
for i in range(240):
    pct = int(100 - (100 / 360.0) * (t / 60000.0))
    est.update(t, pct, mv_for_pct(pct))
    t += POLL_MS
before = len(est.history)
t += 100_000  # PC asleep / keyboard asleep longer than the 90 s guard
pct = int(100 - (100 / 360.0) * (t / 60000.0))
est.update(t, pct, mv_for_pct(pct))
print(f"samples before gap={before}, after first post-gap sample={len(est.history)}")

print()
print("=== Edge case C: no estimate before any real drop ===")
est = Estimator()
t = 0
for i in range(24):  # 2 min of perfectly flat voltage
    est.update(t, 100, mv_for_pct(100))
    t += POLL_MS
print(f"flat 2 min -> estimate={'yes' if est.estimate else 'no'} (expected: no)")