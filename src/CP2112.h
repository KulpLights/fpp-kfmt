#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Forward declaration for HID device
struct hid_device_;
typedef struct hid_device_ hid_device;

class CP2112 {
public:
    CP2112();
    ~CP2112();

    // Initialize the device (returns true if successful)
    bool init();
    bool isOpen() const { return h != nullptr; }
    static bool adapterPresent();
    bool tryOpen(bool blinkLeds = true);
    void closeHandle();

    // The bridge last opened. Kept across a disconnect, so the board type is
    // still known while it is unplugged.
    bool hasOpened() const { return openedPid != 0; }
    bool isKFMT() const;
    // A K-FMT board's hardware revision, from the release version programmed
    // into the CP2112 (BCD, 0x0100 = 1.0). Boards built before the revision
    // was programmed read the factory 1.0; 0 when unknown or not a K-FMT.
    uint16_t boardRelease() const { return openedRelease; }

    // Write/Read operations matching I2CUtils interface
    void writeByteData(uint8_t regAddr, uint8_t data);
    uint8_t readByteData(uint8_t regAddr, bool resetOnFail = true);

    void writeWordData(uint8_t regAddr, uint16_t data);
    uint16_t readWordData(uint8_t regAddr);

    void writeBlockData(uint8_t regAddr, const std::vector<uint8_t>& data);
    std::vector<uint8_t> readBlockData(uint8_t regAddr, uint8_t length);

    void writeI2CBlock(const std::vector<uint8_t>& data);
    std::vector<uint8_t> readI2CBlock(uint8_t length);

    // HID communication methods
    void sendFeatureReport(const std::vector<uint8_t>& data);
    std::vector<uint8_t> readInputReport(int length);

    // Error handling
    void handleI2CError();

private:
    hid_device* h = nullptr;
    uint8_t i2cAddress = 0x2c;  // Default I2C address for QN8027
    bool initialized = false;
    uint16_t openedPid = 0;
    uint16_t openedRelease = 0;

    void readRelease();

    // Helper methods
    void sendHIDCommand(const std::vector<uint8_t>& data);
    void configureGPIO();
    void configureSMB();
    void pollTransferStatus();   // (declared but not used in current .cpp)
};

