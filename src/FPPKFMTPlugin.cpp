#include <fpp-pch.h>

#include <string>
#include <string_view>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>

#include <unistd.h>
#include <termios.h>

#include "mediadetails.h"
#include "common.h"
#include "settings.h"
#include "Plugin.h"
#include "log.h"
#include "commands/Commands.h"
#include "CurlManager.h"
#include "Player.h"
#include "QN8027.h"
#include "Warnings.h"
#include "fpphttp.h"

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

    // TransmitterState's stored values, and the labels settings.json shows
    // for them. The command takes either, so its dropdown can offer the same
    // words as the settings page while a REST caller can still pass the
    // stored "2". An empty return means the string was neither.
    std::string transmitterStateValue(const std::string &s) {
        if (s == "0" || s == "Follow Idle Setting")  return "0";
        if (s == "1" || s == "Force On"  || s == "On")  return "1";
        if (s == "2" || s == "Force Off" || s == "Off") return "2";
        return "";
    }
    const char *transmitterStateName(const std::string &v) {
        if (v == "1") return "Force On";
        if (v == "2") return "Force Off";
        return "Follow Idle Setting";
    }
}

class FPPKFMTPlugin
    : public FPPPlugins::Plugin
    , public FPPPlugins::PlaylistEventPlugin
    , public FPPPlugins::APIProviderPlugin {
public:
    QN8027 qn8027;
    bool   detected = false;

    std::atomic<bool> running{false};
    std::mutex                    lock;
    std::condition_variable       condition;
    std::thread                   sendThread;
    std::queue<std::function<void()>> functions;

    uint32_t stationIdCycleTime = 5;
    bool     playlistActive = false;
    bool     carrierEnabled = false;
    std::string lastRetuneResult = "none";
    bool        lastRetuneOk = false;

    // Everything a queued radio job touches on the caller's behalf. Both the
    // waiter and the radio thread hold a shared_ptr to it, so the job stays
    // valid even if the waiter has already given up and returned.
    struct RadioJob {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        Json::Value result;
        bool ok = false;
    };

    std::vector<std::string> stationIdStrings;
    int       curStationIdString = -1; // -1 = not yet started; advanced to 0 on first loop
    uint64_t  nextStationTime    = 0;  // when to advance to the next PS fragment
    uint64_t  nextPSTime         = 0;  // when to send the next PS packet (~400 ms)
    uint64_t  nextRDSTime        = 0;  // when to send the next RT/RT+ sequence

    // After Hours Music Player streams over mpd while FPP is idle. Polling its
    // track title lets the RDS text follow the stream instead of sitting on
    // the static station text between playlists.
    bool      mpcAvailable   = false;
    uint64_t  nextMpcPoll    = 0;
    std::string mpcTitle;
    std::string mpcArtist;

    // Temporary overrides driven by FPP commands. Non-empty wins over the
    // configured value; an empty string is how a command restores the
    // configuration, so these are deliberately not "is set" flags.
    //
    // The transmitter state is deliberately NOT one of these. It is a saved
    // setting: forcing the carrier off overnight has to survive an fppd
    // restart at 3am, and it has to be visible on the settings page rather
    // than being invisible state the page has to apologise for.
    std::string stationIdOverride;
    std::string rdsTextOverride;

    // Which branch last produced the RadioText. Set where the choice is made,
    // so the page can say where the text came from rather than leaving someone
    // to work out why it is not what the settings page shows. Sender thread
    // only, like the sends themselves.
    const char *rdsSource = "nothing sent yet";

    // playlistActive is a hint, not the truth - see showIsPlaying().
    uint64_t playlistIdleSince = 0;
    std::vector<Command*> myCommands;

    std::string title;
    std::string artist;
    std::string album;
    int         track       = 0;
    int         mediaLength = 0;

    FPPKFMTPlugin()
        : FPPPlugins::Plugin("fpp-kfmt", true)
        , FPPPlugins::PlaylistEventPlugin()
        , FPPPlugins::APIProviderPlugin() {
        LogDebug(VB_PLUGIN, "KFMT: constructor start\n");

        // Absolutely no exceptions escape this constructor.
        try {
            setDefaultSettings();

            stationIdCycleTime = safeStoi(settings["StationIDTime"], 5, "StationIDTime");

            mpcAvailable = FileExists("/usr/bin/mpc") || FileExists("/bin/mpc") ||
                           FileExists("/usr/local/bin/mpc");

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
                // If a playlist is already running we must not apply idle
                // mute/stop — the "start" callback already happened before
                // this plugin was loaded. FPP will send "playing" (not
                // "start") on a restart of the current playlist.
                playlistActive = Player::INSTANCE.IsPlaying();
                queueApplyRadioState();
                formatAndSendText(effectiveStationId(), 0);
                condition.notify_all();
            }

            registerCommands();

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

    // Stop transmitting and JOIN the sender before FPP destroys this plugin. A
    // destructor is too late for that: the thread body is this plugin's code and
    // touches every member here, so it has to be finished while the object is
    // still whole - and finished before the library it lives in can be unmapped.
    // The last queued item is the stopTransmit(), so the join also guarantees
    // the carrier is off before the plugin goes. Everything here is synchronous,
    // so no readiness predicate is needed.
    virtual std::function<bool()> shutdown() override {
        unregisterApis();
        unregisterCommands();
        // Commands are gone, so nothing can start another request; this takes
        // back one a command may have left in flight. See applyTransmitterState().
        CurlManager::INSTANCE.cancelRequests(name);
        stopSending();
        // This plugin raised the warning, so it takes it back rather than
        // leaving a stale one on the UI for a plugin that is no longer loaded.
        WarningHolder::RemoveWarning("Could not detect QN8027 device.");
        return nullptr;
    }

    virtual ~FPPKFMTPlugin() {
        LogDebug(VB_PLUGIN, "KFMT: destructor start\n");
        // Backstop for a teardown that never called shutdown(). All three are
        // no-ops once shutdown() has run - unregisterCommands() empties the
        // list it iterates, so nothing is removed or deleted twice, and
        // cancelRequests() finds nothing left tagged with this name.
        unregisterCommands();
        CurlManager::INSTANCE.cancelRequests(name);
        stopSending(); // no-op if shutdown() already ran
        LogDebug(VB_PLUGIN, "KFMT: destructor complete\n");
    }

    // Idempotent, so shutdown() and the destructor can both call it.
    void stopSending() {
        if (!sendThread.joinable()) {
            return;
        }

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

        sendThread.join();
    }

    virtual void settingChanged(const std::string &key,
                                const std::string &value) override {
        LogInfo(VB_PLUGIN, "KFMT: Setting changed %s = %s\n",
                key.c_str(), value.c_str());

        if (key == "IdleAction" || key == "TransmitterState") {
            // Both feed the same decision, and applyRadioState() makes it
            // from scratch - so neither needs to know what the other is, and
            // neither has to guess whether a playlist is running.
            queueApplyRadioState();
            return;
        } else if (key == "StationID") {
            formatAndSendText(effectiveStationId(), 0);
        } else if (key == "StationName" || key == "StationURL") {
            return;
        } else if (key == "StationIDTime") {
            stationIdCycleTime = safeStoi(settings["StationIDTime"], 5, "StationIDTime");
        } else if (key == "AfterHoursRDS") {
            // Read at point of use; turning it off should also drop the title
            // it was showing rather than leaving the last stream track up.
            if (settings["AfterHoursRDS"] == "0" && !mpcTitle.empty()) {
                mpcTitle.clear();
                mpcArtist.clear();
                title.clear();
                artist.clear();
                formatAndSendText(effectiveStationId(), 0);
                nextRDSTime = 0;
            }
        } else if (key == "Frequency") {
            // Channel change retunes the antenna; a full bring-up is required.
            queueRadioWork([this]() { initializeQN8027(); });
            queueApplyRadioState();
        } else if (key == "TransmitPower" && detected) {
            queueRadioWork([this]() { applyTransmitPower(); });
            queueApplyRadioState();
        } else if (detected &&
                   (key == "Preemphasis" || key == "InputImpedance" ||
                    key == "TXDigitalGain" || key == "TXInputBufferGain" ||
                    key == "ProgramType" || key == "StationCode" ||
                    key == "RDSEnable")) {
            queueRadioWork([this]() { applyLiveRadioSetting(); });
        }
    }

    void queueApplyRadioState() {
        queueRadioWork([this]() { applyRadioState(); });
    }

    void queueRadioWork(std::function<void()> fn) {
        if (!running) {
            return;   // never queue work the sender will not come back for
        }
        std::lock_guard<std::mutex> lk(lock);
        functions.emplace(std::move(fn));
        condition.notify_all();
    }

    // Run I2C work on the sender thread and wait for it. HTTP handlers must not
    // talk to the chip themselves — that races the RDS loop.
    //
    // A timeout here does NOT cancel the job: it stays queued and the radio
    // thread runs it later, after this call has returned. So the job must not
    // reach anything owned by the caller's frame. That is why it writes its
    // output into the RadioJob it is handed rather than into a captured local,
    // and why `fn` must capture only `this` — never `[&]`. Getting this wrong
    // is what SIGSEGV'd fppd after Disable Carrier.
    //
    // Returns the job on success, or nullptr if it did not finish in time.
    std::shared_ptr<RadioJob> runOnRadioThread(
            const std::function<void(RadioJob &)> &fn, int timeoutMs) {
        if (!detected || !running) {
            return nullptr;
        }
        auto job = std::make_shared<RadioJob>();
        {
            std::lock_guard<std::mutex> lk(lock);
            functions.emplace([fn, job]() {
                try {
                    fn(*job);
                } catch (const std::exception &e) {
                    LogErr(VB_PLUGIN, "KFMT: exception in radio job: %s\n", e.what());
                } catch (...) {
                    LogErr(VB_PLUGIN, "KFMT: unknown exception in radio job\n");
                }
                {
                    std::lock_guard<std::mutex> g(job->mutex);
                    job->done = true;
                }
                job->cv.notify_one();
            });
        }
        condition.notify_all();
        std::unique_lock<std::mutex> ul(job->mutex);
        if (!job->cv.wait_for(ul, std::chrono::milliseconds(timeoutMs),
                              [&job]() { return job->done; })) {
            return nullptr;
        }
        return job;
    }

    void initializeQN8027() {
        if (!qn8027.busOpen()) {
            LogErr(VB_PLUGIN, "KFMT: initialize skipped, USB/I2C not open\n");
            return;
        }

        try {
            qn8027.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

            float ffreq = safeStof(settings["Frequency"], 87.9f, "Frequency");
            LogInfo(VB_PLUGIN, "Initializing QN8027 at %0.2f MHz\n", ffreq);

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

            // UI 1–100 -> PAC 20–75. The old code did ui/55+20, which mapped
            // the whole slider to PAC 20–22 (~1 dB), so Airspy barely moved.
            // Datasheet: Power(dBuV) = 0.62*PAC + 71, PAC valid 20–75.
            float ui = safeStof(settings["TransmitPower"], 50.0f, "TransmitPower");
            if (ui < 0.0f) ui = 0.0f;
            if (ui > 100.0f) ui = 100.0f;
            float tp = 20.0f + (ui / 100.0f) * 55.0f;
            uint8_t pac = static_cast<uint8_t>(std::round(tp));
            LogInfo(VB_PLUGIN, "KFMT: TransmitPower UI=%0.0f -> PAC=%u (~%0.1f dBuV)\n",
                    ui, pac, 0.62f * pac + 71.0f);
            qn8027.setTxPower(pac);

            qn8027.RDS(rdsEnabled() ? 1 : 0);
            qn8027.setMonoAudio(false);
            qn8027.unmute();
            qn8027.startTransmit(true);
            // RECAL can leave the chip on its reset channel (88.80). Program
            // the saved frequency again now that TX is up.
            qn8027.setChannel(ffreq);
            carrierEnabled = true;
            qn8027.printInfo();
        } catch (const std::exception &e) {
            LogErr(VB_PLUGIN, "KFMT: initializeQN8027() exception: %s\n", e.what());
        } catch (...) {
            LogErr(VB_PLUGIN, "KFMT: initializeQN8027() unknown exception\n");
        }
    }

    uint8_t mappedPaTarget() {
        float ui = safeStof(settings["TransmitPower"], 50.0f, "TransmitPower");
        if (ui < 0.0f) ui = 0.0f;
        if (ui > 100.0f) ui = 100.0f;
        return static_cast<uint8_t>(std::round(20.0f + (ui / 100.0f) * 55.0f));
    }

    void applyTransmitPower() {
        uint8_t pac = mappedPaTarget();
        LogInfo(VB_PLUGIN, "KFMT: TransmitPower UI=%s -> PAC=%u\n",
                settings["TransmitPower"].c_str(), pac);
        qn8027.setTxPower(pac);
        if (carrierEnabled) {
            qn8027.stopTransmit();
            qn8027.startTransmit(false);
            carrierEnabled = true;
        }
    }

    void applyLiveRadioSetting() {
        uint8_t pe = static_cast<uint8_t>(
            safeStoi(settings["Preemphasis"], 1, "Preemphasis"));
        qn8027.setPreemphasis(pe);
        uint8_t inputImpd = static_cast<uint8_t>(
            safeStoi(settings["InputImpedance"], 20, "InputImpedance"));
        qn8027.setAudioInpImp(inputImpd);
        uint8_t ibg = static_cast<uint8_t>(
            safeStoi(settings["TXInputBufferGain"], 3, "TXInputBufferGain"));
        qn8027.setTxInputBufferGain(ibg);
        uint8_t dg = static_cast<uint8_t>(
            safeStoi(settings["TXDigitalGain"], 0, "TXDigitalGain"));
        qn8027.setTxDigitalGain(dg);
        std::string sc = settings["StationCode"];
        while (sc.length() < 4) sc += "A";
        qn8027.setStationCode(sc);
        uint8_t pt = static_cast<uint8_t>(
            safeStoi(settings["ProgramType"], 0, "ProgramType"));
        qn8027.setProgramType(pt);
        qn8027.RDS(rdsEnabled() ? 1 : 0);
    }

    bool rdsEnabled() const {
        auto it = settings.find("RDSEnable");
        if (it == settings.end()) {
            return true;
        }
        return it->second != "0";
    }

    static constexpr const char *kDisconnectWarning = "K-FMT USB adapter disconnected.";
    static constexpr const char *kReconnectRestartWarning =
        "K-FMT USB adapter is connected but the transmitter is not responding.";
    // Retry forever, just not at full speed. A show controller is usually
    // unattended, so giving up permanently means the radio stays dark until
    // someone notices and restarts FPPD - whereas whatever is wrong (a brown-
    // out, a hub renumbering, a chip that needs a moment) may well clear on
    // its own. Warn after a few failures so the UI says something, then keep
    // trying on a slower cadence.
    static constexpr int kReconnectWarnAfter = 6;
    static constexpr uint64_t kReconnectFastMs = 5000;
    static constexpr uint64_t kReconnectSlowMs = 60000;
    uint64_t nextReconnectTry = 0;
    int reconnectFails = 0;

    uint64_t reconnectInterval() const {
        return reconnectFails < kReconnectWarnAfter ? kReconnectFastMs
                                                    : kReconnectSlowMs;
    }

    bool attemptReconnect() {
        if (!qn8027.adapterPresent()) {
            return false;
        }
        if (qn8027.tryReconnect()) {
            reconnectFails = 0;
            bringUpAfterReconnect();
            return true;
        }
        reconnectFails++;
        LogErr(VB_PLUGIN, "KFMT: reconnect attempt %d failed, retrying in %llus\n",
               reconnectFails, (unsigned long long)(reconnectInterval() / 1000));
        if (reconnectFails == kReconnectWarnAfter) {
            WarningHolder::AddWarning(kReconnectRestartWarning);
            LogErr(VB_PLUGIN, "KFMT: backing off to a slow reconnect retry\n");
        }
        return false;
    }

    void bringUpAfterReconnect() {
        LogInfo(VB_PLUGIN, "KFMT: USB adapter reconnected, initializing transmitter\n");
        WarningHolder::RemoveWarning(kDisconnectWarning);
        WarningHolder::RemoveWarning("Could not detect QN8027 device.");
        WarningHolder::RemoveWarning(kReconnectRestartWarning);
        detected = true;
        initializeQN8027();
        // Already on the sender thread (the run loop drops the queue lock
        // before calling attemptReconnect), so apply directly rather than
        // queueing behind whatever else is waiting.
        applyRadioState();
    }

    void markDisconnected(const char *link) {
        detected = false;
        carrierEnabled = false;
        reconnectFails = 0;
        WarningHolder::RemoveWarning(kReconnectRestartWarning);
        WarningHolder::AddWarning(kDisconnectWarning);
        LogInfo(VB_PLUGIN, "KFMT: %s\n", link);
    }

    Json::Value statusJson() {
        Json::Value root;
        root["playlistActive"] = playlistActive;
        root["carrierEnabled"] = carrierEnabled;
        root["lastRetune"] = lastRetuneResult;
        root["lastRetuneOk"] = lastRetuneOk;

        // Above the early returns below on purpose. An override is plugin
        // state, not chip state, and someone is most likely to have this page
        // open when the transmitter is unhappy - which is the worst moment for
        // the reason the radio is behaving oddly to vanish from the page.
        //
        // No locking: this runs on the sender thread via runOnRadioThread(),
        // which is the same thread the override commands write from.
        //
        // The idle override is sent as its label rather than its stored "1",
        // so the page does not need a second copy of the mapping.
        root["stationIdOverride"] = stationIdOverride;
        root["rdsTextOverride"]   = rdsTextOverride;

        // The page explains the carrier with the same string applyRadioState()
        // logged, computed the same way, so what it shows and what the radio
        // was told can never drift apart.
        const RadioTarget t = desiredRadioState();
        root["carrierWhy"] = t.why;
        auto tsIt = settings.find("TransmitterState");
        const std::string ts = (tsIt == settings.end() || tsIt->second.empty())
                                   ? std::string("0") : tsIt->second;
        root["transmitterState"]  = transmitterStateName(ts);
        root["transmitterForced"] = (ts == "1" || ts == "2");
        // The stored value as well as the label: a command can change this
        // setting behind the settings page's back, and the page needs the
        // option value to put its own field back in step.
        root["transmitterStateValue"] = ts;

        // What is actually going out, as recorded where it was sent. The chip
        // cannot be asked - it has no register for this - so nothing here
        // costs an I2C exchange. See QN8027::lastStationName().
        root["psOnAir"]   = qn8027.lastStationName();
        root["rtOnAir"]   = qn8027.lastRadioText();
        root["rdsSource"] = rdsSource;
        // Nothing is on the air without both of these, so the page can say
        // "not being sent" instead of showing text nobody can hear.
        root["rdsEnabled"] = rdsEnabled();
        {
            // The PS fragments are written by formatAndSendText() from the
            // callback and HTTP threads under this lock, so snapshot them
            // rather than reading them bare. Safe to take here: statusJson()
            // only ever runs from a queued job, and the run loop releases the
            // lock before invoking one.
            std::lock_guard<std::mutex> lk(lock);
            std::string full;
            for (const auto &f : stationIdStrings) {
                full += f;
            }
            root["psFullText"] = full;
            root["psScreen"]   = curStationIdString + 1;   // 1-based for display
            root["psScreens"]  = (int)stationIdStrings.size();
        }

        const bool adapter = qn8027.adapterPresent();
        root["adapterPresent"] = adapter;

        if (!adapter) {
            if (detected) {
                markDisconnected("USB adapter disconnected");
            }
            root["detected"] = false;
            root["link"] = "USB adapter disconnected";
            root["fsmName"] = "Disconnected";
            return root;
        }

        // Deliberately no detect() here. Polling it proved liveness the
        // expensive way: it reads the CID registers through the resetting
        // read, so one transient HID hiccup closed the bus, reported the chip
        // missing, and reconnected - and reconnecting means a full SWRST and
        // bring-up, measured at ~150 ms of dead carrier with RDS restarting.
        // A glitch that should have cost one bad register read became an
        // audible dropout, and only ever while someone had this page open.
        //
        // The RDS loop is already talking to the chip every 400 ms, and any
        // real failure there closes the handle via handleI2CError(), so
        // busOpen() is a sufficient and side-effect-free liveness signal.
        if (!detected) {
            attemptReconnect();
        } else if (!qn8027.busOpen()) {
            detected = false;
            attemptReconnect();
        }

        root["detected"] = detected;
        if (!detected) {
            root["link"] = "adapter connected, transmitter not responding, retrying";
            root["fsmName"] = "Reconnecting";
            root["reconnectFails"] = reconnectFails;
            return root;
        }

        auto s = qn8027.snapshot();
        // A failed observer read no longer resets the bus, so say the values
        // are stale instead of pretending the transmitter went away.
        root["link"] = s.valid ? "ok" : "stale read";
        root["stale"] = !s.valid;
        root["fsm"] = s.fsm;
        root["fsmName"] = QN8027::fsmName(s.fsm);
        root["audioPeak"] = s.audioPeak;
        root["ant"] = s.ant;
        char antHex[8];
        snprintf(antHex, sizeof(antHex), "%02X", s.ant);
        root["antHex"] = antHex;
        root["pac"] = s.pac;
        root["transmitting"] = s.transmitting;
        root["muted"] = s.muted;
        root["channel"] = s.channel;
        root["antRail"] = (s.ant == 0x3F);
        root["audioClip"] = (s.audioPeak >= 15);
        return root;
    }

    void registerApis() override {
        auto handler = [this](const HttpRequestPtr &req, HttpCallback &&callback) {
            handleKfmtApi(req, std::move(callback));
        };
        FPPPlugins::registerPluginApi("/kfmt", handler, {drogon::Get, drogon::Post}, false);
        FPPPlugins::registerPluginApi("/kfmt/retune", handler, {drogon::Get, drogon::Post}, false);
        FPPPlugins::registerPluginApi("/kfmt/clearpeak", handler, {drogon::Get, drogon::Post}, false);
        // One route per override rather than one that takes which: the handler
        // matches on the path by substring, so the discriminator has to be in
        // the path anyway, and a typo then 404s instead of silently clearing
        // whichever one the fallback happened to pick.
        FPPPlugins::registerPluginApi("/kfmt/clearoverride/stationid", handler, {drogon::Post}, false);
        FPPPlugins::registerPluginApi("/kfmt/clearoverride/rdstext", handler, {drogon::Post}, false);
    }

    void unregisterApis() override {
        FPPPlugins::unregisterPluginApi("/kfmt");
        FPPPlugins::unregisterPluginApi("/kfmt/retune");
        FPPPlugins::unregisterPluginApi("/kfmt/clearpeak");
        FPPPlugins::unregisterPluginApi("/kfmt/clearoverride/stationid");
        FPPPlugins::unregisterPluginApi("/kfmt/clearoverride/rdstext");
    }

    void handleKfmtApi(const HttpRequestPtr &req, HttpCallback &&callback) {
        const std::string path = req->path();
        const auto method = req->method();
        Json::Value root;

        if (path.find("clearpeak") != std::string::npos && method == drogon::Post) {
            if (!detected) {
                root["ok"] = false;
                root["error"] = "QN8027 not detected";
                callback(makeStringResponse(root.toStyledString(), 503, "application/json"));
                return;
            }
            auto job = runOnRadioThread([this](RadioJob &j) {
                qn8027.clearAudioPeak();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                j.result = statusJson();
                j.result["ok"] = true;
            }, 2000);
            if (!job) {
                root["ok"] = false;
                root["error"] = "timeout";
                callback(makeStringResponse(root.toStyledString(), 504, "application/json"));
                return;
            }
            callback(makeStringResponse(job->result.toStyledString(), 200, "application/json"));
            return;
        }

        if (path.find("retune") != std::string::npos && method == drogon::Post) {
            if (!detected) {
                root["ok"] = false;
                root["error"] = "QN8027 not detected";
                callback(makeStringResponse(root.toStyledString(), 503, "application/json"));
                return;
            }
            // Retune and read the resulting status in one job: two waits meant
            // two chances to time out, and the status read only made sense
            // right after the retune anyway.
            auto job = runOnRadioThread([this](RadioJob &j) {
                qn8027.retuneAntenna();
                j.ok = true;
                lastRetuneOk = true;
                lastRetuneResult = "done";
                // The retune drops the carrier, so forget it is up and let
                // applyRadioState() decide whether to bring it back - rather
                // than this path keeping its own copy of that decision.
                carrierEnabled = false;
                applyRadioState();
                j.result = statusJson();
                j.result["ok"] = j.ok;
            }, 8000);
            if (!job) {
                root["ok"] = false;
                root["error"] = "timeout";
                callback(makeStringResponse(root.toStyledString(), 504, "application/json"));
                return;
            }
            callback(makeStringResponse(job->result.toStyledString(), 200, "application/json"));
            return;
        }

        if (path.find("clearoverride") != std::string::npos && method == drogon::Post) {
            // Reuses the command handlers, so the page and a command clear an
            // override by exactly the same route - including their logging and
            // their "put the radio back the way it should be" follow-up.
            if (path.find("stationid") != std::string::npos) {
                applyStationIdOverride("");
            } else if (path.find("rdstext") != std::string::npos) {
                applyRdsTextOverride("");
            } else {
                root["ok"] = false;
                root["error"] = "unknown override";
                callback(makeStringResponse(root.toStyledString(), 404, "application/json"));
                return;
            }

            // The clear is queued above and runs whether or not the chip is
            // there, but runOnRadioThread() refuses when it is not detected.
            // Answering without a status read beats reporting a timeout for
            // work that in fact went through; the page's next poll carries the
            // new state either way.
            if (!detected) {
                root["ok"] = true;
                callback(makeStringResponse(root.toStyledString(), 200, "application/json"));
                return;
            }
            // Queued behind the clear, so the status this answers with
            // already reflects it.
            auto cleared = runOnRadioThread([this](RadioJob &j) {
                j.result = statusJson();
                j.result["ok"] = true;
            }, 4000);
            if (!cleared) {
                root["ok"] = false;
                root["error"] = "timeout";
                callback(makeStringResponse(root.toStyledString(), 504, "application/json"));
                return;
            }
            callback(makeStringResponse(cleared->result.toStyledString(), 200, "application/json"));
            return;
        }

        auto job = runOnRadioThread([this](RadioJob &j) { j.result = statusJson(); }, 4000);
        if (!job) {
            const bool adapter = qn8027.adapterPresent();
            root["detected"] = false;
            root["adapterPresent"] = adapter;
            root["error"] = "timeout";
            root["link"] = adapter ? "status timeout" : "USB adapter disconnected";
            root["fsmName"] = adapter ? "Unknown" : "Disconnected";
            callback(makeStringResponse(root.toStyledString(), 200, "application/json"));
            return;
        }
        callback(makeStringResponse(job->result.toStyledString(), 200, "application/json"));
    }

    void run() {
        std::unique_lock<std::mutex> lk(lock);

        while (running) {
            uint64_t ct = GetTimeMS();

            if (!detected && ct > nextReconnectTry) {
                nextReconnectTry = ct + reconnectInterval();
                lk.unlock();
                attemptReconnect();
                lk.lock();
            }

            // playlistActive is a latch: the start/playing/media callbacks set
            // it, and only a "stop" callback clears it. FPP emits "stop" for
            // section changes in the middle of a playlist as well as at the
            // end of one, and the final one can be missed entirely - at which
            // point the latch is stuck on, every idle action silently becomes
            // a no-op, and nothing short of restarting fppd recovers it. That
            // is exactly what was seen in the field.
            //
            // The player itself always knows, so reconcile against it. The
            // delay is because the callbacks can land marginally before the
            // player reports itself playing, and a bare disagreement would
            // then flap the carrier at the start of every show.
            if (playlistActive && !Player::INSTANCE.IsPlaying()) {
                if (playlistIdleSince == 0) {
                    playlistIdleSince = ct;
                } else if (ct - playlistIdleSince > 2000) {
                    LogInfo(VB_PLUGIN, "KFMT: player is idle but the playlist flag "
                                       "was still set - clearing it\n");
                    playlistActive = false;
                    playlistIdleSince = 0;
                    // Queued, not called: this holds the queue lock, and
                    // playlistStopped() takes it again via formatAndSendText().
                    functions.emplace([this]() { playlistStopped(); });
                }
            } else if (playlistIdleSince != 0) {
                playlistIdleSince = 0;
            }

            // While nothing is playing, follow the After Hours stream's title.
            // Only when idle: a running playlist's own media data is better.
            if (afterHoursEnabled() && !playlistActive && detected &&
                    ct > nextMpcPoll) {
                nextMpcPoll = ct + 12000;
                // Everything here runs with the queue lock released:
                // readMpcTitle() runs a subprocess, and formatAndSendText()
                // takes that same non-recursive lock itself, so calling it
                // while holding it deadlocks this thread - which stops RDS,
                // the status API and the reconnect poll dead.
                lk.unlock();
                std::string t = readMpcField("%title%");
                // Streams usually carry only a title, and often put
                // "Artist - Title" in it, so artist is frequently empty -
                // ask anyway, and let RT+ tag it properly when it is there.
                std::string a = readMpcField("[%artist%|%performer%|%albumartist%]");
                bool changed = (t != mpcTitle || a != mpcArtist);
                if (changed) {
                    mpcTitle = t;
                    mpcArtist = a;
                    title = t;
                    artist = a;
                    album.clear();
                    LogInfo(VB_PLUGIN, "KFMT: After Hours \"%s\"%s%s\n", t.c_str(),
                            a.empty() ? "" : " by ", a.c_str());
                    formatAndSendText(effectiveStationId(), 0);
                }
                lk.lock();
                if (changed) {
                    nextRDSTime = 0;   // refresh RT/RT+ now rather than in 2s
                }
            }

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
            if (rdsEnabled() && carrierEnabled && detected && qn8027.busOpen() &&
                    ct > nextPSTime && curStationIdString >= 0 &&
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
            if (rdsEnabled() && carrierEnabled && detected && qn8027.busOpen() &&
                    ct > nextRDSTime) {
                if (detected) {
                    functions.emplace([this]() {
                        try {
                            if (!rdsTextOverride.empty()) {
                                // A command has taken the RadioText over; send
                                // it plainly rather than RT+, which tags fields
                                // that an announcement does not have.
                                rdsSource = "command override";
                                qn8027.sendRadioText(rdsTextOverride);
                            } else if (title.empty() && artist.empty() && album.empty()) {
                                rdsSource = "station name and URL";
                                qn8027.sendStationRadioTextPlus(settings["StationName"],
                                                                settings["StationURL"]);
                            } else {
                                rdsSource = playlistActive ? "playlist media"
                                                           : "After Hours stream";
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
                condition.wait_for(lk, std::chrono::milliseconds(50));
            }
        }
    }

    // Player::INSTANCE is the authority on whether a show is running.
    // playlistActive only carries what the callbacks have told us since, which
    // matters because mediaCallback can learn that audio is flowing before the
    // player reports itself playing. The run loop reconciles the two, so this
    // cannot stay stuck on the way the bare latch could.
    bool showIsPlaying() const {
        return Player::INSTANCE.IsPlaying() || playlistActive;
    }

    struct RadioTarget {
        bool carrier;
        bool mute;
        const char *why;
    };

    // What the radio should be doing right now, from scratch, given the saved
    // settings and whether a show is running. Deriving it in one place is what
    // makes applyRadioState() safe to queue from anywhere: two of them landing
    // together converge on the same answer instead of the later one clobbering
    // the earlier, which is how a command and a playlist callback arriving in
    // the same millisecond used to leave the carrier up after a "turn it off".
    RadioTarget desiredRadioState() const {
        auto get = [this](const char *k, const char *dflt) {
            auto it = settings.find(k);
            return (it == settings.end() || it->second.empty()) ? std::string(dflt)
                                                                : it->second;
        };
        const std::string ts = get("TransmitterState", "0");
        if (ts == "2") return {false, false, "transmitter forced off"};
        if (ts == "1") return {true,  false, "transmitter forced on"};
        if (showIsPlaying()) return {true, false, "show is playing"};

        const std::string idle = get("IdleAction", "0");
        if (idle == "2") return {false, false, "idle: disable carrier"};
        if (idle == "1") return {true,  true,  "idle: mute"};
        return {true, false, "idle: leave alone"};
    }

    // Drive the chip to that state. The only place start/stop/mute are issued.
    // Runs on the sender thread; queue it with queueApplyRadioState().
    void applyRadioState() {
        if (!detected) return;
        try {
            const RadioTarget t = desiredRadioState();
            // Logged on every pass, both branches. The old startAction() said
            // nothing, so a carrier coming back up right after an idle action
            // turned it off was invisible in the log - which is what made this
            // look like the command had done nothing at all.
            LogInfo(VB_PLUGIN, "KFMT: radio state -> carrier %s%s (%s)\n",
                    t.carrier ? "on" : "off", t.mute ? ", muted" : "", t.why);
            if (!t.carrier) {
                qn8027.stopTransmit();
                carrierEnabled = false;
                return;
            }
            if (!carrierEnabled) {
                qn8027.startTransmit(false);
                carrierEnabled = true;
            }
            if (t.mute) {
                qn8027.mute();
            } else {
                qn8027.unmute();
            }
        } catch (...) {
            LogErr(VB_PLUGIN, "KFMT: exception in applyRadioState()\n");
        }
    }

    // Everything that has to happen once the show is over, from wherever we
    // learn it - the "stop" callback, or the run loop noticing the player went
    // idle without one. Runs on the sender thread, so formatAndSendText() is
    // safe to call here.
    void playlistStopped() {
        artist.clear();
        title.clear();
        album.clear();
        track       = 0;
        mediaLength = 0;
        formatAndSendText(effectiveStationId(), 0);
        nextRDSTime     = 0;
        nextStationTime = 0;
        applyRadioState();
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
                // constexpr string_view, not static const std::string: a
                // static local in an inline (in-class) function is emitted
                // as an STB_GNU_UNIQUE symbol, and glibc permanently refuses
                // to unload any library that first defines one - dlclose()
                // then succeeds and unmaps nothing. These need no dynamic
                // initialisation, so they produce no guard variable at all.
                constexpr std::string_view ARTIST = "{Artist}";
                constexpr std::string_view TITLE  = "{Title}";
                constexpr std::string_view ALBUM  = "{Album}";
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
        LogInfo(VB_PLUGIN, "KFMT: playlistCallback action=%s section=%s item=%d\n",
                action.c_str(), section.c_str(), item);

        // FPP sends "start" only when coming from idle. Restarting an already
        // running playlist (or advancing items) sends "playing".
        if (action == "start" || action == "playing") {
            playlistActive = true;
            mpcTitle.clear();   // the playlist's own media data takes over
            mpcArtist.clear();
            queueApplyRadioState();
        } else if (action == "stop") {
            // FPP sends "stop" for section changes inside a running playlist
            // too, so this is not on its own proof the show is over. Clearing
            // the flag is still right - the run loop reconciles against the
            // player, and a section change is followed immediately by a
            // "playing" that sets it again.
            playlistActive = false;
            queueRadioWork([this]() { playlistStopped(); });
        }
    }

    virtual void mediaCallback(const Json::Value &playlist,
                               const MediaDetails &mediaDetails) override {
        // Audio is actually starting; undo idle mute even if we missed
        // playlist "start" (plugin loaded mid-show, or FPP sent "playing").
        playlistActive = true;
        queueApplyRadioState();

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

        formatAndSendText(effectiveStationId(), 0);
        nextRDSTime     = 0;
        nextStationTime = 0;
        condition.notify_all();
    }

    // Set an override and push it out now. Runs on the sender thread, so the
    // command handler itself never touches the chip.
    void applyStationIdOverride(const std::string &text) {
        queueRadioWork([this, text]() {
            stationIdOverride = text;
            LogInfo(VB_PLUGIN, "KFMT: station ID override %s\n",
                    text.empty() ? "cleared" : ("-> \"" + text + "\"").c_str());
            formatAndSendText(effectiveStationId(), 0);
            curStationIdString = -1;   // restart the cycle on the new text
            nextStationTime = 0;
        });
    }
    void applyRdsTextOverride(const std::string &text) {
        queueRadioWork([this, text]() {
            rdsTextOverride = text;
            LogInfo(VB_PLUGIN, "KFMT: RDS text override %s\n",
                    text.empty() ? "cleared" : ("-> \"" + text + "\"").c_str());
            nextRDSTime = 0;           // send it on the next pass, not in 2s
        });
    }

    // The transmitter state is a saved setting, and FPP has exactly one writer
    // for those: the endpoint the settings page itself posts to. Writing
    // config/plugin.<name> from here would be a second implementation of that
    // file's format, quoting and locking, in another language.
    //
    // Nothing is applied here. FPP writes the file, the settings FileMonitor
    // sees the key differ, and settingChanged("TransmitterState") queues the
    // apply - the identical path a change made on the settings page takes, so
    // the page and a command cannot end up disagreeing.
    //
    // Localhost needs no credentials even on a password-protected player:
    // FPP's apache site OR's "Require local" with the password config.
    //
    // Tagged with this plugin's name so shutdown() can cancel it. The
    // callback's code lives in this library, so one still in flight after an
    // unload is a call into an unmapped .so; cancelRequests() destroys it
    // uninvoked, which is what releases it while the library is still mapped.
    void applyTransmitterState(const std::string &value) {
        CurlManager::INSTANCE.addPut(
            "http://localhost/api/plugin/" + name + "/settings/TransmitterState",
            value, "text/plain",
            [value](int rc, const std::string &resp) {
                if (rc == 200) {
                    LogInfo(VB_PLUGIN, "KFMT: transmitter state saved as %s\n",
                            transmitterStateName(value));
                } else {
                    LogErr(VB_PLUGIN,
                           "KFMT: could not save transmitter state (HTTP %d): %s\n",
                           rc, resp.c_str());
                }
            },
            name);
    }

    const std::string &effectiveStationId() {
        return stationIdOverride.empty() ? settings["StationID"] : stationIdOverride;
    }

    bool afterHoursEnabled() const {
        auto it = settings.find("AfterHoursRDS");
        return mpcAvailable && it != settings.end() && it->second != "0";
    }

    // Ask mpd for one formatted field. Runs on the sender thread with the
    // queue lock released - it is a subprocess, and must not be holding
    // anything the callbacks need.
    //
    // stderr is discarded on purpose: with no mpd running, mpc writes
    // "MPD error: Connection refused" there and leaves stdout empty, so
    // without this the station would cheerfully broadcast that as its
    // RadioText.
    static std::string readMpcField(const char *format) {
        std::string out;
        std::string cmd = std::string("mpc current -f '") + format + "' 2>/dev/null";
        FILE *f = popen(cmd.c_str(), "r");
        if (f == nullptr) {
            return out;
        }
        char buf[256];
        if (fgets(buf, sizeof(buf), f) != nullptr) {
            out = buf;
        }
        pclose(f);
        while (!out.empty() &&
               (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
            out.pop_back();
        }
        return out;
    }

    // FPP commands, so a show can put something on the air without editing
    // the configuration - "Back in 10 minutes", "Tune to 88.1", and so on.
    // Running either with an empty string restores the configured text, which
    // is why blanks are allowed on the argument.
    class StationIdCommand : public Command {
    public:
        StationIdCommand(FPPKFMTPlugin *p) :
            Command("KFMT Station ID",
                    "Temporarily replace the RDS station ID. Send an empty value to go "
                    "back to the configured station ID."),
            plugin(p) {
            args.push_back(CommandArg("text", "string", "Station ID", true));
        }
        std::unique_ptr<Command::Result> run(const std::vector<std::string> &a) override {
            plugin->applyStationIdOverride(a.empty() ? "" : a[0]);
            return std::make_unique<Command::Result>(
                (a.empty() || a[0].empty()) ? "Station ID restored" : "Station ID set");
        }
        FPPKFMTPlugin *plugin;
    };

    class RdsTextCommand : public Command {
    public:
        RdsTextCommand(FPPKFMTPlugin *p) :
            Command("KFMT RDS Text",
                    "Temporarily replace the RDS RadioText. Send an empty value to go "
                    "back to the configured text and song information."),
            plugin(p) {
            args.push_back(CommandArg("text", "string", "RDS Text", true));
        }
        std::unique_ptr<Command::Result> run(const std::vector<std::string> &a) override {
            plugin->applyRdsTextOverride(a.empty() ? "" : a[0]);
            return std::make_unique<Command::Result>(
                (a.empty() || a[0].empty()) ? "RDS text restored" : "RDS text set");
        }
        FPPKFMTPlugin *plugin;
    };

    // Says what it does and does it now, whether or not a playlist is running.
    // The idle behaviour it replaced could only ever take effect at the next
    // "stop" callback, which is no use to a show that wants the transmitter
    // off at the end of the night - and no use at all if that callback never
    // arrives. Unlike the text commands this one saves, so a transmitter
    // forced off overnight is still off after a 3am restart.
    class TransmitterCommand : public Command {
    public:
        TransmitterCommand(FPPKFMTPlugin *p) :
            Command("KFMT Transmitter",
                    "Force the FM carrier on or off, or let it follow the Playlist Idle "
                    "Behavior setting. Takes effect immediately and is saved, so Force "
                    "Off keeps the transmitter off until it is set back."),
            plugin(p) {
            args.push_back(CommandArg("state", "string", "Transmitter")
                               .setContentList({"Follow Idle Setting", "Force On",
                                                "Force Off"})
                               .setDefaultValue("Follow Idle Setting"));
        }
        std::unique_ptr<Command::Result> run(const std::vector<std::string> &a) override {
            if (a.empty()) {
                return std::make_unique<Command::ErrorResult>("No transmitter state given");
            }
            const std::string v = transmitterStateValue(a[0]);
            if (v.empty()) {
                return std::make_unique<Command::ErrorResult>(
                    "Unknown transmitter state \"" + a[0] + "\"");
            }
            plugin->applyTransmitterState(v);
            return std::make_unique<Command::Result>(
                std::string("Transmitter set to ") + transmitterStateName(v));
        }
        FPPKFMTPlugin *plugin;
    };

    void registerCommands() {
        myCommands.push_back(new StationIdCommand(this));
        myCommands.push_back(new RdsTextCommand(this));
        myCommands.push_back(new TransmitterCommand(this));
        for (auto *c : myCommands) {
            CommandManager::INSTANCE.addCommand(c);
        }
    }
    // A plugin owns what it registers. removeCommand() only unregisters - it
    // does not delete, and does not wait for anything in flight - so the delete
    // is ours, and it has to happen before this library is unmapped: a Command
    // subclass declared here has its vtable in this .so.
    //
    // FPP keeps a backstop that deletes whatever a plugin leaves behind, and it
    // compares the registered pointer before doing so, so withdrawing here is
    // not a double delete - it is the path FPP expects, and skipping it earns a
    // warning naming this plugin at unload.
    //
    // Idempotent: the list is cleared, so a second call finds nothing.
    void unregisterCommands() {
        for (auto *c : myCommands) {
            CommandManager::INSTANCE.removeCommand(c);
            delete c;
        }
        myCommands.clear();
    }

    void setDefaultSettings() {
        setIfNotFound("Frequency", "87.9");
        setIfNotFound("IdleAction", "0");
        setIfNotFound("TransmitterState", "0");
        setIfNotFound("Preemphasis", "1");

        setIfNotFound("RDSEnable", "1");
        setIfNotFound("StationID", "Merry   Christ- mas", true);
        setIfNotFound("StationName", "", true);
        setIfNotFound("StationURL", "", true);
        setIfNotFound("StationIDTime", "5");
        setIfNotFound("ProgramType", "0");
        setIfNotFound("StationCode", "WFPP");

        setIfNotFound("TXInputBufferGain", "3");
        setIfNotFound("TXDigitalGain", "0");
        setIfNotFound("InputImpedance", "20");
        setIfNotFound("TransmitPower", "50");
        setIfNotFound("AfterHoursRDS", "0");

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

// Safe to dlclose() on unload: the only thread is the I2C sender, and shutdown()
// stops and joins it. HTTP routes go through registerPluginApi() and are
// withdrawn in unregisterApis()/shutdown(). Commands are withdrawn AND deleted
// there too, and the CurlManager request the transmitter command issues is
// tagged with this plugin's name and cancelled there, which destroys its
// callback while this library is still mapped. No timers, no epoll descriptors.
// The settings FileMonitor entry is registered and removed by
// FPPPlugins::Plugin itself, in its destructor, which runs before the library
// is unmapped.
//
// The USB HID path (CP2112) does not change that, which is worth spelling out
// because fpp-vastfmt also talks to USB HID and deliberately does NOT opt in.
// What matters is not whether a plugin uses USB, but whose address space the
// HID backend's code lives in and whether it runs threads:
//
//   - This plugin links -lhidapi-hidraw, so hidapi is a SYSTEM shared library.
//     It defines no hid_* symbol itself (nm -D --defined-only shows 0; all 7 are
//     imported), and it makes no pthread_create call. The hidraw backend is a
//     thin wrapper over read/write/ioctl on /dev/hidraw* and starts no threads
//     of its own. Even if it did, that code would live in libhidapi-hidraw.so,
//     which is never unloaded, so nothing there could point back into here.
//   - fpp-vastfmt compiles hidapi's LIBUSB backend into its own .so (src/hid.o)
//     and links -lusb-1.0. That backend runs a read thread per open device whose
//     entry point is therefore inside the plugin - exactly the thing dlclose()
//     would unmap.
FPP_PLUGIN_SUPPORTS_UNLOAD()

extern "C" {
    FPPPlugins::Plugin *createPlugin() {
        return new FPPKFMTPlugin();
    }
}
