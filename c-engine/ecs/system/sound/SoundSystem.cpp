#include "ecs/system/sound/SoundSystem.h"
#include "ecs/system/System.h"
#include "ecs/system/window/WindowSystem.h"
#include "soloud/git/include/soloud_c.h"
#include "soloud/git/include/soloud_c.h"
#include "soloud/git/src/backend/miniaudio/miniaudio.h"

namespace engine {
struct Sound {
    Wav sample;
    int handle;
};

static struct {
    Soloud soloud;
    std::vector<int> loopingHandles;
    Sound* buttonHoverEffect;
    Sound* buttonClickEffect;
    Sound* errorEffect;
} audio;

static void settingsSaved(void* _);

SoundSystem soundSystem;

SoundSystem::SoundSystem() : System("sound") {}

void SoundSystem::added() {
    // -- loop playback devices
    // ma_result result;
    // ma_context context;
    // ma_device_info* pPlaybackDeviceInfos;
    // ma_uint32 playbackDeviceCount;
    // result = ma_context_init(NULL, 0, NULL, &context);
    // if (result != MA_SUCCESS) error("Failed to initialize context.");
    // result = ma_context_get_devices(&context, &pPlaybackDeviceInfos, &playbackDeviceCount, NULL, NULL);
    // if (result != MA_SUCCESS) error("Failed to enumerate playback devices.");
    // for (u32 iAvailableDevice = 0; iAvailableDevice < playbackDeviceCount; iAvailableDevice += 1) {
    //     debug("  %d: %s", iAvailableDevice, pPlaybackDeviceInfos[iAvailableDevice].name);
    // }

    audio.soloud = (Soloud)Soloud_create();
    Soloud_init((Soloud*)audio.soloud);

    audio.buttonHoverEffect = soundLoad("sound/whipstick.ogg");
    audio.buttonClickEffect = soundLoad("sound/click.ogg");
    audio.errorEffect       = soundLoad("sound/error.ogg");

    utils::signalSubscribe("settingsSaved", settingsSaved);

    const char* backend = Soloud_getBackendString((Soloud*)audio.soloud);
    ma_device* device    = static_cast<ma_device*>(Soloud_getBackendData(audio.soloud));
    utils::debug("soundSystem: audio engine  %s", "SoLoud");
    utils::debug("soundSystem: audio backend %s", backend);
    utils::debug("soundSystem: audio device  %s", device->playback.name);
}

static void soundRemovedDelayed(void* _) {
    soundDestroy(audio.buttonClickEffect);
    soundDestroy(audio.buttonHoverEffect);
    soundDestroy(audio.errorEffect);

    // Soloud_deinit(audio.soloud);
    // Soloud_destroy(audio.soloud);
    audio.soloud = 0;
}

void SoundSystem::removed() {
    // let systems using sound files close their handles first
    utils::futureTaskAdd(0, soundRemovedDelayed, 0);
}

Sound* soundLoad(const char* path) {
    Sound* sound = new Sound{};
    sound->sample = (Wav)Wav_create();

    struct utils::String soundFileContents = utils::dataManagerRead(path);
    Wav_loadMemEx((AudioSource*)sound->sample, reinterpret_cast<const unsigned char*>(soundFileContents.data), soundFileContents.size, 1, 1);
    utils::stringDestroy(&soundFileContents);
    return sound;
}

void soundDestroy(Sound* sound) {
    if (!sound) return;
    soundStop(sound);
    Wav_destroy((AudioSource*)sound->sample);
    delete sound;
    // Note: the handle is unlinked from audio.loopingHandles inside
    // soundStop.  That array is process-wide (owned by the sound system,
    // shared by every looping sound), so it must NOT be freed here — freeing
    // it per-sound corrupts the heap for the next looping sound.
}

void soundPlay(Sound* sound, float volume, bool loop) {
    if (loop) {
        sound->handle = Soloud_playBackgroundEx((Soloud*)audio.soloud, (AudioSource*)sound->sample, volume, 0, 0);
        Soloud_setLooping((Soloud*)audio.soloud, sound->handle, 1);
        audio.loopingHandles.push_back(sound->handle);
    } else {
        Soloud_playEx((Soloud*)audio.soloud, (AudioSource*)sound->sample, volume, 0, 0, 0);
    }
}

void soundStop(Sound* sound) {
    if (!sound || !sound->handle) {
        return;
    }
    Soloud_stop((Soloud*)audio.soloud, sound->handle);

    for (i32 i = 0, s = static_cast<i32>(audio.loopingHandles.size()); i < s; i++) {
        if (audio.loopingHandles[i] == sound->handle) {
            audio.loopingHandles[i] = audio.loopingHandles.back();
            audio.loopingHandles.pop_back();
            break;
        }
    }
}

void settingsSaved(void* _) {
    for (i32 i = 0, si = static_cast<i32>(audio.loopingHandles.size()); i < si; i++) {
        int handle = audio.loopingHandles[i];
        Soloud_setVolume((Soloud*)audio.soloud, handle, utils::settingsGetDouble("music") / 100.);
    }
}

static double lastPlayed;

void soundPlayHover(void) {
    if (utils::nanos() > lastPlayed + MILLION * 50) {
        lastPlayed = utils::nanos();
        soundPlay(audio.buttonHoverEffect, utils::settingsGetDouble("effects") / 100 / 5.f, 0);
    }
}

void soundPlayClick(void) {
    if (utils::nanos() > lastPlayed + MILLION * 50) {
        lastPlayed = utils::nanos();
        soundPlay(audio.buttonClickEffect, utils::settingsGetDouble("effects") / 100., 0);
    }
}

void soundPlayError(void) {
    if (utils::nanos() > lastPlayed + MILLION * 50) {
        lastPlayed = utils::nanos();
        soundPlay(audio.errorEffect, utils::settingsGetDouble("effects") / 100., 0);
    }
}

void soundPlayClickOnMusicLevel(void) {
    soundPlay(audio.buttonClickEffect, utils::settingsGetDouble("music") / 100., 0);
}

void SoundSystem::preUpdate() {
    if (input.ctrl && input.pressed == KEY_M) {
        utils::settingsSetDouble("music", 0);
        utils::settingsWrite();
    }
}
}  // namespace engine
