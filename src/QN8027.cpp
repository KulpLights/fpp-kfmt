
#include "QN8027.h" 

#include <chrono>
#include <thread>
#include <cstring>

#include "log.h"

#ifdef PLATFORM_PI
constexpr int I2CBus = 1;
#else
constexpr int I2CBus = 2;
#endif

constexpr uint8_t REG_SYSTEM = 0x00; // Sets device modes.
constexpr uint8_t REG_CH1 = 0x01;     // Sets the channel frequency
constexpr uint8_t REG_GPLT = 0x02;     // Audio control and TX pilot freq.
constexpr uint8_t REG_XTL = 0x03;  // XCLK pin control
constexpr uint8_t REG_VGA = 0x04; // tx impedance, crystal frequency
constexpr uint8_t REG_CID1= 0x05; // Device ID
constexpr uint8_t REG_CID2= 0x06; // Device ID
constexpr uint8_t REG_STATUS = 0x07; // Status register
constexpr uint8_t REG_RDSD0 = 0x08; // RDS data byte 0
constexpr uint8_t REG_RDSD1 = 0x09; // RDS data byte 1
constexpr uint8_t REG_RDSD2 = 0x0A; // RDS data byte 2
constexpr uint8_t REG_RDSD3 = 0x0B; // RDS data byte 3
constexpr uint8_t REG_RDSD4 = 0x0C; // RDS data byte 4
constexpr uint8_t REG_RDSD5 = 0x0D; // RDS data byte 5  
constexpr uint8_t REG_RDSD6 = 0x0E; // RDS data byte 6
constexpr uint8_t REG_RDSD7 = 0x0F; // RDS data byte 7
constexpr uint8_t REG_PAC = 0x10; // PA output power target control
constexpr uint8_t REG_FDEV = 0x11; // TX frequency deviation control
constexpr uint8_t REG_RDS = 0x12; // Specify RDS frequency deviation, RDS mode selection
constexpr uint8_t REG_ANT = 0x1E; // Antenna tuning control

const char * mapFSM(uint8_t fsm) {
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



QN8027::QN8027() :  i2c(I2CBus, 0x2c) {
    channel = 87.9f;
    piCode = 0;
    ptyCode = 0;
}

QN8027::~QN8027() {

}

bool QN8027::detect() {
    uint8_t cid1 = i2c.readByteData(REG_CID1);
    uint8_t cid2 = i2c.readByteData(REG_CID2);
    LogDebug(VB_PLUGIN, "CID1: %02X   CID2: %02X\n", cid1, cid2);
    return cid1 != 0 && cid1 != 0xFF && cid2 != 0 && cid2 != 0xFF;
}

void QN8027::write1Byte(uint8_t regAddr, uint8_t data) {
    i2c.writeByteData(regAddr, data);
}

void QN8027::sendRDS(uint8_t by0, uint8_t by1, uint8_t by2, uint8_t by3, uint8_t by4, uint8_t by5, uint8_t by6, uint8_t by7) {    
    statusReg.byte = i2c.readByteData(REG_STATUS);
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
void QN8027::updateSYSTEM_REG(){
	write1Byte(REG_SYSTEM, systemReg.byte);
}

void QN8027::reset() {
    systemReg.byte = 0x00;
    chReg = 0;
    gpltReg.byte = 0xA5;
    xtlReg.byte = 0x10;
    vgaReg.byte = 0xB2;
    rdsReg.byte = 0x06;
    pacReg.byte = 0x7F;

    write1Byte(REG_SYSTEM,0x80);
    waitForIdle(5);
	write1Byte(REG_SYSTEM,0x00); 
    waitForIdle(5);

    setClockSource(0x00); // XTAL on pins 1 & 2.
    setCrystalFreq(12);
    setCrystalCurrent(30); // 30% of 400uA Max = 120uA.
    setTxFreqDeviation(0x81); // 75Khz, Total Broadcast channel Bandwidth
    setTxPilotFreqDeviation(9);
    setTxPower(75);    

    setTxInputBufferGain(0x03);
    setTxDigitalGain(0);
    setAudioInpImp(20);
    RDS(1); // Enable RDS
    setRDSFreqDeviation(10);
    setChannel(channel);
}

void QN8027::waitForIdle(int maxms) {
    StatusReg sr1;
    int waited = 0;
    do {
        sr1.byte = i2c.readByteData(REG_STATUS);
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
    sr1.byte = i2c.readByteData(REG_STATUS);

    systemReg.fields.recalibrate = 1;
    systemReg.fields.radioStatus = 1;
    updateSYSTEM_REG();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    int waited = 0;
    do {
        sr1.byte = i2c.readByteData(REG_STATUS);
        if (sr1.fields.fsm == 2 || sr1.fields.fsm == 5 || sr1.fields.fsm == 0) {
            // 2 is idle, 5 is transmitting, 0 is reset
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        waited += 1;
    } while (waited < 50);

    systemReg.fields.recalibrate = 0;
    updateSYSTEM_REG();
    waitForIdle(50);
}

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

void QN8027::setChannel(float frequency) {
    channel = frequency;

    uint16_t frequencyB = ((frequency + 0.001f) * 100.0f - 7600.0f) / 5.0f;
	uint8_t frequencyH = frequencyB >> 8;
	systemReg.fields.channelHigh = frequencyH & 0x03;
	chReg = frequencyB & 0XFF;
    updateSYSTEM_REG();
	write1Byte(REG_CH1, chReg);
    waitForIdle(50);
}
float QN8027::getChannel() {
    uint8_t frequencyH = i2c.readByteData(REG_SYSTEM) & 0x03;
	uint8_t frequencyL = i2c.readByteData(REG_CH1);
	float freqCombine = (float)(((frequencyH<<8) | frequencyL)*5+7600)/100;
	return freqCombine;
}

void QN8027::scrambleAudio(bool scramble) {
    gpltReg.fields.privateMode = scramble ? 1 : 0;
    write1Byte(REG_GPLT, gpltReg.byte);
}
void QN8027::setPreemphasis(bool us) {
    gpltReg.fields.preEmphTime = us ? 1 : 0;
    write1Byte(REG_GPLT, gpltReg.byte);
}

void QN8027::setClockSource(uint8_t Type){
	xtlReg.fields.clockSource = Type;
    write1Byte(REG_XTL, xtlReg.byte);
}
void QN8027::setCrystalCurrent(float percentOfMax) {  //current between 0 to 400 uA
	uint8_t CrystalCurrentuA = (uint8_t)((percentOfMax*64)/100);
    xtlReg.fields.currentControl = CrystalCurrentuA;
    write1Byte(REG_XTL, xtlReg.byte);
}


void QN8027::setTxPilotFreqDeviation(uint8_t PGain) {
    gpltReg.fields.TxPilotFreqDeviation = PGain;
    write1Byte(REG_GPLT, gpltReg.byte);
}

void QN8027::setCrystalFreq(uint8_t Freq){
	if (Freq == 24) {
        vgaReg.fields.crystalFreqMHz = 1;
	} else { //if 12 or wrong value
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
	switch (impdInKOhms){
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

void QN8027::setTxFreqDeviation(uint8_t Fdev){
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


void QN8027::printInfo() {
    statusReg.byte = i2c.readByteData(REG_STATUS);

    LogInfo(VB_PLUGIN, "Status: %02X   \n", statusReg.byte);
    LogInfo(VB_PLUGIN, "  Channel: %0.2f MHz\n", getChannel());
    LogInfo(VB_PLUGIN, "  Audio Peak: %d\n", statusReg.fields.audioPeak);
    LogInfo(VB_PLUGIN, "  FSM: %1X  %s\n", statusReg.fields.fsm, mapFSM(statusReg.fields.fsm));
    LogInfo(VB_PLUGIN, "  RDS Sent Status: %d\n", statusReg.fields.rdsSentStatus);
    uint8_t ant = i2c.readByteData(REG_ANT);
    LogInfo(VB_PLUGIN, "  Antenna Tuning: %02X\n", ant);
}

void QN8027::setStationCode(const std::string &sc) {
    if (sc.length() < 4) {
        LogInfo(VB_PLUGIN, "Station Code too short, must be 4 characters\n");
        return;
    }
    if (sc[0] != 'K' && sc[0] != 'W') {
        piCode = std::strtol(sc.c_str(), nullptr, 16) & 0xFFFF;
    } else {
        uint16_t v2 = sc[1] - 65;
        uint16_t v3 = sc[2] - 65;
        uint16_t v4 = sc[3] - 65;
        if (v2 < 0 || v2 > 26) v2 = 0;
        if (v3 < 0 || v3 > 26) v3 = 0;
        if (v4 < 0 || v4 > 26) v4 = 0;
        uint16_t v1 = (sc[0] == 'W') ? 21672 : 4096;
        v1 += v4;
        v1 += v3 * 26;
        v1 += v2 * 26 * 26;
        piCode = v1 & 0xFFFF;
    }
}
void QN8027::setProgramType(uint8_t pty) {
    ptyCode = pty & 0x1F;
}


const uint8_t RDS_SEND_DELAY = 5;
void QN8027::waitForRDSSend(){
    StatusReg status = statusReg;
    uint8_t timeout = 0;
    do {
        status.byte = i2c.readByteData(REG_STATUS);
        if(timeout++ > (100/RDS_SEND_DELAY)) {  // Allow up to 100mS RDS Send time.
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RDS_SEND_DELAY));
    } while (status.fields.rdsSentStatus == statusReg.fields.rdsSentStatus); // Wait for rdsSentStatus to toggle.
    statusReg = status;
}

void QN8027::sendStationName(const std::string &sn) {
    char char_array[9] = {0}; // PS Name is max 8 characters + null terminator.
    strncpy(char_array, sn.c_str(), sizeof(char_array));
    int str_len = strlen(char_array);
	int rds_len = str_len + (str_len % 2);    // Make it a multiple of 2.

    LogDebug(VB_PLUGIN, "Sending RDS Station ID: \"%s\"\n", sn.c_str());
	for(int i = 0; i < rds_len; i += 2) {
        uint8_t ptyHi = (ptyCode & 0x18) >> 3; //top 2 bits of PTY are in bottom 2 bits of byte 3
        uint8_t ptyLo = (ptyCode << 5) & 0xE0; //bottom 3 bits of PTY are in top 3 bits of byte 4
		sendRDS((piCode >> 8) * 0xFF, piCode & 0xFF, 
                 ptyHi, ptyLo | (0x08+(i/2)), 
                 0xE0, 0xCD, char_array[i], char_array[i + 1]);
		waitForRDSSend();
	}

}
void QN8027::sendRadioText(const std::string &rt) {
    LogDebug(VB_PLUGIN, "Sending RDS Radio Text: \"%s\"\n", rt.c_str());
    char char_array[65];
    memset(char_array, 0, sizeof(char_array));
    strncpy(char_array, rt.c_str(), sizeof(char_array));
    int str_len = strlen(char_array) + 1;
	int rds_len = str_len + (str_len % 2);    // Make it a multiple of 2.

	for (int i = 0; i < rds_len; i += 4) {
        uint8_t ptyHi = (ptyCode & 0x18) >> 3; //top 2 bits of PTY are in bottom 2 bits of byte 3
        uint8_t ptyLo = (ptyCode << 5) & 0xE0; //bottom 3 bits of PTY are in top 3 bits of byte 4
		sendRDS((piCode >> 8) * 0xFF, piCode & 0xFF, 
                0x20 | ptyHi, ptyLo | (i/4),
                char_array[i], char_array[i+1], char_array[i+2], char_array[i+3]);
		waitForRDSSend();
	}
}


void QN8027::sendRTPlusSegments(std::string &rt, std::vector<std::tuple<uint8_t, uint8_t, uint8_t>> &rtPlusSegments, bool enable) {
    uint8_t ptyHi = (ptyCode & 0x18) >> 3; //top 2 bits of PTY are in bottom 2 bits of byte 3
    uint8_t ptyLo = (ptyCode << 5) & 0xE0; //bottom 3 bits of PTY are in top 3 bits of byte 4

    while (!rt.empty()) {
        int maxSeg = rtPlusSegments.size() - 1;
        int maxIdx = rt.length();
        for (int x = 1; x < rtPlusSegments.size(); x++) {
            auto segment = rtPlusSegments[x];
            if (std::get<0>(segment) + std::get<1>(segment) > 63) {
                // segment won't fit in this send
                maxSeg = x - 1;
                maxIdx = std::get<0>(rtPlusSegments[maxSeg]) + std::get<1>(rtPlusSegments[maxSeg]);
            }
        }
        std::string rt2 = rt.substr(0, maxIdx);
        sendRadioText(rt2);

        int numSend = 2;
        for (int x = 0; x < maxSeg; x += numSend) {
            auto segment = rtPlusSegments[x];
            uint8_t idx = std::get<0>(segment);
            uint8_t len = std::get<1>(segment);
            uint8_t type = std::get<2>(segment);

            std::tuple<uint8_t, uint8_t, uint8_t> nextSegment = {0,0,0};
            numSend = 2;
            if (len < 32 && x + 1 <= maxSeg) {
                nextSegment = rtPlusSegments[x + 1];
                // first segment is less than 32 so can fit in second if required
                // if second segment is longer than 32 then swap them
                if (std::get<1>(nextSegment) >= 32) {
                    std::swap(segment, nextSegment);
                }                
            } else if (x + 1 > maxSeg) {
                // first segment is longer than 32 so must be in first position
                nextSegment = rtPlusSegments[x + 1];                
            } else {
                numSend = 1;
            }

            uint8_t idx1 = std::get<0>(segment) & 0x3F;
            uint8_t len1 = std::get<1>(segment) & 0x3F;
            uint8_t type1 = std::get<2>(segment) & 0x3F;

            uint8_t idx2 = std::get<0>(nextSegment) & 0x3F;
            uint8_t len2 = std::get<1>(nextSegment) & 0x3F;
            uint8_t type2 = std::get<2>(nextSegment) & 0x1F;

            uint8_t rb = rtPlusToggle ? 0x010 : 0x00; // RT+ toggle bit

            sendRDS((piCode >> 8) * 0xFF, piCode & 0xFF, 
                0x16 | ptyHi, ptyLo | rb | (type1 >> 3) & 0x07,
                ((type1 & 0x07) << 5) | (idx1 >> 1) , (idx1 & 0x01) << 7 | (len1 << 1) | (type2 & 0x20) ? 0x01 : 0x00,
                ((type2 & 0x1F) << 3) | (idx2 >> 3), ((idx2 << 5) & 0xE0 ) | len2);
            waitForRDSSend();
        }

        rt.erase(0, rt2.length());
        //remove the segments that were sent
        for (int i = 0; i <= maxSeg; i++) {
            rtPlusSegments.erase(rtPlusSegments.begin());
        }
        // adjust the remaining segments to account for sent text
        for (int i = 0; i < rtPlusSegments.size(); i++) {
            auto &segment = rtPlusSegments[i];
            std::get<0>(segment) -= rt2.length();
        }
    }

    if (enable) {
        uint8_t rb = rtPlusToggle ? 0x018 : 0x08; // RT+ toggle bit and "running bit"
        sendRDS((piCode >> 8) * 0xFF, piCode & 0xFF, 
            0x16 | ptyHi, ptyLo | rb,
            0x00, 0x00,
            0x00, 0x00);
    }
}

void QN8027::sendStationRadioTextPlus(const std::string &stationName,
                                const std::string &homepage) {
    uint8_t ptyHi = (ptyCode & 0x18) >> 3; //top 2 bits of PTY are in bottom 2 bits of byte 3
    uint8_t ptyLo = (ptyCode << 5) & 0xE0; //bottom 3 bits of PTY are in top 3 bits of byte 4
    sendRDS((piCode >> 8) * 0xFF, piCode & 0xFF, 
            0x30 | ptyHi, ptyLo | 0x16,
            0x00, 0x00,
            0x4B, 0xD7);
    waitForRDSSend();

    std::string rt;
    int stationNameLen = 1;
    int homepageLen = 1;

    int stationNameIdx = rt.length();
    if (!stationName.empty()) {
        stationNameLen = stationName.length();
        rt += stationName;
    } else {
        rt += " ";
    }
    int homepageIdx = rt.length();
    if (!homepage.empty()) {
        rt += " - ";
        homepageLen = homepage.length();
        rt += homepage;
    } else {
        rt += " ";
    }

    std::vector<std::tuple<uint8_t, uint8_t, uint8_t>> rtPlusSegments;
    rtPlusSegments.emplace_back(stationNameIdx, stationNameLen, RT_PLUS_STATIONNAME);
    rtPlusSegments.emplace_back(homepageIdx, homepageLen, RT_PLUS_HOMEPAGE);
    sendRTPlusSegments(rt, rtPlusSegments, false);                                    
}
void QN8027::sendItemRadioTextPlus(const std::string &artist,
                            const std::string &title,
                            const std::string &album) {
    rtPlusToggle = !rtPlusToggle;

    uint8_t ptyHi = (ptyCode & 0x18) >> 3; //top 2 bits of PTY are in bottom 2 bits of byte 3
    uint8_t ptyLo = (ptyCode << 5) & 0xE0; //bottom 3 bits of PTY are in top 3 bits of byte 4
    sendRDS((piCode >> 8) * 0xFF, piCode & 0xFF, 
            0x30 | ptyHi, ptyLo | 0x16,
            0x00, 0x00,
            0x4B, 0xD7);
    waitForRDSSend();

    std::string rt;
    int titleLen = 1;
    int albumLen = 1;
    int artistLen = 1;
    int stationNameLen = 1;
    int homepageLen = 1;

    int titleIdx = rt.length();
    if (!title.empty()) {
        titleLen = title.length();
        rt += title;
    } else {
        rt += " ";
    }
    int artistIdx = rt.length();
    if (!artist.empty()) {
        rt += " - ";
        artistIdx = rt.length();
        artistLen = artist.length();
        rt += artist;
    } else {
        rt += " ";
    }           
    int albumIdx = rt.length();
    if (albumIdx > 63) {
        artistLen = 63 - artistIdx + 1;
        rt.resize(63);
        albumLen = 1;
        albumIdx = rt.length();
        rt += " ";
    } else {
        if (!album.empty()) {
            if (rt.length() + album.length() + 3 > 64) {
                // not enough space to add album
                albumLen = 1;
                rt += " ";
            } else {
                rt += " - ";
                albumIdx = rt.length();
                albumLen = album.length();
                rt += album;
            }
        } else {
            rt += " ";
        }
    }

    rtPlusToggle = !rtPlusToggle;
    std::vector<std::tuple<uint8_t, uint8_t, uint8_t>> rtPlusSegments;
    rtPlusSegments.emplace_back(titleIdx, titleLen, RT_PLUS_TITLE);
    rtPlusSegments.emplace_back(artistIdx, artistLen, RT_PLUS_ARTIST);
    rtPlusSegments.emplace_back(albumIdx, albumLen, RT_PLUS_ALBUM);
    sendRTPlusSegments(rt, rtPlusSegments, !title.empty() || !album.empty() || !artist.empty());
}


void QN8027::disableRadioTextPlus() {
    LogDebug(VB_PLUGIN, "Disabling RDS RT+\n");

    uint8_t ptyHi = (ptyCode & 0x18) >> 3; //top 2 bits of PTY are in bottom 2 bits of byte 3
    uint8_t ptyLo = (ptyCode << 5) & 0xE0; //bottom 3 bits of PTY are in top 3 bits of byte 4
    sendRDS((piCode >> 8) * 0xFF, piCode & 0xFF, 
            0x30 | ptyHi, ptyLo | 0x16,
            0x00, 0x00,
            0x4B, 0xD7);
    waitForRDSSend();


    sendRDS((piCode >> 8) * 0xFF, piCode & 0xFF, 
        0x16 | ptyHi, ptyLo,  //item running bit is off, rest is irrelevant
        0x00, 0x00,
        0x00, 0x00);
}