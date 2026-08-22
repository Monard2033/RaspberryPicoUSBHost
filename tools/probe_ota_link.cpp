/* Diagnostic probe for the strict OTA reverse chain (PC -> Receiver -> ESB
 * ACK payload -> Transmitter -> SPI MISO -> RP2040 -> DFU_STATUS -> PC).
 * Sends one harmless QUERY and reports any session/token echo for 80 s,
 * covering two OTA radio-discovery cycles. No flash is touched. */
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#endif

namespace {

constexpr uint16_t RECEIVER_VID = 0x1B4F;
constexpr uint16_t RECEIVER_PID = 0x0001;
constexpr uint8_t HID_REPORT_ID_DFU = 0x04;
constexpr uint8_t DFU_CMD_QUERY = 0x17;
constexpr uint8_t OTA_TARGET_RP2040 = 0x01;
constexpr uint8_t OTA_PROTOCOL_VERSION = 0x01;
constexpr uint16_t OTA_BOARD = 0x2040;

uint32_t ReadLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

HANDLE OpenReceiverDongle() {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devices = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGuid, index,
                                         &interfaceData)) {
            break;
        }
        DWORD requiredBytes = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0,
                                         &requiredBytes, nullptr);
        if (requiredBytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;

        std::vector<uint8_t> detailBytes(requiredBytes);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            detailBytes.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail,
                                              requiredBytes, nullptr, nullptr)) {
            continue;
        }

        HANDLE handle = CreateFileW(
            detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        std::array<uint8_t, 9> feature{HID_REPORT_ID_DFU};
        if (HidD_GetAttributes(handle, &attributes) &&
            attributes.VendorID == RECEIVER_VID &&
            attributes.ProductID == RECEIVER_PID &&
            HidD_GetFeature(handle, feature.data(),
                            static_cast<ULONG>(feature.size()))) {
            SetupDiDestroyDeviceInfoList(devices);
            return handle;
        }
        CloseHandle(handle);
    }
    SetupDiDestroyDeviceInfoList(devices);
    return INVALID_HANDLE_VALUE;
}

bool GetDfuStatus(HANDLE handle, std::array<uint8_t, 9>& report) {
    report = {HID_REPORT_ID_DFU};
    return HidD_GetFeature(handle, report.data(),
                           static_cast<ULONG>(report.size())) != FALSE;
}

bool SetDfuFeature(HANDLE handle, const std::array<uint8_t, 8>& payload) {
    std::array<uint8_t, 9> report{HID_REPORT_ID_DFU};
    std::copy(payload.begin(), payload.end(), report.begin() + 1);
    return HidD_SetFeature(handle, report.data(),
                           static_cast<ULONG>(report.size())) != FALSE;
}

} // namespace

int main(int argc, char* argv[]) {
    DWORD const windowMs =
        (argc > 1) ? static_cast<DWORD>(std::atoi(argv[1])) * 1000u : 80000u;

    HANDLE receiver = OpenReceiverDongle();
    if (receiver == INVALID_HANDLE_VALUE) {
        std::printf("PROBE FAILED: Receiver dongle not found\n");
        return 2;
    }
    std::printf("Receiver opened.\n");

    std::array<uint8_t, 9> baseline{};
    if (!GetDfuStatus(receiver, baseline)) {
        std::printf("GetFeature failed\n");
        CloseHandle(receiver);
        return 2;
    }
    std::printf("Baseline: %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                baseline[0], baseline[1], baseline[2], baseline[3],
                baseline[4], baseline[5], baseline[6], baseline[7],
                baseline[8]);

    uint8_t session = static_cast<uint8_t>(GetTickCount64() | 1u);
    std::array<uint8_t, 8> query = {
        DFU_CMD_QUERY, session, OTA_TARGET_RP2040, OTA_PROTOCOL_VERSION,
        static_cast<uint8_t>(OTA_BOARD),
        static_cast<uint8_t>(OTA_BOARD >> 8), 0, 0,
    };
    bool watchingPending = false;
    if (!SetDfuFeature(receiver, query)) {
        std::printf("SetFeature rejected (a command is still held end-to-end)."
                    " Watching the held command instead...\n");
        watchingPending = true;
        session = baseline[2];
    } else {
        std::printf("Sending QUERY session=%u...\n", session);
    }

    ULONGLONG const start = GetTickCount64();
    bool echoed = false;
    std::array<uint8_t, 9> last = baseline;
    while (GetTickCount64() - start < windowMs) {
        std::array<uint8_t, 9> current{};
        if (GetDfuStatus(receiver, current) &&
            std::memcmp(current.data(), last.data(), current.size()) != 0) {
            last = current;
            std::printf("[%5llu ms] %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                        GetTickCount64() - start,
                        current[0], current[1], current[2], current[3],
                        current[4], current[5], current[6], current[7],
                        current[8]);
            if (current[2] == session && current[3] != 0 &&
                current[1] != 0x01 /*BUSY*/) {
                std::printf("ECHO OK: status=0x%02X token=%u detail=%u value=%u "
                            "(0x%08X)\n",
                            current[1], current[3], current[4],
                            ReadLe32(&current[5]), ReadLe32(&current[5]));
                echoed = true;
                break;
            }
        }
        Sleep(250);
    }

    if (!echoed) {
        std::printf("NO ECHO within 80 s: the RP2040 never confirmed the held "
                    "command (radio link to the keyboard is silent).\n");
    }
    CloseHandle(receiver);
    return echoed ? 0 : 1;
}
