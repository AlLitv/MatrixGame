// MatrixGame - SR2 Planetary battles engine
// Copyright (C) 2012, Elemental Games, Katauri Interactive, CHK-Games
// Licensed under GPLv2 or any later version
// Refer to the LICENSE file included

#ifndef NETPLAY_HPP
#define NETPLAY_HPP

// Сетевой слой (фаза 3, docs/NETPLAY_PLAN.md §6).
//
// UDP, один сокет на процесс, сообщения:
//   HELLO / HELLO_ACK  — рукопожатие, сверка версии и сида матча;
//   CMD                — пачка приказов с номером (доставка с подтверждением);
//   ACK                — что принято и до какого такта досчитан мир;
//   READY              — до какого такта партнёр точно всё отправил;
//   HASH               — хеш мира на такте, для раннего обнаружения десинка.
//
// Барьер: такт T не начинается, пока не пришёл READY >= T. Потеря пакета не
// рассинхронизирует мир, а замораживает его до перепосылки: приказы всегда
// применяются на своём такте и в одном порядке на обеих машинах.
//
// Включается MATRIXGAME_NET=1, лог — MATRIXGAME_NETLOG.

struct SCommand;

// Включён ли сетевой режим (MATRIXGAME_NET).
bool NetIsActive(void);
// Сторона, которой управляет партнёр (-1, пока не пришёл HELLO).
int NetRemoteSide(void);
// Сторона, которой управляет этот процесс.
int NetLocalSide(void);

void NetInit(void);      // из MatrixGameInit, после CmdInit
void NetShutdown(void);  // из MatrixGameDeinit

// Принять пакеты и отправить накопленное. Вызывается и в замороженном
// состоянии: иначе из заморозки не выйти.
void NetPump(void);

// Можно ли начинать такт tick.
bool NetCanAdvance(int tick);

// Локальный приказ уходит партнёру.
void NetSendCommand(const SCommand &cmd);

// Мир досчитан до такта tick: отправляем ACK и, если пора, хеш.
void NetOnTickApplied(int tick);

#endif  // NETPLAY_HPP
