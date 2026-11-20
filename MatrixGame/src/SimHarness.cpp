// MatrixGame - SR2 Planetary battles engine
// Copyright (C) 2012, Elemental Games, Katauri Interactive, CHK-Games
// Licensed under GPLv2 or any later version

#include "stdafx.h"

#ifdef MATRIXGAME_SIM_HARNESS

#include "SimHarness.hpp"

#include "MatrixMap.hpp"
#include "MatrixMapStatic.hpp"
#include "MatrixRobot.hpp"

#include <stdio.h>
#include <stdlib.h>

// stdafx.h подменяет rand() на SimRand(), а сама обёртка должна звать настоящий.
#undef rand

namespace {

unsigned long long g_randCalls = 0;

const wchar_t *EnvStr(const wchar_t *name) {
    const wchar_t *v = _wgetenv(name);
    return (v && *v) ? v : NULL;
}

int EnvInt(const wchar_t *name) {
    const wchar_t *v = EnvStr(name);
    return v ? _wtoi(v) : 0;
}

unsigned long long Fnv1a(unsigned long long h, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= (unsigned long long)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Хеш состояния мира. Порядок объектов в связном списке между запусками может
// отличаться, поэтому хеши отдельных объектов складываются, а не сцепляются.
unsigned long long HashWorld(int *objectsOut) {
    unsigned long long acc = 0;
    int count = 0;

    for (CMatrixMapStatic *ms = CMatrixMapStatic::GetFirstLogic(); ms; ms = ms->GetNextLogic()) {
        const D3DXVECTOR3 &pos = ms->GetGeoCenter();

        const int type = (int)ms->GetObjectType();
        const int side = ms->GetSide();

        unsigned long long h = 14695981039346656037ULL;
        h = Fnv1a(h, &type, sizeof(type));
        h = Fnv1a(h, &side, sizeof(side));
        h = Fnv1a(h, &pos.x, sizeof(float));
        h = Fnv1a(h, &pos.y, sizeof(float));

        acc += h;
        ++count;
    }

    if (objectsOut)
        *objectsOut = count;

    return acc ^ ((unsigned long long)count << 47);
}

// Дамп объектов для поиска первого разошедшегося: строки сортируются по
// (тип, сторона, X, Y), поэтому порядок связного списка не важен.
struct SObjRec {
    int type;
    int side;
    float x;
    float y;
    float vx;
    float vy;
    float speed;
    float ang;
    int state;
    // Поля скорости/коррекции — чтобы увидеть, какое из них неинициализировано.
    float maxsp;
    float wcorr;
    float scup;
    float scdown;
    float rotspeed;
    float grpsp;
    float colsp;
};

int __cdecl ObjRecCmp(const void *a, const void *b) {
    const SObjRec *p = (const SObjRec *)a;
    const SObjRec *q = (const SObjRec *)b;

    if (p->type != q->type)
        return p->type - q->type;
    if (p->side != q->side)
        return p->side - q->side;
    if (p->x != q->x)
        return (p->x < q->x) ? -1 : 1;
    if (p->y != q->y)
        return (p->y < q->y) ? -1 : 1;
    return 0;
}

void DumpObjects(FILE *f, int time) {
    static SObjRec recs[1024];

    int count = 0;
    for (CMatrixMapStatic *ms = CMatrixMapStatic::GetFirstLogic(); ms && count < 1024; ms = ms->GetNextLogic()) {
        const D3DXVECTOR3 &pos = ms->GetGeoCenter();

        SObjRec &r = recs[count];
        r.type = (int)ms->GetObjectType();
        r.side = ms->GetSide();
        r.x = pos.x;
        r.y = pos.y;
        r.vx = r.vy = r.speed = r.ang = 0.0f;
        r.state = -1;
        r.maxsp = r.wcorr = r.scup = r.scdown = r.rotspeed = r.grpsp = r.colsp = 0.0f;

        if (r.type == OBJECT_TYPE_ROBOTAI) {
            CMatrixRobotAI *robot = ms->AsRobot();
            if (robot) {
                r.vx = robot->m_Velocity.x;
                r.vy = robot->m_Velocity.y;
                r.speed = robot->m_Speed;
                r.ang = robot->m_BotRotAngle;
                r.state = (int)robot->m_CurrState;
                r.maxsp = robot->GetMaxSpeed();
                r.wcorr = robot->GetSpeedWaterCorr();
                r.scup = robot->GetSpeedSlopeCorrUp();
                r.scdown = robot->GetSpeedSlopeCorrDown();
                r.rotspeed = robot->GetMaxRotationSpeed();
                r.grpsp = robot->m_GroupSpeed;
                r.colsp = robot->m_ColSpeed;
            }
        }

        ++count;
    }

    qsort(recs, count, sizeof(SObjRec), ObjRecCmp);

    for (int i = 0; i < count; ++i)
        fprintf(f,
                "t=%d type=%d side=%d x=%.3f y=%.3f vx=%.4f vy=%.4f sp=%.4f ang=%.4f st=%d maxsp=%.2f wcorr=%.2f "
                "scup=%.2f scdown=%.2f rots=%.2f grp=%.2f col=%.2f\n",
                time, recs[i].type, recs[i].side, recs[i].x, recs[i].y, recs[i].vx, recs[i].vy, recs[i].speed,
                recs[i].ang, recs[i].state, recs[i].maxsp, recs[i].wcorr, recs[i].scup, recs[i].scdown,
                recs[i].rotspeed, recs[i].grpsp, recs[i].colsp);
}

// Фильтр необработанных исключений: пишет код, адрес и модуль падения,
// чтобы отлаживать без отладчика.
LONG WINAPI SimCrashFilter(EXCEPTION_POINTERS *ep) {
    const wchar_t *path = EnvStr(L"MATRIXGAME_TRACE");
    if (path) {
        FILE *f = _wfopen(path, L"a");
        if (f) {
            void *addr = (void *)ep->ExceptionRecord->ExceptionAddress;
            wchar_t modname[MAX_PATH] = L"?";
            HMODULE mod = NULL;

            if (GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCWSTR)addr, &mod) &&
                mod) {
                GetModuleFileNameW(mod, modname, MAX_PATH);
            }

            const unsigned long long offset =
                    mod ? (unsigned long long)((unsigned char *)addr - (unsigned char *)mod) : 0;

            fprintf(f, "CRASH code=%08lx addr=%p module=%ls offset=0x%llx\n", ep->ExceptionRecord->ExceptionCode, addr,
                    modname, offset);
            fclose(f);
        }
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

bool SimModeEnabled(void) {
    const wchar_t *v = EnvStr(L"MATRIXGAME_SIM");
    return v && v[0] != L'0';
}

unsigned SimSeed(unsigned fallback) {
    const wchar_t *v = EnvStr(L"MATRIXGAME_SEED");
    return v ? (unsigned)_wtoi64(v) : fallback;
}

unsigned SimRand(void) {
    ++g_randCalls;
    return (unsigned)rand();
}

unsigned long long SimRandCalls(void) {
    return g_randCalls;
}

void SimInstallCrashFilter(void) {
    SetUnhandledExceptionFilter(SimCrashFilter);
}

void SimTrace(const char *message) {
    const wchar_t *path = EnvStr(L"MATRIXGAME_TRACE");
    if (!path)
        return;

    FILE *f = _wfopen(path, L"a");
    if (!f)
        return;

    fprintf(f, "%s\n", message);
    fclose(f);
}

void SimTraceW(const wchar *message) {
    const wchar_t *path = EnvStr(L"MATRIXGAME_TRACE");
    if (!path)
        return;

    FILE *f = _wfopen(path, L"a");
    if (!f)
        return;

    fprintf(f, "%ls\n", message);
    fclose(f);
}

const wchar *SimMapOverride(void) {
    static wchar buf[128];
    const wchar_t *v = EnvStr(L"MATRIXGAME_MAP");
    if (!v)
        return NULL;

    wcsncpy(buf, v, 127);
    buf[127] = 0;
    return buf;
}

void SimLogTakt(int step) {
    (void)step;

    static FILE *log = NULL;
    static FILE *objlog = NULL;
    static int nextLogAt = 0;

    if (!log) {
        const wchar_t *path = EnvStr(L"MATRIXGAME_HASHLOG");
        const wchar_t *objpath = EnvStr(L"MATRIXGAME_OBJLOG");

        if (!path && !objpath)
            return;

        if (path)
            log = _wfopen(path, L"w");
        if (objpath)
            objlog = _wfopen(objpath, L"w");

        if (!log && !objlog)
            return;
        nextLogAt = 0;
    }

    const int time = g_MatrixMap->GetTime();
    const int maxTicks = EnvInt(L"MATRIXGAME_MAXTICKS");

    if (time >= nextLogAt) {
        nextLogAt = time + SIM_HASH_PERIOD;

        int objects = 0;
        const unsigned long long hash = HashWorld(&objects);

        if (log) {
            fprintf(log, "t=%d objects=%d rand=%llu hash=%016llx\n", time, objects, SimRandCalls(), hash);
            fflush(log);
        }

        if (objlog) {
            DumpObjects(objlog, time);
            fflush(objlog);
        }
    }

    if (maxTicks > 0 && time >= maxTicks) {
        int objects = 0;
        const unsigned long long hash = HashWorld(&objects);

        if (log) {
            fprintf(log, "FINAL t=%d objects=%d rand=%llu hash=%016llx\n", time, objects, SimRandCalls(), hash);
            fflush(log);
            fclose(log);
            log = NULL;
        }

        if (objlog) {
            fflush(objlog);
            fclose(objlog);
            objlog = NULL;
        }

        SETFLAG(g_Flags, GFLAG_EXITLOOP);
    }
}

#endif  // MATRIXGAME_SIM_HARNESS
