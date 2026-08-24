#pragma once
#include "ecs/system/System.h"

namespace engine {
class SoundSystem : public System {
public:
    SoundSystem();
    void added() override;
    void removed() override;
    void preUpdate() override;
};

extern SoundSystem soundSystem;

struct Sound;
struct Sound* soundLoad(const char* path);
void soundDestroy(struct Sound* sound);

void soundPlay(struct Sound* sound, float volume, bool loop);
void soundStop(struct Sound* sound);

void soundPlayHover(void);
void soundPlayClick(void);
void soundPlayError(void);

void soundPlayClickOnMusicLevel(void);


}  // namespace engine
