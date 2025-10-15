
#include "QN8027.h" 

#include <chrono>
#include <thread>
#include <cstring>

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


QN8027::QN8027() :  i2c(I2CBus, 0x2c) {
    channel = 87.9f;
    piCode = 0;
    ptyCode = 0;
    uint8_t cid1 = i2c.readByteData(REG_CID1);
    uint8_t cid2 = i2c.readByteData(REG_CID2);

    printf("CID1: %02X   CID2: %02X\n", cid1, cid2);

    reset();
}

QN8027::~QN8027() {

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
    printf("Reset\n");
    systemReg.byte = 0x00;
    chReg = 0;
    gpltReg.byte = 0xA5;
    xtlReg.byte = 0x10;
    vgaReg.byte = 0xB2;
    rdsReg.byte = 0x06;
    pacReg.byte = 0x7F;

    write1Byte(REG_SYSTEM,0x80);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
	write1Byte(REG_SYSTEM,0x00); 

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
    printf("Calibrate\n");
    write1Byte(REG_SYSTEM,0x40);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
	write1Byte(REG_SYSTEM, systemReg.byte);
}

void QN8027::mute() {
    printf("Mute\n");
    systemReg.fields.muteAudio = 1;
    updateSYSTEM_REG();
}
void QN8027::unmute() {
    printf("Unmute\n");
    systemReg.fields.muteAudio = 0;
    updateSYSTEM_REG();
}
void QN8027::setMonoAudio(bool mono) {
    printf("Set Mono Audio: %d\n", mono);
    systemReg.fields.monoAudio = mono ? 1 : 0;
    updateSYSTEM_REG();
}

void QN8027::startTransmit() {
    printf("Start Transmit\n");
    systemReg.fields.radioStatus = 1;
    updateSYSTEM_REG();
    waitForIdle(50);
    calibrate();
    waitForIdle(50);
}
void QN8027::stopTransmit() {
    printInfo();
    printf("Stop Transmit\n");
    systemReg.fields.radioStatus = 0;
    updateSYSTEM_REG();
}

void QN8027::setChannel(float frequency) {
    printf("Set Channel: %0.2f\n", frequency);
    channel = frequency;

    uint16_t frequencyB = ((frequency + 0.001f) * 100.0f - 7600.0f) / 5.0f;
	uint8_t frequencyH = frequencyB >> 8;
	systemReg.fields.channelHigh = frequencyH & 0x03;
	chReg = frequencyB & 0XFF;
	write1Byte(REG_SYSTEM, frequencyH);
	write1Byte(REG_CH1, chReg);
}
float QN8027::getChannel() {
    uint8_t frequencyH = i2c.readByteData(REG_SYSTEM) & 0x03;
	uint8_t frequencyL = i2c.readByteData(REG_CH1);
	float freqCombine = (float)(((frequencyH<<8) | frequencyL)*5+7600)/100;
	return freqCombine;
}

void QN8027::scrambleAudio(bool scramble) {
    printf("Set Scramble Audio: %d\n", scramble);
    gpltReg.fields.privateMode = scramble ? 1 : 0;
    write1Byte(REG_GPLT, gpltReg.byte);
}
void QN8027::setPreemphasis(bool us) {
    printf("Set Preemphasis: %d\n", us);
    gpltReg.fields.preEmphTime = us ? 1 : 0;
    write1Byte(REG_GPLT, gpltReg.byte);
}

/*
Type::meaning
0   :: Using XTAL 				between pin1 and pin2
1	:: Inject digital clock 	between pin1 and ground.
2	:: single end sin wave 		between pin1 and ground.
3	:: differential sin wave 	between pin1 and pin2
*/
void QN8027::setClockSource(uint8_t Type){
	xtlReg.fields.clockSource = Type;
    write1Byte(REG_XTL, xtlReg.byte);
}
/*
maximum current can be 400 uA
you can input percentage of 400 in parameter
for example setCrystalCurrent(50) means 50% of 400 = 200uA
default is 100 micro ampere.
*/
void QN8027::setCrystalCurrent(float percentOfMax) {  //current between 0 to 400 uA
	uint8_t CrystalCurrentuA = (uint8_t)((percentOfMax*64)/100);
    xtlReg.fields.currentControl = CrystalCurrentuA;
    write1Byte(REG_XTL, xtlReg.byte);
}


void QN8027::setTxPilotFreqDeviation(uint8_t PGain) {
    gpltReg.fields.TxPilotFreqDeviation = PGain;
    write1Byte(REG_GPLT, gpltReg.byte);
}


/*
if clock input source is XTAL then you can set which XTAL was used.
Freq::Meaning
12  :: 12 MHz
24  :: 24 MHz (default)
*/
void QN8027::setCrystalFreq(uint8_t Freq){
	if (Freq == 24) {
        vgaReg.fields.crystalFreqMHz = 1;
	} else { //if 12 or wrong value
		vgaReg.fields.crystalFreqMHz = 0;
	}
    write1Byte(REG_VGA, vgaReg.byte);
}

/*
set audio amplification in input buffer. actual gain is also depends on inputImpedence() functions parameter.
actual gain in dB = Gain = [(IBGain+1)*3] - [LRInputImpdKOhm*6]
you can set IBGain value from 0 to 5
default is 3
*/
void QN8027::setTxInputBufferGain(uint8_t IBGain) {
    if (IBGain > 5) {
        IBGain = 5;
    }
    vgaReg.fields.TxInputBufferGain = IBGain;
    write1Byte(REG_VGA, vgaReg.byte);
}

/* Digital Audio Amplification in decible.
DGain::Meaning
0    :: 0 dB (default)
1	 :: 1 dB
2	 :: 2 dB
*/
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

//---------------------------RDS_REG-------------------------------------------------------
/* set RDS channel ON or OFF */
void QN8027::RDS(uint8_t onOffCtrl) {
    rdsReg.fields.rdsEnable = (onOffCtrl) ? 1 : 0;
    write1Byte(REG_RDS, rdsReg.byte);
}

/* set bandwidth of RDS channel.
actual bandwidth in KHz = RDSFreqDev * 0.35
RDSFreqDev value can be set from 0 to 127
default is 6 which means 2.1 KHz
maximum bandwidth can be 44.45 KHz by setting RDSFreqDev value to 127
*/
void QN8027::setRDSFreqDeviation(uint8_t RDSFreqDev) {
    rdsReg.fields.rdsFdev = RDSFreqDev;
    write1Byte(REG_RDS, rdsReg.byte);
}




void QN8027::clearAudioPeak() {
    pacReg.fields.AudioPeakClear = !pacReg.fields.AudioPeakClear;
    write1Byte(REG_PAC, pacReg.byte);
}
/*
sets power of internal RF Power Amplifier.
you can set value from 20 to 75 (decimal).
actual power = 0.62 * setX + 71 dBu
maximum power can be 117.5 dBu
minimum power can be 83.4  dBu
default value of setX is 127 which makes no sense(not between 20 and 75). but we assumes that default is max.
although setX is 7 bit long, which means you can set values from 0 to 127. but they said not to be valid.
NOTE: Power will not be updated until RF carrier state is toggled Off-On (radioStatus bit).
*/

void QN8027::setTxPower(uint8_t setX) {
    pacReg.fields.paTarget = setX & 0x7F;
    write1Byte(REG_PAC, pacReg.byte);
}

void QN8027::printInfo() {
    statusReg.byte = i2c.readByteData(REG_STATUS);
    printf("Status: %02X   \n", statusReg.byte);
    printf("  Channel: %0.2f MHz\n", getChannel());

    uint8_t ant = i2c.readByteData(REG_ANT);
    printf("  Antenna Tuning: %02X\n", ant);
    printf("  Audio Peak: %d\n", statusReg.fields.audioPeak);
}


void QN8027::setStationCode(const std::string &sc) {
    if (sc.length() < 4) {
        printf("Station Code too short, must be 4 characters\n");
        return;
    }
    if (sc[0] != 'K' && sc[0] != 'W') {
        printf("Station Code must start with K or W\n");
        return;
    }
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
    //printf("Send Station Name: %s\n", sn.c_str());

    char char_array[9] = {0}; // PS Name is max 8 characters + null terminator.
    strncpy(char_array, sn.c_str(), sizeof(char_array));
    int str_len = strlen(char_array);
	int rds_len = str_len + (str_len % 2);    // Make it a multiple of 2.

	for(int i = 0; i < rds_len; i += 2) {
        uint8_t ptyHi = (ptyCode & 0x18) >> 3; //top 2 bits of PTY are in bottom 2 bits of byte 3
        uint8_t ptyLo = (ptyCode << 5) & 0xE0; //bottom 3 bits of PTY are in top 3 bits of byte 4
		sendRDS((piCode >> 8) * 0xFF, piCode & 0xFF, ptyHi, ptyLo | (0x08+(i/2)), 0xE0, 0xCD, char_array[i], char_array[i + 1]);
		waitForRDSSend();
	}

}
void QN8027::sendRadioText(const std::string &rt) {
    //printf("Send Radio Text: %s\n", rt.c_str());

    char char_array[65];
    strncpy(char_array, rt.c_str(), sizeof(char_array));
    int str_len = strlen(char_array);
	int rds_len = str_len + (str_len % 2);    // Make it a multiple of 2.

	for (int i = 0; i < rds_len; i += 4) {
        uint8_t ptyHi = (ptyCode & 0x18) >> 3; //top 2 bits of PTY are in bottom 2 bits of byte 3
        uint8_t ptyLo = (ptyCode << 5) & 0xE0; //bottom 3 bits of PTY are in top 3 bits of byte 4
		sendRDS((piCode >> 8) * 0xFF, piCode & 0xFF, 0x20 | ptyHi, ptyLo | (i/4),
                char_array[i], char_array[i+1], char_array[i+2], char_array[i+3]);
		waitForRDSSend();
	}
}


