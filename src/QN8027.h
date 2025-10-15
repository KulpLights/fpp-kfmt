#include <string>
#include "util/I2CUtils.h"


class QN8027 {
public:
    QN8027();
    ~QN8027();

    void reset();

    void printInfo();


    void sendStationName(const std::string &sn);
    void sendRadioText(const std::string &rt);

    void setStationCode(const std::string &sc);
    void setProgramType(uint8_t pty);

    void calibrate();
    void setPreemphasis(bool us);
    void setChannel(float freq);


    void startTransmit();
    void stopTransmit();
    void mute();
    void unmute();
    void setMonoAudio(bool mono = false);
    void scrambleAudio(bool scramble = false);
    void setClockSource(uint8_t Type);
    void setCrystalCurrent(float percentOfMax);
    void setCrystalFreq(uint8_t Freq);

    void setTxDigitalGain(uint8_t DGain);
    void setTxInputBufferGain(uint8_t IBGain);
    void setAudioInpImp(uint8_t impdInKOhms);
    void setTxFreqDeviation(uint8_t Fdev);
    void clearAudioPeak();
    void setTxPower(uint8_t setX);
    void RDS(uint8_t onOffCtrl);
    void setRDSFreqDeviation(uint8_t RDSFreqDev);
    void setTxPilotFreqDeviation(uint8_t PGain);

    float getChannel();

    void waitForIdle(int maxms);
private:
    I2CUtils i2c;
    void write1Byte(uint8_t regAddr, uint8_t data);
    void updateSYSTEM_REG();
    void waitForRDSSend();
    void sendRDS(uint8_t by0, uint8_t by1, uint8_t by2, uint8_t by3, uint8_t by4, uint8_t by5, uint8_t by6, uint8_t by7);

    float channel;
    uint16_t piCode;
    uint8_t ptyCode;

    union SystemReg {
        uint8_t byte; // Access the full 8-bit value
        struct {
            uint8_t channelHigh : 2; // 2-bit field for high bits of channel
            uint8_t rdsReady : 1;  
            uint8_t muteAudio : 1; 
            uint8_t monoAudio : 1; 
            uint8_t radioStatus : 1;
            uint8_t recalibrate : 1;
            uint8_t reset : 1;
        } fields;
    } systemReg;
    uint8_t chReg = 0;

    union StatusReg {
        uint8_t byte; // Access the full 8-bit value
        struct {
            uint8_t fsm : 3;  
            uint8_t rdsSentStatus : 1; 
            uint8_t audioPeak : 4; 
        } fields;
    } statusReg;


    union GPLTReg {
        uint8_t byte; // Access the full 8-bit value
        struct {
            uint8_t TxPilotFreqDeviation : 4;
            uint8_t PAAutoOffTime : 2;
            uint8_t privateMode : 1;
            uint8_t preEmphTime : 1;
        } fields;
    } gpltReg;

    union XTLReg {
        uint8_t byte; // Access the full 8-bit value
        struct {
            uint8_t currentControl : 6;
            uint8_t clockSource : 2;
        } fields;
    } xtlReg;

    union VGAReg {
        uint8_t byte; // Access the full 8-bit value
        struct {
            uint8_t LRInputImpdKOhm : 2;
            uint8_t TxDigitalGain : 2;
            uint8_t TxInputBufferGain : 3;
            uint8_t crystalFreqMHz : 1;
        } fields;
    } vgaReg;

    union RDSReg {
        uint8_t byte; // Access the full 8-bit value
        struct {
            uint8_t rdsFdev : 7;
            uint8_t rdsEnable : 1;
        } fields;
    } rdsReg;

    union PACReg {
        uint8_t byte; // Access the full 8-bit value
        struct {
            uint8_t paTarget : 7;
            uint8_t AudioPeakClear : 1;
        } fields;
    } pacReg;
};