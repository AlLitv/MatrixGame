// MatrixGame - SR2 Planetary battles engine
// Copyright (C) 2012, Elemental Games, Katauri Interactive, CHK-Games
// Licensed under GPLv2 or any later version
//
// Харнесс детерминизма. Собирается только с -DMATRIXGAME_SIM_HARNESS и
// активируется переменными окружения, поэтому обычная сборка не меняется.
//
//   MATRIXGAME_SIM=1              включить режим автоигры без меню
//   MATRIXGAME_SEED=<uint>        фиксированный сид ГПСЧ вместо time(NULL)
//   MATRIXGAME_FIXEDSTEP=<ms>     жёсткий шаг логики вместо измеренного
//   MATRIXGAME_MAXTICKS=<ms>      через столько мс игрового времени выйти
//   MATRIXGAME_HASHLOG=<file>     куда писать хеш состояния мира

#pragma once

#ifdef MATRIXGAME_SIM_HARNESS

// Включён ли режим автоигры (без меню, сразу в бой).
bool SimModeEnabled(void);

// Сид для srand: значение из окружения либо переданный запасной вариант.
unsigned SimSeed(unsigned fallback);

// Раз в сколько мс писать хеш состояния.
#define SIM_HASH_PERIOD 100

// Вызывается из CMatrixMapLogic::Takt после логического шага.
void SimLogTakt(int step);

// Трассировка этапов инициализации в файл MATRIXGAME_TRACE (append).
void SimTrace(const char *message);

// Фильтр исключений: пишет адрес падения в MATRIXGAME_TRACE.
void SimInstallCrashFilter(void);

// Принудительная карта из MATRIXGAME_MAP (иначе NULL — берём выбор игры).
const wchar *SimMapOverride(void);

// Трассировка в широкой строке.
void SimTraceW(const wchar *message);

// Обёртка над rand(): считает вызовы, чтобы в логе было видно,
// разошлась ли последовательность ГПСЧ у двух прогонов.
unsigned SimRand(void);
unsigned long long SimRandCalls(void);

#endif
