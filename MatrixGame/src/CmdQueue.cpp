// MatrixGame - SR2 Planetary battles engine
// Copyright (C) 2012, Elemental Games, Katauri Interactive, CHK-Games
// Licensed under GPLv2 or any later version
// Refer to the LICENSE file included

#include "stdafx.h"

#include "SimHarness.hpp"
#include "CmdQueue.hpp"
#include "MatrixMap.hpp"
#include "MatrixLogic.hpp"
#include "MatrixSide.hpp"
#include "MatrixRobot.hpp"
#include "MatrixMapStatic.hpp"
#include "MatrixObjectBuilding.hpp"
#include "MatrixObjectCannon.hpp"
#include "RndSplit.hpp"
#include "Logic/MatrixAIGroup.h"
#include "Logic/MatrixRoadNetwork.hpp"

#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace {

std::vector<SCommand> g_Queue;

FILE *g_Log = NULL;
bool g_Replay = false;
bool g_InitDone = false;
unsigned g_Seq = 0;
int g_IssuedCnt = 0;
int g_AppliedCnt = 0;

const char *CmdTypeName(int t) {
    switch (t) {
        case CMD_STOP:
            return "STOP";
        case CMD_MOVE_TO:
            return "MOVE";
        case CMD_ATTACK:
            return "ATTACK";
        case CMD_CAPTURE:
            return "CAPTURE";
        case CMD_PATROL:
            return "PATROL";
        case CMD_REPAIR:
            return "REPAIR";
        case CMD_BOMB:
            return "BOMB";
        case CMD_AUTO_CAPTURE:
            return "AUTOCAP";
        case CMD_AUTO_ATTACK:
            return "AUTOATK";
        case CMD_AUTO_DEFENCE:
            return "AUTODEF";
    }
    return "NONE";
}

int CmdTypeByName(const char *name) {
    for (int i = CMD_NONE + 1; i < CMD_TYPE_MAX; ++i) {
        if (strcmp(name, CmdTypeName(i)) == 0)
            return i;
    }
    return CMD_NONE;
}

// Порядок применения: сначала по такту, потом по стороне, потом по номеру.
// Для одного матча этого достаточно, а для сети это ровно то правило, по
// которому хост разложит команды обеих сторон.
bool CmdLess(const SCommand &a, const SCommand &b) {
    if (a.m_Tick != b.m_Tick)
        return a.m_Tick < b.m_Tick;
    if (a.m_Side != b.m_Side)
        return a.m_Side < b.m_Side;
    return a.m_Seq < b.m_Seq;
}

const wchar_t *CmdEnvStr(const wchar_t *name) {
    static wchar_t buf[4][260];
    static int slot = 0;

    wchar_t *b = buf[slot];
    slot = (slot + 1) & 3;

    DWORD n = GetEnvironmentVariableW(name, b, 260);
    if (n == 0 || n >= 260)
        return NULL;
    return b;
}

void CmdWarn(const char *msg) {
#ifdef MATRIXGAME_SIM_HARNESS
    SimTrace(msg);
#else
    (void)msg;
#endif
    if (g_Log) {
        fprintf(g_Log, "# WARNING: %s\n", msg);
        fflush(g_Log);
    }
}

void CmdLogHeader(void) {
    if (!g_Log || !g_MatrixMap)
        return;

    char mapname[260];
    mapname[0] = 0;
    const wchar_t *mn = g_MatrixMap->MapName();
    if (mn)
        WideCharToMultiByte(CP_ACP, 0, mn, -1, mapname, sizeof(mapname), NULL, NULL);

    fprintf(g_Log, "# MatrixGame command log v1\n");
    fprintf(g_Log, "# Apply tick = command tick + %d. All coordinates are map cells.\n", CMD_INPUT_LAG_TICKS);
    fprintf(g_Log, "v=1\n");
    fprintf(g_Log, "map=%s\n", mapname);
    fprintf(g_Log, "seed=%u\n", RndMatchSeed());
    fprintf(g_Log, "lag=%d\n", CMD_INPUT_LAG_TICKS);
    fflush(g_Log);
}

void CmdLogCommand(const SCommand &c) {
    if (!g_Log)
        return;

    fprintf(g_Log, "t=%d side=%d type=%s obj=%d x=%d y=%d flags=%d selcnt=%d sel=", c.m_Tick, c.m_Side,
            CmdTypeName(c.m_Type), c.m_ObjId, c.m_X, c.m_Y, c.m_Flags, c.m_SelCnt);
    for (int i = 0; i < c.m_SelCnt; ++i)
        fprintf(g_Log, "%s%d", i ? "," : "", c.m_Sel[i]);
    fprintf(g_Log, " seq=%u\n", c.m_Seq);
    fflush(g_Log);
}

// Разбор одной строки приказа: набор пар key=value в любом порядке.
bool CmdParseLine(char *line, SCommand &c) {
    ZeroMemory(&c, sizeof(c));
    c.m_ObjId = -1;

    for (char *tok = strtok(line, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
        char *eq = strchr(tok, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *key = tok;
        const char *val = eq + 1;

        if (strcmp(key, "t") == 0)
            c.m_Tick = atoi(val);
        else if (strcmp(key, "side") == 0)
            c.m_Side = atoi(val);
        else if (strcmp(key, "type") == 0)
            c.m_Type = CmdTypeByName(val);
        else if (strcmp(key, "obj") == 0)
            c.m_ObjId = atoi(val);
        else if (strcmp(key, "x") == 0)
            c.m_X = atoi(val);
        else if (strcmp(key, "y") == 0)
            c.m_Y = atoi(val);
        else if (strcmp(key, "flags") == 0)
            c.m_Flags = atoi(val);
        else if (strcmp(key, "seq") == 0)
            c.m_Seq = (unsigned)atoi(val);
        else if (strcmp(key, "sel") == 0) {
            int n = 0;
            for (const char *p = val; *p && n < CMD_SEL_MAX;) {
                while (*p == ',')
                    ++p;
                if (!*p)
                    break;
                c.m_Sel[n++] = atoi(p);
                while (*p && *p != ',')
                    ++p;
            }
            if (n > c.m_SelCnt)
                c.m_SelCnt = n;
        }
    }

    if (c.m_SelCnt < 0)
        c.m_SelCnt = 0;
    if (c.m_SelCnt > CMD_SEL_MAX)
        c.m_SelCnt = CMD_SEL_MAX;

    return c.m_Type > CMD_NONE && c.m_Type < CMD_TYPE_MAX;
}

CMatrixSideUnit *CmdFindSide(int id) {
    if (!g_MatrixMap)
        return NULL;
    for (int i = 0; i < g_MatrixMap->m_SideCnt; ++i) {
        if (g_MatrixMap->m_Side[i].m_Id == id)
            return &g_MatrixMap->m_Side[i];
    }
    return NULL;
}

CMatrixMapStatic *CmdFindObject(int id) {
    if (id < 0)
        return NULL;
    for (CMatrixMapStatic *obj = CMatrixMapStatic::GetFirstLogic(); obj; obj = obj->GetNextLogic()) {
        if ((int)obj->GetId() == id)
            return obj;
    }
    return NULL;
}

// Замена CMatrixSideUnit::SelGroupToLogicGroup на стороне применения: та же
// логика, но роботы берутся по id из команды, а не из локального выделения.
int CmdAssignGroup(CMatrixSideUnit *su, const SCommand &c) {
    for (int i = 0; i < MAX_LOGIC_GROUP; ++i)
        su->m_PlayerGroup[i].m_RobotCnt = 0;

    for (CMatrixMapStatic *obj = CMatrixMapStatic::GetFirstLogic(); obj; obj = obj->GetNextLogic()) {
        if (obj->IsLiveRobot() && obj->GetSide() == su->m_Id) {
            int g = obj->AsRobot()->GetGroupLogic();
            if (g >= 0 && g < MAX_LOGIC_GROUP)
                su->m_PlayerGroup[g].m_RobotCnt++;
        }
    }

    int no = 0;
    while (no < MAX_LOGIC_GROUP && su->m_PlayerGroup[no].m_RobotCnt > 0)
        ++no;
    if (no >= MAX_LOGIC_GROUP)
        return -1;

    su->m_PlayerGroup[no].Order(mpo_Stop);
    su->m_PlayerGroup[no].m_Obj = NULL;
    su->m_PlayerGroup[no].SetWar(false);
    su->m_PlayerGroup[no].m_RoadPath->Clear();

    int cnt = 0;
    for (int k = 0; k < c.m_SelCnt; ++k) {
        CMatrixMapStatic *obj = CmdFindObject(c.m_Sel[k]);
        if (obj && obj->IsLiveRobot() && obj->GetSide() == su->m_Id) {
            obj->AsRobot()->SetGroupLogic(no);
            su->m_PlayerGroup[no].m_RobotCnt++;
            ++cnt;
        }
    }

    return cnt > 0 ? no : -1;
}

// SelectedGroupBreakOrders, но по списку роботов из команды.
void CmdBreakOrders(const SCommand &c) {
    for (int k = 0; k < c.m_SelCnt; ++k) {
        CMatrixMapStatic *obj = CmdFindObject(c.m_Sel[k]);
        if (obj && obj->IsLiveRobot())
            obj->AsRobot()->BreakAllOrders();
    }
}

void CmdApply(const SCommand &c) {
    if (!g_MatrixMap)
        return;

    CMatrixSideUnit *su = CmdFindSide(c.m_Side);
    if (!su || su->GetStatus() == SS_NONE)
        return;

    int no = CmdAssignGroup(su, c);
    if (no < 0)
        return;

    CPoint tp(c.m_X, c.m_Y);
    CMatrixMapStatic *target = CmdFindObject(c.m_ObjId);

    switch (c.m_Type) {
        case CMD_STOP:
            su->PGOrderStop(no);
            break;
        case CMD_MOVE_TO:
            su->PGOrderMoveTo(no, tp);
            break;
        case CMD_ATTACK:
            // Цель могла умереть за время лага — тогда бьём по точке, как если
            // бы игрок кликнул по земле.
            su->PGOrderAttack(no, tp, target);
            break;
        case CMD_CAPTURE:
            if (target && target->IsLiveBuilding())
                su->PGOrderCapture(no, (CMatrixBuilding *)target);
            else
                su->PGOrderMoveTo(no, tp);
            break;
        case CMD_PATROL:
            su->PGOrderPatrol(no, tp);
            break;
        case CMD_REPAIR:
            if (target && target->IsLive())
                su->PGOrderRepair(no, target);
            else
                su->PGOrderMoveTo(no, tp);
            break;
        case CMD_BOMB:
            su->PGOrderBomb(no, tp, target);
            break;
        case CMD_AUTO_CAPTURE:
            su->PGOrderAutoCapture(no);
            break;
        case CMD_AUTO_ATTACK:
            su->PGOrderAutoAttack(no);
            break;
        case CMD_AUTO_DEFENCE:
            su->PGOrderAutoDefence(no);
            break;
        default:
            break;
    }

    if (FLAG(c.m_Flags, CMD_F_BREAK_ORDERS))
        CmdBreakOrders(c);
}

void CmdLoadReplay(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rt");
    if (!f) {
        CmdWarn("MATRIXGAME_CMDREPLAY: cannot open file");
        return;
    }

    char line[4096];
    char wantMap[260];
    char wantMapName[260];
    unsigned wantSeed = 0;
    int wantLag = 0;
    int lines = 0;

    wantMap[0] = 0;
    wantMapName[0] = 0;
    if (g_MatrixMap && g_MatrixMap->MapName())
        WideCharToMultiByte(CP_ACP, 0, g_MatrixMap->MapName(), -1, wantMapName, sizeof(wantMapName), NULL, NULL);

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == 0)
            continue;

        if (strncmp(p, "t=", 2) == 0) {
            SCommand c;
            if (CmdParseLine(p, c)) {
                g_Queue.push_back(c);
                ++lines;
            }
            continue;
        }

        char key[64];
        char val[260];
        key[0] = val[0] = 0;
        if (sscanf(p, "%63[^=]=%259[^\r\n]", key, val) == 2) {
            if (strcmp(key, "map") == 0)
                strncpy(wantMap, val, sizeof(wantMap) - 1);
            else if (strcmp(key, "seed") == 0)
                wantSeed = (unsigned)_strtoui64(val, NULL, 10);
            else if (strcmp(key, "lag") == 0)
                wantLag = atoi(val);
        }
    }
    fclose(f);

    if (wantMap[0] && wantMapName[0] && _stricmp(wantMap, wantMapName) != 0)
        CmdWarn("command replay: map name differs from the current one");
    if (wantSeed != 0 && wantSeed != RndMatchSeed())
        CmdWarn("command replay: seed differs from the current one");
    if (wantLag != 0 && wantLag != CMD_INPUT_LAG_TICKS)
        CmdWarn("command replay: input lag differs from the build");

    std::sort(g_Queue.begin(), g_Queue.end(), CmdLess);

    g_Seq = 0;
    for (size_t i = 0; i < g_Queue.size(); ++i) {
        if (g_Queue[i].m_Seq > g_Seq)
            g_Seq = g_Queue[i].m_Seq;
    }

    g_Replay = true;

    char msg[128];
    sprintf(msg, "command replay loaded: %d commands, last tick %d", lines,
            g_Queue.empty() ? 0 : g_Queue.back().m_Tick);
    CmdWarn(msg);
}

}  // namespace

void CmdInit(void) {
    if (g_InitDone)
        return;
    g_InitDone = true;

    g_Queue.clear();
    g_Seq = 0;
    g_IssuedCnt = 0;
    g_AppliedCnt = 0;

    const wchar_t *logPath = CmdEnvStr(L"MATRIXGAME_CMDLOG");
    const wchar_t *replayPath = CmdEnvStr(L"MATRIXGAME_CMDREPLAY");

    if (logPath) {
        g_Log = _wfopen(logPath, L"wt");
        if (!g_Log)
            CmdWarn("MATRIXGAME_CMDLOG: cannot open file");
    }

    if (replayPath)
        CmdLoadReplay(replayPath);

    CmdLogHeader();

    char msg[160];
    sprintf(msg, "command layer: log=%d replay=%d seed=%u", g_Log ? 1 : 0, g_Replay ? 1 : 0, RndMatchSeed());
    CmdWarn(msg);
#ifdef MATRIXGAME_SIM_HARNESS
    sprintf(msg, "command bot enabled=%d", SimCommandBotEnabled() ? 1 : 0);
    CmdWarn(msg);
#endif
}

void CmdShutdown(void) {
    if (!g_InitDone)
        return;

    if (g_Log) {
        fprintf(g_Log, "# applied=%d issued=%d replay=%d\n", g_AppliedCnt, g_IssuedCnt, g_Replay ? 1 : 0);
        fclose(g_Log);
        g_Log = NULL;
    }

    g_Queue.clear();
    g_Seq = 0;
    g_Replay = false;
    g_InitDone = false;
}

void CmdIssue(SCommand &cmd) {
    if (!g_InitDone || !g_MatrixMap)
        return;
    // В режиме реплея команды приходят из файла: новые не принимаем, иначе
    // прогон перестал бы быть воспроизведением.
    if (g_Replay)
        return;

    if (cmd.m_Tick <= 0)
        cmd.m_Tick = CmdCurrentTick() + CMD_INPUT_LAG_TICKS;

    cmd.m_Seq = ++g_Seq;
    if (cmd.m_SelCnt < 0)
        cmd.m_SelCnt = 0;
    if (cmd.m_SelCnt > CMD_SEL_MAX)
        cmd.m_SelCnt = CMD_SEL_MAX;

    g_Queue.push_back(cmd);
    ++g_IssuedCnt;

    CmdLogCommand(cmd);
}

void CmdTick(int tick) {
    if (!g_InitDone)
        return;

#ifdef MATRIXGAME_SIM_HARNESS
    // Скриптовый «игрок» для проверки реплея: ставит команды через тот же
    // CmdIssue, что и настоящий ввод.
    SimCommandBotTick(tick);
#endif

    if (g_Queue.empty())
        return;

    size_t dueEnd = 0;
    while (dueEnd < g_Queue.size() && g_Queue[dueEnd].m_Tick <= tick)
        ++dueEnd;
    if (dueEnd == 0)
        return;

    std::vector<SCommand> due(g_Queue.begin(), g_Queue.begin() + (std::vector<SCommand>::difference_type)dueEnd);
    g_Queue.erase(g_Queue.begin(), g_Queue.begin() + (std::vector<SCommand>::difference_type)dueEnd);

    std::sort(due.begin(), due.end(), CmdLess);

    for (size_t i = 0; i < due.size(); ++i) {
        CmdApply(due[i]);
        ++g_AppliedCnt;
    }
}

int CmdCurrentTick(void) {
    if (!g_MatrixMap)
        return 0;
    return g_MatrixMap->GetTime() / LOGIC_TAKT_PERIOD;
}

bool CmdReplayMode(void) {
    return g_Replay;
}

bool CmdLogEnabled(void) {
    return g_Log != NULL;
}

int CmdCaptureSelection(CMatrixSideUnit *su, SCommand &cmd) {
    cmd.m_SelCnt = 0;
    if (!su)
        return 0;

    cmd.m_Side = su->m_Id;

    CMatrixGroup *group = su->GetCurGroup();
    if (!group)
        return 0;

    for (CMatrixGroupObject *obj = group->m_FirstObject; obj && cmd.m_SelCnt < CMD_SEL_MAX; obj = obj->m_NextObject) {
        CMatrixMapStatic *o = obj->GetObject();
        if (o && o->IsLiveRobot() && o->GetSide() == su->m_Id)
            cmd.m_Sel[cmd.m_SelCnt++] = (int)o->GetId();
    }

    return cmd.m_SelCnt;
}

int CmdOrder(CMatrixSideUnit *su, int type, const CPoint &tp, CMatrixMapStatic *target, bool breakOrders) {
    if (!g_InitDone || !g_MatrixMap || !su)
        return -1;

    SCommand cmd;
    ZeroMemory(&cmd, sizeof(cmd));
    cmd.m_ObjId = -1;
    cmd.m_Type = type;
    cmd.m_X = tp.x;
    cmd.m_Y = tp.y;
    if (target)
        cmd.m_ObjId = (int)target->GetId();
    if (breakOrders)
        cmd.m_Flags |= CMD_F_BREAK_ORDERS;

    if (CmdCaptureSelection(su, cmd) <= 0)
        return -1;

    CmdIssue(cmd);
    return cmd.m_Tick;
}

CPoint CmdObjectPos(CMatrixMapStatic *obj) {
    if (!obj)
        return CPoint(0, 0);

    switch (obj->GetObjectType()) {
        case OBJECT_TYPE_ROBOTAI:
            return CPoint(obj->AsRobot()->GetMapPosX(), obj->AsRobot()->GetMapPosY());
        case OBJECT_TYPE_CANNON:
            return CPoint(int(obj->AsCannon()->m_Pos.x / GLOBAL_SCALE_MOVE),
                          int(obj->AsCannon()->m_Pos.y / GLOBAL_SCALE_MOVE));
        case OBJECT_TYPE_BUILDING:
            return CPoint(int(obj->AsBuilding()->m_Pos.x / GLOBAL_SCALE_MOVE),
                          int(obj->AsBuilding()->m_Pos.y / GLOBAL_SCALE_MOVE));
        default:
            break;
    }

    D3DXVECTOR3 c = obj->GetGeoCenter();
    return CPoint(int(c.x / GLOBAL_SCALE_MOVE), int(c.y / GLOBAL_SCALE_MOVE));
}

int CmdOrderObject(CMatrixSideUnit *su, int type, CMatrixMapStatic *target, bool breakOrders) {
    CPoint tp(0, 0);
    if (target)
        tp = CmdObjectPos(target);
    return CmdOrder(su, type, tp, target, breakOrders);
}
