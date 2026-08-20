#pragma once

extern struct System soundSystem;

struct Sound;
struct Sound* soundLoad(const char* path);
void soundDestroy(struct Sound* sound);

void soundPlay(struct Sound* sound, float volume, bool loop);
void soundStop(struct Sound* sound);

void soundPlayHover(void);
void soundPlayClick(void);
void soundPlayError(void);

void soundPlayClickOnMusicLevel(void);


