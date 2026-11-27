// MatrixGame - SR2 Planetary battles engine
// Copyright (C) 2012, Elemental Games, Katauri Interactive, CHK-Games
// Licensed under GPLv2 or any later version
// Refer to the LICENSE file included

#include "stdafx.h"

#include "SimHarness.hpp"
#include "CmdQueue.hpp"
#include "NetPlay.hpp"
#include "MatrixMap.hpp"
#include "MatrixLogic.hpp"
#include "MatrixSide.hpp"
#include "MatrixRobot.hpp"
#include "MatrixMapStatic.hpp"
#include "MatrixObjectBuilding.hpp"
#include "MatrixObjectCannon.hpp"
#include "Interface/CInterface.h"
#include "RndSplit.hpp"
#include "Logic/MatrixAIGroup.h"
#include "Logic/MatrixRoadNetwork.hpp"
#include "Interface/CConstructor.h"

#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace {

std::vector<SCommand> g_Queue;

FILE *g_Log = NULL;
bool g_Replay = false;
bool g_InitDone = false;
unsigned g_SideSeq[CMD_SIDE_MAX] = {0};
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
        case CMD_BUILD_ROBOT:
            return "BUILD";
        case CMD_BUILD_TURRET:
            return "TURRET";
        case CMD_ARCADE_ENTER:
            return "ARCADE_IN";
        case CMD_ARCADE_LEAVE:
            return "ARCADE_OUT";
        case CMD_ARCADE_CONTROL:
            return "ARCADE_CTL";
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

void CmdLogCommand(const SCommand &c, bool remote = false) {
    if (!g_Log)
        return;

    fprintf(g_Log, "t=%d side=%d type=%s obj=%d x=%d y=%d flags=%d selcnt=%d sel=", c.m_Tick, c.m_Side,
            CmdTypeName(c.m_Type), c.m_ObjId, c.m_X, c.m_Y, c.m_Flags, c.m_SelCnt);
    for (int i = 0; i < c.m_SelCnt; ++i)
        fprintf(g_Log, "%s%d", i ? "," : "", c.m_Sel[i]);
    fprintf(g_Log, " seq=%u", c.m_Seq);
    if (c.m_Type == CMD_BUILD_ROBOT || c.m_Type == CMD_BUILD_TURRET) {
        for (int i = 0; i < CMD_PARAM_CNT; ++i)
            fprintf(g_Log, " p%d=%d", i, c.m_Param[i]);
    }
    if (remote)
        fprintf(g_Log, " net=1");
    fprintf(g_Log, "\n");
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
        else if (key[0] == 'p' && key[1] >= '0' && key[1] < '0' + CMD_PARAM_CNT && key[2] == 0)
            c.m_Param[key[1] - '0'] = atoi(val);
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

// Постройка робота по описанию — тем же конструктором, что и у ИИ, и с теми же
// проверками, что у человека в интерфейсе (лимит роботов и цена в ресурсах).
void CmdApplyBuild(CMatrixSideUnit *su, const SCommand &c) {
    if (!g_MatrixMap || !su->m_Constructor)
        return;

    // Без корпуса и брони конструктор считать нечего (и матрица оружия
    // индексируется номером брони с единицы).
    if (c.m_Param[0] <= 0 || c.m_Param[1] <= 0)
        return;

    CMatrixBuilding *base = NULL;
    if (c.m_ObjId >= 0) {
        CMatrixMapStatic *o = CmdFindObject(c.m_ObjId);
        if (o && o->IsLiveBuilding() && o->AsBuilding()->IsBase() && o->GetSide() == su->m_Id)
            base = o->AsBuilding();
    }
    if (!base) {
        for (CMatrixMapStatic *o = CMatrixMapStatic::GetFirstLogic(); o; o = o->GetNextLogic()) {
            if (o->IsLiveBuilding() && o->GetSide() == su->m_Id && o->IsBase()) {
                base = o->AsBuilding();
                break;
            }
        }
    }
    if (!base)
        return;

    CConstructor *con = su->m_Constructor;
    con->SetBase(base);

    // Порядок важен: броня обнуляет оружие, поэтому оружие ставится последним.
    con->OperateUnit(MRT_CHASSIS, (ERobotUnitKind)c.m_Param[0]);
    con->OperateUnit(MRT_ARMOR, (ERobotUnitKind)c.m_Param[1]);
    con->OperateUnit(MRT_HEAD, (ERobotUnitKind)c.m_Param[2]);
    for (int i = 0; i < MAX_WEAPON_CNT; ++i) {
        if (c.m_Param[3 + i])
            con->OperateUnit(MRT_WEAPON, (ERobotUnitKind)c.m_Param[3 + i]);
    }

    int price[MAX_RESOURCES];
    ZeroMemory(price, sizeof(price));
    con->GetConstructionPrice(price);

    const int count = c.m_Param[8] > 0 ? c.m_Param[8] : 1;

    for (int n = 0; n < count; ++n) {
        if (base->m_BS.GetItemsCnt() >= 6)
            break;
        if (su->GetRobotsCnt() + su->GetRobotsInStack() >= su->GetMaxSideRobots())
            break;
        if (!su->IsEnoughResources(price))
            break;

        for (int r = 0; r < MAX_RESOURCES; ++r)
            su->AddResourceAmount(ERes(r), -price[r]);

        con->StackRobot(NULL, 0);
    }
}

// Постройка турели на здании. Раньше пушка создавалась прямо в обработчике
// клика (CMatrixSideUnit::OnLButtonDown), то есть в момент кадра: в сети это
// дало бы турель только у того, кто её поставил.
void CmdApplyTurret(CMatrixSideUnit *su, const SCommand &c) {
    if (!g_MatrixMap || !su)
        return;

    const int num = c.m_Param[0];
    if (num < 1 || num > CANNON_TYPE_CNT)
        return;

    CMatrixMapStatic *obj = CmdFindObject(c.m_ObjId);
    if (!obj || !obj->IsLiveBuilding() || obj->GetSide() != su->m_Id)
        return;

    CMatrixBuilding *parent = obj->AsBuilding();
    if (parent->m_TurretsHave >= parent->m_TurretsMax)
        return;

    const SCannonProps &props = g_Config.m_CannonsProps[num - 1];
    if (!su->IsEnoughResources(props.m_Resources))
        return;

    for (int r = 0; r < MAX_RESOURCES; ++r)
        su->AddResourceAmount(ERes(r), -props.m_Resources[r]);

    CMatrixCannon *ca = g_MatrixMap->StaticAdd<CMatrixCannon>(true);
    ca->m_CurrState = CANNON_UNDER_CONSTRUCTION;
    ca->SetInvulnerability();

    ca->m_Pos.x = (float)c.m_Param[1] / 100.0f;
    ca->m_Pos.y = (float)c.m_Param[2] / 100.0f;
    ca->m_Place = c.m_Param[4];

    ca->SetSide(su->m_Id);
    ca->UnitInit(num);
    ca->m_Angle = c.m_Param[3];
    ca->m_AddH = 0;

    ca->m_ShadowType = SHADOW_STENCIL;
    ca->m_ShadowSize = 128;

    ca->RNeed(MR_Matrix | MR_Graph);
    ca->m_ParentBuilding = parent;
    ca->JoinToGroup();

    parent->m_TurretsHave++;
    ca->SetHitPoint(0);
    parent->m_BS.AddItem(ca);
}

// Приказ на движение для леталок. Раньше это делал обработчик клика в кадре, и
// там же тратились числа логического ГПСЧ (разброс цели), из-за чего движение
// леталок зависело от частоты кадров. Теперь — на такте применения.
void CmdOrderFlyers(const SCommand &c) {
    int flyers = 0;
    for (int k = 0; k < c.m_SelCnt; ++k) {
        CMatrixMapStatic *o = CmdFindObject(c.m_Sel[k]);
        if (o && o->IsFlyer())
            ++flyers;
    }
    if (flyers <= 0)
        return;

    // Разброс тем шире, чем больше леталок в группе (как в прежнем коде).
    int param = flyers - 1;
    if (param > 4)
        param = 4;

    const float cx = (float)c.m_X * GLOBAL_SCALE_MOVE;
    const float cy = (float)c.m_Y * GLOBAL_SCALE_MOVE;

    for (int k = 0; k < c.m_SelCnt; ++k) {
        CMatrixMapStatic *o = CmdFindObject(c.m_Sel[k]);
        if (!o || !o->IsFlyer())
            continue;

        const float x = (float)g_MatrixMap->RndFloat(cx - param * GLOBAL_SCALE_MOVE, cx + param * GLOBAL_SCALE_MOVE);
        const float y = (float)g_MatrixMap->RndFloat(cy - param * GLOBAL_SCALE_MOVE, cy + param * GLOBAL_SCALE_MOVE);

        o->AsFlyer()->SetTarget(D3DXVECTOR2(x, y));
    }
}

// Ручное управление роботом на такте применения: вход/выход из аркады и сам
// поток управления. Раньше всё это делал код, читающий клавиатуру и курсор
// прямо в такте (`CMatrixRobotAI::Takt`), — то есть у каждой машины по-своему.
void CmdApplyArcade(CMatrixSideUnit *su, const SCommand &c) {
    if (!su)
        return;

    if (c.m_Type == CMD_ARCADE_ENTER) {
        if (su->IsArcadeMode())
            return;  // уже в роботе

        CMatrixMapStatic *obj = CmdFindObject(c.m_ObjId);
        if (!obj || !obj->IsLiveRobot() || obj->GetSide() != su->m_Id)
            return;

        su->SetArcadedObject(obj);
        return;
    }

    if (c.m_Type == CMD_ARCADE_LEAVE) {
        if (su->IsArcadeMode())
            su->SetArcadedObject(NULL);
        return;
    }

    // CMD_ARCADE_CONTROL: состояние на этот такт.
    if (!su->IsArcadeMode() || !su->GetArcadedObject() || !su->GetArcadedObject()->IsLiveRobot())
        return;

    CMatrixRobotAI *robot = su->GetArcadedObject()->AsRobot();
    if (!robot->IsLive())
        return;

    const int turn = c.m_Param[0];
    const int move = c.m_Param[1];

    robot->m_ManualTurn = (turn < 0) ? -1 : ((turn > 0) ? 1 : 0);
    robot->m_ManualFire = (c.m_Param[2] != 0);
    robot->m_ManualAimX = c.m_Param[3];
    robot->m_ManualAimY = c.m_Param[4];

    // Ход: приказ «вперёд/назад» ставится на переходе, дальше робота ведёт
    // m_ManualMove в Takt (как раньше делал зажатый шифт клавиши).
    if (move != robot->m_ManualMove) {
        if (move > 0) {
            robot->BreakAllOrders();
            const D3DXVECTOR3 vel = robot->m_Forward * robot->GetMaxSpeed();
            CPoint tp = CmdObjectPos(robot);
            tp.x += Float2Int(vel.x * INVERT(GLOBAL_SCALE_MOVE));
            tp.y += Float2Int(vel.y * INVERT(GLOBAL_SCALE_MOVE));
            robot->MoveTo(tp.x, tp.y);
        }
        else if (move < 0) {
            robot->BreakAllOrders();
            const D3DXVECTOR3 vel = robot->m_Forward * robot->GetMaxSpeed();
            CPoint tp = CmdObjectPos(robot);
            tp.x -= Float2Int(vel.x * INVERT(GLOBAL_SCALE_MOVE));
            tp.y -= Float2Int(vel.y * INVERT(GLOBAL_SCALE_MOVE));
            robot->MoveToBack(tp.x, tp.y);
        }
    }
    robot->m_ManualMove = (move < 0) ? -1 : ((move > 0) ? 1 : 0);
}

void CmdApply(const SCommand &c) {
    if (!g_MatrixMap)
        return;

    CMatrixSideUnit *su = CmdFindSide(c.m_Side);
    if (!su || su->GetStatus() == SS_NONE)
        return;

    if (c.m_Type == CMD_BUILD_ROBOT) {
        CmdApplyBuild(su, c);
        return;
    }
    if (c.m_Type == CMD_BUILD_TURRET) {
        CmdApplyTurret(su, c);
        return;
    }
    if (c.m_Type == CMD_ARCADE_ENTER || c.m_Type == CMD_ARCADE_LEAVE || c.m_Type == CMD_ARCADE_CONTROL) {
        CmdApplyArcade(su, c);
        return;
    }

    // Леталки двигаются помимо групп роботов: у них своя цель и свой разброс.
    if (c.m_Type == CMD_MOVE_TO)
        CmdOrderFlyers(c);

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

    for (int s = 0; s < CMD_SIDE_MAX; ++s)
        g_SideSeq[s] = 0;
    for (size_t i = 0; i < g_Queue.size(); ++i) {
        const int s = g_Queue[i].m_Side;
        if (s >= 0 && s < CMD_SIDE_MAX && g_Queue[i].m_Seq > g_SideSeq[s])
            g_SideSeq[s] = g_Queue[i].m_Seq;
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
    for (int s = 0; s < CMD_SIDE_MAX; ++s)
        g_SideSeq[s] = 0;
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
    for (int s = 0; s < CMD_SIDE_MAX; ++s)
        g_SideSeq[s] = 0;
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

    // Номер сквозной внутри стороны: порядок применения задаётся парой
    // (такт, сторона, номер), поэтому нумерация каждой стороны независима и
    // совпадает на обеих машинах.
    if (cmd.m_Side < 0)
        cmd.m_Side = 0;
    if (cmd.m_Side >= CMD_SIDE_MAX)
        cmd.m_Side = CMD_SIDE_MAX - 1;
    cmd.m_Seq = ++g_SideSeq[cmd.m_Side];

    if (cmd.m_SelCnt < 0)
        cmd.m_SelCnt = 0;
    if (cmd.m_SelCnt > CMD_SEL_MAX)
        cmd.m_SelCnt = CMD_SEL_MAX;

    g_Queue.push_back(cmd);
    ++g_IssuedCnt;

    CmdLogCommand(cmd);

    // В сетевом режиме свой приказ уходит партнёру: у него он попадёт в очередь
    // с тем же тактом и тем же номером, а применится тем же CmdApply.
    NetSendCommand(cmd);
}

void CmdIssueRemote(SCommand &cmd) {
    if (!g_InitDone || !g_MatrixMap)
        return;
    if (cmd.m_Side < 0 || cmd.m_Side >= CMD_SIDE_MAX)
        return;
    if (cmd.m_Tick <= 0)
        return;
    if (cmd.m_SelCnt < 0)
        cmd.m_SelCnt = 0;
    if (cmd.m_SelCnt > CMD_SEL_MAX)
        cmd.m_SelCnt = CMD_SEL_MAX;

    // Пакеты UDP могут прийти не по порядку (перепосылка после потери), поэтому
    // очередь не обязана быть отсортированной: CmdTick выбирает всё, что созрело,
    // и сортирует выбранное.
    g_Queue.push_back(cmd);
    ++g_IssuedCnt;

    CmdLogCommand(cmd, true);
}

void CmdTick(int tick) {
    if (!g_InitDone)
        return;

#ifdef MATRIXGAME_SIM_HARNESS
    // Скриптовый «игрок» для проверки реплея: ставит команды через тот же
    // CmdIssue, что и настоящий ввод.
    SimCommandBotTick(tick);
#endif

    // Ручное управление: то, что игрок держал в кадре, уходит командой на этот
    // такт (и приедет на N + CMD_INPUT_LAG_TICKS, как любой ввод).
    ArcadeInputTick();

    NetPump();

    // Забираем всё, чей такт уже наступил. Очередь может быть не отсортирована
    // (приказы из сети приходят в порядке доставки, а не по тактам), поэтому
    // просматриваем её целиком и сортируем только созревшие.
    std::vector<SCommand> due;
    size_t keep = 0;
    for (size_t i = 0; i < g_Queue.size(); ++i) {
        if (g_Queue[i].m_Tick <= tick)
            due.push_back(g_Queue[i]);
        else
            g_Queue[keep++] = g_Queue[i];
    }
    g_Queue.resize(keep);

    if (!due.empty()) {
        std::sort(due.begin(), due.end(), CmdLess);

        for (size_t i = 0; i < due.size(); ++i) {
            CmdApply(due[i]);
            ++g_AppliedCnt;
        }
    }

    // Мир досчитан до этого такта: партнёру уходит ACK и, если пора, хеш.
    NetOnTickApplied(tick);
}

int CmdCurrentTick(void) {
    if (!g_MatrixMap)
        return 0;
    return g_MatrixMap->GetTime() / LOGIC_TAKT_PERIOD;
}

SArcadeInput g_ArcadeInput;

void ArcadeInputMove(int dir) {
    g_ArcadeInput.m_Move = (dir < 0) ? -1 : ((dir > 0) ? 1 : 0);
}

void ArcadeInputMouseTurn(int dir) {
    g_ArcadeInput.m_MouseTurn = (dir < 0) ? -1 : ((dir > 0) ? 1 : 0);
}

static bool KeyHeld(int idx) {
    return (GetAsyncKeyState(g_Config.m_KeyActions[idx]) & 0x8000) == 0x8000;
}

// Раз в кадр: что игрок держит и куда смотрит курсор. Только для своей стороны
// и только когда она в роботе — иначе это не ручное управление.
void ArcadeInputSample(void) {
    if (!g_MatrixMap)
        return;

    // Харнессный бот играет сам и ставит свои команды: подмешивать к ним ввод
    // (пустой, но зависящий от кадров) нельзя — прогон перестал бы быть
    // воспроизводимым.
#ifdef MATRIXGAME_SIM_HARNESS
    if (SimCommandBotEnabled()) {
        return;
    }
#endif

    CMatrixSideUnit *ps = g_MatrixMap->GetPlayerSide();
    if (!ps || !ps->IsRobotMode() || !ps->GetArcadedObject() || !ps->GetArcadedObject()->IsRobot()) {
        g_ArcadeInput.m_Turn = 0;
        g_ArcadeInput.m_Move = 0;
        g_ArcadeInput.m_Fire = false;
        g_ArcadeInput.m_MouseTurn = 0;
        return;
    }

    int turn = g_ArcadeInput.m_MouseTurn;
    if (KeyHeld(KA_UNIT_LEFT) || KeyHeld(KA_UNIT_LEFT_ALT))
        --turn;
    if (KeyHeld(KA_UNIT_RIGHT) || KeyHeld(KA_UNIT_RIGHT_ALT))
        ++turn;

    g_ArcadeInput.m_Turn = (turn < 0) ? -1 : ((turn > 0) ? 1 : 0);

    // Огонь — только когда фокус не в интерфейсе (как было в Takt).
    g_ArcadeInput.m_Fire = KeyHeld(KA_FIRE) && (!g_IFaceList || g_IFaceList->m_InFocus != INTERFACE);

    g_ArcadeInput.m_AimX = Float2Int(g_MatrixMap->m_TraceStopPos.x * INVERT(GLOBAL_SCALE_MOVE));
    g_ArcadeInput.m_AimY = Float2Int(g_MatrixMap->m_TraceStopPos.y * INVERT(GLOBAL_SCALE_MOVE));
}

// Раз в такт: отправить состояние командой, если оно изменилось. Доставка
// надёжная (ACK + перепосылка), поэтому «только на изменение» ничего не теряет;
// иначе это было бы сто команд в секунду на ровном месте.
void ArcadeInputTick(void) {
    static int s_last[5] = {-1000, 0, 0, 0, 0};

#ifdef MATRIXGAME_SIM_HARNESS
    if (SimCommandBotEnabled()) {
        return;
    }
#endif

    if (!g_MatrixMap) {
        return;
    }

    CMatrixSideUnit *ps = g_MatrixMap->GetPlayerSide();
    if (!ps || !ps->IsRobotMode()) {
        s_last[0] = -1000;  // вне аркады следующее состояние считаем новым
        g_ArcadeInput.m_MouseTurn = 0;
        return;
    }

    const int cur[5] = {g_ArcadeInput.m_Turn, g_ArcadeInput.m_Move, g_ArcadeInput.m_Fire ? 1 : 0, g_ArcadeInput.m_AimX,
                        g_ArcadeInput.m_AimY};

    if (memcmp(cur, s_last, sizeof(cur)) == 0)
        return;

    SCommand c;
    ZeroMemory(&c, sizeof(c));
    c.m_Type = CMD_ARCADE_CONTROL;
    c.m_Side = ps->m_Id;
    c.m_ObjId = -1;
    for (int i = 0; i < 5; ++i)
        c.m_Param[i] = cur[i];

    CmdIssue(c);

    memcpy(s_last, cur, sizeof(cur));
    g_ArcadeInput.m_MouseTurn = 0;
}

int CmdArcadeEnter(CMatrixSideUnit *su, CMatrixMapStatic *robot) {
    if (!su || !robot)
        return -1;

    if (!g_InitDone) {
        su->SetArcadedObject(robot);
        return -1;
    }

    SCommand c;
    ZeroMemory(&c, sizeof(c));
    c.m_Type = CMD_ARCADE_ENTER;
    c.m_Side = su->m_Id;
    c.m_ObjId = (int)robot->GetId();
    CmdIssue(c);
    return c.m_Tick;
}

int CmdArcadeLeave(CMatrixSideUnit *su) {
    if (!su)
        return -1;

    if (!g_InitDone) {
        su->SetArcadedObject(NULL);
        return -1;
    }

    SCommand c;
    ZeroMemory(&c, sizeof(c));
    c.m_Type = CMD_ARCADE_LEAVE;
    c.m_Side = su->m_Id;
    c.m_ObjId = -1;
    CmdIssue(c);
    return c.m_Tick;
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
        if (!o || o->GetSide() != su->m_Id)
            continue;
        // Леталки тоже попадают в выделение: приказ на движение назначает им
        // цель, а группу роботов из них всё равно не соберёшь.
        if (o->IsLiveRobot() || o->IsFlyer())
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
