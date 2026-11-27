// MatrixGame - SR2 Planetary battles engine
// Copyright (C) 2012, Elemental Games, Katauri Interactive, CHK-Games
// Licensed under GPLv2 or any later version
// Refer to the LICENSE file included

// winsock.h (v1) приходит из windows.h; winsock2.h включать нельзя — он
// объявляет typedef GROUP, а в проекте GROUP — перечислитель ESelType.
#include "stdafx.h"

#include "NetPlay.hpp"

#include "CmdQueue.hpp"
#include "MatrixMap.hpp"
#include "MatrixMapStatic.hpp"
#include "MatrixRobot.hpp"
#include "MatrixObjectBuilding.hpp"
#include "MatrixObjectCannon.hpp"
#include "MatrixLogic.hpp"
#include "MatrixGameDll.hpp"
#include "RndSplit.hpp"

#pragma comment(lib, "ws2_32.lib")

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

// 'MGNP'
#define NET_MAGIC   0x504E474D
#define NET_VERSION 1

enum ENetMsg {
    NET_HELLO = 1,
    NET_HELLO_ACK = 2,
    NET_CMD = 3,
    NET_ACK = 4,
    NET_READY = 5,
    NET_HASH = 6,
};

#define NET_MAX_PACKET 2048
#define NET_RING       512
#define NET_RESEND_MS  100
#define NET_STATE_MS   50

struct SNetHeader {
    unsigned m_Magic;
    unsigned short m_Version;
    unsigned short m_Type;
    unsigned m_Len;
};

struct SBuf {
    unsigned char m_Data[NET_MAX_PACKET];
    int m_Len;

    void Reset(void) { m_Len = 0; }
    bool Room(int n) const { return m_Len + n <= NET_MAX_PACKET; }
    void U16(unsigned v) {
        m_Data[m_Len++] = (unsigned char)(v & 0xFF);
        m_Data[m_Len++] = (unsigned char)((v >> 8) & 0xFF);
    }
    void U32(unsigned v) {
        for (int i = 0; i < 4; ++i)
            m_Data[m_Len++] = (unsigned char)((v >> (i * 8)) & 0xFF);
    }
    void I32(int v) { U32((unsigned)v); }
    void U64(unsigned long long v) {
        for (int i = 0; i < 8; ++i)
            m_Data[m_Len++] = (unsigned char)((v >> (i * 8)) & 0xFF);
    }
};

struct SReader {
    const unsigned char *m_Data;
    int m_Len;
    int m_Pos;

    bool Has(int n) const { return m_Pos + n <= m_Len; }
    unsigned U16(void) {
        unsigned v = (unsigned)m_Data[m_Pos] | ((unsigned)m_Data[m_Pos + 1] << 8);
        m_Pos += 2;
        return v;
    }
    unsigned U32(void) {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i)
            v |= ((unsigned)m_Data[m_Pos + i]) << (i * 8);
        m_Pos += 4;
        return v;
    }
    int I32(void) { return (int)U32(); }
    unsigned long long U64(void) {
        unsigned long long v = 0;
        for (int i = 0; i < 8; ++i)
            v |= ((unsigned long long)m_Data[m_Pos + i]) << (i * 8);
        m_Pos += 8;
        return v;
    }
};

struct SPending {
    unsigned m_Seq;
    int m_Len;
    unsigned long long m_LastSend;
    unsigned char m_Data[NET_MAX_PACKET];
};

struct SHashSlot {
    int m_Tick;
    unsigned long long m_Hash;
};

SOCKET g_Sock = INVALID_SOCKET;
bool g_WsaStarted = false;
bool g_Active = false;
bool g_Connected = false;
bool g_Stopped = false;
bool g_InitDone = false;

sockaddr_in g_Peer;
bool g_PeerKnown = false;
bool g_PeerConfigured = false;  // адрес партнёра задан в MATRIXGAME_NET_PEER
unsigned long long g_LastHelloMs = 0;

int g_LocalSide = 0;
int g_PeerSide = -1;
int g_PeerReadyTick = -1;  // до какого такта партнёр всё отправил
int g_AppliedTick = 0;
unsigned g_PeerBatch = 0;        // последняя принятая пачка партнёра
unsigned g_PeerBatchContig = 0;  // принято без пропусков, до этой включительно
unsigned g_SeenBatch[64];
int g_SeenPos = 0;
unsigned g_PeerAckBatch = 0;  // подтверждённая партнёром наша пачка

unsigned g_OutSeq = 1;
unsigned g_LocalBatchSeq = 0;

std::vector<SCommand> g_PendingCmds;
std::vector<SPending> g_Pending;
SHashSlot g_Ring[NET_RING];

int g_HashPeriod = 50;
int g_WaitMs = 60000;
bool g_StopOnDesync = false;
unsigned long long g_StartMs = 0;
unsigned long long g_LastStateMs = 0;
bool g_StateDirty = true;

unsigned long long g_SentPackets = 0;
unsigned long long g_RecvPackets = 0;
unsigned long long g_Resends = 0;
unsigned long long g_HashesOk = 0;
unsigned long long g_HashesBad = 0;
unsigned long long g_Desyncs = 0;

FILE *g_Log = NULL;
FILE *g_HashLog = NULL;

const unsigned char *g_In = NULL;
int g_InLen = 0;

unsigned long long NowMs(void) {
    return (unsigned long long)GetTickCount();
}

const wchar_t *EnvStr(const wchar_t *name) {
    static wchar_t buf[4][260];
    static int slot = 0;

    wchar_t *b = buf[slot];
    slot = (slot + 1) & 3;

    DWORD n = GetEnvironmentVariableW(name, b, 260);
    if (n == 0 || n >= 260)
        return NULL;
    return b;
}

int EnvInt(const wchar_t *name, int fallback) {
    const wchar_t *v = EnvStr(name);
    return v ? _wtoi(v) : fallback;
}

void Log(const char *fmt, ...) {
    if (!g_Log)
        return;

    va_list ap;
    va_start(ap, fmt);
    fprintf(g_Log, "[%6llu] ", NowMs() - g_StartMs);
    vfprintf(g_Log, fmt, ap);
    va_end(ap);
    fprintf(g_Log, "\n");
    fflush(g_Log);
}

void SendRaw(unsigned type, const unsigned char *payload, int len) {
    if (g_Sock == INVALID_SOCKET || !g_PeerKnown)
        return;

    unsigned char packet[NET_MAX_PACKET];
    if (len + (int)sizeof(SNetHeader) > NET_MAX_PACKET)
        len = NET_MAX_PACKET - (int)sizeof(SNetHeader);

    SNetHeader h;
    h.m_Magic = NET_MAGIC;
    h.m_Version = NET_VERSION;
    h.m_Type = (unsigned short)type;
    h.m_Len = (unsigned)len;

    memcpy(packet, &h, sizeof(h));
    if (len > 0)
        memcpy(packet + sizeof(h), payload, len);

    if (sendto(g_Sock, (const char *)packet, (int)sizeof(h) + len, 0, (const sockaddr *)&g_Peer, sizeof(g_Peer)) !=
        SOCKET_ERROR) {
        ++g_SentPackets;
    }
}

void SendState(void) {
    SBuf b;
    b.Reset();
    b.U32(g_PeerBatchContig);
    b.I32(g_AppliedTick);
    SendRaw(NET_ACK, b.m_Data, b.m_Len);

    g_LastStateMs = NowMs();
    g_StateDirty = false;
}

void FlushCommands(void) {
    if (g_PendingCmds.empty())
        return;

    SBuf b;
    b.Reset();
    unsigned seq = ++g_LocalBatchSeq;
    b.U32(seq);

    size_t cntPos = (size_t)b.m_Len;
    b.U32(0);

    unsigned cnt = 0;
    for (size_t i = 0; i < g_PendingCmds.size(); ++i) {
        const SCommand &c = g_PendingCmds[i];

        if (!b.Room(28 + CMD_PARAM_CNT * 4 + c.m_SelCnt * 4))
            break;  // остальное уйдёт следующей пачкой

        b.U16((unsigned)c.m_Type);
        b.U16((unsigned)c.m_Flags);
        b.U16((unsigned)c.m_Side);
        b.U16((unsigned)c.m_SelCnt);
        b.U32((unsigned)c.m_Tick);
        b.I32(c.m_ObjId);
        b.I32(c.m_X);
        b.I32(c.m_Y);
        b.U32(c.m_Seq);
        for (int k = 0; k < CMD_PARAM_CNT; ++k)
            b.I32(c.m_Param[k]);
        for (int k = 0; k < c.m_SelCnt; ++k)
            b.U32((unsigned)c.m_Sel[k]);

        ++cnt;
    }

    if (cnt == 0) {
        // даже одна команда не влезла — такого быть не должно, но не зацикливаемся
        g_PendingCmds.clear();
        return;
    }

    memcpy(b.m_Data + cntPos, &cnt, 4);

    SPending p;
    p.m_Seq = seq;
    p.m_Len = (int)sizeof(SNetHeader) + b.m_Len;
    p.m_LastSend = NowMs();

    SNetHeader h;
    h.m_Magic = NET_MAGIC;
    h.m_Version = NET_VERSION;
    h.m_Type = (unsigned short)NET_CMD;
    h.m_Len = (unsigned)b.m_Len;
    memcpy(p.m_Data, &h, sizeof(h));
    memcpy(p.m_Data + sizeof(h), b.m_Data, b.m_Len);

    if (p.m_Len <= NET_MAX_PACKET) {
        g_Pending.push_back(p);
        sendto(g_Sock, (const char *)p.m_Data, p.m_Len, 0, (const sockaddr *)&g_Peer, sizeof(g_Peer));
        ++g_SentPackets;
    }

    g_PendingCmds.erase(g_PendingCmds.begin(), g_PendingCmds.begin() + cnt);
    if (!g_PendingCmds.empty())
        FlushCommands();
}

void ResendPending(void) {
    const unsigned long long now = NowMs();

    for (size_t i = 0; i < g_Pending.size(); ++i) {
        SPending &p = g_Pending[i];
        if (p.m_Seq <= g_PeerAckBatch)
            continue;
        if (now - p.m_LastSend < NET_RESEND_MS)
            continue;

        sendto(g_Sock, (const char *)p.m_Data, p.m_Len, 0, (const sockaddr *)&g_Peer, sizeof(g_Peer));
        p.m_LastSend = now;
        ++g_Resends;
        g_StateDirty = true;
    }

    // подтверждённые пачки больше не нужны
    size_t keep = 0;
    for (size_t i = 0; i < g_Pending.size(); ++i) {
        if (g_Pending[i].m_Seq > g_PeerAckBatch)
            g_Pending[keep++] = g_Pending[i];
    }
    g_Pending.resize(keep);
}

unsigned long long FloatBits(float f) {
    unsigned u;
    memcpy(&u, &f, sizeof(u));
    return (unsigned long long)u;
}

void Mix(unsigned long long &h, unsigned long long v) {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
}

// Хеш мира для сверки машин. Достаточно чувствительный, чтобы поймать
// расхождение позиции или здоровья, но не зависит от графики: берём только
// то, что живёт в логике.
unsigned long long WorldHash(void) {
    unsigned long long h = 0xCBF29CE484222325ULL;

    Mix(h, (unsigned long long)(unsigned)g_MatrixMap->GetTime());

    int cnt = 0;
    for (CMatrixMapStatic *o = CMatrixMapStatic::GetFirstLogic(); o; o = o->GetNextLogic()) {
        Mix(h, (unsigned long long)o->GetId());
        Mix(h, (unsigned long long)(unsigned)o->GetObjectType());
        Mix(h, (unsigned long long)(unsigned)o->GetSide());

        if (o->IsRobot()) {
            CMatrixRobotAI *r = o->AsRobot();
            Mix(h, (unsigned long long)(unsigned)r->GetMapPosX());
            Mix(h, (unsigned long long)(unsigned)r->GetMapPosY());
            Mix(h, FloatBits(r->GetGeoCenter().x));
            Mix(h, FloatBits(r->GetGeoCenter().y));
            Mix(h, FloatBits(r->GetHitPoint()));
            Mix(h, (unsigned long long)(unsigned)r->GetGroupLogic());
        }
        else if (o->IsBuilding()) {
            Mix(h, FloatBits(o->GetGeoCenter().x));
            Mix(h, FloatBits(o->GetGeoCenter().y));
            Mix(h, FloatBits(o->AsBuilding()->GetHitPoint()));
        }
        else if (o->IsCannon()) {
            Mix(h, FloatBits(o->GetGeoCenter().x));
            Mix(h, FloatBits(o->GetGeoCenter().y));
        }

        ++cnt;
    }

    Mix(h, (unsigned long long)(unsigned)cnt);
    return h;
}

void HandleHello(SReader &r, const sockaddr_in &from) {
    unsigned side = r.U32();
    unsigned seed = r.U32();

    g_Peer = from;
    g_PeerKnown = true;

    SBuf b;
    b.Reset();

    if (seed != RndMatchSeed()) {
        b.U32(0);
        b.U32(RndMatchSeed());
        b.U32(LOGIC_TAKT_PERIOD);
        b.U32(CMD_INPUT_LAG_TICKS);
        b.U32((unsigned)g_LocalSide);
        SendRaw(NET_HELLO_ACK, b.m_Data, b.m_Len);
        Log("HELLO rejected: peer seed %u, local seed %u", seed, RndMatchSeed());
        return;
    }

    g_PeerSide = (int)side;
    const bool first = !g_Connected;
    g_Connected = true;

    b.U32(1);
    b.U32(RndMatchSeed());
    b.U32(LOGIC_TAKT_PERIOD);
    b.U32(CMD_INPUT_LAG_TICKS);
    b.U32((unsigned)g_LocalSide);
    SendRaw(NET_HELLO_ACK, b.m_Data, b.m_Len);

    if (first)
        Log("HELLO from %s, peer side %d, seed %u", inet_ntoa(from.sin_addr), g_PeerSide, seed);
}

void HandleHelloAck(SReader &r, const sockaddr_in &from) {
    unsigned ok = r.U32();
    unsigned seed = r.U32();
    (void)r.U32();  // период такта
    (void)r.U32();  // лаг ввода
    unsigned side = r.U32();

    if (!ok) {
        Log("HELLO rejected by host: seed %u, local seed %u", seed, RndMatchSeed());
        g_Stopped = true;
        g_ExitState = 1;
        SETFLAG(g_Flags, GFLAG_EXITLOOP);
        return;
    }

    if (g_Connected)
        return;

    g_Peer = from;
    g_PeerKnown = true;
    g_PeerSide = (int)side;
    g_Connected = true;

    Log("HELLO_ACK from %s, peer side %d, seed %u", inet_ntoa(from.sin_addr), g_PeerSide, seed);
}

void HandleCmd(SReader &r) {
    unsigned batch = r.U32();
    unsigned cnt = r.U32();

    // Повторная пачка (наш ACK потерялся и партнёр переслал) не должна
    // применить те же приказы второй раз.
    bool dup = false;
    for (int i = 0; i < 64; ++i) {
        if (batch != 0 && g_SeenBatch[i] == batch)
            dup = true;
    }

    if (dup) {
        g_StateDirty = true;  // подтвердим ещё раз, чтобы партнёр успокоился
        return;
    }

    g_SeenBatch[g_SeenPos++ & 63] = batch;
    g_StateDirty = true;

    if (batch > g_PeerBatch)
        g_PeerBatch = batch;

    // Подтверждаем только непрерывный префикс: иначе потерянная пачка
    // «подтвердится» следующей, и приказы пропадут.
    bool advanced = true;
    while (advanced) {
        advanced = false;
        const unsigned next = g_PeerBatchContig + 1;
        for (int i = 0; i < 64; ++i) {
            if (g_SeenBatch[i] == next) {
                g_PeerBatchContig = next;
                advanced = true;
                break;
            }
        }
    }

    for (unsigned i = 0; i < cnt; ++i) {
        if (!r.Has(28 + CMD_PARAM_CNT * 4))
            break;

        SCommand c;
        ZeroMemory(&c, sizeof(c));
        c.m_Type = (int)r.U16();
        c.m_Flags = (int)r.U16();
        c.m_Side = (int)r.U16();
        c.m_SelCnt = (int)r.U16();
        c.m_Tick = (int)r.U32();
        c.m_ObjId = r.I32();
        c.m_X = r.I32();
        c.m_Y = r.I32();
        c.m_Seq = r.U32();
        for (int k = 0; k < CMD_PARAM_CNT; ++k)
            c.m_Param[k] = r.I32();

        if (c.m_SelCnt < 0)
            c.m_SelCnt = 0;
        if (c.m_SelCnt > CMD_SEL_MAX)
            c.m_SelCnt = CMD_SEL_MAX;
        if (!r.Has(c.m_SelCnt * 4))
            break;
        for (int k = 0; k < c.m_SelCnt; ++k)
            c.m_Sel[k] = (int)r.U32();

        CmdIssueRemote(c);
    }
}

void HandleAck(SReader &r) {
    unsigned ackBatch = r.U32();
    int applied = r.I32();

    if (ackBatch > g_PeerAckBatch) {
        g_PeerAckBatch = ackBatch;
        g_StateDirty = true;
    }

    (void)applied;
}

void HandleReady(SReader &r) {
    int tick = r.I32();
    if (tick > g_PeerReadyTick) {
        g_PeerReadyTick = tick;
        if (g_PeerReadyTick > 0 && g_PeerReadyTick % 1000 == 0)
            Log("READY up to tick %d", g_PeerReadyTick);
    }
}

void HandleHash(SReader &r) {
    unsigned tick = r.U32();
    unsigned long long hash = r.U64();

    SHashSlot &slot = g_Ring[tick % NET_RING];
    if (slot.m_Tick == (int)tick) {
        if (slot.m_Hash == hash) {
            ++g_HashesOk;
        }
        else {
            ++g_HashesBad;
            ++g_Desyncs;
            Log("DESYNC tick=%u our=%016llx peer=%016llx", tick, slot.m_Hash, hash);
            if (g_StopOnDesync) {
                g_Stopped = true;
                g_ExitState = 1;
                SETFLAG(g_Flags, GFLAG_EXITLOOP);
            }
        }
        slot.m_Tick = -1;
    }
}

void Receive(void) {
    while (g_Sock != INVALID_SOCKET) {
        unsigned char buf[NET_MAX_PACKET + 64];
        sockaddr_in from;
        int fromlen = (int)sizeof(from);

        int n = recvfrom(g_Sock, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &fromlen);
        if (n == SOCKET_ERROR || n <= 0)
            break;

        if (n < (int)sizeof(SNetHeader))
            continue;

        SNetHeader h;
        memcpy(&h, buf, sizeof(h));
        if (h.m_Magic != NET_MAGIC || h.m_Version != NET_VERSION)
            continue;
        if (h.m_Len > (unsigned)(n - (int)sizeof(h)))
            continue;

        ++g_RecvPackets;

        SReader r;
        r.m_Data = buf + sizeof(h);
        r.m_Len = (int)h.m_Len;
        r.m_Pos = 0;

        switch (h.m_Type) {
            case NET_HELLO:
                HandleHello(r, from);
                break;
            case NET_HELLO_ACK:
                if (r.Has(20))
                    HandleHelloAck(r, from);
                break;
            case NET_CMD:
                HandleCmd(r);
                break;
            case NET_ACK:
                if (r.Has(8))
                    HandleAck(r);
                break;
            case NET_READY:
                if (r.Has(4))
                    HandleReady(r);
                break;
            case NET_HASH:
                if (r.Has(12))
                    HandleHash(r);
                break;
            default:
                break;
        }
    }
}

}  // namespace

bool NetIsActive(void) {
    return g_Active && !g_Stopped;
}

int NetRemoteSide(void) {
    return g_PeerSide;
}

int NetLocalSide(void) {
    return g_LocalSide;
}

void NetInit(void) {
    if (g_InitDone)
        return;
    g_InitDone = true;

    g_StartMs = NowMs();
    g_LocalSide = EnvInt(L"MATRIXGAME_NET_LOCAL_SIDE", 0);
    g_HashPeriod = EnvInt(L"MATRIXGAME_NET_HASH_PERIOD", 50);
    g_WaitMs = EnvInt(L"MATRIXGAME_NET_WAIT_MS", 60000);
    g_StopOnDesync = EnvInt(L"MATRIXGAME_NET_STOP_ON_DESYNC", 0) != 0;

    for (int i = 0; i < NET_RING; ++i) {
        g_Ring[i].m_Tick = -1;
        g_Ring[i].m_Hash = 0;
    }

    // Лог хешей пишется и без сети: эталонный прогон без сокета даёт ту же
    // последовательность, с которой потом сверяется сетевой прогон.
    const wchar_t *hashPath = EnvStr(L"MATRIXGAME_NETHASHLOG");
    if (hashPath) {
        g_HashLog = _wfopen(hashPath, L"wt");
        if (g_HashLog) {
            fprintf(g_HashLog, "# net world hash (t in ms)\n");
            fflush(g_HashLog);
        }
    }

    const bool netEnabled = EnvStr(L"MATRIXGAME_NET") && EnvStr(L"MATRIXGAME_NET")[0] != L'0';

    // Стороны, за которые играют люди. Список обязан совпадать на обеих
    // машинах: это часть правил матча, а не настройка клиента. Читается и без
    // сокета: офлайн-прогон с двумя «людьми» даёт эталон для сети.
    const wchar_t *sides = EnvStr(L"MATRIXGAME_NET_SIDES");
    if (sides && g_MatrixMap) {
        for (int i = 0; i < g_MatrixMap->m_SideCnt; ++i) {
            const int id = g_MatrixMap->m_Side[i].m_Id;
            bool found = false;
            for (const wchar_t *p = sides; *p && !found;) {
                while (*p == L' ' || *p == L',')
                    ++p;
                if (!*p)
                    break;
                if (_wtoi(p) == id)
                    found = true;
                while (*p && *p != L',')
                    ++p;
            }
            g_MatrixMap->m_Side[i].m_HumanSide = found;
        }
    }

    if (!netEnabled)
        return;

    // MATRIXGAME_NET_PEER=ip:port — этот процесс сам стучится к партнёру.
    const wchar_t *peerW = EnvStr(L"MATRIXGAME_NET_PEER");
    if (peerW) {
        char peerBuf[128];
        peerBuf[0] = 0;
        WideCharToMultiByte(CP_ACP, 0, peerW, -1, peerBuf, sizeof(peerBuf), NULL, NULL);

        char host[64];
        int port = 0;
        host[0] = 0;
        if (sscanf(peerBuf, "%63[^:]:%d", host, &port) >= 1 && host[0]) {
            if (port <= 0)
                port = 27777;
            ZeroMemory(&g_Peer, sizeof(g_Peer));
            g_Peer.sin_family = AF_INET;
            g_Peer.sin_port = htons((unsigned short)port);
            g_Peer.sin_addr.s_addr = inet_addr(host);
            g_PeerKnown = true;
            g_PeerConfigured = true;
        }
    }

    const wchar_t *logPath = EnvStr(L"MATRIXGAME_NETLOG");
    if (logPath) {
        g_Log = _wfopen(logPath, L"wt");
        if (g_Log) {
            fprintf(g_Log, "# MatrixGame net log\n");
            fprintf(g_Log, "# local side=%d seed=%u\n", g_LocalSide, RndMatchSeed());
            fflush(g_Log);
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(1, 1), &wsa) != 0) {
        Log("WSAStartup failed");
        return;
    }
    g_WsaStarted = true;

    g_Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_Sock == INVALID_SOCKET) {
        Log("socket() failed: %d", WSAGetLastError());
        return;
    }

    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)EnvInt(L"MATRIXGAME_NET_PORT", 27777));

    // По умолчанию слушаем только localhost: тестовый прогон не должен светить
    // сокет в сеть. Для игры между машинами задаётся MATRIXGAME_NET_BIND.
    char bindAddr[64];
    bindAddr[0] = 0;
    const wchar_t *bindW = EnvStr(L"MATRIXGAME_NET_BIND");
    if (bindW)
        WideCharToMultiByte(CP_ACP, 0, bindW, -1, bindAddr, sizeof(bindAddr), NULL, NULL);
    if (bindAddr[0] == 0)
        strcpy(bindAddr, "127.0.0.1");
    addr.sin_addr.s_addr = inet_addr(bindAddr);

    if (bind(g_Sock, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        Log("bind() failed: %d", WSAGetLastError());
        closesocket(g_Sock);
        g_Sock = INVALID_SOCKET;
        return;
    }

    u_long mode = 1;
    ioctlsocket(g_Sock, FIONBIO, &mode);

    for (int i = 0; i < NET_RING; ++i) {
        g_Ring[i].m_Tick = -1;
        g_Ring[i].m_Hash = 0;
    }

    g_Active = true;
    Log("listening on %s:%d", bindAddr, EnvInt(L"MATRIXGAME_NET_PORT", 27777));
}

void NetShutdown(void) {
    if (g_HashLog) {
        fclose(g_HashLog);
        g_HashLog = NULL;
    }

    if (!g_Active) {
        g_InitDone = false;
        return;
    }

    if (g_Log) {
        fprintf(g_Log, "# packets sent=%llu recv=%llu resends=%llu hashes ok=%llu bad=%llu\n", g_SentPackets,
                g_RecvPackets, g_Resends, g_HashesOk, g_HashesBad);
        fclose(g_Log);
        g_Log = NULL;
    }

    if (g_Sock != INVALID_SOCKET) {
        closesocket(g_Sock);
        g_Sock = INVALID_SOCKET;
    }
    if (g_WsaStarted) {
        WSACleanup();
        g_WsaStarted = false;
    }

    g_Active = false;
    g_Connected = false;
    g_Pending.clear();
    g_PendingCmds.clear();
}

void NetPump(void) {
    if (!g_Active || g_Stopped)
        return;

    Receive();

    if (!g_Connected) {
        // Клиент сам представляется хосту, пока не получит HELLO_ACK.
        if (g_PeerConfigured && NowMs() - g_LastHelloMs >= 200) {
            g_LastHelloMs = NowMs();

            SBuf b;
            b.Reset();
            b.U32((unsigned)g_LocalSide);
            b.U32(RndMatchSeed());
            SendRaw(NET_HELLO, b.m_Data, b.m_Len);
        }

        if (g_WaitMs > 0 && NowMs() - g_StartMs > (unsigned long long)g_WaitMs) {
            Log("peer did not connect in %d ms, running offline", g_WaitMs);
            g_Active = false;
        }
        return;
    }

    FlushCommands();
    ResendPending();

    if (g_StateDirty || NowMs() - g_LastStateMs >= NET_STATE_MS)
        SendState();
}

bool NetCanAdvance(int tick) {
    if (!g_Active || g_Stopped)
        return true;
    if (!g_Connected)
        return false;
    return g_PeerReadyTick >= tick;
}

void NetSendCommand(const SCommand &cmd) {
    if (!g_Active || g_Stopped)
        return;

    // Приказы партнёра обратно не отправляем. Остальное уходит: в режиме, где
    // один процесс командует обеими сторонами (-CmdBotAll), наблюдатель должен
    // получить и их тоже, иначе его мир разъедется.
    if (cmd.m_Side == g_PeerSide && g_PeerSide >= 0)
        return;

    g_PendingCmds.push_back(cmd);
    g_StateDirty = true;
}

void NetOnTickApplied(int tick) {
    if (g_Stopped)
        return;

    if (g_Active) {
        g_AppliedTick = tick;
        g_StateDirty = true;
    }

    if (!g_Active && !g_HashLog)
        return;

    if (g_HashPeriod <= 0 || (tick % g_HashPeriod) != 0)
        return;

    const unsigned long long hash = WorldHash();

    if (g_HashLog) {
        fprintf(g_HashLog, "t=%d hash=%016llx\n", tick * LOGIC_TAKT_PERIOD, hash);
        fflush(g_HashLog);
    }

    if (!g_Active)
        return;

    SHashSlot &slot = g_Ring[tick % NET_RING];
    slot.m_Tick = tick;
    slot.m_Hash = hash;

    SBuf b;
    b.Reset();
    b.U32((unsigned)tick);
    b.U64(hash);
    SendRaw(NET_HASH, b.m_Data, b.m_Len);
}
