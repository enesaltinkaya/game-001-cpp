#pragma once

#include <stddef.h>

/* ── Forward declarations ─────────────────────────────────────────────────── */

typedef struct Scene   Scene;

/* ── States ────────────────────────────────────────────────────────────────── */

typedef enum {
    STATE_NONE,
    STATE_MAIN_MENU,
    STATE_LOADING_AZGAAR,
    STATE_GAMEPLAY,
} GameState;

/* ── Callbacks per state ───────────────────────────────────────────────────── */

typedef struct {
    void (*enter)(void);
    void (*exit)(void);
    void (*update)(void);
} StateCallbacks;

/* ── Public API ────────────────────────────────────────────────────────────── */

void gameStateInit(void);
void gameStateTransition(GameState target);
void gameStateUpdate(void);
GameState gameStateCurrent(void);

/* ── Helpers for state callbacks ───────────────────────────────────────────── */

static inline void gameStateRegister(GameState state, StateCallbacks callbacks) {
    extern void gameStateRegisterInternal(GameState, StateCallbacks);
    gameStateRegisterInternal(state, callbacks);
}

/* ── Callback declarations (implemented in GameState.c) ────────────────────── */

void gameStateMainMenuEnter(void);
void gameStateMainMenuExit(void);
void gameStateMainMenuUpdate(void);

void gameStateLoadingAzgaarEnter(void);
void gameStateLoadingAzgaarExit(void);
void gameStateLoadingAzgaarUpdate(void);

void gameStateGameplayEnter(void);
void gameStateGameplayExit(void);
void gameStateGameplayUpdate(void);

/* ── Load state for async transitions ──────────────────────────────────────── */

typedef enum {
    GAMEPLAY_LOADED_NONE,
    GAMEPLAY_LOADED_ANIMATIONS,
    GAMEPLAY_LOADED_READY,
} GameplayLoadState;

GameplayLoadState gameStateGameplayLoadState(void);

/* ── Asset transfer from Loading state to Gameplay state ─────────────────── */

void gameStateSetLoadedScene(Scene* scene);
void gameStateSetLoadedAnimationsScene(Scene* scene);
