#include "mod/VoiceMod.h"

#include <chrono>
#include <filesystem>

#include "voice/AndroidPermission.h"

#include <pl/Mod.hpp>

namespace voicechat {

VoiceMod &VoiceMod::instance() {
    static VoiceMod instance;
    return instance;
}

VoiceMod::VoiceMod() : mSelf(*ll::mod::NativeMod::current()) {}

bool VoiceMod::load() {
    auto &self = getSelf();
    self.getLogger().debug("Loading...");

    std::error_code ec;
    std::filesystem::create_directories(self.getDataDir(), ec);
    if (ec) {
        self.getLogger().error("Failed to create data directory {}: {}", self.getDataDir().string(),
                               ec.message());
        return false;
    }

    std::filesystem::create_directories(self.getConfigDir(), ec);
    if (ec) {
        self.getLogger().error("Failed to create config directory {}: {}",
                               self.getConfigDir().string(), ec.message());
        return false;
    }

    mConfigFile.emplace();
    if (!mConfigFile->load()) {
        self.getLogger().warn("Failed to load typed config");
        return false;
    }
    mConfig = mConfigFile->value();

    self.getLogger().info("Loaded {} from {}", self.getName(), self.getModDir().string());
    return true;
}

bool VoiceMod::enable() {
    auto &self = getSelf();
    self.getLogger().debug("Enabling...");
    if (!mConfig.enabled) {
        self.getLogger().info("voice chat is disabled by config");
        return true;
    }

    // Position tracking (needs the BedrockTools mod enabled alongside this one).
    mTracker = std::make_unique<PlayerTracker>(
        [this](const std::string &msg) { getSelf().getLogger().info("{}", msg); }, mConfig.logPosition);
    mTracker->start();

    // Self Hearing is the primary toggle (default ON). loopbackTest is kept as a
    // legacy alias so older configs still enable the same path.
    const bool wantSelfHearing = mConfig.selfHearing || mConfig.loopbackTest;
    if (wantSelfHearing) {
        self.getLogger().info(
            "Self Hearing ON (volume={:.0f}%, throughResound={}) - Resound Compatible path",
            mConfig.selfHearingVolume * 100.0f, mConfig.throughResound ? "yes" : "no");
        {
            std::lock_guard<std::mutex> lock(mAudioMu);
            mAudioStop = false;
        }
        // Off the game thread: may have to wait for the user to answer the permission prompt.
        mAudioThread = std::thread([this] { audioStartupLoop(); });
    } else {
        self.getLogger().info("Self Hearing OFF; networked voice not implemented yet (server {}:{}, radius {})",
                              mConfig.serverHost, mConfig.serverPort, mConfig.radius);
    }
    return true;
}

void VoiceMod::audioStartupLoop() {
    auto &log = getSelf().getLogger();
    auto keepWaiting = [this](std::chrono::milliseconds d) {
        std::unique_lock<std::mutex> lk(mAudioMu);
        return !mAudioCv.wait_for(lk, d, [this] { return mAudioStop; });
    };

    bool requested = false;
    for (int i = 0; i < 90; i++) { // ~3 minutes of polling, then give up
        const MicStatus st = queryMicPermission();
        if (!st.ok) {
            if (i == 0 || i % 15 == 0) log.warn("Microphone check not ready yet: {}", st.error);
        } else {
            if (i == 0)
                log.info("Microphone: app={} declared={} granted={}", st.package, st.declared, st.granted);
            if (st.granted) {
                Loopback::Options opt;
                opt.inputPreset = mConfig.micInputPreset;
                opt.micGainDb = mConfig.micGainDb;
                opt.playbackVolume = mConfig.selfHearingVolume;
                auto loop = std::make_unique<Loopback>();
                if (loop->start(opt)) {
                    mLoopback = std::move(loop);
                    if (mConfig.throughResound) {
                        log.info("Self Hearing running (Resound Compatible mode requested, preset={}, micGain={:.1f}dB). "
                                 "Mixer path not fully wired yet - using direct loopback fallback. "
                                 "Speak with headphones on.",
                                 mConfig.micInputPreset, mConfig.micGainDb);
                    } else {
                        log.info("Self Hearing running (direct loopback, preset={}, micGain={:.1f}dB). "
                                 "Speak, you should hear yourself.",
                                 mConfig.micInputPreset, mConfig.micGainDb);
                    }
                } else {
                    log.error("Microphone is allowed but the audio stream failed to open (see 'VoiceChat' log lines)");
                }
                return;
            }
            if (!st.declared) {
                log.error("{} does not declare the RECORD_AUDIO permission, so it can't be granted. "
                          "Voice chat needs a launcher build that declares it.", st.package);
                return;
            }
            if (mConfig.requestMicPermission && !requested) {
                requested = true;
                log.info("Requesting microphone permission: {}", requestMicPermission());
            }
        }
        if (!keepWaiting(std::chrono::seconds(2))) return; // disable() was called
    }
    log.warn("Gave up waiting for the microphone permission (allow it in Android Settings > Apps > LeviLauncher)");
}

bool VoiceMod::disable() {
    getSelf().getLogger().debug("Disabling...");
    {
        std::lock_guard<std::mutex> lock(mAudioMu);
        mAudioStop = true;
    }
    mAudioCv.notify_all();
    if (mAudioThread.joinable()) mAudioThread.join(); // after this nobody else touches mLoopback
    if (mTracker) {
        mTracker->stop();
        mTracker.reset();
    }
    if (mLoopback) {
        mLoopback->stop();
        mLoopback.reset();
    }
    return true;
}

bool VoiceMod::unload() {
    getSelf().getLogger().debug("Unloading...");
    mConfigFile.reset();
    return true;
}

} // namespace voicechat
