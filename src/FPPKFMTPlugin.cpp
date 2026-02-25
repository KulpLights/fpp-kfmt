#include <fpp-pch.h>

#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

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

namespace {
    int safeStoi(const std::string &s, int defVal, const char *name) {
        try {
            if (s.empty()) return defVal;
            return std::stoi(s);
        } catch (const std::exception &e) {
            LogErr(VB_PLUGIN, "KFMT: stoi failed for %s=\"%s\": %s, using %d\n",
                   name, s.c_str(), e.what(), defVal);
            return defVal;
        }
    }

    float safeStof(const std::string &s, float defVal, const char *name) {
        try {
            if (s.empty()) return defVal;
            return std::stof(s);
        } catch (const std::exception &e) {
            LogErr(VB_PLUGIN, "KFMT: stof failed for %s=\"%s\": %s, using %0.2f\n",
                   name, s.c_str(), e.what(), defVal);
            return defVal;
        }
    }

    void padTo(std::string &s, int l) {
        size_t n = l - s.size();
        if (n) s.append(n, ' ');
    }
}

class FPPKFMTPlugin
    : public FPPPlugins::Plugin
    , public FPPPlugins::PlaylistEventPlugin {
public:
    QN8027 qn8027;
    bool   detected = false;

    std::atomic<bool> running{false};
    std::mutex                    lock;
    std::condition_variable       condition;
    std::thread                   sendThread;
    std::queue<std::function<void()>> functions;

    uint32_t stationIdCycleTime = 5;

    std::vector<std::string> stationIdStrings;
    int       curStationIdString = -1; // -1 = not yet started; advanced to 0 on first loop
    uint64_t  nextStationTime    = 0;  // when to advance to the next PS fragment
    uint64_t  nextPSTime         = 0;  // when to send the next PS packet (~400 ms)
    uint64_t  nextRDSTime        = 0;  // when to send the next RT/RT+ sequence

    std::string title;
    std::string artist;
    std::string album;
    int         track       = 0;
    int         mediaLength = 0;

    FPPKFMTPlugin()
        : FPPPlugins::Plugin("fpp-kfmt", true)
        , FPPPlugins::PlaylistEventPlugin() {
        LogDebug(VB_PLUGIN, "KFMT: constructor start\n");

        // Absolutely no exceptions escape this constructor.
        try {
            setDefaultSettings();

            stationIdCycleTime = safeStoi(settings["StationIDTime"], 5, "StationIDTime");

            detected = qn8027.detect();
            if (!detected) {
                WarningHolder::AddWarning("Could not detect QN8027 device.");
            }

            running = true;
            sendThread = std::thread([this]() {
                // Top‑level guard: nothing escapes this thread.
                try {
                    this->run();
                } catch (const std::exception &e) {
                    LogErr(VB_PLUGIN, "KFMT: run() top‑level exception: %s\n", e.what());
                } catch (...) {
                    LogErr(VB_PLUGIN, "KFMT: run() top‑level unknown exception\n");
                }
            });

            if (detected) {
                {
                    std::lock_guard<std::mutex> lk(lock);
                    functions.emplace([this]() {
                        initializeQN8027();
                    });
                }
                stopAction();
                formatAndSendText(settings["StationID"], 0);
                condition.notify_all();
            }

            LogInfo(VB_PLUGIN, "KFMT: constructor complete\n");
        } catch (const std::exception &e) {
            LogErr(VB_PLUGIN, "KFMT: constructor caught exception: %s\n", e.what());
            // Leave running=false so thread (if started) will exit.
            running = false;
            condition.notify_all();
        } catch (...) {
            LogErr(VB_PLUGIN, "KFMT: constructor caught unknown exception\n");
            running = false;
            condition.notify_all();
        }
    }

    virtual ~FPPKFMTPlugin() {
        LogDebug(VB_PLUGIN, "KFMT: destructor start\n");

        // Never enqueue new work after we decide to shut down.
        running = false;

        {
            std::lock_guard<std::mutex> lk(lock);
            if (detected) {
                functions.emplace([this]() {
                    try {
                        qn8027.stopTransmit();
                    } catch (const std::exception &e) {
                        LogErr(VB_PLUGIN, "KFMT: exception in stopTransmit(): %s\n", e.what());
                    } catch (...) {
                        LogErr(VB_PLUGIN, "KFMT: unknown exception in stopTransmit()\n");
                    }
                });
            }
        }
        condition.notify_all();

        if (sendThread.joinable()) {
            sendThread.join();
        }

        LogDebug(VB_PLUGIN, "KFMT: destructor complete\n");
    }

    virtual void settingChanged(const std::string &key,
                                const std::string &value) override {
        LogInfo(VB_PLUGIN, "KFMT: Setting changed %s = %s\n",
                key.c_str(), value.c_str());

        if (key == "IdleAction") {
            return;
        } else if (key == "StationID") {
            formatAndSendText(settings["StationID"], 0);
        } else if (key == "StationName" || key == "StationURL") {
            return;
        } else if (key == "StationIDTime") {
            stationIdCycleTime = safeStoi(settings["StationIDTime"], 5, "StationIDTime");
        } else if (detected) {
            std::lock_guard<std::mutex> lk(lock);
            functions.emplace([this]() {
                initializeQN8027();
            });
            condition.notify_all();
        }
    }

    void initializeQN8027() {
        if (!detected) return;

        LogInfo(VB_PLUGIN, "Initializing QN8027\n");

        try {
            qn8027.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

            float ffreq = safeStof(settings["Frequency"], 87.9f, "Frequency");

            std::string sc = settings["StationCode"];
            while (sc.length() < 4) sc += "A";

            uint8_t pt = static_cast<uint8_t>(
                safeStoi(settings["ProgramType"], 0, "ProgramType"));
            uint8_t pe = static_cast<uint8_t>(
                safeStoi(settings["Preemphasis"], 1, "Preemphasis"));

            qn8027.setChannel(ffreq);
            qn8027.setPreemphasis(pe);
            qn8027.setStationCode(sc);
            qn8027.setProgramType(pt);

            uint8_t inputImpd = static_cast<uint8_t>(
                safeStoi(settings["InputImpedance"], 20, "InputImpedance"));
            qn8027.setAudioInpImp(inputImpd);

            uint8_t ibg = static_cast<uint8_t>(
                safeStoi(settings["TXInputBufferGain"], 3, "TXInputBufferGain"));
            qn8027.setTxInputBufferGain(ibg);

            uint8_t dg = static_cast<uint8_t>(
                safeStoi(settings["TXDigitalGain"], 0, "TXDigitalGain"));
            qn8027.setTxDigitalGain(dg);

            float tp = safeStof(settings["TransmitPower"], 100.0f, "TransmitPower");
            tp /= 55.0f;
            tp += 20.0f;
            if (tp < 20.0f) tp = 20.0f;
            if (tp > 75.0f) tp = 75.0f;
            qn8027.setTxPower(static_cast<uint8_t>(std::round(tp)));

            qn8027.RDS(1);
            qn8027.startTransmit();
            qn8027.printInfo();
        } catch (const std::exception &e) {
            LogErr(VB_PLUGIN, "KFMT: initializeQN8027() exception: %s\n", e.what());
        } catch (...) {
            LogErr(VB_PLUGIN, "KFMT: initializeQN8027() unknown exception\n");
        }
    }

    void run() {
        std::unique_lock<std::mutex> lk(lock);

        while (running) {
            uint64_t ct = GetTimeMS();

            // Advance to the next PS fragment every stationIdCycleTime seconds.
            // curStationIdString starts at -1 so the first advance sets it to 0.
            if (ct > nextStationTime && !stationIdStrings.empty()) {
                curStationIdString++;
                if (curStationIdString >= (int)stationIdStrings.size()) {
                    curStationIdString = 0;
                }
                nextStationTime = ct + stationIdCycleTime * 1000;
            }

            // Send the current PS fragment every ~400 ms so receivers can lock on quickly.
            if (ct > nextPSTime && curStationIdString >= 0 &&
                    curStationIdString < (int)stationIdStrings.size()) {
                std::string s = stationIdStrings[curStationIdString];
                if (detected) {
                    functions.emplace([this, s]() {
                        try {
                            qn8027.sendStationName(s);
                        } catch (const std::exception &e) {
                            LogErr(VB_PLUGIN, "KFMT: exception in sendStationName: %s\n", e.what());
                        } catch (...) {
                            LogErr(VB_PLUGIN, "KFMT: unknown exception in sendStationName\n");
                        }
                    });
                }
                nextPSTime = ct + 400;
            }

            // Send RT/RT+ every 2 seconds (includes Group 2A RT so all receivers benefit).
            if (ct > nextRDSTime) {
                if (detected) {
                    functions.emplace([this]() {
                        try {
                            if (title.empty() && artist.empty() && album.empty()) {
                                qn8027.sendStationRadioTextPlus(settings["StationName"],
                                                                settings["StationURL"]);
                            } else {
                                qn8027.sendItemRadioTextPlus(artist, title, album);
                            }
                        } catch (const std::exception &e) {
                            LogErr(VB_PLUGIN, "KFMT: exception in RDS lambda: %s\n", e.what());
                        } catch (...) {
                            LogErr(VB_PLUGIN, "KFMT: unknown exception in RDS lambda\n");
                        }
                    });
                }
                nextRDSTime = ct + 2000;
            }

            while (!functions.empty()) {
                auto f = functions.front();
                functions.pop();
                lk.unlock();
                try {
                    f();
                } catch (const std::exception &e) {
                    LogErr(VB_PLUGIN, "KFMT: exception in queued function: %s\n", e.what());
                } catch (...) {
                    LogErr(VB_PLUGIN, "KFMT: unknown exception in queued function\n");
                }
                lk.lock();
            }

            if (running && functions.empty()) {
                condition.wait_for(lk, std::chrono::milliseconds(200));
            }
        }
    }

    void startAction() {
        if (!detected) return;

        std::lock_guard<std::mutex> lk(lock);
        if (settings["IdleAction"] == "1") {
            functions.emplace([this]() {
                try {
                    qn8027.unmute();
                } catch (...) {
                    LogErr(VB_PLUGIN, "KFMT: exception in unmute()\n");
                }
            });
        } else if (settings["IdleAction"] == "2") {
            functions.emplace([this]() {
                try {
                    qn8027.startTransmit();
                } catch (...) {
                    LogErr(VB_PLUGIN, "KFMT: exception in startTransmit()\n");
                }
            });
        }
        condition.notify_all();
    }

    void stopAction() {
        if (!detected) return;

        std::lock_guard<std::mutex> lk(lock);
        if (settings["IdleAction"] == "1") {
            functions.emplace([this]() {
                try {
                    qn8027.mute();
                } catch (...) {
                    LogErr(VB_PLUGIN, "KFMT: exception in mute()\n");
                }
            });
        } else if (settings["IdleAction"] == "2") {
            functions.emplace([this]() {
                try {
                    qn8027.stopTransmit();
                } catch (...) {
                    LogErr(VB_PLUGIN, "KFMT: exception in stopTransmit()\n");
                }
            });
        }
        condition.notify_all();
    }

    void formatAndSendText(const std::string &text, int location) {
        std::string output;

        for (int x = 0; x < (int)text.length(); x++) {
            if (text[x] == '[') {
                if (artist.empty() && title.empty()) {
                    while (x < (int)text.length() && text[x] != ']') x++;
                }
            } else if (text[x] == ']') {
                // ignore
            } else if (text[x] == '{') {
                static const std::string ARTIST = "{Artist}";
                static const std::string TITLE  = "{Title}";
                static const std::string ALBUM  = "{Album}";
                std::string subs = text.substr(x);
                if (subs.rfind(ARTIST, 0) == 0) {
                    x += ARTIST.length() - 1;
                    output += artist;
                } else if (subs.rfind(TITLE, 0) == 0) {
                    x += TITLE.length() - 1;
                    output += title;
                } else if (subs.rfind(ALBUM, 0) == 0) {
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
            while (!output.empty()) {
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
                fragments.emplace_back("        ");
            }

            if (detected) {
                std::lock_guard<std::mutex> lk(lock);
                stationIdStrings   = fragments;
                curStationIdString = -1; // run() will advance to 0 immediately
                nextStationTime    = 0;
                nextPSTime         = 0;
                condition.notify_all();
            }
        } else {
            LogDebug(VB_PLUGIN, "Setting RDS %d text to \"%s\"\n", location, output.c_str());
        }
    }

    virtual void playlistCallback(const Json::Value &playlist,
                                  const std::string &action,
                                  const std::string &section,
                                  int item) override {
        if (action == "start") {
            startAction();
        } else if (action == "stop") {
            {
                std::lock_guard<std::mutex> lk(lock);
                if (detected) {
                    functions.emplace([this]() {
                        try {
                            qn8027.disableRadioTextPlus();
                        } catch (...) {
                            LogErr(VB_PLUGIN, "KFMT: exception in disableRadioTextPlus()\n");
                        }
                    });
                }
            }

            artist.clear();
            title.clear();
            album.clear();
            track       = 0;
            mediaLength = 0;
            formatAndSendText(settings["StationID"], 0);
            nextRDSTime     = 0;
            nextStationTime = 0;

            stopAction();
        }
    }

    virtual void mediaCallback(const Json::Value &playlist,
                               const MediaDetails &mediaDetails) override {
        title       = mediaDetails.title;
        artist      = mediaDetails.artist;
        album       = mediaDetails.album;
        track       = mediaDetails.track;
        mediaLength = mediaDetails.length;

        std::string type = playlist["currentEntry"]["type"].asString();
        if (type != "both" && type != "media") {
            title.clear();
            artist.clear();
            album.clear();
            track       = 0;
            mediaLength = 0;
        }

        {
            std::lock_guard<std::mutex> lk(lock);
            if (detected) {
                functions.emplace([this]() {
                    try {
                        if (title.empty() && artist.empty() && album.empty()) {
                            qn8027.sendStationRadioTextPlus(settings["StationName"],
                                                            settings["StationURL"]);
                        } else {
                            qn8027.startNewItem();
                            qn8027.sendItemRadioTextPlus(artist, title, album);
                        }
                    } catch (...) {
                        LogErr(VB_PLUGIN, "KFMT: exception in mediaCallback RDS\n");
                    }
                });
            }
        }

        formatAndSendText(settings["StationID"], 0);
        nextRDSTime     = 0;
        nextStationTime = 0;
        condition.notify_all();
    }

    void setDefaultSettings() {
        setIfNotFound("Frequency", "87.9");
        setIfNotFound("IdleAction", "0");
        setIfNotFound("Preemphasis", "1");

        setIfNotFound("StationID", "Merry   Christ- mas", true);
        setIfNotFound("StationName", "", true);
        setIfNotFound("StationURL", "", true);
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

    void setIfNotFound(const std::string &s,
                       const std::string &v,
                       bool emptyAllowed = false) {
        if (settings.find(s) == settings.end()) {
            settings[s] = v;
        } else if (!emptyAllowed && settings[s].empty()) {
            settings[s] = v;
        }
        LogDebug(VB_PLUGIN, "Setting \"%s\": \"%s\"\n",
                 s.c_str(), settings[s].c_str());
    }
};

extern "C" {
    FPPPlugins::Plugin *createPlugin() {
        return new FPPKFMTPlugin();
    }
}
