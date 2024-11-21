#include <fpp-pch.h>


#include "QN8007.h" 

#ifdef PLATFORM_PI
constexpr int I2CBus = 1;
#else
constexpr int I2CBus = 2;
#endif

constexpr uint8_t REG_SYSTEM1 = 0x00; // Sets device modes.
constexpr uint8_t REG_SYSTEM2 = 0x01; // Sets device modes, resets.
constexpr uint8_t REG_DEV_ADD = 0x02; //  Sets device address.
constexpr uint8_t REG_ANACTL1 = 0x03; //  Analog control functions.
constexpr uint8_t REG_REG_VGA = 0x04; //  TX mode input impedance, crystal cap load setting.
constexpr uint8_t REG_CID1 = 0x05; //  Device ID numbers.
constexpr uint8_t REG_CID2 = 0x06; //  Device ID numbers.
constexpr uint8_t REG_I2S = 0x07; //  Sets I2S parameters.
constexpr uint8_t REG_CH = 0x08; //  Lower 8 bits of 10-bit channel index.
constexpr uint8_t REG_CH_START = 0x09; //  Lower 8 bits of 10-bit channel scan start channel index.
constexpr uint8_t REG_CH_STOP = 0x0A; //  Lower 8 bits of 10-bit channel scan stop channel index.
constexpr uint8_t REG_CH_STEP = 0x0B; //  Channel scan frequency step. Highest 2 bits of channel indexes.
constexpr uint8_t REG_PAC_TARGET = 0x0C; //  Output power calibration control.
constexpr uint8_t REG_TXAGC = 0x0D; // GAIN Sets TX parameters.
constexpr uint8_t REG_TX_FDEV = 0x0E; //  Specify total TX frequency deviation.
constexpr uint8_t REG_GAIN_TXPLT = 0x0F; //  Gain of TX pilot frequency deviation, I2S buffer clear.
constexpr uint8_t REG_RDSD0 = 0x10; //  RDS data byte 0.
constexpr uint8_t REG_RDSD1 = 0x11; //  RDS data byte 1.
constexpr uint8_t REG_RDSD2 = 0x12; //  RDS data byte 2.
constexpr uint8_t REG_RDSD3 = 0x13; //  RDS data byte 3.
constexpr uint8_t REG_RDSD4 = 0x14; //  RDS data byte 4.
constexpr uint8_t REG_RDSD5 = 0x15; //  RDS data byte 5.
constexpr uint8_t REG_RDSD6 = 0x16; //  RDS data byte 6.
constexpr uint8_t REG_RDSD7 = 0x17; //  RDS data byte 7.
constexpr uint8_t REG_RDSFDEV = 0x18; //  Specify RDS frequency deviation, RDS mode selection.
constexpr uint8_t REG_CCA = 0x19; //  Sets CCA parameters.
constexpr uint8_t REG_STATUS1 = 0x1A; //  Device status indicators.
constexpr uint8_t REG_STATUS2 = 0x1B; //  Device status indicators.
constexpr uint8_t REG_REG_XLT3 = 0x49; //  XCLK pin control.
constexpr uint8_t REG_PAC_CAL = 0x59; //  PA tuning cap calibration.
constexpr uint8_t REG_PAG_CAL = 0x5A; //  PA gain calibration.

QN8007::QN8007() :  i2c(I2CBus, 0x43) {
    uint8_t cid1 = i2c.readByteData(REG_CID1);
    uint8_t cid2 = i2c.readByteData(REG_CID2);

    printf("CID1: %02X   CID2: %02X\n", cid1, cid2);


    chReg = i2c.readByteData(REG_CH);
    chStepReg = i2c.readByteData(REG_CH_STEP);
}

QN8007::~QN8007() {

}

inline const char *mapCurrent(int i) {
    switch(i) {
        case 1: return "2mA";
        case 2: return "1mA";
        case 3: return "0.5mA";
    }
    return "4mA";
}

void QN8007::printInfo() {
    uint16_t ch1 = i2c.readByteData(REG_CH);
    ch1 &= 0xFF;
    uint8_t ch2 = i2c.readByteData(REG_CH_STEP);
    ch1 |= (ch2 << 8) & 0x300;
    float freq = 76.0 + ((float)ch1) * 0.05;
    printf("Frequency: %0.2f\n", freq);
    uint8_t cap = i2c.readByteData(REG_PAC_CAL);
    cap &= 0x3F;
    float f = cap;
    f *= 0.3;
    printf("Cap:  %0.1fpF\n", f);
    cap = i2c.readByteData(REG_PAG_CAL);
    uint8_t current = (cap >> 4) & 0x3;
    uint8_t gain = cap & 0x0F;
    float fgain = gain;
    fgain *= 1.5;
    fgain = 124.0f - fgain;
    printf("Gain: %0.1fdBuV\n", fgain);
    printf("Current: %s\n", mapCurrent(current));

}

void QN8007::calibrate() {
    uint8_t cap = i2c.readByteData(REG_PAC_CAL);
    cap &= 0x3F;
    cap |= 0x80;
    i2c.writeByteData(REG_PAC_CAL, cap);
    cap &= 0x3F;
    i2c.writeByteData(REG_PAC_CAL, cap);
}

void QN8007::startTransmit() {
    sysReg1 |= 0x40;
    i2c.writeByteData(REG_SYSTEM1, sysReg1);
}
void QN8007::stopTransmit() {
    sysReg1 &= ~0x40;
    i2c.writeByteData(REG_SYSTEM1, sysReg1);
}
void QN8007::setPreemphasis(bool us) {
    if (us) {
        sysReg2 |= 0x80;
    } else {
        sysReg2 &= 0x80;
    }
    i2c.writeByteData(REG_SYSTEM2, sysReg2);
}


void QN8007::setChannel(float freq) {
    // freq = 76 + CH * 0.05
    freq -= 76;
    freq /= 0.05;
    //bottom 8 bits in chReg
    uint32_t CH = std::round(freq);
    chReg = CH & 0xFF;

    // top 2 bits in chStepReg
    chStepReg &= 0xFC; 
    chStepReg |= (CH >> 8) & 0x03;

    i2c.writeByteData(REG_CH, chReg);
    i2c.writeByteData(REG_CH_STEP, chStepReg);
}

void QN8007::setStationCode(const std::string &sc) {
    
}
void QN8007::setProgramType(uint8_t pty) {

}

void QN8007::sendStationName(const std::string &sn) {
    
}
void QN8007::sendRadioText(const std::string &rt) {

}

void QN8007::mute() {
    
}
void QN8007::unmute() {

}
