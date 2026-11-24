// MatrixGame - SR2 Planetary battles engine
// Copyright (C) 2012, Elemental Games, Katauri Interactive, CHK-Games
// Licensed under GPLv2 or any later version
//
// Два потока ГПСЧ.
//
// Раньше логика ИИ и рендер брали числа из одного rand(), и любой лишний кадр
// (видимый луч ремонта, клуб пыли, пролетевший обломок) сдвигал
// последовательность, которую читает ИИ: картинка на экране меняла поведение
// мира. Поэтому потоков два:
//
//   * логический (xorshift) — всё, что влияет на симуляцию: приказы, урон,
//     выбор цели, разброс попадания;
//   * визуальный (libc rand) — эффекты, звук, дрожание, — то, чьё количество
//     зависит от кадров и от того, что попало в кадр.
//
// rand() разворачивается в RndDispatch(), а фаза переключается на весь кадр
// отрисовки в CFormMatrixGame::Draw.
#pragma once

// Значение, которое вернул бы rand(): логический поток вне фазы отрисовки,
// визуальный — внутри неё.
unsigned RndDispatch(void);

// Фаза: true — внутри кадра отрисовки.
void RndSetDrawPhase(bool draw);
bool RndDrawPhase(void);

// Начать бой с чистого логического потока. В сетевой сборке сид придёт из
// лобби, поэтому он параметр, а не time(NULL) внутри.
void RndResetLogic(unsigned seed);

// Счётчики вызовов — для логов и проверок.
unsigned long long RndLogicCallCount(void);
unsigned long long RndVisualCallCount(void);
