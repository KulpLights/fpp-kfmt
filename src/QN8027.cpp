#include "QN8027.h"

#include "common_mini.h"
#include "log.h"

#include <chrono>
#include <thread>

// ---------------------------------------------------------------------------
// Register Constants
// ---------------------------------------------------------------------------

constexpr uint8_t REG_SYSTEM = 0x00; // Sets device modes.
constexpr uint8_t REG_CH1    = 0x01; // Sets the channel frequency
constexpr uint8_t REG_GPLT   = 0x02; // Audio control and TX pilot freq.
constexpr uint8_t REG_XTL    = 0x03; // XCLK pin control
constexpr uint8_t REG_VGA    = 0x04; // TX impedance, crystal frequency
constexpr uint8_t REG_CID1   = 0x05; // Device ID
constexpr uint8_t REG_CID2   = 0x06; // Device ID
constexpr uint8_t REG_STATUS = 0x07; // Status register
constexpr uint8_t REG_RDSD0  = 0x08; // RDS data byte 0
constexpr uint8_t REG_RDSD1  = 0x09; // RDS data byte 1
constexpr uint8_t REG_RDSD2  = 0x0A; // RDS data byte 2
constexpr uint8_t REG_RDSD3  = 0x0B; // RDS data byte 3
constexpr uint8_t REG_RDSD4  = 0x0C; // RDS data byte 4
constexpr uint8_t REG_RDSD5  = 0x0D; // RDS data byte 5
constexpr uint8_t REG_RDSD6  = 0x0E; // RDS data byte 6
constexpr uint8_t REG_RDSD7  = 0x0F; // RDS data byte 7
constexpr uint8_t REG_PAC    = 0x10; // PA output power target control
constexpr uint8_t REG_FDEV   = 0x11; // TX frequency deviation control
constexpr uint8_t REG_RDS    = 0x12; // RDS deviation/mode
constexpr uint8_t REG_ANT    = 0x1E; // Antenna tuning control

// ---------------------------------------------------------------------------
// FSM Mapping
// ---------------------------------------------------------------------------

static const char* mapFSM(uint8_t fsm) {
    switch (fsm) {
        case 0: return "RESET";
        case 1: return "Calibrating";
        case 2: return "Idle";
        case 3: return "TX-RSTB";
        case 4: return "PA Calibration";
        case 5: return "Transmitting";
        case 6: return "PA Off";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

QN8027::QN8027() {
    // Try native Linux I2C first
    for (int bus = 1; bus < 10; bus++) {
        if (FileExists("/dev/i2c-" + std::to_string(bus))) {
            i2c = new I2CUtils(bus, 0x2c);
            if (detect()) {
                LogInfo(VB_PLUGIN, "QN8027 detected on native I2C bus %d\n", bus);
                channel = 87.9f;
                return;
            }
            delete i2c;
            i2c = nullptr;
        }
    }

    // Fallback: CP2112 HID-to-I2C bridge
    cp2112 = new CP2112();
    if (cp2112->init() && detect()) {
        LogInfo(VB_PLUGIN, "QN8027 detected via CP2112 HID bridge\n");
        channel = 87.9f;
        return;
    }

    LogErr(VB_PLUGIN, "QN8027: No I2C or CP2112 device detected\n");
    if (cp2112) {
        delete cp2112;
        cp2112 = nullptr;
    }
    channel = 87.9f;
}

QN8027::~QN8027() {
    if (i2c) {
        delete i2c;
        i2c = nullptr;
    }
    if (cp2112) {
        delete cp2112;
        cp2112 = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Backend-agnostic I2C helpers
// ---------------------------------------------------------------------------

void QN8027::write1Byte(uint8_t regAddr, uint8_t data) {
    if (i2c) {
        i2c->writeByteData(regAddr, data);
    } else if (cp2112) {
        cp2112->writeByteData(regAddr, data);
    }
}

uint8_t QN8027::read1Byte(uint8_t regAddr) {
    if (i2c) {
        return i2c->readByteData(regAddr);
    } else if (cp2112) {
        return cp2112->readByteData(regAddr);
    }
    return 0xFF;
}

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

bool QN8027::detect() {
    uint8_t cid1 = read1Byte(REG_CID1);
    uint8_t cid2 = read1Byte(REG_CID2);
    LogDebug(VB_PLUGIN, "CID1: %02X   CID2: %02X\n", cid1, cid2);
    return cid1 != 0 && cid1 != 0xFF && cid2 != 0 && cid2 != 0xFF;
}

// ---------------------------------------------------------------------------
// RDS core
// ---------------------------------------------------------------------------

void QN8027::sendRDS(uint8_t by0, uint8_t by1, uint8_t by2, uint8_t by3,
                     uint8_t by4, uint8_t by5, uint8_t by6, uint8_t by7) {
    statusReg.byte = read1Byte(REG_STATUS);

    write1Byte(REG_RDSD0, by0);
    write1Byte(REG_RDSD1, by1);
    write1Byte(REG_RDSD2, by2);
    write1Byte(REG_RDSD3, by3);
    write1Byte(REG_RDSD4, by4);
    write1Byte(REG_RDSD5, by5);
    write1Byte(REG_RDSD6, by6);
    write1Byte(REG_RDSD7, by7);

    if (systemReg.fields.rdsReady == 1) {
        systemReg.fields.rdsReady = 0;
    } else {
        systemReg.fields.rdsReady = 1;
    }
    updateSYSTEM_REG();
}

void QN8027::updateSYSTEM_REG() {
    write1Byte(REG_SYSTEM, systemReg.byte);
}

// ---------------------------------------------------------------------------
// Reset / calibration / idle
// ---------------------------------------------------------------------------

void QN8027::reset() {
    systemReg.byte = 0x00;
    chReg = 0;
    gpltReg.byte = 0xA5;
    xtlReg.byte = 0x10;
    vgaReg.byte = 0xB2;
    rdsReg.byte = 0x06;
    pacReg.byte = 0x7F;

    write1Byte(REG_SYSTEM, 0x80);
    waitForIdle(5);
    write1Byte(REG_SYSTEM, 0x00);
    waitForIdle(5);

    setClockSource(0x00);
    setCrystalFreq(12);
    setCrystalCurrent(30);
    setTxFreqDeviation(0x81);
    setTxPilotFreqDeviation(9);
    setTxPower(75);

    setTxInputBufferGain(0x03);
    setTxDigitalGain(0);
    setAudioInpImp(20);
    RDS(1);
    setRDSFreqDeviation(10);
    setChannel(channel);
}

void QN8027::waitForIdle(int maxms) {
    StatusReg sr1;
    int waited = 0;
    do {
        sr1.byte = read1Byte(REG_STATUS);
        if (sr1.fields.fsm == 2 || sr1.fields.fsm == 5) {
            // 2 is idle, 5 is carrier off
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        waited += 2;
    } while (waited < maxms);
}

void QN8027::calibrate() {
    StatusReg sr1;
    sr1.byte = read1Byte(REG_STATUS);

    systemReg.fields.recalibrate = 1;
    systemReg.fields.radioStatus = 1;
    updateSYSTEM_REG();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    int waited = 0;
    do {
        sr1.byte = read1Byte(REG_STATUS);
        if (sr1.fields.fsm == 2 || sr1.fields.fsm == 5 || sr1.fields.fsm == 0) {
            // 2 idle, 5 transmitting, 0 reset
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        waited += 1;
    } while (waited < 50);

    systemReg.fields.recalibrate = 0;
    updateSYSTEM_REG();
    waitForIdle(50);
}

// ---------------------------------------------------------------------------
// Audio / TX control
// ---------------------------------------------------------------------------

void QN8027::mute() {
    systemReg.fields.muteAudio = 1;
    updateSYSTEM_REG();
    waitForIdle(50);
}

void QN8027::unmute() {
    systemReg.fields.muteAudio = 0;
    updateSYSTEM_REG();
    waitForIdle(50);
}

void QN8027::setMonoAudio(bool mono) {
    systemReg.fields.monoAudio = mono ? 1 : 0;
    updateSYSTEM_REG();
    waitForIdle(50);
    rdsBuilder_.setStereo(!mono);
}

void QN8027::startTransmit() {
    calibrate();
    systemReg.fields.radioStatus = 1;
    updateSYSTEM_REG();
    waitForIdle(50);
}

void QN8027::stopTransmit() {
    systemReg.fields.radioStatus = 0;
    updateSYSTEM_REG();
    waitForIdle(50);
}

// ---------------------------------------------------------------------------
// Channel control
// ---------------------------------------------------------------------------

void QN8027::setChannel(float frequency) {
    channel = frequency;

    uint16_t frequencyB = ((frequency + 0.001f) * 100.0f - 7600.0f) / 5.0f;
    uint8_t frequencyH = frequencyB >> 8;
    systemReg.fields.channelHigh = frequencyH & 0x03;
    chReg = frequencyB & 0xFF;
    updateSYSTEM_REG();
    write1Byte(REG_CH1, chReg);
    waitForIdle(50);
}

float QN8027::getChannel() {
    uint8_t frequencyH = read1Byte(REG_SYSTEM) & 0x03;
    uint8_t frequencyL = read1Byte(REG_CH1);
    float freqCombine = (float)(((frequencyH << 8) | frequencyL) * 5 + 7600) / 100;
    return freqCombine;
}

// ---------------------------------------------------------------------------
// Audio configuration
// ---------------------------------------------------------------------------

void QN8027::scrambleAudio(bool scramble) {
    gpltReg.fields.privateMode = scramble ? 1 : 0;
    write1Byte(REG_GPLT, gpltReg.byte);
}

void QN8027::setPreemphasis(bool us) {
    gpltReg.fields.preEmphTime = us ? 1 : 0;
    write1Byte(REG_GPLT, gpltReg.byte);
}

void QN8027::setClockSource(uint8_t Type) {
    xtlReg.fields.clockSource = Type;
    write1Byte(REG_XTL, xtlReg.byte);
}

void QN8027::setCrystalCurrent(float percentOfMax) {  // current between 0 to 400 uA
    uint8_t CrystalCurrentuA = (uint8_t)((percentOfMax * 64) / 100);
    xtlReg.fields.currentControl = CrystalCurrentuA;
    write1Byte(REG_XTL, xtlReg.byte);
}

void QN8027::setTxPilotFreqDeviation(uint8_t PGain) {
    gpltReg.fields.TxPilotFreqDeviation = PGain;
    write1Byte(REG_GPLT, gpltReg.byte);
}

void QN8027::setCrystalFreq(uint8_t Freq) {
    if (Freq == 24) {
        vgaReg.fields.crystalFreqMHz = 1;
    } else {
        vgaReg.fields.crystalFreqMHz = 0;
    }
    write1Byte(REG_VGA, vgaReg.byte);
}

void QN8027::setTxInputBufferGain(uint8_t IBGain) {
    if (IBGain > 5) {
        IBGain = 5;
    }
    vgaReg.fields.TxInputBufferGain = IBGain;
    write1Byte(REG_VGA, vgaReg.byte);
}

void QN8027::setTxDigitalGain(uint8_t DGain) {
    if (DGain > 2) {
        DGain = 2;
    }
    vgaReg.fields.TxDigitalGain = DGain;
    write1Byte(REG_VGA, vgaReg.byte);
}

void QN8027::setAudioInpImp(uint8_t impdInKOhms) {
    switch (impdInKOhms) {
        case 5:
            vgaReg.fields.LRInputImpdKOhm = 0;
            break;
        case 10:
            vgaReg.fields.LRInputImpdKOhm = 1;
            break;
        case 20:
            vgaReg.fields.LRInputImpdKOhm = 2;
            break;
        case 40:
            vgaReg.fields.LRInputImpdKOhm = 3;
            break;
        default:
            vgaReg.fields.LRInputImpdKOhm = 2;
            break;
    }
    write1Byte(REG_VGA, vgaReg.byte);
}

void QN8027::setTxFreqDeviation(uint8_t Fdev) {
    write1Byte(REG_FDEV, Fdev);
}

void QN8027::RDS(uint8_t onOffCtrl) {
    rdsReg.fields.rdsEnable = (onOffCtrl) ? 1 : 0;
    write1Byte(REG_RDS, rdsReg.byte);
}

void QN8027::setRDSFreqDeviation(uint8_t RDSFreqDev) {
    rdsReg.fields.rdsFdev = RDSFreqDev;
    write1Byte(REG_RDS, rdsReg.byte);
}

void QN8027::clearAudioPeak() {
    pacReg.fields.AudioPeakClear = !pacReg.fields.AudioPeakClear;
    write1Byte(REG_PAC, pacReg.byte);
}

void QN8027::setTxPower(uint8_t setX) {
    pacReg.fields.paTarget = setX & 0x7F;
    write1Byte(REG_PAC, pacReg.byte);
}

// ---------------------------------------------------------------------------
// Debug info
// ---------------------------------------------------------------------------

void QN8027::printInfo() {
    statusReg.byte = read1Byte(REG_STATUS);

    LogInfo(VB_PLUGIN, "Status: %02X   \n", statusReg.byte);
    LogInfo(VB_PLUGIN, "  Channel: %0.2f MHz\n", getChannel());
    LogInfo(VB_PLUGIN, "  Audio Peak: %d\n", statusReg.fields.audioPeak);
    LogInfo(VB_PLUGIN, "  FSM: %1X  %s\n", statusReg.fields.fsm, mapFSM(statusReg.fields.fsm));
    LogInfo(VB_PLUGIN, "  RDS Sent Status: %d\n", statusReg.fields.rdsSentStatus);
    uint8_t ant = read1Byte(REG_ANT);
    LogInfo(VB_PLUGIN, "  Antenna Tuning: %02X\n", ant);
}

// ---------------------------------------------------------------------------
// Station code / PTY
// ---------------------------------------------------------------------------

void QN8027::setStationCode(const std::string &sc) {
    if (sc.length() < 4) {
        LogInfo(VB_PLUGIN, "Station Code too short, must be 4 characters\n");
        return;
    }
    rdsBuilder_.setStationCode(sc);
}

void QN8027::setProgramType(uint8_t pty) {
    rdsBuilder_.setProgramType(pty);
}

void QN8027::startNewItem() {
    rdsBuilder_.toggleRTPlus();
}

// ---------------------------------------------------------------------------
// RDS send timing
// ---------------------------------------------------------------------------

const uint8_t RDS_SEND_DELAY = 5;

void QN8027::waitForRDSSend() {
    StatusReg status = statusReg;
    uint8_t timeout = 0;
    do {
        status.byte = read1Byte(REG_STATUS);
        if (timeout++ > (100 / RDS_SEND_DELAY)) {  // Allow up to 100ms RDS send time.
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RDS_SEND_DELAY));
    } while (status.fields.rdsSentStatus == statusReg.fields.rdsSentStatus); // Wait for toggle.
    statusReg = status;
}

// ---------------------------------------------------------------------------
// Station Name (PS)
// ---------------------------------------------------------------------------

void QN8027::sendStationName(const std::string &sn) {
    LogDebug(VB_PLUGIN, "Sending RDS Station ID: \"%s\"\n", sn.c_str());
    for (auto &p : rdsBuilder_.buildPSPackets(sn)) {
        sendRDS(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        waitForRDSSend();
    }
}

// ---------------------------------------------------------------------------
// Radio Text (RT)
// ---------------------------------------------------------------------------

void QN8027::sendRadioText(const std::string &rt) {
    LogDebug(VB_PLUGIN, "Sending RDS Radio Text: \"%s\"\n", rt.c_str());
    for (auto &p : rdsBuilder_.buildRTPackets(rt)) {
        sendRDS(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        waitForRDSSend();
    }
}

// ---------------------------------------------------------------------------
// RT+ segments
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Station RT+
// ---------------------------------------------------------------------------

void QN8027::sendStationRadioTextPlus(const std::string &stationName,
                                      const std::string &homepage) {
    for (auto &p : rdsBuilder_.buildStationRTPlusSequence(stationName, homepage)) {
        sendRDS(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        waitForRDSSend();
    }
}

// ---------------------------------------------------------------------------
// Item RT+ (artist/title/album)
// ---------------------------------------------------------------------------

void QN8027::sendItemRadioTextPlus(const std::string &artist,
                                   const std::string &title,
                                   const std::string &album) {
    for (auto &p : rdsBuilder_.buildItemRTPlusSequence(artist, title, album)) {
        sendRDS(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        waitForRDSSend();
    }
}

// ---------------------------------------------------------------------------
// Disable RT+
// ---------------------------------------------------------------------------

void QN8027::disableRadioTextPlus() {
    LogDebug(VB_PLUGIN, "Disabling RDS RT+\n");
    for (auto &p : rdsBuilder_.buildRTPlusDisableSequence()) {
        sendRDS(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        waitForRDSSend();
    }
}
