#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Forward declaration for HID device
struct hid_device_;
typedef struct hid_device_ hid_device;

class CP2112 {
public:
    CP2112(const char* serial = nullptr);
    ~CP2112();

    // Initialize the device (returns true if successful)
    bool init();

    // Write/Read operations matching I2CUtils interface
    void writeByteData(uint8_t regAddr, uint8_t data);
    uint8_t readByteData(uint8_t regAddr);

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

    // Helper methods
    void sendHIDCommand(const std::vector<uint8_t>& data);
    void configureGPIO();
    void configureSMB();
    void pollTransferStatus();   // (declared but not used in current .cpp)
};

