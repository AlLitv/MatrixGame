// MatrixGame - SR2 Planetary battles engine
// Copyright (C) 2012, Elemental Games, Katauri Interactive, CHK-Games
// Licensed under GPLv2 or any later version
// Refer to the LICENSE file included

#ifndef CMDQUEUE_HPP
#define CMDQUEUE_HPP

#include <vector>

// Слой команд (фаза 2, docs/NETPLAY_PLAN.md §5).
//
// Раньше обработчик ввода звал PGOrder* напрямую: мир менялся в момент кадра,
// а вместе с ним уходили вызовы ГПСЧ из самих приказов (звуки, выбор позы) и
// перегруппировка выделения. Для сети это недопустимо — две машины видят разные
// кадры, значит и приказы у них легли бы в разные такты.
//
// Теперь ввод собирает SCommand, а очередь применяет её на такте
// N + CMD_INPUT_LAG_TICKS — в одном и том же месте симуляции у всех. Порядок
// команд задаётся парой (такт, сторона, номер), поэтому он тоже одинаков.
//
// Текстовый лог команд (`MATRIXGAME_CMDLOG`) — он же реплей: прогон, записавший
// лог, и прогон, прочитавший его (`MATRIXGAME_CMDREPLAY`), должны дать
// побитово одинаковый мир.

#define CMD_INPUT_LAG_TICKS 3

enum ECommandType {
    CMD_NONE = 0,
    CMD_STOP,
    CMD_MOVE_TO,
    CMD_ATTACK,
    CMD_CAPTURE,
    CMD_PATROL,
    CMD_REPAIR,
    CMD_BOMB,
    CMD_AUTO_CAPTURE,
    CMD_AUTO_ATTACK,
    CMD_AUTO_DEFENCE,
    // Построить робота на базе по описанию: m_Param[0] корпус, [1] броня,
    // [2] голова, [3..7] оружие, [8] сколько.
    CMD_BUILD_ROBOT,
    // Построить турель на здании: m_Param[0] тип, [1] x*100, [2] y*100,
    // [3] угол, [4] индекс места; m_ObjId — здание.
    CMD_BUILD_TURRET,
    // Аркадный режим. Вход/выход — m_ObjId робот; управление на такт:
    // m_Param[0] поворот (-1/0/+1), [1] ход (-1/0/+1), [2] огонь,
    // [3] x прицела в клетках, [4] y прицела в клетках.
    CMD_ARCADE_ENTER,
    CMD_ARCADE_LEAVE,
    CMD_ARCADE_CONTROL,

    CMD_TYPE_MAX
};

enum ECommandFlags {
    // Приказ «стоять» в игре ещё и сбрасывает низкоуровневые приказы роботов
    // (MatrixFormGame зовёт SelectedGroupBreakOrders сразу после PGOrderStop).
    CMD_F_BREAK_ORDERS = 1 << 0,
};

// MAX_ROBOTS == 60, больше роботов в выделении быть не может.
#define CMD_SEL_MAX 64

// Небольшой набор целых параметров под тип команды (постройка робота: корпус,
// броня, голова, 5 оружий, количество; турель: тип, позиция, угол, место).
#define CMD_PARAM_CNT 9

// Стороны нумеруются маленькими id (0 — нейтрал, 1 — игрок).
#define CMD_SIDE_MAX 16

struct SCommand {
    int m_Type;    // ECommandType
    int m_Side;    // id стороны (CMatrixSideUnit::m_Id)
    int m_Tick;    // такт, на котором команду нужно применить
    int m_ObjId;   // id объекта-цели, -1 — цели нет
    int m_X, m_Y;  // точка приказа в клетках карты (CPoint)
    int m_Flags;   // ECommandFlags
    int m_Param[CMD_PARAM_CNT];
    int m_SelCnt;  // сколько роботов в выделении
    int m_Sel[CMD_SEL_MAX];
    unsigned m_Seq;
};

class CMatrixSideUnit;
class CMatrixMapStatic;

// Вызывается из MatrixGameInit после загрузки карты: читает MATRIXGAME_CMDLOG и
// MATRIXGAME_CMDREPLAY, пишет шапку лога, грузит реплей в очередь.
void CmdInit(void);
// Закрывает лог и пишет итоговую строку. Вызывается при выходе из боя.
void CmdShutdown(void);

// Начало логического такта: генерирует команды бота (в харнессе) и применяет
// все команды, срок которых наступил.
void CmdTick(int tick);

int CmdCurrentTick(void);
bool CmdReplayMode(void);
bool CmdLogEnabled(void);

// Кладёт команду в очередь. Если m_Tick <= 0, такт считается сам:
// текущий + CMD_INPUT_LAG_TICKS; m_Seq тоже заполняется здесь.
void CmdIssue(SCommand &cmd);

// Команда от партнёра по сети: такт и номер пришли с той стороны, очередь
// только держит её до нужного такта.
void CmdIssueRemote(SCommand &cmd);

// Собирает команду из текущего выделения стороны — замена
// CMatrixSideUnit::SelGroupToLogicGroup() на стороне ввода (сам мир не трогает).
int CmdCaptureSelection(CMatrixSideUnit *su, SCommand &cmd);

// Удобная обёртка для обработчиков ввода: выделение + тип + точка + цель.
// Возвращает такт, на котором команда будет применена, или -1, если команда не
// поставлена (пустое выделение).
int CmdOrder(CMatrixSideUnit *su, int type, const CPoint &tp, CMatrixMapStatic *target, bool breakOrders = false);
int CmdOrderObject(CMatrixSideUnit *su, int type, CMatrixMapStatic *target, bool breakOrders = false);

// Позиция объекта в клетках карты — для приказов «по объекту».
CPoint CmdObjectPos(CMatrixMapStatic *obj);

// Ручное управление (аркадный режим). Кадр собирает состояние клавиш и мыши в
// g_ArcadeInput, такт отправляет его командой CMD_ARCADE_CONTROL — так мир
// получает ввод по тактам, а не по кадрам, и одинаково на обеих машинах.
struct SArcadeInput {
    int m_Turn;  // -1 / 0 / +1 — поворот корпуса, накопленный с прошлого такта
    int m_Move;  // -1 / 0 / +1 — назад / стоять / вперёд
    bool m_Fire;  // держим огонь
    int m_AimX;   // прицел в клетках карты
    int m_AimY;
    int m_MouseTurn;  // мышь крутила корпус: -1 / 0 / +1, сбрасывается тактом
};

extern SArcadeInput g_ArcadeInput;

// Раз в кадр: собрать клавиши/курсор в g_ArcadeInput (только для своей стороны).
void ArcadeInputSample(void);
// Раз в такт: если игрок в роботе, поставить CMD_ARCADE_CONTROL на изменение.
void ArcadeInputTick(void);
// Из OnForward/OnBackward: -1 / 0 / +1.
void ArcadeInputMove(int dir);
// Из MouseMove: мышь повернула корпус.
void ArcadeInputMouseTurn(int dir);

// Вход/выход из робота через очередь команд (интерфейс зовёт это вместо
// SetArcadedObject, чтобы обе машины узнали об этом на одном такте).
int CmdArcadeEnter(CMatrixSideUnit *su, CMatrixMapStatic *robot);
int CmdArcadeLeave(CMatrixSideUnit *su);

#endif  // CMDQUEUE_HPP
