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
//   MATRIXGAME_HASHSTEP=<ms>      период дампа (по умолчанию 100 мс)
//   MATRIXGAME_OBJLOG=<file>      дамп полей объектов (ключ — стабильный id)
//   MATRIXGAME_RANDLOG=<file>     вызовы rand() по адресам возврата, с дельтой за период
//   MATRIXGAME_FILL=<hex>         байт заливки свежих аллокаций (дифференциальный режим)
//   MATRIXGAME_TRACE=<file>       трассировка этапов инициализации и падений
//   MATRIXGAME_MAP=<name>         принудительная карта

#pragma once

#include <stdio.h>

#ifdef MATRIXGAME_SIM_HARNESS

// Включён ли режим автоигры (без меню, сразу в бой).
bool SimModeEnabled(void);

// Отключена ли отрисовка (MATRIXGAME_NODRAW=1). Нужно для длинных прогонов:
// без кадров симуляция идёт на порядок быстрее.
bool SimNoDraw(void);

// Сид для srand: значение из окружения либо переданный запасной вариант.
unsigned SimSeed(unsigned fallback);

// Сброс логического ГПСЧ к началу боя. До боя меню игры (превью карты в
// «Планетарных боях») успевает вызвать rand(), и без сброса бой стартовал бы с
// произвольно сдвинутого состояния потока: два прогона давали разные войска.
void SimResetLogicRng(void);

// Раз в сколько мс писать хеш состояния.
#define SIM_HASH_PERIOD 100

// Вызывается из CMatrixMapLogic::Takt после логического шага.
void SimLogTakt(int step);

// Трассировка этапов инициализации в файл MATRIXGAME_TRACE (append).
void SimTrace(const char *message);

// Фильтр исключений: пишет адрес падения в MATRIXGAME_TRACE.
void SimInstallCrashFilter(void);

// Фильтр для __except внутри MatrixGameDll: игра глушит исключения в
// protectedBlock1 и выходит молча, поэтому без этой записи падение выглядит как
// «бой просто закончился». Возвращает EXCEPTION_EXECUTE_HANDLER.
int SimExceptionFilter(void *exceptionPointers);

// Принудительная карта из MATRIXGAME_MAP (иначе NULL — берём выбор игры).
const wchar *SimMapOverride(void);

// Трассировка в широкой строке.
void SimTraceW(const wchar *message);

// Обёртка над rand(): считает вызовы, чтобы в логе было видно,
// разошлась ли последовательность ГПСЧ у двух прогонов.
unsigned SimRand(void);
unsigned long long SimRandCalls(void);

// Счётчики ремонтных лучей: сколько объектов создано, сколько раз Draw
// вызывался, сколько раз прошёл проверку флага. Нужны, чтобы отличить
// «луч не нарисовали» от «луча нет». Значения печатаются в хеш-логе.
#define SIMREP_ALLOC 0
#define SIMREP_ENTER 1
#define SIMREP_PASS  2
void SimRepairStat(int ev);

// Фаза выполнения: в фазе отрисовки rand() берётся из отдельного потока, чтобы
// визуальные эффекты не сдвигали последовательность, которую использует логика.
#define SIMPHASE_LOGIC  0
#define SIMPHASE_VISUAL 1
void SimSetPhase(int phase);
int SimPhase(void);

// Сколько чисел выдал именно логический поток (визуальный не считается).
unsigned long long SimLogicRandCalls(void);

// Печатает в файл таблицу «имя поля -> смещение» для классов робота.
// Нужна, чтобы переводить смещения, найденные сканером заливки, в имена полей.
void SimDumpLayouts(FILE *f);

// Трассировка одного объекта: MATRIXGAME_TRACEBOT=<id> включает её,
// MATRIXGAME_TRACEFROM/TRACETO (мс игрового времени) сужают окно,
// MATRIXGAME_TRACEBOTLOG задаёт файл (по умолчанию tracebot.log).
// Нужна, чтобы видеть, кто и с какими аргументами меняет поля робота, когда
// обычный дамп уже не показывает разницы.
bool SimTraceBotOn(unsigned id);
void SimTraceBotf(unsigned id, const char *fmt, ...);

#endif
