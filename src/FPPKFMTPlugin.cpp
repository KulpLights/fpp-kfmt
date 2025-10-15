#include <fpp-pch.h>

#include <string>
#include <vector>
#include <queue>

#include <unistd.h>
#include <termios.h>

#include "mediadetails.h"
#include "common.h"
#include "settings.h"
#include "Plugin.h"
#include "log.h"
#include "commands/Commands.h"
#include "QN8027.h"
#include "Warnings.h"



class FPPKFMTPlugin : public FPPPlugins::Plugin, public FPPPlugins::PlaylistEventPlugin {
public:
    QN8027 qn8027;
    bool detected = false;
    

    volatile bool running = true;
    std::mutex lock;
    std::condition_variable condition;
    std::thread *sendThread = nullptr;
    std::queue<std::function<void()>> functions;

    uint32_t stationIdCycleTime = 5;
    uint32_t rdsCycleTime = 15;

    std::vector<std::string> stationIdStrings;
    int curStationIdString = 0;
    uint64_t nextStationTime = 0;

    std::array<std::string, 3> rdsStrings;
    int curRDSString = 0;
    uint64_t nextRDSTime = 0;

    FPPKFMTPlugin() : FPPPlugins::Plugin("fpp-kfmt", true), FPPPlugins::PlaylistEventPlugin() {
        setDefaultSettings();

        stationIdCycleTime = std::stoi(settings["StationIDTime"]);
        rdsCycleTime = std::stoi(settings["RDSCycleTime"]);

        detected = qn8027.detect();
        if (detected) {
            std::unique_lock<std::mutex> lk(lock);
            functions.emplace([this]() {
                initializeQN8027();
            });

            lk.unlock();
            stopAction();

            formatAndSendText(settings["StationID"], "", "", "", 0, 0);
            formatAndSendText(settings["RDS"], "", "", "", 0, 1);
            formatAndSendText(settings["RDS2"], "", "", "", 0, 2);
            formatAndSendText(settings["RDS3"], "", "", "", 0, 3);
        } else {
            WarningHolder::AddWarning("Could not detect QN8027 device.");
        }
        sendThread = new std::thread([this] () {this->run();});
        condition.notify_all();
    }

    virtual ~FPPKFMTPlugin() {
        if (detected) {
            functions.emplace([this]() {
                qn8027.stopTransmit();
            });
        }
        condition.notify_all();
        std::unique_lock<std::mutex> lk(lock);
        while (!functions.empty()) {
            lk.unlock();
            condition.notify_all();
            lk.lock();
        }
        running = false;
        lk.unlock();
        condition.notify_all();
        if (sendThread->joinable()) {
            condition.notify_all();
            sendThread->join();
        }
        delete sendThread;
    }
    virtual void settingChanged(const std::string& key, const std::string& value) override {
        printf("Setting changed %s = %s\n", key.c_str(), value.c_str());
        if (key == "IdleAction") {
            // keys that don't need to reset the radio
            return;
        } else if (key == "StationID") {
            formatAndSendText(settings["StationID"], "", "", "", 0, 0);
        } else if (key == "RDS" || key == "RDS2" || key == "RDS3") {
            // these will be sent on the next appropriate cycle
            return;
        } else if (key == "StationIDTime") {
            stationIdCycleTime = std::stoi(settings["StationIDTime"]);
        } else if (key == "RDSCycleTime") {
            rdsCycleTime = std::stoi(settings["RDSCycleTime"]);
        } else if (detected) {
            std::unique_lock<std::mutex> lk(lock);
            functions.emplace([this] () {
                initializeQN8027();
            });
            lk.unlock();
        }
    }

    void initializeQN8027() {
        if (!detected) {
            return;
        }
        printf("Initializing QN8027\n");
        qn8027.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        std::string freq = settings["Frequency"];
        float ffreq = std::stof(freq);

        std::string sc = settings["StationCode"];
        while (sc.length() < 4) {
            sc += "A";
        }
        uint8_t pt = std::stoi(settings["ProgramType"]);
        uint8_t pe = std::stoi(settings["Preemphasis"]);

        qn8027.setChannel(ffreq);
        qn8027.setPreemphasis(pe);
        qn8027.setStationCode(sc);
        qn8027.setProgramType(pt);

        uint8_t inputImpd = std::stoi(settings["InputImpedance"]);
        qn8027.setAudioInpImp(inputImpd);

        uint8_t ibg = std::stoi(settings["TXInputBufferGain"]);
        qn8027.setTxInputBufferGain(ibg);

        uint8_t dg = std::stoi(settings["TXDigitalGain"]);
        qn8027.setTxDigitalGain(dg);

        // need range of 20 to 75
        float tp = std::stof(settings["TransmitPower"]);
        tp /= 55.0f;
        tp += 20;
        qn8027.setTxPower(std::round(tp));


        qn8027.RDS(1); // Enable RDS

        qn8027.startTransmit();            
        qn8027.printInfo();
/*

        setIfNotFound("TXFreqDeviation", "129");
        setIfNotFound("RDSFreqDeviation", "10");
        setIfNotFound("TXPilotFreqDeviation", "9");
        void setTxFreqDeviation(uint8_t Fdev);
        void setRDSFreqDeviation(uint8_t RDSFreqDev);
        void setTxPilotFreqDeviation(uint8_t PGain);
*/

    }

    void run() {
        std::unique_lock<std::mutex> lk(lock);
        while (running) {
            uint64_t ct = GetTimeMS();
            if (ct > nextStationTime && curStationIdString < stationIdStrings.size()) {
                std::string s = stationIdStrings[curStationIdString];
                if (detected) {
                    functions.emplace([this, s]() {
                        qn8027.sendStationName(s);
                    });
                }

                curStationIdString++;
                if (curStationIdString >= stationIdStrings.size()) {
                    curStationIdString = 0;
                }
                nextStationTime = ct + stationIdCycleTime * 1000;
            }
            if (ct > nextRDSTime) {
                std::string s = rdsStrings[curRDSString];
                if (s == "") {
                    s = std::string(" ");
                }
                if (detected) {
                    functions.emplace([this, s]() {
                        qn8027.sendRadioText(s);
                    });
                }
                curRDSString++;
                if (curRDSString == 3 || rdsStrings[curRDSString] == "") {
                    curRDSString = 0;
                }
                nextRDSTime = ct + rdsCycleTime * 1000;
            }

            while (!functions.empty()) {
                std::string resp;
                auto f = functions.front();
                functions.pop();
                lk.unlock();
                f();
                lk.lock();
            }
            if (running && functions.empty()) {
                condition.wait_for(lk, std::chrono::seconds(1));
            }
        }
    }
    void startAction() {
        if (detected) {
            if (settings["IdleAction"] == "1") {
                std::unique_lock<std::mutex> lk(lock);
                functions.emplace([this]() {
                    qn8027.unmute();
                });
            } else if (settings["IdleAction"] == "2") {
                std::unique_lock<std::mutex> lk(lock);
                functions.emplace([this]() {
                    qn8027.startTransmit();
                });
            }
            condition.notify_all();
        }
    }
   
    void stopAction() {
        if (detected) {
            if (settings["IdleAction"] == "1") {
                std::unique_lock<std::mutex> lk(lock);
                functions.emplace([this]() {
                    qn8027.mute();
                });
            } else if (settings["IdleAction"] == "2") {
                std::unique_lock<std::mutex> lk(lock);
                functions.emplace([this]() {
                    qn8027.stopTransmit();
                });
            }
            condition.notify_all();
        }
    }
    
    static void padTo(std::string &s, int l) {
        size_t n = l - s.size();
        if (n) {
            s.append(n, ' ');
        }
    }

    void formatAndSendText(const std::string &text, const std::string &artist, const std::string &title, const std::string &album, int length, int location) {
        std::string output;
        
        int artistIdx = -1;
        int titleIdx = -1;
        int albumIdx = -1;

        for (int x = 0; x < text.length(); x++) {
            if (text[x] == '[') {
                if (artist == "" && title == "") {
                    while (text[x] != ']' && x < text.length()) {
                        x++;
                    }
                }
            } else if (text[x] == ']') {
                //nothing
            } else if (text[x] == '{') {
                const static std::string ARTIST = "{Artist}";
                const static std::string TITLE = "{Title}";
                const static std::string ALBUM = "{Album}";
                std::string subs = text.substr(x);
                if (subs.rfind(ARTIST) == 0) {
                    artistIdx = output.length();
                    x += ARTIST.length() - 1;
                    output += artist;
                } else if (subs.rfind(TITLE) == 0) {
                    titleIdx = output.length();
                    x += TITLE.length() - 1;
                    output += title;
                } else if (subs.rfind(ALBUM) == 0) {
                    titleIdx = output.length();
                    x += ALBUM.length() - 1;
                    output += album;
                } else {
                    output += text[x];
                }
            } else {
                output += text[x];
            }
        }
        if (location == 0) {
            LogDebug(VB_PLUGIN, "Setting RDS Station text to \"%s\"\n", output.c_str());
            std::vector<std::string> fragments;
            while (output.size()) {
                if (output.size() <= 8) {
                    padTo(output, 8);
                    fragments.push_back(output);
                    output.clear();
                } else {
                    std::string lft = output.substr(0, 8);
                    padTo(lft, 8);
                    output = output.substr(8);
                    fragments.push_back(lft);
                }
            }
            if (fragments.empty()) {
                std::string m = "        ";
                fragments.push_back(m);
            }
            if (detected) {
                std::unique_lock<std::mutex> lk(lock);
                stationIdStrings = fragments;
                curStationIdString = 0;
                lk.unlock();
                condition.notify_all();
            }
        } else {
            LogDebug(VB_PLUGIN, "Setting RDS %d text to \"%s\"\n", location, output.c_str());
            if (detected) {
                std::unique_lock<std::mutex> lk(lock);
                rdsStrings[location - 1] = output;
                curRDSString = 0;
                lk.unlock();
                condition.notify_all();
            }
        }
    }
    
    virtual void playlistCallback(const Json::Value &playlist, const std::string &action, const std::string &section, int item) {
        if (action == "stop") {
            formatAndSendText(settings["StationID"], "", "", "", 0, 0);
            formatAndSendText(settings["RDS"], "", "", "", 0, 1);
            formatAndSendText(settings["RDS2"], "", "", "", 0, 2);
            formatAndSendText(settings["RDS3"], "", "", "", 0, 3);
            nextRDSTime = 0;
            nextStationTime = 0;
        }
        if (action == "start") {
            startAction();
        } else if (action == "stop") {
            stopAction();
        }
        
    }
    virtual void mediaCallback(const Json::Value &playlist, const MediaDetails &mediaDetails) {
        std::string title = mediaDetails.title;
        std::string artist = mediaDetails.artist;
        std::string album = mediaDetails.album;
        int track = mediaDetails.track;
        int length = mediaDetails.length;
        
        std::string type = playlist["currentEntry"]["type"].asString();
        if (type != "both" && type != "media") {
            title = "";
            artist = "";
            album = "";
        }
        
        formatAndSendText(settings["StationID"], artist, title, album, length, 0);
        formatAndSendText(settings["RDS"], artist, title, album, length, 1);
        formatAndSendText(settings["RDS2"], artist, title, album, length, 2);
        formatAndSendText(settings["RDS3"], artist, title, album, length, 3);
        nextRDSTime = 0;
        nextStationTime = 0;
    }
    
    
    void setDefaultSettings() {
        setIfNotFound("Frequency", "87.9");
        setIfNotFound("IdleAction", "0");
        setIfNotFound("Preemphasis", "1");

        setIfNotFound("StationID", "Merry   Christ- mas", true);        
        setIfNotFound("RDS", "[{Artist} - {Title}]", true);
        setIfNotFound("RDS2", "", true);
        setIfNotFound("RDS3", "", true);
        setIfNotFound("RDSCycleTime", "15");
        setIfNotFound("StationIDTime", "5");
        setIfNotFound("ProgramType", "0");
        setIfNotFound("StationCode", "WFPP");

        setIfNotFound("TXInputBufferGain", "3");
        setIfNotFound("TXDigitalGain", "0");
        setIfNotFound("InputImpedance", "20");
        setIfNotFound("TransmitPower", "100");

        setIfNotFound("TXFreqDeviation", "129");
        setIfNotFound("RDSFreqDeviation", "10");
        setIfNotFound("TXPilotFreqDeviation", "9");
    }
    void setIfNotFound(const std::string &s, const std::string &v, bool emptyAllowed = false) {
        if (settings.find(s) == settings.end()) {
            settings[s] = v;
        } else if (!emptyAllowed && settings[s] == "") {
            settings[s] = v;
        }
        LogDebug(VB_PLUGIN, "Setting \"%s\": \"%s\"\n", s.c_str(), settings[s].c_str());
    }
};

extern "C" {
    FPPPlugins::Plugin *createPlugin() {
        return new FPPKFMTPlugin();
    }
}
