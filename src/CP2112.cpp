#include "CP2112.h"
#include "log.h"
#include <chrono>
#include <cstring>
#include <cwchar>
#include <hidapi/hidapi.h>
#include <thread>

/*
 * CP2112 HID USB-to-SMBus Bridge Driver
 *
 * This class provides a minimal I2C interface for the QN8027 tuner chip
 * using the Silicon Labs CP2112 HID-to-SMBus bridge.
 *
 * IMPORTANT:
 * - The CP2112 returns I2C status/data via HID INPUT REPORTS.
 * - Therefore, readInputReport() uses hid_read_timeout(), not hid_get_feature_report().
 * - All logic is preserved exactly as in the working version.
 */

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr uint16_t SILICON_LABS_VID = 0x10C4;
constexpr uint16_t CP2112_PID       = 0xEA90;
constexpr uint16_t KFMT_PID         = 0x8E51;

// CP2112 command codes
constexpr uint8_t CMD_GPIO_CONFIG       = 0x02;
constexpr uint8_t CMD_GPIO_SET          = 0x03;
constexpr uint8_t CMD_GPIO_GET          = 0x04;
constexpr uint8_t CMD_SMB_CONFIG        = 0x06;
constexpr uint8_t CMD_I2C_WRITE         = 0x14;
constexpr uint8_t CMD_I2C_READ          = 0x10;
constexpr uint8_t CMD_I2C_WRITE_READ    = 0x11;
constexpr uint8_t CMD_I2C_STATUS        = 0x15;
constexpr uint8_t CMD_I2C_READ_FORCE    = 0x12;
constexpr uint8_t CMD_I2C_RESPONSE      = 0x16;
constexpr uint8_t CMD_I2C_DATA_RESPONSE = 0x13;
constexpr uint8_t CMD_RESET             = 0x01;

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

CP2112::CP2112(const char *serial) {
    hid_device_info *devs = hid_enumerate(SILICON_LABS_VID, KFMT_PID);
    if (!devs) {
        devs = hid_enumerate(SILICON_LABS_VID, CP2112_PID);
    }
    if (!devs) {
        LogErr(VB_PLUGIN, "CP2112: No HID devices found\n");
        return;
    }

    hid_device_info *cur = devs;
    bool found = false;

    while (cur) {
        if (cur->vendor_id == SILICON_LABS_VID &&
            (cur->product_id == KFMT_PID || cur->product_id == CP2112_PID)) {

            bool serialMatch =
                (serial == nullptr) ||
                (cur->serial_number &&
                 wcscmp(cur->serial_number, (const wchar_t *)serial) == 0);

            if (serialMatch) {
                h = hid_open_path(cur->path);
                if (h) {
                    LogInfo(VB_PLUGIN, "CP2112: Device opened successfully\n");
                    LogInfo(VB_PLUGIN, "Manufacturer: %ls\n", cur->manufacturer_string);
                    LogInfo(VB_PLUGIN, "Product: %ls\n", cur->product_string);
                    LogInfo(VB_PLUGIN, "Serial: %ls\n", cur->serial_number);
                    found = true;
                    break;
                }
            }
        }
        cur = cur->next;
    }

    hid_free_enumeration(devs);

    if (found) {
        configureGPIO();
        configureSMB();
        initialized = true;
    } else {
        LogErr(VB_PLUGIN, "CP2112: Device not found or failed to open\n");
        if (h) {
            hid_close(h);
            h = nullptr;
        }
    }
}

CP2112::~CP2112() {
    if (h) {
        hid_close(h);
        h = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool CP2112::init() {
    return initialized && h != nullptr;
}

// ---------------------------------------------------------------------------
// GPIO / SMBus Configuration
// ---------------------------------------------------------------------------

void CP2112::configureGPIO() {
    // Initial LED blink
    std::vector<uint8_t> cmd = {CMD_GPIO_SET, 0x00, 0xFF};
    sendHIDCommand(cmd);

    for (int i = 0; i < 3; i++) {
        cmd = {CMD_GPIO_SET, 0x00, 0xFF};
        sendHIDCommand(cmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        cmd = {CMD_GPIO_SET, 0xFF, 0xFF};
        sendHIDCommand(cmd);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Configure GPIO directions and modes
    uint8_t gpio_direction = 0x83;
    uint8_t gpio_pushpull  = 0xFF;
    uint8_t gpio_special   = 0xFF;
    uint8_t gpio_clockdiv  = 1;

    cmd = {CMD_GPIO_CONFIG, gpio_direction, gpio_pushpull,
           gpio_special, gpio_clockdiv};
    sendHIDCommand(cmd);

    LogDebug(VB_PLUGIN, "CP2112: GPIO configured\n");
}

void CP2112::configureSMB() {
    // SMBus configuration from AN495
    std::vector<uint8_t> cmd = {
        CMD_SMB_CONFIG,
        0x00, 0x01, 0x86, 0xA0,
        0x02, 0x00, 0x00, 0xFF,
        0x00, 0xFF, 0x01, 0x00, 0x0F
    };

    sendHIDCommand(cmd);
    LogDebug(VB_PLUGIN, "CP2112: SMB configured\n");
}

// ---------------------------------------------------------------------------
// HID Communication Helpers
// ---------------------------------------------------------------------------

void CP2112::sendHIDCommand(const std::vector<uint8_t> &data) {
    if (!h)
        return;
    hid_send_feature_report(h, data.data(), data.size());
}

void CP2112::sendFeatureReport(const std::vector<uint8_t> &data) {
    sendHIDCommand(data);
}

/*
 * readInputReport()
 *
 * Reads a HID INPUT REPORT from the CP2112.
 * This is how the CP2112 returns I2C status and data.
 *
 * - Uses hid_read_timeout()
 * - Returns an empty vector if no data is available yet
 * - Caller decides when a timeout becomes an error
 */
std::vector<uint8_t> CP2112::readInputReport(int length) {
    std::vector<uint8_t> result(length);
    if (!h)
        return {};

    int ret = hid_read_timeout(h, result.data(), length, 20);
    if (ret > 0) {
        result.resize(ret);
    } else {
        result.clear();
    }
    return result;
}

// ---------------------------------------------------------------------------
// I2C Byte Operations
// ---------------------------------------------------------------------------

void CP2112::writeByteData(uint8_t regAddr, uint8_t data) {
    if (!h)
        return;

    std::vector<uint8_t> cmd = {
        CMD_I2C_WRITE,
        static_cast<uint8_t>(i2cAddress << 1),
        0x02,
        regAddr,
        data
    };

    if (hid_write(h, cmd.data(), cmd.size()) < 0) {
        LogErr(VB_PLUGIN, "CP2112: Write byte HID error\n");
        handleI2CError();
        return;
    }

    LogDebug(VB_PLUGIN,
             "CP2112: Write byte to reg 0x%02X = 0x%02X\n",
             regAddr, data);
}

uint8_t CP2112::readByteData(uint8_t regAddr) {
    if (!h)
        return 0xFF;

    // Issue read request
    std::vector<uint8_t> cmd = {
        CMD_I2C_WRITE_READ,
        static_cast<uint8_t>(i2cAddress << 1),
        0x00,
        0x01,
        0x01,
        regAddr
    };

    if (hid_write(h, cmd.data(), cmd.size()) < 0) {
        LogErr(VB_PLUGIN, "CP2112: readByteData HID write error\n");
        handleI2CError();
        return 0xFF;
    }

    // Poll for status
    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> statusCmd = {CMD_I2C_STATUS, 0x01};
        if (hid_write(h, statusCmd.data(), statusCmd.size()) < 0) {
            LogErr(VB_PLUGIN, "CP2112: readByteData status HID write error\n");
            handleI2CError();
            return 0xFF;
        }

        std::vector<uint8_t> resp = readInputReport(7);
        if (resp.size() >= 3 &&
            resp[0] == CMD_I2C_RESPONSE &&
            resp[2] == 5) {

            // Data ready -> force read
            std::vector<uint8_t> readCmd = {CMD_I2C_READ_FORCE, 0x00, 0x01};
            if (hid_write(h, readCmd.data(), readCmd.size()) < 0) {
                LogErr(VB_PLUGIN, "CP2112: readByteData force HID write error\n");
                handleI2CError();
                return 0xFF;
            }

            resp = readInputReport(4);
            if (resp.size() >= 4) {
                LogDebug(VB_PLUGIN,
                         "CP2112: Read byte from reg 0x%02X = 0x%02X\n",
                         regAddr, resp[3]);
                return resp[3];
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    LogErr(VB_PLUGIN,
           "CP2112: Byte Data Read Error for register 0x%02X\n",
           regAddr);
    handleI2CError();
    return 0xFF;
}

// ---------------------------------------------------------------------------
// I2C Word Operations
// ---------------------------------------------------------------------------

void CP2112::writeWordData(uint8_t regAddr, uint16_t data) {
    if (!h)
        return;

    std::vector<uint8_t> cmd = {
        CMD_I2C_WRITE,
        static_cast<uint8_t>(i2cAddress << 1),
        0x03,
        regAddr,
        static_cast<uint8_t>(data >> 8),
        static_cast<uint8_t>(data & 0xFF)
    };

    if (hid_write(h, cmd.data(), cmd.size()) < 0) {
        LogErr(VB_PLUGIN, "CP2112: Write word HID error\n");
        handleI2CError();
        return;
    }

    LogDebug(VB_PLUGIN,
             "CP2112: Write word to reg 0x%02X = 0x%04X\n",
             regAddr, data);
}

uint16_t CP2112::readWordData(uint8_t regAddr) {
    if (!h)
        return 0xFFFF;

    // Issue read request
    std::vector<uint8_t> cmd = {
        CMD_I2C_WRITE_READ,
        static_cast<uint8_t>(i2cAddress << 1),
        0x00,
        0x02,
        0x01,
        regAddr
    };

    if (hid_write(h, cmd.data(), cmd.size()) < 0) {
        LogErr(VB_PLUGIN, "CP2112: readWordData HID write error\n");
        handleI2CError();
        return 0xFFFF;
    }

    // Force read
    std::vector<uint8_t> readCmd = {CMD_I2C_READ_FORCE, 0x00, 0x02};
    if (hid_write(h, readCmd.data(), readCmd.size()) < 0) {
        LogErr(VB_PLUGIN, "CP2112: readWordData force HID write error\n");
        handleI2CError();
        return 0xFFFF;
    }

    // Poll for data
    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> resp = readInputReport(10);
        if (resp.size() >= 5 &&
            resp[0] == CMD_I2C_DATA_RESPONSE &&
            resp[2] == 2) {

            uint16_t value =
                (static_cast<uint16_t>(resp[4]) << 8) | resp[3];

            LogDebug(VB_PLUGIN,
                     "CP2112: Read word from reg 0x%02X = 0x%04X\n",
                     regAddr, value);
            return value;
        }

        // Retry sequence
        if (hid_write(h, cmd.data(), cmd.size()) < 0 ||
            hid_write(h, readCmd.data(), readCmd.size()) < 0) {
            LogErr(VB_PLUGIN,
                   "CP2112: readWordData retry HID write error\n");
            handleI2CError();
            return 0xFFFF;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    LogErr(VB_PLUGIN,
           "CP2112: Word Read Error for register 0x%02X\n",
           regAddr);
    handleI2CError();
    return 0xFFFF;
}

// ---------------------------------------------------------------------------
// I2C Block Operations
// ---------------------------------------------------------------------------

void CP2112::writeBlockData(uint8_t regAddr,
                            const std::vector<uint8_t> &data) {
    if (!h || data.empty())
        return;

    // Build block: [regAddr, data...]
    std::vector<uint8_t> block;
    block.reserve(data.size() + 1);
    block.push_back(regAddr);
    block.insert(block.end(), data.begin(), data.end());

    writeI2CBlock(block);

    LogDebug(VB_PLUGIN,
             "CP2112: Write block of %zu bytes at reg 0x%02X\n",
             data.size(), regAddr);
}

std::vector<uint8_t> CP2112::readBlockData(uint8_t regAddr, uint8_t length) {
    std::vector<uint8_t> result;
    if (!h || length == 0)
        return result;

    // Write register address first
    std::vector<uint8_t> addr = {regAddr};
    writeI2CBlock(addr);

    // Then read block
    result = readI2CBlock(length);

    LogDebug(VB_PLUGIN,
             "CP2112: Read block of %zu bytes at reg 0x%02X\n",
             result.size(), regAddr);

    return result;
}

void CP2112::writeI2CBlock(const std::vector<uint8_t> &data) {
    if (!h || data.empty() || data.size() > 61) {
        LogErr(VB_PLUGIN,
               "CP2112: I2C block write too large or empty (max 61 bytes)\n");
        return;
    }

    std::vector<uint8_t> cmd = {
        CMD_I2C_WRITE,
        static_cast<uint8_t>(i2cAddress << 1),
        static_cast<uint8_t>(data.size())
    };

    cmd.insert(cmd.end(), data.begin(), data.end());

    if (hid_write(h, cmd.data(), cmd.size()) < 0) {
        LogErr(VB_PLUGIN, "CP2112: Write I2C block HID error\n");
        handleI2CError();
        return;
    }

    LogDebug(VB_PLUGIN,
             "CP2112: Write I2C block of %zu bytes\n",
             data.size());
}

std::vector<uint8_t> CP2112::readI2CBlock(uint8_t length) {
    std::vector<uint8_t> result;
    if (!h || length == 0)
        return result;

    // Issue read request
    std::vector<uint8_t> cmd = {
        CMD_I2C_READ,
        static_cast<uint8_t>(i2cAddress << 1),
        0x00,
        length
    };

    if (hid_write(h, cmd.data(), cmd.size()) < 0) {
        LogErr(VB_PLUGIN, "CP2112: readI2CBlock HID write error\n");
        handleI2CError();
        return result;
    }

    // Poll for status
    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> statusCmd = {CMD_I2C_STATUS, 0x01};
        if (hid_write(h, statusCmd.data(), statusCmd.size()) < 0) {
            LogErr(VB_PLUGIN, "CP2112: readI2CBlock status HID write error\n");
            handleI2CError();
            return result;
        }

        std::vector<uint8_t> resp = readInputReport(7);
        if (resp.size() >= 3 &&
            resp[0] == CMD_I2C_RESPONSE &&
            resp[2] == 5) {

            // Data ready -> force read
            std::vector<uint8_t> readCmd = {
                CMD_I2C_READ_FORCE,
                resp[5],
                resp[6]
            };

            if (hid_write(h, readCmd.data(), readCmd.size()) < 0) {
                LogErr(VB_PLUGIN,
                       "CP2112: readI2CBlock force HID write error\n");
                handleI2CError();
                return result;
            }

            resp = readInputReport(length + 3);
            if (resp.size() > 3) {
                result.assign(resp.begin() + 3, resp.end());

                LogDebug(VB_PLUGIN,
                         "CP2112: Read I2C block of %zu bytes\n",
                         result.size());
                return result;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    LogErr(VB_PLUGIN, "CP2112: I2C Block Read Error\n");
    return result;
}

// ---------------------------------------------------------------------------
// Error Handling
// ---------------------------------------------------------------------------

void CP2112::handleI2CError() {
    LogErr(VB_PLUGIN, "CP2112: I2C Error detected, resetting device\n");
    if (h) {
        std::vector<uint8_t> resetCmd = {CMD_RESET, 0x01};
        hid_send_feature_report(h, resetCmd.data(), resetCmd.size());
        std::this_thread::sleep_for(std::chrono::seconds(3));
        hid_close(h);
        h = nullptr;
    }
    // No throw here: errors are handled by return values instead of aborting
}
