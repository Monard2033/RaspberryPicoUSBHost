"""Offline model of the DrainEstimator implemented in src/main.cpp.

Replicates the C++ DrainEstimator logic statement-for-statement and validates
both the arithmetic and edge cases (including sleep gaps, voltage relaxation,
and extended ETA formatting up to 90 days).
"""

CAPACITY = 1024
SMOOTH_LEN = 9
SAMPLE_GAP_RESET_MS = 6 * 60 * 60 * 1000  # 6 hours
RATE_WINDOW_MS = 60 * 60 * 1000           # 60 min look-back
MIN_RATE_ELAPSED_MS = 5 * 60 * 1000       # 5 min before rate reported
ESTIMATOR_MIN_DROP_MV = 5.0               # ~0.4 % of 3050..4190 mV
CHARGE_RISE_RESET_MV = 75.0               # Rise this large means charging missed
BATT_MIN_MV = 3050                        # Firmware reference 0 %
BATT_MAX_MV = 4190                        # Firmware reference 100 %
MV_PER_PERCENT = (BATT_MAX_MV - BATT_MIN_MV) / 100.0  # 11.4 mV per 1 %
MEDIAN_WINDOW_LEN = 5                     # 25 s median filter on raw mV
MEDIAN_HALF_BLOCK = 4                     # 20 s median per window endpoint
POLL_MS = 5000


class Estimator:
    def __init__(self):
        self.history = []
        self.rates = []
        self.med_window = []
        self.last_sample_ms = 0
        self.has_last_sample = False
        self.last_elapsed_ms = 0
        self.estimate = None

    def clear(self):
        self.history = []
        self.rates = []
        self.med_window = []
        self.last_sample_ms = 0
        self.has_last_sample = False
        self.last_elapsed_ms = 0

    def append_median(self, mv):
        self.med_window.append(mv)
        if len(self.med_window) > MEDIAN_WINDOW_LEN:
            self.med_window.pop(0)

    def median_millivolts(self):
        if not self.med_window:
            return BATT_MIN_MV
        s = sorted(self.med_window)
        return s[len(s) // 2]

    def median_of_samples(self, from_idx, to_exclusive):
        span = to_exclusive - from_idx
        if span == 0:
            return 0.0
        n = min(span, MEDIAN_HALF_BLOCK)
        block = [self.history[from_idx + i][2] for i in range(n)]
        block.sort()
        return block[n // 2]

    def compute_window_rate(self):
        count = len(self.history)
        if count < MEDIAN_HALF_BLOCK * 2:
            return None, None

        newest = self.history[-1]
        idx = count - 1
        while idx > 0 and (newest[0] - self.history[idx - 1][0]) <= RATE_WINDOW_MS:
            idx -= 1

        span = count - idx
        block_len = min(span // 2, MEDIAN_HALF_BLOCK)
        if block_len == 0:
            return None, None

        oldest_block_begin = idx
        oldest_block_end = idx + block_len
        newest_block_begin = count - block_len
        newest_block_end = count

        oldest_mv = self.median_of_samples(oldest_block_begin, oldest_block_end)
        newest_mv = self.median_of_samples(newest_block_begin, newest_block_end)

        oldest_centre = self.history[oldest_block_begin + block_len // 2][0]
        newest_centre = self.history[newest_block_begin + block_len // 2][0]
        if newest_centre <= oldest_centre:
            return None, None

        elapsed_ms = newest_centre - oldest_centre
        self.last_elapsed_ms = elapsed_ms
        if elapsed_ms < MIN_RATE_ELAPSED_MS:
            return None, None

        drop_mv = oldest_mv - newest_mv
        if drop_mv < ESTIMATOR_MIN_DROP_MV:
            return None, None

        elapsed_minutes = elapsed_ms / 60000.0
        mv_per_minute = drop_mv / elapsed_minutes
        rate_per_minute = mv_per_minute / MV_PER_PERCENT
        if rate_per_minute <= 0.0:
            return None, None
        return rate_per_minute, mv_per_minute

    # mirrors DrainEstimator::Update
    def update(self, now_ms, pct, mv, charging=False, live=True, battery_state=None):
        if battery_state is not None:
            charging = live and (battery_state in (1, 3))
            discharging = live and (battery_state in (0, 2))
        else:
            discharging = live and not charging

        if charging:
            self.clear()
            self.estimate = None
            return self.estimate

        if not live:
            return self.estimate

        if not discharging:
            return self.estimate

        if self.has_last_sample and (now_ms - self.last_sample_ms) > SAMPLE_GAP_RESET_MS:
            self.clear()

        # Charge rise check: only clear if batteryState is charging or rise exceeds 75 mV
        if len(self.history) > 0 and (
            (battery_state in (1, 3)) if battery_state is not None else False
            or (mv - self.median_millivolts() > CHARGE_RISE_RESET_MV)
        ):
            self.clear()

        self.history.append((now_ms, pct, mv))
        if len(self.history) > CAPACITY:
            self.history.pop(0)
        self.last_sample_ms = now_ms
        self.has_last_sample = True
        self.append_median(mv)

        rate_per_min, mv_per_min = self.compute_window_rate()
        if rate_per_min is not None:
            self.rates.append(rate_per_min)
            if len(self.rates) > SMOOTH_LEN:
                self.rates.pop(0)
            sorted_rates = sorted(self.rates)
            smoothed = sorted_rates[len(sorted_rates) // 2]
            from_mv = self.median_millivolts() - BATT_MIN_MV
            pct_equiv = (from_mv / MV_PER_PERCENT) if from_mv > 0.0 else float(pct)
            minutes_left = (pct_equiv / smoothed) if smoothed > 0.0 else 0.0
            self.estimate = {
                "valid": smoothed > 0.0,
                "rate_per_min": smoothed,
                "minutes_left": minutes_left,
                "percent_per_minute": smoothed,
                "millivolts_per_minute": smoothed * MV_PER_PERCENT if smoothed > 0 else 0.0,
                "minutes_remaining": minutes_left,
            }
        elif self.estimate is not None and self.estimate.get("valid", False) and discharging:
            # Retain previous estimate so sleep wake does not reset calculation to zero
            pass
        else:
            self.estimate = None

        return self.estimate


# Mirror FormatEtaMinutes from src/main.cpp
def format_eta_minutes(minutes):
    if not (minutes > 0.0) or minutes > 60.0 * 24.0 * 90.0:
        return None
    if minutes < 10.0:
        return "< 10 min"
    total_minutes = int(minutes + 0.5)
    days = total_minutes // (24 * 60)
    remaining_minutes = total_minutes % (24 * 60)
    hours = remaining_minutes // 60
    mins = remaining_minutes % 60
    if days > 0:
        if hours == 0:
            return f"{days} d"
        else:
            return f"{days} d {hours} h"
    elif hours == 0:
        return f"{mins} min"
    elif mins == 0:
        return f"{hours} h"
    else:
        return f"{hours} h {mins} min"


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
print("=== Edge case B: sleep gap preserves window, pathological gap clears ===")
est = Estimator()
t = 0
for i in range(240):  # 20 min discharge
    pct = int(100 - (100 / 360.0) * (t / 60000.0))
    est.update(t, pct, mv_for_pct(pct))
    t += POLL_MS
before = len(est.history)
# 65 min sleep gap (less than 6 h pathological threshold)
t += 65 * 60 * 1000
pct = int(100 - (100 / 360.0) * (t / 60000.0))
est.update(t, pct, mv_for_pct(pct))
print(f"samples before 65m sleep={before}, after first wake sample={len(est.history)} (expected: {before + 1})")
assert len(est.history) == before + 1, "Sleep gap should not clear history!"

# Pathological gap (> 6 h)
t += 7 * 60 * 60 * 1000
pct = int(100 - (100 / 360.0) * (t / 60000.0))
est.update(t, pct, mv_for_pct(pct))
print(f"samples after 7h gap={len(est.history)} (expected: 1)")
assert len(est.history) == 1, "7h gap must clear history!"

print()
print("=== Edge case C: no estimate before any real drop ===")
est = Estimator()
t = 0
for i in range(24):  # 2 min of perfectly flat voltage
    est.update(t, 100, mv_for_pct(100))
    t += POLL_MS
print(f"flat 2 min -> estimate={'yes' if est.estimate else 'no'} (expected: no)")
assert est.estimate is None, "Should have no estimate with 0 drop in 2 min"

print()
print("=== Verification: 4400 minutes at 88% formatting ===")
fmt_4400 = format_eta_minutes(4400.0)
print(f"4400 min format -> '{fmt_4400}' (expected: '3 d 1 h')")
assert fmt_4400 == "3 d 1 h", f"Format mismatch: expected '3 d 1 h', got '{fmt_4400}'"
# Check other formatting boundaries
assert format_eta_minutes(1440.0) == "1 d", f"1440 min failed: {format_eta_minutes(1440.0)}"
assert format_eta_minutes(1500.0) == "1 d 1 h", f"1500 min failed: {format_eta_minutes(1500.0)}"
assert format_eta_minutes(120.0) == "2 h", f"120 min failed: {format_eta_minutes(120.0)}"
assert format_eta_minutes(125.0) == "2 h 5 min", f"125 min failed: {format_eta_minutes(125.0)}"
assert format_eta_minutes(45.0) == "45 min", f"45 min failed: {format_eta_minutes(45.0)}"
assert format_eta_minutes(5.0) == "< 10 min", f"5 min failed: {format_eta_minutes(5.0)}"
assert format_eta_minutes(0.0) is None, f"0 min should return None"
assert format_eta_minutes(60.0 * 24.0 * 90.0 + 1.0) is None, f"> 90 days should return None"
print("Formatting test PASSED.")

print()
print("=== Verification: Sleep gap & relaxation do not destroy estimate_ ===")
est = Estimator()
t = 0
rate_per_min = 1.2 / 60.0  # 1.2 %/h
for i in range(152 * 12):  # 152 minutes of operation
    minutes = t / 60000.0
    pct = 91.0 - rate_per_min * minutes
    mv = int(BATT_MIN_MV + pct * MV_PER_PERCENT)
    est.update(t, int(pct), mv)
    t += POLL_MS

assert est.estimate is not None and est.estimate["valid"], "Failed to generate estimate after 152 min"
est_before_sleep = est.estimate["minutes_left"]
print(f"Before sleep (t=152 min): estimate valid=True, minutes_left={est_before_sleep:.1f} ({format_eta_minutes(est_before_sleep)})")

# Keyboard enters sleep for 65 minutes.
# Open-circuit relaxation causes a 25 mV voltage rise on wake.
t += 65 * 60 * 1000
pct_wake = 88
mv_wake = int(BATT_MIN_MV + pct_wake * MV_PER_PERCENT) + 25  # +25 mV relaxation
e_wake = est.update(t, pct_wake, mv_wake)

print(f"Immediately post-wake (after 65 min sleep + 25 mV relaxation):")
print(f"  estimate valid={e_wake is not None and e_wake['valid']}")
print(f"  minutes_left={e_wake['minutes_left']:.1f} ({format_eta_minutes(e_wake['minutes_left'])})")
assert e_wake is not None and e_wake["valid"], "ERROR: Estimate was destroyed immediately upon waking from sleep!"
assert e_wake["minutes_left"] == est_before_sleep, "ERROR: Estimate was not retained from previous discharge history!"

# Subsequent samples during the 5-minute warm-up window before new window rate can be computed
for step in range(60):  # 5 minutes at 5s polls
    t += POLL_MS
    e_step = est.update(t, pct_wake, mv_wake)
    assert e_step is not None and e_step["valid"], f"ERROR: Estimate destroyed during post-wake warm-up at step {step}"

print(f"Throughout 5 min post-wake warm-up: estimate consistently retained.")
print("Sleep gap resilience test PASSED.")
