// MatrixGame - SR2 Planetary battles engine
// Copyright (C) 2012, Elemental Games, Katauri Interactive, CHK-Games
// Licensed under GPLv2 or any later version

#include "stdafx.h"

#include "RndSplit.hpp"
#include "SimHarness.hpp"

// stdafx.h подменяет rand() на RndDispatch(), а визуальному потоку нужен
// настоящий libc rand.
#undef rand

#ifdef MATRIXGAME_SIM_HARNESS

// В харнессе вся работа уже сделана в SimHarness: он считает вызовы, пишет
// гистограмму по адресам возврата и умеет пересеивать поток. Здесь только
// пробрасываем интерфейс, чтобы игра звала одно и то же в любой сборке.

unsigned RndDispatch(void) {
    return SimRand();
}

void RndSetDrawPhase(bool draw) {
    SimSetPhase(draw ? SIMPHASE_VISUAL : SIMPHASE_LOGIC);
}

bool RndDrawPhase(void) {
    return SimPhase() == SIMPHASE_VISUAL;
}

void RndResetLogic(unsigned seed) {
    (void)seed;  // харнесс берёт сид из MATRIXGAME_SEED
    SimResetLogicRng();
}

unsigned long long RndLogicCallCount(void) {
    return SimLogicRandCalls();
}

unsigned long long RndVisualCallCount(void) {
    return SimRandCalls() - SimLogicRandCalls();
}

#else  // обычная сборка

namespace {

bool g_drawPhase = false;
unsigned g_logic = 0;
bool g_logicSeeded = false;
unsigned long long g_logicCalls = 0;
unsigned long long g_visualCalls = 0;

void LogicSeedOnce(void) {
    if (g_logicSeeded)
        return;
    g_logic = (unsigned)rand() ^ ((unsigned)time(NULL) << 13);
    if (g_logic == 0)
        g_logic = 0x9E3779B9;
    g_logicSeeded = true;
}

}  // namespace

unsigned RndDispatch(void) {
    if (g_drawPhase) {
        ++g_visualCalls;
        return (unsigned)rand();
    }

    ++g_logicCalls;
    LogicSeedOnce();
    // xorshift32: дешёвый, без таблиц и без обращения к памяти.
    unsigned x = g_logic;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_logic = x;
    // RND/FRND рассчитаны на диапазон 0..RAND_MAX.
    return x & RAND_MAX;
}

void RndSetDrawPhase(bool draw) {
    g_drawPhase = draw;
}

bool RndDrawPhase(void) {
    return g_drawPhase;
}

void RndResetLogic(unsigned seed) {
    g_logic = seed ? seed : 0x9E3779B9;
    g_logicSeeded = true;
    g_logicCalls = 0;
    g_visualCalls = 0;
}

unsigned long long RndLogicCallCount(void) {
    return g_logicCalls;
}

unsigned long long RndVisualCallCount(void) {
    return g_visualCalls;
}

#endif  // MATRIXGAME_SIM_HARNESS

// Сид матча общий для обеих реализаций: его пишет лог команд, его проверяет
// реплей, и его же в будущем будет присылать лобби.
static unsigned s_matchSeed = 0;

unsigned RndMatchSeed(void) {
    return s_matchSeed;
}

void RndSetMatchSeed(unsigned seed) {
    s_matchSeed = seed;
    RndResetLogic(seed);
}
