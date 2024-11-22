#include <string>
#include "util/I2CUtils.h"


class QN8007 {
public:
    QN8007();
    ~QN8007();

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

private:
    I2CUtils i2c;

    uint8_t sysReg1 = 0x01;
    uint8_t sysReg2 = 0x08;

    uint8_t anaCtlReg1 = 0x2B;
    uint8_t vgaReg = 0x60;

    uint8_t chReg = 0x00;
    uint8_t chStepReg = 0x00;
};