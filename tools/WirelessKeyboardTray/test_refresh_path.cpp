/* Reproduces the tray worker's exact ON-DEMAND-REFRESH sequence against the
 * live Receiver to find out whether the SET_REPORT that "Refresh now" sends
 * poisons the following GET_FEATURE (observed as "HID read error: 87",
 * ERROR_INVALID_PARAMETER) inside the tray.
 */
#define wWinMain wWinMain_DisabledForTest
#include "src/main.cpp"
#undef wWinMain

#include <cstdio>

static void Show(char const *tag, HANDLE handle)
{
    BatterySnapshot const s = ReadBattery(handle);
    std::printf("  %-26s avail=%d err=%lu pct=%u state=%u mV=%u flags=0x%02X\n",
                tag, static_cast<int>(s.availability),
                static_cast<unsigned long>(s.error),
                static_cast<unsigned>(s.percentage),
                static_cast<unsigned>(s.batteryState),
                static_cast<unsigned>(s.millivolts),
                static_cast<unsigned>(s.flags));
}

int wmain()
{
    std::printf("=== refresh path probe ===\n");

    HANDLE handle = OpenBatteryCollection();
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("  open failed: %lu\n", GetLastError());
        return 1;
    }
    std::printf("  detected report length = %zu (payload %zu)\n",
                gBatteryReportLength, kBatteryPayloadLength);

    Show("baseline #1", handle);
    Show("baseline #2", handle);

    /* Exactly what WorkerMain does for WAIT_OBJECT_0 + 1 ("Refresh now"). */
    std::printf("\n-- SET_FEATURE with %zu bytes (tray array size) --\n",
                kBatteryReportLength);
    std::array<uint8_t, kBatteryReportLength> pollReq{};
    pollReq[0] = kBatteryReportId;
    pollReq[1] = 0x01;
    BOOL const setOk = HidD_SetFeature(handle, pollReq.data(),
                                       static_cast<ULONG>(pollReq.size()));
    std::printf("  HidD_SetFeature(%zu) -> %s err=%lu\n", pollReq.size(),
                setOk ? "OK" : "FAIL",
                setOk ? 0UL : static_cast<unsigned long>(GetLastError()));
    Sleep(150);
    Show("after set #1", handle);
    Show("after set #2", handle);
    Show("after set #3", handle);

    /* Now the legacy size, to see whether the length is what matters. */
    std::printf("\n-- SET_FEATURE with 9 bytes (legacy size) --\n");
    std::array<uint8_t, 9> legacy{};
    legacy[0] = kBatteryReportId;
    legacy[1] = 0x01;
    BOOL const legacyOk = HidD_SetFeature(handle, legacy.data(),
                                          static_cast<ULONG>(legacy.size()));
    std::printf("  HidD_SetFeature(9) -> %s err=%lu\n", legacyOk ? "OK" : "FAIL",
                legacyOk ? 0UL : static_cast<unsigned long>(GetLastError()));
    Sleep(150);
    Show("after legacy set #1", handle);
    Show("after legacy set #2", handle);

    /* Does a fresh handle recover? */
    std::printf("\n-- reopen and read --\n");
    CloseHandle(handle);
    handle = OpenBatteryCollection();
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("  reopen failed: %lu\n", GetLastError());
        return 1;
    }
    Show("fresh handle #1", handle);
    Show("fresh handle #2", handle);

    CloseHandle(handle);
    return 0;
}
