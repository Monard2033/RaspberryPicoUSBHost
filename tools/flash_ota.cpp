#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#endif

namespace {

constexpr uint16_t RECEIVER_VID = 0x1B4F;
constexpr uint16_t RECEIVER_PID = 0x0001;
constexpr uint8_t HID_REPORT_ID_DFU = 0x04;
constexpr uint8_t OTA_TARGET_RP2040 = 0x01;
constexpr uint8_t OTA_PROTOCOL_VERSION = 0x01;
constexpr uint16_t OTA_BOARD_WEACT_RP2040_4MB = 0x2040;

constexpr uint8_t DFU_CMD_START = 0x10;
constexpr uint8_t DFU_CMD_DATA = 0x11;
constexpr uint8_t DFU_CMD_FINISH = 0x12;
constexpr uint8_t DFU_CMD_CRC = 0x14;
constexpr uint8_t DFU_CMD_ACTIVATE = 0x15;
constexpr uint8_t DFU_CMD_ABORT = 0x16;
constexpr uint8_t DFU_CMD_QUERY = 0x17;

constexpr uint8_t DFU_STATUS_IDLE = 0x00;
constexpr uint8_t DFU_STATUS_BUSY = 0x01;
constexpr uint8_t DFU_STATUS_OK = 0x02;
constexpr uint8_t DFU_STATUS_ERR_SIZE = 0x03;
constexpr uint8_t DFU_STATUS_ERR_CRC = 0x04;
constexpr uint8_t DFU_STATUS_ERR_FLASH = 0x05;
constexpr uint8_t DFU_STATUS_VERIFIED = 0x06;
constexpr uint8_t DFU_STATUS_ERR_TARGET = 0x07;
constexpr uint8_t DFU_STATUS_ERR_PROTOCOL = 0x08;
constexpr uint8_t DFU_STATUS_ERR_SESSION = 0x09;
constexpr uint8_t DFU_STATUS_ERR_STATE = 0x0A;
constexpr uint8_t DFU_STATUS_APPLYING = 0x0B;
constexpr uint8_t DFU_STATUS_BOOT_OK = 0x0C;
constexpr uint8_t DFU_STATUS_ABORTED = 0x0D;

/* The application is linked at the start of XIP flash (boot2 at 0, ARM
 * vectors at 0x100); the strict port keeps the non-relocated layout. */
constexpr uint32_t ACTIVE_XIP_START = 0x10000000;
constexpr uint32_t ACTIVE_XIP_END = 0x10200000;
constexpr uint32_t SRAM_START = 0x20000000;
constexpr uint32_t SRAM_END = 0x20042000;
constexpr uint32_t APP_VECTOR_OFFSET = 0x100;

constexpr std::array<uint8_t, 8> PACKAGE_MAGIC = {
    'W', 'K', 'R', 'P', 'O', 'T', 'A', '1'
};

#pragma pack(push, 1)
struct OtaPackageHeader {
    uint8_t magic[8];
    uint16_t formatVersion;
    uint8_t target;
    uint8_t protocol;
    uint16_t boardId;
    uint16_t headerSize;
    uint32_t payloadSize;
    uint32_t payloadCrc32;
    uint32_t vectorOffset;
    uint32_t headerCrc32;
};
#pragma pack(pop)

static_assert(sizeof(OtaPackageHeader) == 32);

struct FirmwarePackage {
    OtaPackageHeader header{};
    std::vector<uint8_t> payload;
};

struct DfuStatus {
    uint8_t status = 0;
    uint8_t session = 0;
    uint8_t token = 0;
    uint8_t detail = 0;
    uint32_t value = 0;
};

struct CommandResult {
    DfuStatus response{};
    uint8_t token = 0;
};

uint32_t ReadLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t CalculateCRC32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^
                  (0xEDB88320u & static_cast<uint32_t>(
                      -static_cast<int32_t>(crc & 1u)));
        }
    }
    return ~crc;
}

std::string StatusName(uint8_t status) {
    switch (status) {
    case DFU_STATUS_IDLE: return "IDLE";
    case DFU_STATUS_BUSY: return "BUSY";
    case DFU_STATUS_OK: return "OK";
    case DFU_STATUS_ERR_SIZE: return "ERR_SIZE";
    case DFU_STATUS_ERR_CRC: return "ERR_CRC";
    case DFU_STATUS_ERR_FLASH: return "ERR_FLASH";
    case DFU_STATUS_VERIFIED: return "VERIFIED";
    case DFU_STATUS_ERR_TARGET: return "ERR_TARGET";
    case DFU_STATUS_ERR_PROTOCOL: return "ERR_PROTOCOL";
    case DFU_STATUS_ERR_SESSION: return "ERR_SESSION";
    case DFU_STATUS_ERR_STATE: return "ERR_STATE";
    case DFU_STATUS_APPLYING: return "APPLYING";
    case DFU_STATUS_BOOT_OK: return "BOOT_OK";
    case DFU_STATUS_ABORTED: return "ABORTED";
    default: return "UNKNOWN";
    }
}

bool IsErrorStatus(uint8_t status) {
    return status >= DFU_STATUS_ERR_SIZE && status <= DFU_STATUS_ERR_STATE &&
           status != DFU_STATUS_VERIFIED;
}

FirmwarePackage LoadPackage(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open OTA package: " + path);
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    if (bytes.size() < sizeof(OtaPackageHeader)) {
        throw std::runtime_error("OTA package is shorter than its header");
    }

    FirmwarePackage package;
    std::memcpy(&package.header, bytes.data(), sizeof(package.header));
    if (!std::equal(PACKAGE_MAGIC.begin(), PACKAGE_MAGIC.end(),
                    package.header.magic)) {
        throw std::runtime_error(
            "not a WirelessKeyboard RP2040 .wkota package");
    }
    if (package.header.formatVersion != 1 ||
        package.header.headerSize != sizeof(OtaPackageHeader) ||
        package.header.target != OTA_TARGET_RP2040 ||
        package.header.protocol != OTA_PROTOCOL_VERSION ||
        package.header.boardId != OTA_BOARD_WEACT_RP2040_4MB) {
        throw std::runtime_error(
            "OTA package target/protocol/board does not match WeAct RP2040 4MB");
    }
    uint32_t const headerCrc = CalculateCRC32(
        bytes.data(), offsetof(OtaPackageHeader, headerCrc32));
    if (headerCrc != package.header.headerCrc32) {
        throw std::runtime_error("OTA package header CRC32 is invalid");
    }
    if (bytes.size() != package.header.headerSize +
                        package.header.payloadSize) {
        throw std::runtime_error("OTA package payload size is inconsistent");
    }

    package.payload.assign(bytes.begin() + package.header.headerSize,
                           bytes.end());
    uint32_t const payloadCrc = CalculateCRC32(package.payload.data(),
                                               package.payload.size());
    if (payloadCrc != package.header.payloadCrc32) {
        throw std::runtime_error("OTA package payload CRC32 is invalid");
    }
    if (package.header.vectorOffset != APP_VECTOR_OFFSET ||
        package.payload.size() < APP_VECTOR_OFFSET + 8) {
        throw std::runtime_error("OTA package has no RP2040 application vectors");
    }
    uint32_t const stackPointer = ReadLe32(
        package.payload.data() + APP_VECTOR_OFFSET);
    uint32_t const resetHandler = ReadLe32(
        package.payload.data() + APP_VECTOR_OFFSET + 4);
    uint32_t const resetAddress = resetHandler & ~1u;
    if (stackPointer < SRAM_START || stackPointer > SRAM_END ||
        (resetHandler & 1u) == 0 || resetAddress < ACTIVE_XIP_START ||
        resetAddress >= ACTIVE_XIP_END) {
        throw std::runtime_error(
            "payload vectors are not linked for RP2040 XIP flash");
    }
    return package;
}

HANDLE OpenReceiverDongle() {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO devices = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGuid, index,
                                         &interfaceData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
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
            result = handle;
            break;
        }
        CloseHandle(handle);
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

bool GetDfuStatus(HANDLE handle, DfuStatus& status) {
    std::array<uint8_t, 9> report{HID_REPORT_ID_DFU};
    if (!HidD_GetFeature(handle, report.data(),
                         static_cast<ULONG>(report.size()))) {
        return false;
    }
    status.status = report[1];
    status.session = report[2];
    status.token = report[3];
    status.detail = report[4];
    status.value = ReadLe32(&report[5]);
    return true;
}

bool SetDfuFeature(HANDLE handle, const std::array<uint8_t, 8>& payload) {
    std::array<uint8_t, 9> report{HID_REPORT_ID_DFU};
    std::copy(payload.begin(), payload.end(), report.begin() + 1);
    return HidD_SetFeature(handle, report.data(),
                           static_cast<ULONG>(report.size())) != FALSE;
}

CommandResult SendCommandAndWait(HANDLE handle,
                                 const std::array<uint8_t, 8>& payload,
                                 std::chrono::milliseconds timeout) {
    DfuStatus baseline{};
    if (!GetDfuStatus(handle, baseline)) {
        throw std::runtime_error("GetFeature failed before DFU command");
    }

    bool accepted = SetDfuFeature(handle, payload);
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    uint8_t token = baseline.token;
    bool tokenObserved = false;

    while (std::chrono::steady_clock::now() < deadline) {
        DfuStatus current{};
        if (GetDfuStatus(handle, current) &&
            current.session == payload[1] &&
            current.token != baseline.token) {
            token = current.token;
            tokenObserved = true;
            if (current.status != DFU_STATUS_BUSY) {
                if (IsErrorStatus(current.status)) {
                    throw std::runtime_error(
                        "device returned " + StatusName(current.status) +
                        " detail=" + std::to_string(current.detail) +
                        " value=" + std::to_string(current.value));
                }
                return {current, token};
            }
        } else if (!accepted && !tokenObserved) {
            /* A failed SetFeature may be a BUSY stall. Before retrying, first
             * look for a changed token proving that Windows delivered it. */
            accepted = SetDfuFeature(handle, payload);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error(
        accepted ? "timeout waiting for end-to-end RP2040 acknowledgement"
                 : "Receiver rejected the DFU Feature command");
}

void ExpectStatus(const CommandResult& result, uint8_t expected,
                  uint32_t expectedValue, bool compareValue = true) {
    if (result.response.status != expected ||
        (compareValue && result.response.value != expectedValue)) {
        throw std::runtime_error(
            "unexpected response " + StatusName(result.response.status) +
            " value=" + std::to_string(result.response.value));
    }
}

bool WaitForBootConfirmation(HANDLE handle, uint8_t session, uint8_t token,
                             uint32_t expectedCrc,
                             std::chrono::milliseconds timeout) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        DfuStatus status{};
        if (GetDfuStatus(handle, status) && status.session == session &&
            status.token == token && status.status == DFU_STATUS_BOOT_OK &&
            status.value == expectedCrc) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        std::string packagePath = "firmware/WirelessKeyboard_OTA.wkota";
        if (argc > 1) packagePath = argv[1];

        FirmwarePackage const package = LoadPackage(packagePath);
        uint8_t session = static_cast<uint8_t>(
            (GetTickCount64() ^ package.header.payloadCrc32 ^
             package.header.payloadSize) & 0xFFu);
        if (session == 0) session = 1;

        std::cout << "WirelessKeyboard strict RP2040 OTA\n"
                  << "Package : " << packagePath << "\n"
                  << "Target  : WeAct RP2040 4MB\n"
                  << "Size    : " << package.payload.size() << " bytes\n"
                  << "CRC32   : 0x" << std::hex << std::uppercase
                  << std::setw(8) << std::setfill('0')
                  << package.header.payloadCrc32 << std::dec << "\n"
                  << "Session : " << static_cast<unsigned>(session) << "\n";

        HANDLE receiver = OpenReceiverDongle();
        if (receiver == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(
                "A4TECH Receiver VID 1B4F/PID 0001 with DFU report was not found");
        }

        auto closeReceiver = [&]() {
            if (receiver != INVALID_HANDLE_VALUE) {
                CloseHandle(receiver);
                receiver = INVALID_HANDLE_VALUE;
            }
        };

        try {
            std::array<uint8_t, 8> start = {
                DFU_CMD_START,
                session,
                OTA_TARGET_RP2040,
                OTA_PROTOCOL_VERSION,
                static_cast<uint8_t>(package.payload.size()),
                static_cast<uint8_t>(package.payload.size() >> 8),
                static_cast<uint8_t>(package.payload.size() >> 16),
                static_cast<uint8_t>(package.payload.size() >> 24),
            };
            std::cout << "Waiting for RP2040 START acknowledgement"
                         " (sleeping radio discovery may take up to 30s)...\n";
            ExpectStatus(SendCommandAndWait(
                             receiver, start, std::chrono::seconds(40)),
                         DFU_STATUS_OK, 0);

            std::array<uint8_t, 8> crc = {
                DFU_CMD_CRC,
                session,
                static_cast<uint8_t>(package.header.payloadCrc32),
                static_cast<uint8_t>(package.header.payloadCrc32 >> 8),
                static_cast<uint8_t>(package.header.payloadCrc32 >> 16),
                static_cast<uint8_t>(package.header.payloadCrc32 >> 24),
                static_cast<uint8_t>(OTA_BOARD_WEACT_RP2040_4MB),
                static_cast<uint8_t>(OTA_BOARD_WEACT_RP2040_4MB >> 8),
            };
            ExpectStatus(SendCommandAndWait(
                             receiver, crc, std::chrono::seconds(5)),
                         DFU_STATUS_OK, 0);

            size_t offset = 0;
            int lastPercent = -1;
            while (offset < package.payload.size()) {
                std::array<uint8_t, 8> data{};
                data[0] = DFU_CMD_DATA;
                data[1] = session;
                size_t const count = std::min<size_t>(
                    6, package.payload.size() - offset);
                std::copy_n(package.payload.begin() + offset, count,
                            data.begin() + 2);

                size_t const nextOffset = offset + count;
                CommandResult const result = SendCommandAndWait(
                    receiver, data, std::chrono::seconds(5));
                ExpectStatus(result, DFU_STATUS_OK,
                             static_cast<uint32_t>(nextOffset));
                offset = nextOffset;

                int const percent = static_cast<int>(
                    offset * 100 / package.payload.size());
                if (percent != lastPercent) {
                    lastPercent = percent;
                    std::cout << "\rTransfer: " << std::setw(3) << percent
                              << "%  " << offset << "/"
                              << package.payload.size() << std::flush;
                }
            }
            std::cout << "\nVerifying complete staging image...\n";

            std::array<uint8_t, 8> finish{};
            finish[0] = DFU_CMD_FINISH;
            finish[1] = session;
            ExpectStatus(SendCommandAndWait(
                             receiver, finish, std::chrono::seconds(15)),
                         DFU_STATUS_VERIFIED,
                         package.header.payloadCrc32);

            std::array<uint8_t, 8> activate{};
            activate[0] = DFU_CMD_ACTIVATE;
            activate[1] = session;
            CommandResult const activation = SendCommandAndWait(
                receiver, activate, std::chrono::seconds(10));
            ExpectStatus(activation, DFU_STATUS_APPLYING,
                         package.header.payloadCrc32);

            std::cout << "Image verified. RP2040 is applying it "
                         "(RAM-resident slot swap)...\n";
            if (!WaitForBootConfirmation(
                    receiver, session, activation.token,
                    package.header.payloadCrc32,
                    std::chrono::seconds(30))) {
                std::array<uint8_t, 8> query = {
                    DFU_CMD_QUERY,
                    static_cast<uint8_t>(session + 1u),
                    OTA_TARGET_RP2040,
                    OTA_PROTOCOL_VERSION,
                    static_cast<uint8_t>(OTA_BOARD_WEACT_RP2040_4MB),
                    static_cast<uint8_t>(OTA_BOARD_WEACT_RP2040_4MB >> 8),
                    0,
                    0,
                };
                CommandResult const queried = SendCommandAndWait(
                    receiver, query, std::chrono::seconds(40));
                ExpectStatus(queried, DFU_STATUS_BOOT_OK,
                             package.header.payloadCrc32);
            }

            closeReceiver();
            std::cout << "OTA COMPLETE: RP2040 booted the exact package CRC32.\n";
            return 0;
        } catch (...) {
            closeReceiver();
            throw;
        }
    } catch (const std::exception& error) {
        std::cerr << "OTA FAILED: " << error.what() << "\n";
        return 1;
    }
}
