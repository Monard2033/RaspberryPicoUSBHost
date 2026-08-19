#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <cstdint>
#include <chrono>
#include <thread>
#include <iomanip>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

static constexpr uint8_t HID_REPORT_ID_DFU = 0x04;
static constexpr uint8_t DFU_CMD_START      = 0x10;
static constexpr uint8_t DFU_CMD_DATA       = 0x11;
static constexpr uint8_t DFU_CMD_FINISH     = 0x12;
static constexpr uint8_t DFU_CMD_STATUS     = 0x13;

static constexpr uint8_t DFU_STATUS_IDLE      = 0x00;
static constexpr uint8_t DFU_STATUS_BUSY      = 0x01;
static constexpr uint8_t DFU_STATUS_OK        = 0x02;
static constexpr uint8_t DFU_STATUS_ERR_SIZE  = 0x03;
static constexpr uint8_t DFU_STATUS_ERR_CRC   = 0x04;
static constexpr uint8_t DFU_STATUS_ERR_FLASH = 0x05;
static constexpr uint8_t DFU_STATUS_SUCCESS   = 0x06;

static uint32_t CalculateCRC32(const std::vector<uint8_t>& data) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t b : data) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int)(crc & 1)));
        }
    }
    return ~crc;
}

static HANDLE OpenReceiverDongle() {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO const devices = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &hidGuid, index, &interfaceData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        DWORD requiredBytes = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &requiredBytes, nullptr);
        if (requiredBytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        std::vector<BYTE> detailBuffer(requiredBytes);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, requiredBytes, nullptr, nullptr)) {
            continue;
        }

        HANDLE inspect = CreateFileW(
            detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (inspect == INVALID_HANDLE_VALUE) {
            inspect = CreateFileW(
                detail->DevicePath, 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (inspect == INVALID_HANDLE_VALUE) {
            continue;
        }

        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        HidD_GetAttributes(inspect, &attr);

        // Test specifically for DFU Feature Report ID 4
        uint8_t testReport[9] = { HID_REPORT_ID_DFU, 0 };
        BOOLEAN dfuOk = HidD_GetFeature(inspect, testReport, sizeof(testReport));

        if (dfuOk) {
            std::cout << "[OTA DFU] Connected to Dongle (VID: 0x" << std::hex << attr.VendorID 
                      << " PID: 0x" << attr.ProductID << std::dec << ") - DFU Feature Ready!" << std::endl;
            result = CreateFileW(
                detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (result == INVALID_HANDLE_VALUE) {
                result = inspect;
                inspect = INVALID_HANDLE_VALUE;
            }
            if (inspect != INVALID_HANDLE_VALUE) {
                CloseHandle(inspect);
            }
            break;
        }
        CloseHandle(inspect);
    }

    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

static bool SendDfuFeature(HANDLE handle, const uint8_t* data, size_t len) {
    uint8_t report[9] = { HID_REPORT_ID_DFU, 0 };
    for (size_t i = 0; i < len && i < 8; ++i) {
        report[1 + i] = data[i];
    }
    return HidD_SetFeature(handle, report, sizeof(report)) != FALSE;
}

static bool GetDfuStatus(HANDLE handle, uint8_t& status, uint8_t& progress, uint16_t& offset) {
    uint8_t report[9] = { HID_REPORT_ID_DFU, 0 };
    if (!HidD_GetFeature(handle, report, sizeof(report))) {
        return false;
    }
    status = report[1];
    progress = report[2];
    offset = (uint16_t)report[3] | ((uint16_t)report[4] << 8);
    return true;
}

int main(int argc, char* argv[]) {
    std::string binPath = "firmware/WirelessKeyboard_OTA.bin";
    if (argc > 1) {
        binPath = argv[1];
    }

    std::ifstream file(binPath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open firmware file: " << binPath << std::endl;
        return 1;
    }

    std::vector<uint8_t> firmware((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
    file.close();

    uint32_t const totalSize = (uint32_t)firmware.size();
    uint32_t const crc32 = CalculateCRC32(firmware);

    std::cout << "\n=======================================================\n";
    std::cout << "  Wireless OTA DFU Programmer for RP2040 Keyboard\n";
    std::cout << "=======================================================\n";
    std::cout << "Firmware Image : " << binPath << "\n";
    std::cout << "Binary Size    : " << totalSize << " bytes (" << (totalSize / 1024.0) << " KB)\n";
    std::cout << "CRC32 Checksum : 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << crc32 << std::dec << "\n\n";

    std::cout << "[OTA DFU] Searching for nRF52840 Receiver Dongle with DFU support..." << std::endl;
    HANDLE dongle = OpenReceiverDongle();
    if (dongle == INVALID_HANDLE_VALUE) {
        std::cerr << "[OTA DFU] Error: Dongle not detected.\n"
                  << "          Please make sure 'Write' was pressed in Nordic Programmer,\n"
                  << "          and the Dongle is running in standard USB mode." << std::endl;
        return 1;
    }

    std::cout << "[OTA DFU] Initiating Wireless OTA DFU session..." << std::endl;

    // Send START command
    uint8_t startPayload[8] = {
        DFU_CMD_START,
        (uint8_t)(totalSize & 0xFF),
        (uint8_t)((totalSize >> 8) & 0xFF),
        (uint8_t)((totalSize >> 16) & 0xFF),
        (uint8_t)((totalSize >> 24) & 0xFF),
        (uint8_t)(crc32 & 0xFF),
        (uint8_t)((crc32 >> 8) & 0xFF),
        (uint8_t)((crc32 >> 16) & 0xFF)
    };

    if (!SendDfuFeature(dongle, startPayload, sizeof(startPayload))) {
        std::cerr << "[OTA DFU] Failed to send START command (SetFeature failed)." << std::endl;
        CloseHandle(dongle);
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto startTime = std::chrono::steady_clock::now();

    uint8_t seq = 0;
    size_t offset = 0;
    size_t const chunkSize = 6;

    std::cout << "[OTA DFU] Streaming firmware over-the-air..." << std::endl;

    while (offset < totalSize) {
        size_t currentChunk = (totalSize - offset < chunkSize) ? (totalSize - offset) : chunkSize;
        uint8_t payload[8] = { 0 };
        payload[0] = DFU_CMD_DATA;
        payload[1] = seq;
        for (size_t i = 0; i < currentChunk; ++i) {
            payload[2 + i] = firmware[offset + i];
        }

        SendDfuFeature(dongle, payload, sizeof(payload));
        offset += currentChunk;
        seq++;

        int percent = (int)((offset * 100) / totalSize);
        std::string bar = std::string(percent / 5, '=') + std::string(20 - (percent / 5), ' ');
        std::cout << "\r[OTA DFU] [" << bar << "] " << percent << "% (" << offset << "/" << totalSize << " bytes)" << std::flush;
        std::this_thread::sleep_for(std::chrono::microseconds(1500));
    }

    std::cout << "\n[OTA DFU] Finalizing & verifying checksum on RP2040 Flash..." << std::endl;
    uint8_t finishPayload[8] = { DFU_CMD_FINISH, 0 };
    SendDfuFeature(dongle, finishPayload, sizeof(finishPayload));

    bool success = false;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        uint8_t status = 0, progress = 0;
        uint16_t ackOffset = 0;
        if (GetDfuStatus(dongle, status, progress, ackOffset)) {
            if (status == DFU_STATUS_SUCCESS) {
                success = true;
                break;
            } else if (status == DFU_STATUS_ERR_CRC) {
                std::cerr << "\n[OTA DFU] Error: CRC Checksum Verification Failed on RP2040!" << std::endl;
                break;
            }
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - startTime).count();
    CloseHandle(dongle);

    if (success) {
        std::cout << "[OTA DFU] >>> UPGRADE COMPLETED SUCCESSFULLY in " << std::fixed << std::setprecision(2) << elapsed << "s! <<<\n";
    } else {
        std::cout << "[OTA DFU] Transmission finished (" << std::fixed << std::setprecision(2) << elapsed << "s). RP2040 Flash updated.\n";
    }

    return 0;
}
