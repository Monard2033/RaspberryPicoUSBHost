/* Live HID harness: compiles the REAL production src/main.cpp and exercises the
 * actual open/read path against the plugged-in Receiver. Proves the dual-length
 * compatibility (legacy 9-byte vs new 11-byte feature report) without needing a
 * visual check of the tray popup.
 */
#define wWinMain wWinMain_DisabledForTest
#include "src/main.cpp"
#undef wWinMain

#include <cstdio>

int wmain()
{
    std::printf("=== live Receiver read via production code ===\n");

    HANDLE const handle = OpenBatteryCollection();
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("  open failed: %lu\n", GetLastError());
        return 1;
    }

    std::printf("  detected feature report length : %zu bytes\n",
                gBatteryReportLength);
    std::printf("  primary  (new firmware) needs  : %zu bytes\n",
                kBatteryReportLength);
    std::printf("  legacy   (old firmware) needs  : %zu bytes\n",
                kBatteryReportLengthLegacy);

    for (int i = 0; i < 3; ++i) {
        BatterySnapshot const s = ReadBattery(handle);
        gLastSnapshot = s;
        gLastEstimate = DrainEstimate{};
        wchar_t eta[64]{};
        bool const haveEta = FormatCountdown(eta, std::size(eta));

        std::printf("  avail=%d pct=%u state=%u mV=%u age=%us flags=0x%02X "
                    "etaValid=%d etaMin=%u\n",
                    static_cast<int>(s.availability),
                    static_cast<unsigned>(s.percentage),
                    static_cast<unsigned>(s.batteryState),
                    static_cast<unsigned>(s.millivolts),
                    static_cast<unsigned>(s.ageSeconds),
                    static_cast<unsigned>(s.flags),
                    s.etaValid ? 1 : 0,
                    static_cast<unsigned>(s.etaMinutes));
        std::printf("      countdown=\"%ls\"\n", haveEta ? eta : L"(none)");
        std::printf("      tooltip  =\"%ls\"\n", BuildTooltip(s).c_str());

        PopupColors colors{};
        PopupLayout const layout = ComputeLayout(s, colors, 96);
        std::printf("      popup width=%d px, %zu lines:\n", layout.width,
                    layout.telemetryLines.size());
        for (auto const &line : layout.telemetryLines) {
            std::printf("        | %ls\n", line.first.c_str());
        }
        Sleep(1200);
    }

    CloseHandle(handle);

    std::printf("\n=== persisted cache state ===\n");
    bool const loaded = LoadEtaCache();
    std::printf("  LoadEtaCache()=%d valid=%d minutes=%u pct=%u savedAt=%u\n",
                loaded ? 1 : 0, gEtaCache.valid ? 1 : 0,
                static_cast<unsigned>(gEtaCache.minutes),
                static_cast<unsigned>(gEtaCache.percent),
                static_cast<unsigned>(gEtaCache.savedAtUnix));

    std::printf("\n=== simulated startup with no Receiver data ===\n");
    gLastSnapshot = BatterySnapshot{};
    gLastEstimate = DrainEstimate{};
    wchar_t eta[64]{};
    if (FormatCountdown(eta, std::size(eta))) {
        std::printf("  countdown=\"%ls\"  (from cache, not 'estimating')\n", eta);
    } else {
        std::printf("  countdown=(none) -> popup would show estimating\n");
    }
    std::printf("\n=== popup width with a long countdown (synthetic) ===\n");
    {
        BatterySnapshot s{};
        s.availability = Availability::Live;
        s.batteryState = 2;
        s.percentage = 71;
        s.millivolts = 3860;
        s.ageSeconds = 4;
        s.sequence = 18;
        s.etaValid = true;
        s.etaMinutes = 1454;   /* 24 h 14 min, the widest realistic value */

        gLastSnapshot = s;
        gEtaCache = EtaCache{};
        gLastEstimate = DrainEstimate{};

        PopupColors colors{};
        PopupLayout const layout = ComputeLayout(s, colors, 96);
        std::printf("  width=%d px (was fixed at 236 before)\n", layout.width);
        std::printf("  tooltip=\"%ls\"\n", BuildTooltip(s).c_str());
        for (auto const &line : layout.telemetryLines) {
            std::printf("    | %ls\n", line.first.c_str());
        }

        /* Now with a long Rate line, which is the widest realistic content. */
        s.etaValid = false;
        gLastEstimate = DrainEstimate{};
        gLastEstimate.valid = true;
        gLastEstimate.minutesRemaining = 612.0;
        gLastEstimate.percentPerMinute = 0.6933;   /* 41.6 %%/h */
        gLastEstimate.millivoltsPerMinute = 7.9;   /* 474 mV/h */
        gLastEstimate.windowMs = 3600000ull;
        gLastEstimate.confidence = DrainConfidence::High;
        PopupLayout const wide = ComputeLayout(s, colors, 96);
        std::printf("  with long Rate line: width=%d px\n", wide.width);
        for (auto const &line : wide.telemetryLines) {
            std::printf("    | %ls\n", line.first.c_str());
        }
    }

    std::printf("\n=== no-data fallback path ===\n");
    {
        gLastSnapshot = BatterySnapshot{};
        gLastEstimate = DrainEstimate{};
        gEtaCache = EtaCache{};
        gEtaCache.valid = true;
        gEtaCache.minutes = 612;
        gEtaCache.percent = 68;

        BatterySnapshot s{};
        s.availability = Availability::Offline;
        wchar_t eta[64]{};
        std::printf("  offline + cache: countdown=\"%ls\"\n",
                    FormatCountdown(eta, std::size(eta)) ? eta : L"(none)");

        s.availability = Availability::Live;
        s.batteryState = 2;
        s.percentage = 68;
        s.millivolts = 3826;
        gLastSnapshot = s;
        std::printf("  live, no receiver eta, cache seeded: \"%ls\"\n",
                    FormatCountdown(eta, std::size(eta)) ? eta : L"(none)");
    }

    return 0;
}