/* Integration harness: compiles the REAL production src/main.cpp (not a copy) and
 * drives the actual DrainEstimator / FormatCountdown / BuildTooltip /
 * ComputeLayout code paths with synthetic snapshots. Verifies the shipped
 * logic and its user-visible strings, not a Python model of it.
 */
#define wWinMain wWinMain_DisabledForTest
#include "src/main.cpp"
#undef wWinMain

#include <cstdio>

namespace {

void Print(const DrainEstimate &e, uint8_t pct, uint16_t mv, const wchar_t *state)
{
    wchar_t eta[64]{};
    bool haveEta = FormatCountdown(eta, std::size(eta));
    unsigned long long winMin = e.windowMs / 60000ull;

    std::printf("  pct=%3u mv=%4u state=%ls | ", pct, mv, state);
    if (e.valid) {
        std::printf("rate=%.4f %%/min (%.2f %%/h) ETA=%.0f min win=%llu min conf=%ls ",
                    e.percentPerMinute, e.percentPerMinute * 60.0,
                    e.minutesRemaining, winMin,
                    DrainConfidenceName(e.confidence, e.voltageBased));
    } else {
        std::printf("no estimate yet (%.0f min of history) ", static_cast<double>(e.windowMs) / 60000.0);
    }
    std::printf("text=\"%ls\"\n", haveEta ? eta : L"Time left: estimating...");
}

uint16_t MvForPct(double pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return static_cast<uint16_t>(3050.0 + pct * (4190.0 - 3050.0) / 100.0);
}

} // namespace

int wmain()
{
    std::printf("=== A. real 6 h discharge, 5 s poll, 1 mV ripple ===\n");
    {
        DrainEstimator est;
        double const rate = 100.0 / 360.0;
        for (unsigned long long t = 0; t <= 300ull * 60000ull; t += 5000ull) {
            double const minutes = static_cast<double>(t) / 60000.0;
            double pctExact = 100.0 - rate * minutes;
            if (pctExact < 0.0) pctExact = 0.0;
            double const ripple = 1.0 * ((static_cast<int>(t / 5000ull) % 5) - 2) / 2.0;
            BatterySnapshot s{};
            s.availability = Availability::Live;
            s.batteryState = 2;
            s.percentage = static_cast<uint8_t>(pctExact);
            s.millivolts = MvForPct(pctExact + ripple / 10.0);
            DrainEstimate e = est.Update(s, t);
            gLastEstimate = e;
            if (t % (30ull * 60000ull) == 0 && t / 60000ull % 30 == 0) {
                double trueLeft = (pctExact > 0.0) ? (360.0 - minutes) : 0.0;
                wchar_t eta[64]{};
                if (FormatCountdown(eta, std::size(eta))) {
                    std::printf("  t=%3llu min pct=%3u true_left=%3.0f min  ->  \"%ls\"\n",
                                t / 60000ull, s.percentage, trueLeft, eta);
                } else {
                    std::printf("  t=%3llu min pct=%3u true_left=%3.0f min  ->  estimating...\n",
                                t / 60000ull, s.percentage, trueLeft);
                }
            }
        }
    }

    std::printf("\n=== B. heavy load, 90 min total ===\n");
    {
        DrainEstimator est;
        double const rate = 100.0 / 90.0;
        for (unsigned long long t = 0; t <= 70ull * 60000ull; t += 5000ull) {
            double const minutes = static_cast<double>(t) / 60000.0;
            double pctExact = 100.0 - rate * minutes;
            if (pctExact < 0.0) pctExact = 0.0;
            BatterySnapshot s{};
            s.availability = Availability::Live;
            s.batteryState = 2;
            s.percentage = static_cast<uint8_t>(pctExact);
            s.millivolts = MvForPct(pctExact);
            DrainEstimate e = est.Update(s, t);
            if (t % (15ull * 60000ull) == 0) {
                Print(e, s.percentage, s.millivolts, L"Discharging");
            }
        }
    }

    std::printf("\n=== C. charging clears history ===\n");
    {
        DrainEstimator est;
        for (unsigned long long t = 0; t <= 20ull * 60000ull; t += 5000ull) {
            BatterySnapshot s{};
            s.availability = Availability::Live;
            s.batteryState = 2;
            s.percentage = static_cast<uint8_t>(100.0 - (100.0 / 360.0) * (t / 60000.0));
            s.millivolts = MvForPct(s.percentage);
            est.Update(s, t);
        }
        BatterySnapshot c{};
        c.availability = Availability::Live;
        c.batteryState = 1;
        c.percentage = 60;
        c.millivolts = MvForPct(60);
        DrainEstimate e = est.Update(c, 21ull * 60000ull);
        Print(e, c.percentage, c.millivolts, L"Charging");
        std::printf("  -> expect: no estimate, history cleared\n");
    }

    std::printf("\n=== D. short gap keeps the window, long gap voids it ===\n");
    {
        DrainEstimator est;
        for (unsigned long long t = 0; t <= 20ull * 60000ull; t += 5000ull) {
            BatterySnapshot s{};
            s.availability = Availability::Live;
            s.batteryState = 2;
            s.percentage = static_cast<uint8_t>(100.0 - (100.0 / 360.0) * (t / 60000.0));
            s.millivolts = MvForPct(s.percentage);
            est.Update(s, t);
        }
        unsigned long long const shortGap = 20ull * 60000ull + 100000ull; /* +100 s */
        BatterySnapshot s{};
        s.availability = Availability::Live;
        s.batteryState = 2;
        s.percentage = 90;
        s.millivolts = MvForPct(90);
        DrainEstimate e = est.Update(s, shortGap);
        Print(e, s.percentage, s.millivolts, L"Discharging");
        std::printf("  -> 100 s gap: window KEPT (a gap is not a charge cycle)\n");

        unsigned long long const longGap = shortGap + 11ull * 60000ull; /* +11 min */
        s.percentage = 80;
        s.millivolts = MvForPct(80);
        e = est.Update(s, longGap);
        Print(e, s.percentage, s.millivolts, L"Discharging");
        std::printf("  -> 11 min gap: window VOIDED (charge cycle may fit inside)\n");
    }

    std::printf("\n=== D2. missed charge cycle (voltage jumped up) voids it ===\n");
    {
        DrainEstimator est;
        for (unsigned long long t = 0; t <= 20ull * 60000ull; t += 5000ull) {
            BatterySnapshot s{};
            s.availability = Availability::Live;
            s.batteryState = 2;
            s.percentage = static_cast<uint8_t>(100.0 - (100.0 / 360.0) * (t / 60000.0));
            s.millivolts = MvForPct(s.percentage);
            est.Update(s, t);
        }
        unsigned long long t = 20ull * 60000ull + 120000ull;
        BatterySnapshot s{};
        s.availability = Availability::Live;
        s.batteryState = 2;
        s.percentage = 99;                 /* charged while we were away */
        s.millivolts = MvForPct(99);
        DrainEstimate e = est.Update(s, t);
        Print(e, s.percentage, s.millivolts, L"Discharging");
        std::printf("  -> +%d mV rise detected: window VOIDED\n",
                    static_cast<int>(s.millivolts) - static_cast<int>(MvForPct(94)));
    }

    std::printf("\n=== E. tooltip + popup lines (Live, discharging) ===\n");
    {
        DrainEstimator est;
        for (unsigned long long t = 0; t <= 180ull * 60000ull; t += 5000ull) {
            double const minutes = static_cast<double>(t) / 60000.0;
            double pctExact = 96.0 - (17.0 / 60.0) * minutes; /* 17 %/h */
            if (pctExact < 0.0) pctExact = 0.0;
            BatterySnapshot s{};
            s.availability = Availability::Live;
            s.batteryState = 2;
            s.percentage = static_cast<uint8_t>(pctExact);
            s.millivolts = MvForPct(pctExact);
            s.ageSeconds = 3;
            s.sequence = 120;
            gLastEstimate = est.Update(s, t);
            if (t == 180ull * 60000ull) {
                std::printf("  tooltip: %ls\n", BuildTooltip(s).c_str());
                PopupColors colors{};
                PopupLayout layout = ComputeLayout(s, colors, 96);
                for (auto const &line : layout.telemetryLines) {
                    std::printf("    | %ls\n", line.first.c_str());
                }
            }
        }
    }

    std::printf("\n=== G. REAL DATA: load-step artifact must NOT fool the rate ===\n");
    {
        /* Operator's real trace: 22:52 -> 83 % / 4000 mV, 23:36 -> 78 % / 3947 mV.
         * Truth over those 44 min: 5 % and 53 mV -> 6.8 %/h, 72 mV/h.
         * A first/last estimator over a 7 min sub-window saw a load-step sag
         * (keypress / radio burst / LED) and reported 41.6 %/h. */
        double const truePctPerMin = 5.0 / 44.0;   /* 0.1136 %/min = 6.8 %/h */
        double const mVPerPct = 11.4;
        DrainEstimator est;
        ULONGLONG const start = 1000000ull;
        /* 30 min of clean discharge, then a 40 mV load-step sag that recovers. */
        for (unsigned long long t = 0; t <= 44ull * 60000ull; t += 5000ull) {
            double const minutes = static_cast<double>(t) / 60000.0;
            double mv = 4000.0 - truePctPerMin * minutes * mVPerPct;
            if (minutes > 30.0 && minutes < 34.0) {
                double const phase = (minutes - 30.0) / 4.0;
                mv -= 40.0 * (1.0 - phase);   /* sag then recover */
            }
            BatterySnapshot s{};
            s.availability = Availability::Live;
            s.batteryState = 2;
            s.percentage = static_cast<uint8_t>(83.0 - truePctPerMin * minutes);
            s.millivolts = static_cast<uint16_t>(mv);
            gLastEstimate = est.Update(s, start + t);
        }
        std::printf("  true rate: 6.8 %%/h (72 mV/h)\n");
        std::printf("  reported : %.1f %%/h (%.1f mV/h) window=%.0f min conf=%ls\n",
                    gLastEstimate.percentPerMinute * 60.0,
                    gLastEstimate.millivoltsPerMinute * 60.0,
                    static_cast<double>(gLastEstimate.windowMs) / 60000.0,
                    DrainConfidenceName(gLastEstimate.confidence,
                                        gLastEstimate.voltageBased));
        wchar_t eta[64]{};
        FormatCountdown(eta, std::size(eta));
        std::printf("  countdown: \"%ls\"  (pct 78 -> true remaining ~%0.f min)\n",
                    eta, 78.0 / truePctPerMin);
    }

    std::printf("\n=== F. guard: countdown hidden while charging ===\n");
    {
        BatterySnapshot s{};
        s.availability = Availability::Live;
        s.batteryState = 1;
        s.percentage = 42;
        s.millivolts = MvForPct(42);
        gLastEstimate = DrainEstimate{};
        gLastEstimate.valid = true;
        gLastEstimate.minutesRemaining = 120.0;
        gLastEstimate.percentPerMinute = 0.35;
        gLastEstimate.windowMs = 30ull * 60000ull;
        gLastEstimate.confidence = DrainConfidence::Medium;
        std::printf("  IsCountdownApplicable(charging)=%d (expect 0), discharging=%d (expect 1)\n",
                    IsCountdownApplicable(s) ? 1 : 0,
                    [] { BatterySnapshot d = BatterySnapshot{};
                         d.availability = Availability::Live;
                         d.batteryState = 2;
                         return IsCountdownApplicable(d) ? 1 : 0; }());
        wchar_t eta[64]{};
        std::printf("  FormatCountdown with valid 120 min = %d -> \"%ls\"\n",
                    FormatCountdown(eta, std::size(eta)) ? 1 : 0, eta);
    }
    return 0;
}