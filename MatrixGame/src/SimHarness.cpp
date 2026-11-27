// MatrixGame - SR2 Planetary battles engine
// Copyright (C) 2012, Elemental Games, Katauri Interactive, CHK-Games
// Licensed under GPLv2 or any later version

#include "stdafx.h"

#ifdef MATRIXGAME_SIM_HARNESS

#include "SimHarness.hpp"

#include "MatrixMap.hpp"
#include "MatrixMapStatic.hpp"
#include "MatrixObject.hpp"
#include "MatrixRobot.hpp"
#include "MatrixSide.hpp"
#include "CmdQueue.hpp"
#include "NetPlay.hpp"
#include "Interface/CConstructor.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <intrin.h>

// stdafx.h подменяет rand() на SimRand(), а сама обёртка должна звать настоящий.
#undef rand

namespace {

unsigned long long g_randCalls = 0;

// Два независимых потока ГПСЧ. Логика берёт числа из своего потока (он же
// уходит в лог и сравнивается между прогонами), отрисовка — из потока libc,
// потому что количество вызовов в кадре зависит от того, что видно на экране,
// и не должно влиять на симуляцию.
int g_simPhase = SIMPHASE_LOGIC;
unsigned int g_logicRng = 0;
bool g_logicRngSeeded = false;
unsigned long long g_logicCalls = 0;

void LogicRngSeedOnce(void);

// Учёт вызовов ГПСЧ по адресам возврата. Когда два прогона расходятся по числу
// вызовов rand(), видно, какая именно функция сгенерировала лишние числа —
// дальше остаётся прочитать её и найти ветвление по неинициализированному полю.
struct SRandSite {
    void *addr;
    unsigned long long count;
    unsigned long long reported;
};

const int RAND_SITES_MAX = 512;
SRandSite g_sites[RAND_SITES_MAX];
int g_sitesCnt = 0;
unsigned long long g_sitesOverflow = 0;

void RandRecord(void *addr) {
    for (int i = 0; i < g_sitesCnt; ++i) {
        if (g_sites[i].addr == addr) {
            ++g_sites[i].count;
            return;
        }
    }
    if (g_sitesCnt < RAND_SITES_MAX) {
        g_sites[g_sitesCnt].addr = addr;
        g_sites[g_sitesCnt].count = 1;
        g_sites[g_sitesCnt].reported = 0;
        ++g_sitesCnt;
    }
    else {
        ++g_sitesOverflow;
    }
}

// Смещение адреса от начала модуля: одинаково у любых двух запусков, в отличие
// от абсолютного адреса, и переводится в имя функции по .map-файлу линкера.
unsigned long long ModuleOffset(void *addr) {
    HMODULE mod = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &mod) ||
        !mod)
        return 0;

    return (unsigned long long)((unsigned char *)addr - (unsigned char *)mod);
}

const wchar_t *EnvStr(const wchar_t *name) {
    const wchar_t *v = _wgetenv(name);
    return (v && *v) ? v : NULL;
}

int EnvInt(const wchar_t *name) {
    const wchar_t *v = EnvStr(name);
    return v ? _wtoi(v) : 0;
}

// Байт заливки свежих аллокаций (тот же, что использует CHeap::Alloc).
unsigned char SimFillByte(void) {
    const wchar_t *v = EnvStr(L"MATRIXGAME_FILL");
    return v ? (unsigned char)(wcstoul(v, NULL, 16) & 0xFF) : 0;
}

// Ищет в памяти объекта 32-битные слова, оставшиеся от заливки. Это и есть
// список полей, которые никто не инициализировал: смещения печатаются в
// obj-логе, а имена берутся из таблицы, которую пишет SimDumpLayouts.
int ScanFill(const void *obj, size_t size, unsigned char fill, unsigned *offsets, int maxOut) {
    const unsigned pattern = (unsigned)fill * 0x01010101u;
    const unsigned *words = (const unsigned *)obj;

    int found = 0;
    for (size_t off = 0; off + 4 <= size; off += 4) {
        if (words[off / 4] != pattern)
            continue;
        if (found < maxOut)
            offsets[found] = (unsigned)off;
        ++found;
    }
    return found;
}

void LogicRngSeedOnce(void) {
    if (g_logicRngSeeded)
        return;

    const wchar_t *v = EnvStr(L"MATRIXGAME_SEED");
    const unsigned seed = v ? (unsigned)_wtoi64(v) : 12345;
    // xorshift не любит нулевое состояние.
    g_logicRng = seed ? seed : 0x9E3779B9;
    g_logicRngSeeded = true;
}

unsigned LogicRngNext(void) {
    LogicRngSeedOnce();

    // xorshift32: дешёвый, без таблиц и без обращения к памяти.
    unsigned x = g_logicRng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_logicRng = x;
    return x;
}

// Заливка стека тем же байтом, что и куча. Рекурсия спускается вниз по стеку,
// поэтому неинициализированные локальные переменные логики получают известное
// значение, и дифференциальный прогон 0xCD/0xAA ловит и их тоже.
__declspec(noinline) void PoisonStackRec(unsigned char fill, int left) {
    volatile unsigned char buf[512];
    for (int i = 0; i < 512; ++i)
        buf[i] = fill;
    if (left > 512)
        PoisonStackRec(fill, left - 512);
}

void PoisonStack(int bytes, unsigned char fill) {
    if (bytes > 512)
        PoisonStackRec(fill, bytes - 512);
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

// Дамп объектов для поиска первого разошедшегося. Строки ключуются стабильным
// id объекта (GetId), поэтому порядок связного списка не важен, а сравнение
// идёт поле в поле. Перечень полей подобран так, чтобы покрыть всю логику
// движения/заданий: расхождение в любом из них видно до того, как разъедутся
// координаты.
struct SObjRec {
    DWORD id;
    int type;
    int side;
    float x;
    float y;
    float vx;
    float vy;
    float speed;
    float rotspeed_obj;
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
    // Ориентация корпуса. mz — z-компонента «вперёд» корпуса, из неё Seek
    // считает уклон местности и поправку скорости; up — сглаженная нормаль.
    float ux;
    float uy;
    float uz;
    float mz;
    float cz;
    // Ориентация корпуса и башни.
    float fx;
    float fy;
    float hfx;
    float hfy;
    // Задание и путь.
    int order;
    int phase;
    float p1;
    float p2;
    float p3;
    int p4;
    int zc;
    int zd;
    int zpnext;
    int zpcnt;
    int mpcnt;
    int mpcur;
    int dx;
    int dy;
    int mx;
    int my;
    int cols;
    float mdist;
    float mfollow;
    // Ремонтные лучи: их отрисовка тоже жрёт rand(), поэтому важно видеть,
    // в каком состоянии она находится (данные есть / луч помечен как видимый).
    int hav;
    int wcnt;
    int repcnt;
    int repflag;
    int camsq;
    // Незаписанные поля объекта: количество слов с байтом заливки и их смещения.
    int garb;
    int gofcnt;
    unsigned gof[24];
    // Живучесть и состояние: по ним видно, что объект уничтожили в одном
    // прогоне и не уничтожили в другом.
    unsigned ostate;
    float hp;
    float hpmax;
    int burn;
    int bhp;
    int bhpmax;
};

int __cdecl ObjRecCmp(const void *a, const void *b) {
    const SObjRec *p = (const SObjRec *)a;
    const SObjRec *q = (const SObjRec *)b;

    if (p->id != q->id)
        return (p->id < q->id) ? -1 : 1;
    return 0;
}

void DumpObjects(FILE *f, int time) {
    static SObjRec recs[1024];

    int count = 0;
    for (CMatrixMapStatic *ms = CMatrixMapStatic::GetFirstLogic(); ms && count < 1024; ms = ms->GetNextLogic()) {
        const D3DXVECTOR3 &pos = ms->GetGeoCenter();

        SObjRec &r = recs[count];
        memset(&r, 0, sizeof(r));
        r.id = ms->GetId();
        r.type = (int)ms->GetObjectType();
        r.side = ms->GetSide();
        r.x = pos.x;
        r.y = pos.y;
        r.state = -1;
        r.order = r.phase = r.p4 = -1;
        r.zc = r.zd = r.zpnext = r.zpcnt = r.mpcnt = r.mpcur = -1;
        r.dx = r.dy = r.mx = r.my = r.cols = -1;
        r.hav = r.wcnt = r.repcnt = r.repflag = -1;
        r.camsq = (int)ms->GetCamDistSq();
        r.garb = -1;
        r.gofcnt = 0;
        r.ostate = ms->GetObjectStateRaw();
        r.hp = r.hpmax = -1.0f;
        r.burn = r.bhp = r.bhpmax = -1;

        if (r.type == OBJECT_TYPE_MAPOBJECT) {
            // Эти два поля лежат в union: у горящего объекта это таймеры
            // горения (m_NextTime, m_BurnTimeTotal), у ломаемого — прочность
            // (m_BreakHitPoint, m_AnimState). Печатаем как есть, сравнение идёт
            // между одинаковыми объектами.
            CMatrixMapObject *mo = (CMatrixMapObject *)ms;
            r.burn = mo->m_NextTime;
            r.bhp = mo->m_BurnTimeTotal;
        }

        if (r.type == OBJECT_TYPE_ROBOTAI) {
            CMatrixRobotAI *robot = ms->AsRobot();
            if (robot) {
                r.vx = robot->m_Velocity.x;
                r.vy = robot->m_Velocity.y;
                r.speed = robot->m_Speed;
                r.rotspeed_obj = robot->m_RotSpeed;
                r.ang = robot->m_BotRotAngle;
                r.state = (int)robot->m_CurrState;
                r.maxsp = robot->GetMaxSpeed();
                r.wcorr = robot->GetSpeedWaterCorr();
                r.scup = robot->GetSpeedSlopeCorrUp();
                r.scdown = robot->GetSpeedSlopeCorrDown();
                r.rotspeed = robot->GetMaxRotationSpeed();
                r.grpsp = robot->m_GroupSpeed;
                r.colsp = robot->m_ColSpeed;
                {
                    const D3DXVECTOR3 coreUp = robot->GetCoreUp();
                    const D3DXVECTOR3 coreFwd = robot->GetCoreForward();
                    r.ux = coreUp.x;
                    r.uy = coreUp.y;
                    r.uz = coreUp.z;
                    r.mz = coreFwd.z;
                    r.cz = robot->GetCoreZ();
                }
                r.fx = robot->m_Forward.x;
                r.fy = robot->m_Forward.y;
                r.hfx = robot->m_HullForward.x;
                r.hfy = robot->m_HullForward.y;
                if (robot->GetOrdersInPool() > 0) {
                    // Первый ордер в пуле — тот, который робот выполняет сейчас.
                    const SOrder *o = robot->GetOrder(0);
                    r.order = (int)o->GetOrderType();
                    r.phase = (int)o->GetOrderPhase();
                    r.p1 = o->m_Param1;
                    r.p2 = o->m_Param2;
                    r.p3 = o->m_Param3;
                    r.p4 = o->m_Param4;
                }
                r.zc = robot->GetZoneCur();
                r.zd = robot->GetZoneDes();
                r.zpnext = robot->GetZonePathNext();
                r.zpcnt = robot->GetZonePathCnt();
                r.mpcnt = robot->GetMovePathCnt();
                r.mpcur = robot->GetMovePathCur();
                r.dx = robot->GetDesX();
                r.dy = robot->GetDesY();
                r.mx = robot->GetMapPosX();
                r.my = robot->GetMapPosY();
                r.cols = robot->GetCols();
                r.mdist = robot->GetMovePathDist();
                r.mfollow = robot->GetMovePathDistFollow();

                r.hav = robot->HaveRepair();
                r.wcnt = robot->GetWeaponsCnt();
                r.hp = robot->GetHitPoint();
                r.hpmax = robot->GetMaxHitPoint();
                r.repcnt = 0;
                r.repflag = 0;
                // Указатель на ремонтные данные не разыменовываем: он живёт своей
                // жизнью (Release/Allocate) и на большом бою дамп на нём падал.
                // Для сравнения прогонов достаточно того, что он есть или нет.
                for (int u = 0; u < robot->m_UnitCnt; ++u) {
                    if (robot->m_Unit[u].m_WeaponRepairData)
                        ++r.repcnt;
                }
                r.repflag = robot->HaveRepair() ? 1 : 0;

                const unsigned char fill = SimFillByte();
                if (fill) {
                    r.garb = ScanFill(robot, sizeof(CMatrixRobotAI), fill, r.gof, 24);
                    r.gofcnt = r.garb < 24 ? r.garb : 24;
                }
            }
        }

        ++count;
    }

    qsort(recs, count, sizeof(SObjRec), ObjRecCmp);

    for (int i = 0; i < count; ++i) {
        // Все float-поля печатаются с девятью значащими цифрами: float хранит
        // около семи, поэтому так значение видно бит в бит. Меньшая точность
        // прячет расхождение в сотых, из-за чего первым «разошедшимся» полем
        // оказывается не причина, а её следствие.
        fprintf(f,
                "t=%d id=%u type=%d side=%d x=%.9g y=%.9g vx=%.9g vy=%.9g sp=%.9g rsp=%.9g ang=%.9g st=%d "
                "maxsp=%.9g wcorr=%.9g scup=%.9g scdown=%.9g rots=%.9g grp=%.9g col=%.9g "
                "ux=%.9g uy=%.9g uz=%.9g mz=%.9g cz=%.9g "
                "fx=%.9g fy=%.9g hfx=%.9g hfy=%.9g "
                "ord=%d ph=%d p1=%.9g p2=%.9g p3=%.9g p4=%d "
                "zc=%d zd=%d zpn=%d zpc=%d mpc=%d mpi=%d dx=%d dy=%d mx=%d my=%d ncols=%d "
                "mdist=%.9g mfollow=%.9g hav=%d wcnt=%d rep=%d repf=%d camsq=%d garb=%d ostate=%x hp=%.9g hpmax=%.9g "
                "burn=%d bhp=%d gof=",
                time, recs[i].id, recs[i].type, recs[i].side, recs[i].x, recs[i].y, recs[i].vx, recs[i].vy,
                recs[i].speed, recs[i].rotspeed_obj, recs[i].ang, recs[i].state, recs[i].maxsp, recs[i].wcorr,
                recs[i].scup, recs[i].scdown, recs[i].rotspeed, recs[i].grpsp, recs[i].colsp, recs[i].ux, recs[i].uy,
                recs[i].uz, recs[i].mz, recs[i].cz, recs[i].fx, recs[i].fy, recs[i].hfx, recs[i].hfy, recs[i].order,
                recs[i].phase, recs[i].p1, recs[i].p2, recs[i].p3, recs[i].p4, recs[i].zc, recs[i].zd, recs[i].zpnext,
                recs[i].zpcnt, recs[i].mpcnt, recs[i].mpcur, recs[i].dx, recs[i].dy, recs[i].mx, recs[i].my,
                recs[i].cols, recs[i].mdist, recs[i].mfollow, recs[i].hav, recs[i].wcnt, recs[i].repcnt,
                recs[i].repflag, recs[i].camsq, recs[i].garb, recs[i].ostate, recs[i].hp, recs[i].hpmax, recs[i].burn,
                recs[i].bhp);
        if (recs[i].gofcnt == 0)
            fprintf(f, "-");
        for (int g = 0; g < recs[i].gofcnt; ++g)
            fprintf(f, "%s%u", g ? "," : "", recs[i].gof[g]);
        fprintf(f, "\n");
    }
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

void SimDumpLayouts(FILE *f) {
    CMatrixMapStatic::SimDumpLayout(f);
    CMatrixRobot::SimDumpLayout(f);
    CMatrixRobotAI::SimDumpLayout(f);
}

bool SimModeEnabled(void) {
    const wchar_t *v = EnvStr(L"MATRIXGAME_SIM");
    return v && v[0] != L'0';
}

bool SimNoDraw(void) {
    const wchar_t *v = EnvStr(L"MATRIXGAME_NODRAW");
    return v && v[0] != L'0';
}

unsigned SimSeed(unsigned fallback) {
    const wchar_t *v = EnvStr(L"MATRIXGAME_SEED");
    return v ? (unsigned)_wtoi64(v) : fallback;
}

void SimResetLogicRng(void) {
    // Меню игры (превью карты в «Планетарных боях») живёт в том же процессе и
    // тоже дёргает rand(), причём число вызовов зависит от того, сколько кадров
    // провисело меню. Без сброса бой начинался бы с разных состояний потока, а
    // от него зависят и состав войск, и все решения ИИ.
    g_logicRngSeeded = false;
    g_logicCalls = 0;
}

unsigned SimRand(void) {
    ++g_randCalls;
    RandRecord(_ReturnAddress());

    if (g_simPhase == SIMPHASE_VISUAL)
        return (unsigned)rand();

    ++g_logicCalls;
    // RND/FRND в Math3D.hpp рассчитаны на диапазон 0..RAND_MAX, поэтому логический
    // поток обязан его соблюдать — иначе игровые формулы уезжают на порядки.
    return LogicRngNext() & RAND_MAX;
}

void SimSetPhase(int phase) {
    g_simPhase = phase;
}

int SimPhase(void) {
    return g_simPhase;
}

unsigned long long SimLogicRandCalls(void) {
    return g_logicCalls;
}

unsigned long long SimRandCalls(void) {
    return g_randCalls;
}

int g_repairStats[3] = {0, 0, 0};

void SimRepairStat(int ev) {
    if (ev >= 0 && ev < 3)
        ++g_repairStats[ev];
}

void SimInstallCrashFilter(void) {
    SetUnhandledExceptionFilter(SimCrashFilter);
}

int SimExceptionFilter(void *exceptionPointers) {
    EXCEPTION_POINTERS *ep = (EXCEPTION_POINTERS *)exceptionPointers;
    char buf[192];
    if (ep && ep->ExceptionRecord) {
        _snprintf(buf, sizeof(buf), "EXCEPTION code=0x%08x addr=0x%p module=0x%llx",
                  (unsigned)ep->ExceptionRecord->ExceptionCode, (void *)ep->ExceptionRecord->ExceptionAddress,
                  ModuleOffset((void *)ep->ExceptionRecord->ExceptionAddress));
    }
    else {
        _snprintf(buf, sizeof(buf), "EXCEPTION without a record");
    }
    buf[sizeof(buf) - 1] = 0;
    SimTrace(buf);

    // Цепочка кадров x86 (ebp) — без символов, но с адресами возврата: по ним
    // видно, откуда пришли, если сверить с .map.
    if (ep && ep->ContextRecord) {
        DWORD *ebp = (DWORD *)ep->ContextRecord->Ebp;
        for (int i = 0; i < 12 && ebp; ++i) {
            const DWORD ret = ebp[1];
            const DWORD next = ebp[0];
            if (!ret)
                break;
            _snprintf(buf, sizeof(buf), "  frame %d ret=0x%08x module=0x%llx", i, (unsigned)ret,
                      ModuleOffset((void *)ret));
            buf[sizeof(buf) - 1] = 0;
            SimTrace(buf);
            if (next <= (DWORD)ebp)
                break;
            ebp = (DWORD *)next;
        }
    }

    return EXCEPTION_EXECUTE_HANDLER;
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

namespace {

struct SBotTrace {
    bool inited;
    unsigned id;
    int from;
    int to;
    FILE *file;
};

SBotTrace g_botTrace = {false, 0, 0, 0, NULL};

void BotTraceInit(void) {
    g_botTrace.inited = true;
    g_botTrace.id = (unsigned)EnvInt(L"MATRIXGAME_TRACEBOT");
    g_botTrace.from = EnvInt(L"MATRIXGAME_TRACEFROM");
    g_botTrace.to = EnvInt(L"MATRIXGAME_TRACETO");

    if (g_botTrace.id == 0)
        return;

    const wchar_t *path = EnvStr(L"MATRIXGAME_TRACEBOTLOG");
    g_botTrace.file = _wfopen(path ? path : L"tracebot.log", L"w");
}

}  // namespace

bool SimTraceBotOn(unsigned id) {
    if (!g_botTrace.inited)
        BotTraceInit();
    if (!g_botTrace.id || !g_botTrace.file || id != g_botTrace.id)
        return false;

    const int time = g_MatrixMap->GetTime();
    if (time < g_botTrace.from)
        return false;
    if (g_botTrace.to > 0 && time > g_botTrace.to)
        return false;
    return true;
}

void SimTraceBotf(unsigned id, const char *fmt, ...) {
    if (!SimTraceBotOn(id))
        return;

    fprintf(g_botTrace.file, "t=%d ", g_MatrixMap->GetTime());

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_botTrace.file, fmt, ap);
    va_end(ap);

    fprintf(g_botTrace.file, "\n");
    fflush(g_botTrace.file);
}

void SimLogTakt(int step) {
    (void)step;

    static FILE *log = NULL;
    static FILE *objlog = NULL;
    static FILE *randlog = NULL;
    static int nextLogAt = 0;
    static int period = 0;

    // Заливаем стек перед логикой: так дифференциальный прогон ловит чтение
    // неинициализированных локальных переменных, а не только кучи.
    {
        const int poisonBytes = EnvInt(L"MATRIXGAME_POISON");
        if (poisonBytes > 0) {
            const wchar_t *fillStr = EnvStr(L"MATRIXGAME_FILL");
            const unsigned char fill = fillStr ? (unsigned char)(wcstoul(fillStr, NULL, 16) & 0xFF) : 0;
            PoisonStack(poisonBytes, fill);
        }
    }

    if (!period) {
        const wchar_t *path = EnvStr(L"MATRIXGAME_HASHLOG");
        const wchar_t *objpath = EnvStr(L"MATRIXGAME_OBJLOG");
        const wchar_t *randpath = EnvStr(L"MATRIXGAME_RANDLOG");

        if (!path && !objpath && !randpath)
            return;

        if (path)
            log = _wfopen(path, L"w");
        if (objpath)
            objlog = _wfopen(objpath, L"w");

        // Рядом с obj-логом кладём таблицу смещений полей: по ней смещения из
        // колонки gof переводятся в имена.
        if (objpath) {
            wchar_t layoutPath[MAX_PATH * 2];
            _snwprintf(layoutPath, MAX_PATH * 2 - 1, L"%ls.layout", objpath);
            layoutPath[MAX_PATH * 2 - 1] = 0;
            FILE *lf = _wfopen(layoutPath, L"w");
            if (lf) {
                SimDumpLayouts(lf);
                fclose(lf);
            }
        }
        if (randpath)
            randlog = _wfopen(randpath, L"w");

        if (!log && !objlog && !randlog)
            return;

        // Период дампа можно уменьшить до одного тика, чтобы поймать точный такт
        // расхождения. По умолчанию — SIM_HASH_PERIOD.
        period = EnvInt(L"MATRIXGAME_HASHSTEP");
        if (period <= 0)
            period = SIM_HASH_PERIOD;

        nextLogAt = 0;
    }

    const int time = g_MatrixMap->GetTime();
    const int maxTicks = EnvInt(L"MATRIXGAME_MAXTICKS");

    if (time >= nextLogAt) {
        nextLogAt = time + period;

        int objects = 0;
        const unsigned long long hash = HashWorld(&objects);

        if (log) {
            // Помимо мира печатаем состояние сторон: исход боя решается по их
            // статусам, а они в хеш не входят — иначе прогон, где бой закончился
            // на такт раньше, расходится «молча», уже после последней строки.
            int active = 0;
            for (int i = 0; i < g_MatrixMap->m_SideCnt; ++i) {
                if (g_MatrixMap->m_Side[i].GetStatus() == SS_ACTIVE)
                    ++active;
            }

            // Статусы всех сторон, а не только «стороны игрока»: в сети у двух
            // машин своя сторона игрока, и поле не должно от неё зависеть.
            char statuses[128];
            statuses[0] = 0;
            for (int i = 0; i < g_MatrixMap->m_SideCnt; ++i) {
                char one[16];
                sprintf(one, "%s%d", i ? "/" : "", (int)g_MatrixMap->m_Side[i].GetStatus());
                strcat(statuses, one);
            }

            fprintf(log, "t=%d objects=%d rand=%llu logic=%llu hash=%016llx rep=%d/%d/%d pst=%s act=%d bwd=%d xl=%d\n",
                    time, objects, SimRandCalls(), SimLogicRandCalls(), hash, g_repairStats[SIMREP_ALLOC],
                    g_repairStats[SIMREP_ENTER], g_repairStats[SIMREP_PASS], statuses, active,
                    g_MatrixMap->GetBeforeWinLooseCount(), FLAG(g_Flags, GFLAG_EXITLOOP) ? 1 : 0);
            g_repairStats[SIMREP_ALLOC] = g_repairStats[SIMREP_ENTER] = g_repairStats[SIMREP_PASS] = 0;
            fflush(log);
        }

        if (objlog) {
            DumpObjects(objlog, time);
            fflush(objlog);
        }

        if (randlog) {
            fprintf(randlog, "t=%d total=%llu", time, SimRandCalls());
            for (int i = 0; i < g_sitesCnt; ++i) {
                const unsigned long long delta = g_sites[i].count - g_sites[i].reported;
                if (delta) {
                    g_sites[i].reported = g_sites[i].count;
                    fprintf(randlog, " s%llx=%llu", ModuleOffset(g_sites[i].addr), delta);
                }
            }
            if (g_sitesOverflow) {
                fprintf(randlog, " overflow=%llu", g_sitesOverflow);
                g_sitesOverflow = 0;
            }
            fprintf(randlog, "\n");
            fflush(randlog);
        }
    }

    if (maxTicks > 0 && time >= maxTicks) {
        int objects = 0;
        const unsigned long long hash = HashWorld(&objects);

        if (log) {
            fprintf(log, "FINAL t=%d objects=%d rand=%llu logic=%llu hash=%016llx\n", time, objects, SimRandCalls(),
                    SimLogicRandCalls(), hash);
            fflush(log);
            fclose(log);
            log = NULL;
        }

        if (objlog) {
            fflush(objlog);
            fclose(objlog);
            objlog = NULL;
        }

        if (randlog) {
            fflush(randlog);
            fclose(randlog);
            randlog = NULL;
        }

        SETFLAG(g_Flags, GFLAG_EXITLOOP);
    }
}

// Командный бот (фаза 2). Играет за сторону игрока теми же приказами, что и
// живой человек: собирает SCommand и кладёт её в CmdQueue. Прогон с ботом
// пишет лог команд, прогон по этому логу (MATRIXGAME_CMDREPLAY) должен дать
// побитово тот же мир — это и есть проверка командного слоя.

static int CmdBotPeriod(void) {
    static int period = -2;
    if (period == -2) {
        period = EnvInt(L"MATRIXGAME_CMDBOT");
        if (period < 0)
            period = 0;
    }
    return period;
}

bool SimCommandBotEnabled(void) {
    return CmdBotPeriod() > 0;
}

// Офлайн-режим «оба игрока за одним столом»: бот командует всеми сторонами,
// за которые играют люди. Нужен, чтобы снять эталонный лог для сетевого теста.
static bool CmdBotAll(void) {
    static int v = -1;
    if (v < 0) {
        const wchar_t *s = EnvStr(L"MATRIXGAME_CMDBOT_ALL");
        v = (s && s[0] != L'0') ? 1 : 0;
    }
    return v != 0;
}

// Наблюдатель: своих приказов не ставит вообще, только принимает чужие. Так
// вторая машина может крутить ту же симуляцию и сверять хеши, не изобретая
// команды (проверка «две симуляции в сети» без лобби).
static bool CmdBotObserver(void) {
    static int v = -1;
    if (v < 0) {
        const wchar_t *s = EnvStr(L"MATRIXGAME_NET_OBSERVER");
        v = (s && s[0] != L'0') ? 1 : 0;
    }
    return v != 0;
}

// Автопилот аркадного режима: MATRIXGAME_ARCADEBOT=1. Раз в 400 тактов бот
// входит в робота, крутит корпус, едет и стреляет, потом выходит. Нужен, чтобы
// ручное управление попадало в эталон и в сетевой тест.
static bool ArcadeBotEnabled(void) {
    static int v = -1;
    if (v < 0) {
        const wchar_t *s = EnvStr(L"MATRIXGAME_ARCADEBOT");
        v = (s && s[0] != L'0') ? 1 : 0;
    }
    return v != 0;
}

// Кем командует бот (и автопилот аркады): в сети — своей стороной, офлайн с
// MATRIXGAME_CMDBOT_ALL — всеми «людьми» сразу, иначе только стороной игрока.
static int SelectBotSides(int *sideIdx) {
    int sideCnt = 0;

    for (int i = 0; i < g_MatrixMap->m_SideCnt && sideCnt < CMD_SIDE_MAX; ++i) {
        CMatrixSideUnit *s = &g_MatrixMap->m_Side[i];
        if (s->GetStatus() == SS_NONE || !s->m_HumanSide)
            continue;

        bool take;
        if (NetIsActive()) {
            // За сторону партнёра не командуем никогда; свою — всегда; остальные
            // человеческие — только если матч ведёт этот процесс (-CmdBotAll,
            // например когда партнёр сидит наблюдателем).
            if (s->m_Id == NetRemoteSide())
                take = false;
            else if (s->m_Id == NetLocalSide())
                take = true;
            else
                take = CmdBotAll();
        }
        else {
            take = CmdBotAll() || s == g_MatrixMap->GetPlayerSide();
        }

        if (take)
            sideIdx[sideCnt++] = i;
    }

    if (CmdBotObserver())
        sideCnt = 0;  // наблюдатель ничего не придумывает, только принимает

    return sideCnt;
}

static void SimArcadeBotSide(int tick, CMatrixSideUnit *su) {
    const int phase = tick % 400;

    if (phase == 2) {
        for (CMatrixMapStatic *o = CMatrixMapStatic::GetFirstLogic(); o; o = o->GetNextLogic()) {
            if (o->IsLiveRobot() && o->GetSide() == su->m_Id) {
                CmdArcadeEnter(su, o);
                break;
            }
        }
        return;
    }
    if (phase == 360) {
        CmdArcadeLeave(su);
        return;
    }
    if (phase < 10 || phase > 355 || (phase % 10) != 0)
        return;

    // Прицел — в первого живого врага, иначе в центр карты.
    int aimX = g_MatrixMap->m_Size.x / 2;
    int aimY = g_MatrixMap->m_Size.y / 2;
    for (CMatrixMapStatic *o = CMatrixMapStatic::GetFirstLogic(); o; o = o->GetNextLogic()) {
        if (o->IsLiveRobot() && o->GetSide() != su->m_Id) {
            aimX = o->AsRobot()->GetMapPosX();
            aimY = o->AsRobot()->GetMapPosY();
            break;
        }
    }

    SCommand c;
    ZeroMemory(&c, sizeof(c));
    c.m_Type = CMD_ARCADE_CONTROL;
    c.m_Side = su->m_Id;
    c.m_ObjId = -1;
    c.m_Param[0] = ((phase / 40) % 2) ? 1 : -1;
    c.m_Param[1] = (phase % 120 < 80) ? 1 : 0;
    c.m_Param[2] = (((phase / 20) % 3) == 0) ? 1 : 0;
    c.m_Param[3] = aimX;
    c.m_Param[4] = aimY;
    CmdIssue(c);
}

static void SimArcadeBotTick(int tick) {
    int sideIdx[CMD_SIDE_MAX];
    const int sideCnt = SelectBotSides(sideIdx);
    for (int i = 0; i < sideCnt; ++i)
        SimArcadeBotSide(tick, &g_MatrixMap->m_Side[sideIdx[i]]);
}

void SimCommandBotTick(int tick) {
    const int period = CmdBotPeriod();

    if (tick <= 0 || !g_MatrixMap)
        return;

    // Автопилот аркадного режима работает каждый такт: управление — это поток
    // состояния, а не редкие приказы.
    if (ArcadeBotEnabled())
        SimArcadeBotTick(tick);

    if (period <= 0)
        return;

    int sideIdx[CMD_SIDE_MAX];
    const int sideCnt = SelectBotSides(sideIdx);

    if (CmdReplayMode() || sideCnt <= 0)
        return;

    // Постройка роботов тоже идёт приказом: раньше её делал харнесс напрямую
    // мимо очереди, и в сети это был бы единственный оставшийся рассинхрон.
    if (SSpecialBot::m_AIRobotTypeCnt > 0 && (tick % 25) == 0) {
        const int typeIdx = (tick / 25) % SSpecialBot::m_AIRobotTypeCnt;
        const SSpecialBot &bot = SSpecialBot::m_AIRobotTypeList[typeIdx];
        for (int sidx = 0; sidx < sideCnt; ++sidx) {
            SCommand cmd;
            ZeroMemory(&cmd, sizeof(cmd));
            cmd.m_Type = CMD_BUILD_ROBOT;
            cmd.m_Side = g_MatrixMap->m_Side[sideIdx[sidx]].m_Id;
            cmd.m_ObjId = -1;  // базу выберет применение: своя живая база стороны
            cmd.m_Param[0] = (int)bot.m_Chassis.m_nKind;
            cmd.m_Param[1] = (int)bot.m_Armor.m_Unit.m_nKind;
            cmd.m_Param[2] = (int)bot.m_Head.m_nKind;
            for (int w = 0; w < MAX_WEAPON_CNT; ++w)
                cmd.m_Param[3 + w] = (int)bot.m_Weapon[w].m_Unit.m_nKind;
            cmd.m_Param[8] = 1;
            CmdIssue(cmd);
        }
    }

    // Турели — тоже приказом, чтобы этот путь попадал в эталон и в сетевой тест.
    if ((tick % 100) == 0) {
        for (int sidx = 0; sidx < sideCnt; ++sidx) {
            CMatrixSideUnit *s = &g_MatrixMap->m_Side[sideIdx[sidx]];
            if (s->GetResourcesAmount(TITAN) < g_Config.m_CannonsProps[0].m_Resources[TITAN])
                continue;

            CMatrixBuilding *bld = NULL;
            for (CMatrixMapStatic *obj = CMatrixMapStatic::GetFirstLogic(); obj; obj = obj->GetNextLogic()) {
                if (!obj->IsLiveBuilding() || obj->GetSide() != s->m_Id)
                    continue;
                if (obj->AsBuilding()->m_TurretsHave >= obj->AsBuilding()->m_TurretsMax)
                    continue;
                bld = obj->AsBuilding();
                break;
            }
            if (!bld)
                continue;

            CPoint places[MAX_PLACES];
            const int placesCnt = bld->GetPlacesForTurrets(places);
            if (placesCnt <= 0)
                continue;

            const CPoint place = places[(tick / 100) % placesCnt];

            SCommand cmd;
            ZeroMemory(&cmd, sizeof(cmd));
            cmd.m_Type = CMD_BUILD_TURRET;
            cmd.m_Side = s->m_Id;
            cmd.m_ObjId = (int)bld->GetId();
            cmd.m_Param[0] = 1 + (tick / 100) % CANNON_TYPE_CNT;
            cmd.m_Param[1] = Float2Int(place.x * GLOBAL_SCALE_MOVE * 100.0f);
            cmd.m_Param[2] = Float2Int(place.y * GLOBAL_SCALE_MOVE * 100.0f);
            cmd.m_Param[3] = 0;
            cmd.m_Param[4] = g_MatrixMap->m_RN.FindInPL(place);
            CmdIssue(cmd);
        }
    }

    if ((tick % period) != 0)
        return;

    const int k = tick / period;
    for (int sidx = 0; sidx < sideCnt; ++sidx) {
        CMatrixSideUnit *ps = &g_MatrixMap->m_Side[sideIdx[sidx]];

        SCommand cmd;
        ZeroMemory(&cmd, sizeof(cmd));
        cmd.m_ObjId = -1;
        cmd.m_Side = ps->m_Id;

        // Выделение — все живые роботы стороны: бот не пользуется мышью, поэтому
        // вместо UI-группы он каждый раз собирает полный список.
        int ownCnt = 0, cx = 0, cy = 0;
        for (CMatrixMapStatic *obj = CMatrixMapStatic::GetFirstLogic(); obj; obj = obj->GetNextLogic()) {
            if (!obj->IsLiveRobot() || obj->GetSide() != ps->m_Id)
                continue;
            if (cmd.m_SelCnt < CMD_SEL_MAX)
                cmd.m_Sel[cmd.m_SelCnt++] = (int)obj->GetId();
            CPoint p = CmdObjectPos(obj);
            cx += p.x;
            cy += p.y;
            ++ownCnt;
        }
        if (cmd.m_SelCnt <= 0)
            continue;
        cx /= ownCnt;
        cy /= ownCnt;

        // Цель — k-й по счёту живой враг в списке логики. Порядок обхода задаёт
        // симуляция, поэтому выбор одинаков во всех прогонах с тем же миром.
        CMatrixMapStatic *targets[32];
        int cnt = 0;
        for (CMatrixMapStatic *obj = CMatrixMapStatic::GetFirstLogic(); obj; obj = obj->GetNextLogic()) {
            if (cnt >= 32)
                break;
            if (obj->GetSide() == ps->m_Id)
                continue;
            if (obj->IsLiveRobot() || obj->IsLiveCannon())
                targets[cnt++] = obj;
        }

        if ((k % 7) == 6) {
            cmd.m_Type = CMD_STOP;
        }
        else if (cnt > 0) {
            CPoint tp = CmdObjectPos(targets[k % cnt]);
            int dx = ((k * 7) % 21) - 10;
            int dy = ((k * 11) % 21) - 10;

            if ((k % 4) == 3) {
                cmd.m_Type = CMD_MOVE_TO;
                cmd.m_X = tp.x + dx;
                cmd.m_Y = tp.y + dy;
            }
            else {
                cmd.m_Type = CMD_ATTACK;
                cmd.m_ObjId = (int)targets[k % cnt]->GetId();
                cmd.m_X = tp.x;
                cmd.m_Y = tp.y;
            }
        }
        else {
            cmd.m_Type = CMD_MOVE_TO;
            cmd.m_X = cx + ((k * 13) % 31) - 15;
            cmd.m_Y = cy + ((k * 17) % 31) - 15;
        }

        CmdIssue(cmd);
    }
}

#endif  // MATRIXGAME_SIM_HARNESS
