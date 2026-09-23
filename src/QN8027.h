#include <string>
#include <vector>
#include <tuple>
#include "util/I2CUtils.h"
#include "CP2112.h"
#include "RDSPacketBuilder.h"

class QN8027 {
public:
    QN8027();
    ~QN8027();

    bool detect();
    bool adapterPresent() const;
    bool busOpen() const;
    bool tryReconnect();
    void reset();

    void printInfo();


    void sendStationName(const std::string &sn);
    void sendRadioText(const std::string &rt);

    void sendStationRadioTextPlus(const std::string &stationName,
                                  const std::string &homepage);
    void sendItemRadioTextPlus(const std::string &artist,
                               const std::string &title,
                               const std::string &album);
    void disableRadioTextPlus();

    // What was last actually put on the air. The QN8027 has no register that
    // reports the RDS it is transmitting - and reading one that does not exist
    // returns stale bus data rather than failing - so this is recorded where
    // it is sent instead of being asked for. No I2C, and no lock: the sender
    // thread is the only thing that calls the send methods above.
    const std::string &lastStationName() const { return lastStationName_; }
    const std::string &lastRadioText() const { return lastRadioText_; }


    void setStationCode(const std::string &sc);
    void setProgramType(uint8_t pty);

    // Advance the RT+ toggle bit – call once when the playing item changes.
    void startNewItem();

    void calibrate();
    void setPreemphasis(bool us);
    void setChannel(float freq);

    // Re-run the chip's adaptive antenna tuning. There is no way to read back
    // whether the result is a good match - see the note in retuneAntenna() -
    // so this reports only that the calibration was carried out.
    void retuneAntenna();

    struct RadioSnapshot {
        uint8_t fsm = 0;
        uint8_t audioPeak = 0;
        uint8_t ant = 0;
        uint8_t pac = 0;
        bool transmitting = false;
        bool muted = false;
        float channel = 0;
        // False when the status register did not read back. The other fields
        // are then stale rather than wrong, which is the trade an observer
        // read makes in exchange for never resetting the bus.
        bool valid = false;
    };
    RadioSnapshot snapshot();
    static const char* fsmName(uint8_t fsm);

    void startTransmit(bool recalibrate = true);
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
    void disablePAAutoOff();

    float getChannel();

    void waitForIdle(int maxms);
    void waitForCalComplete(int maxms);
private:
    std::string lastStationName_;
    std::string lastRadioText_;

    I2CUtils *i2c = nullptr;
    CP2112 *cp2112 = nullptr;

    void write1Byte(uint8_t regAddr, uint8_t data);
    uint8_t read1Byte(uint8_t regAddr);
    uint8_t read1ByteOptional(uint8_t regAddr);
    bool refreshStatus();
    void updateSYSTEM_REG();
    void waitForRDSSend();
    void sendRDS(uint8_t by0, uint8_t by1, uint8_t by2, uint8_t by3, uint8_t by4, uint8_t by5, uint8_t by6, uint8_t by7);

    float channel;
    RDSPacketBuilder rdsBuilder_;
    
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

