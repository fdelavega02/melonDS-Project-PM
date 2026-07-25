/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifdef _WIN32
    // must precede any windows.h (pulled in by SDL) to get winsock2, not winsock1
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <shellapi.h>
    #include <wctype.h>
#endif

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include <optional>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>

#include <SDL2/SDL.h>

#include "main.h"

#include "types.h"
#include "version.h"

#include "ScreenLayout.h"

#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include "NDSCart.h"
#include "GBACart.h"
#include "GPU.h"
#include "SPU.h"
#include "Wifi.h"
#include "Platform.h"
#include "LocalMP.h"
#include "MPInterface.h"
#include "LAN.h"
#include "Config.h"
#include "RTC.h"
#include "DSi.h"
#include "DSi_I2C.h"
#include "GPU_Soft.h"
#include "GPU_OpenGL.h"

#include "Savestate.h"

#include "EmuInstance.h"

using namespace melonDS;


EmuThread::EmuThread(EmuInstance* inst, QObject* parent) : QThread(parent)
{
    emuInstance = inst;

    emuStatus = emuStatus_Paused;
    emuPauseStack = emuPauseStackRunning;
    emuActive = false;
}

void EmuThread::attachWindow(MainWindow* window)
{
    connect(this, SIGNAL(windowTitleChange(QString)), window, SLOT(onTitleUpdate(QString)));
    connect(this, SIGNAL(windowEmuStart()), window, SLOT(onEmuStart()));
    connect(this, SIGNAL(windowEmuStop()), window, SLOT(onEmuStop()));
    connect(this, SIGNAL(windowEmuPause(bool)), window, SLOT(onEmuPause(bool)));
    connect(this, SIGNAL(windowEmuReset()), window, SLOT(onEmuReset()));
    connect(this, SIGNAL(autoScreenSizingChange(int)), window->panel, SLOT(onAutoScreenSizingChanged(int)));
    connect(this, SIGNAL(windowFullscreenToggle()), window, SLOT(onFullscreenToggled()));
    connect(this, SIGNAL(screenEmphasisToggle()), window, SLOT(onScreenEmphasisToggled()));

    if (window->winHasMenu())
    {
        connect(this, SIGNAL(windowLimitFPSChange()), window->actLimitFramerate, SLOT(trigger()));
        connect(this, SIGNAL(swapScreensToggle()), window->actScreenSwap, SLOT(trigger()));
    }
}

void EmuThread::detachWindow(MainWindow* window)
{
    disconnect(this, SIGNAL(windowTitleChange(QString)), window, SLOT(onTitleUpdate(QString)));
    disconnect(this, SIGNAL(windowEmuStart()), window, SLOT(onEmuStart()));
    disconnect(this, SIGNAL(windowEmuStop()), window, SLOT(onEmuStop()));
    disconnect(this, SIGNAL(windowEmuPause(bool)), window, SLOT(onEmuPause(bool)));
    disconnect(this, SIGNAL(windowEmuReset()), window, SLOT(onEmuReset()));
    disconnect(this, SIGNAL(autoScreenSizingChange(int)), window->panel, SLOT(onAutoScreenSizingChanged(int)));
    disconnect(this, SIGNAL(windowFullscreenToggle()), window, SLOT(onFullscreenToggled()));
    disconnect(this, SIGNAL(screenEmphasisToggle()), window, SLOT(onScreenEmphasisToggled()));

    if (window->winHasMenu())
    {
        disconnect(this, SIGNAL(windowLimitFPSChange()), window->actLimitFramerate, SLOT(trigger()));
        disconnect(this, SIGNAL(swapScreensToggle()), window->actScreenSwap, SLOT(trigger()));
    }
}


// ---------------------------------------------------------------------------
// Scripted test autopilot + RAM telemetry (debug tooling, env-driven).
//   MELONDS_AP=host|join      enable; pick LAN role
//   MELONDS_AP_IP=<ip>        join target (default 127.0.0.1)
// Drives the ROM through boot -> multiplayer activation, runs movement
// patterns, and logs the game's own view of every player (positions read
// straight out of emulated main RAM via the ROM's discovery block) to
// melonds_ap_<role>.csv for offline comparison.  Inert unless the env
// variable is set.
// ---------------------------------------------------------------------------
namespace
{

int apMode = -2;                // -2 uninit, -1 off, 0 host, 1 join
const char* apJoinIP = "127.0.0.1";
melonDS::u32 apFrame = 0;
melonDS::u32 apConnFrame = 0;   // frame the session connected (0 = not yet)
melonDS::u32 apDiscBase = 0;    // emu-addr of the ROM discovery block
FILE* apCsv = nullptr;

melonDS::u32 apRd32(melonDS::NDS* nds, melonDS::u32 addr)
{
    return *(melonDS::u32*)&nds->MainRAM[addr & nds->MainRAMMask];
}
melonDS::s16 apRd16s(melonDS::NDS* nds, melonDS::u32 addr)
{
    return (melonDS::s16)*(melonDS::u16*)&nds->MainRAM[addr & nds->MainRAMMask];
}
melonDS::u8 apRd8(melonDS::NDS* nds, melonDS::u32 addr)
{
    return nds->MainRAM[addr & nds->MainRAMMask];
}
void apWr32(melonDS::NDS* nds, melonDS::u32 addr, melonDS::u32 v)
{
    *(melonDS::u32*)&nds->MainRAM[addr & nds->MainRAMMask] = v;
}

// Activate/stop the wireless session via the ROM's debug inbox (sDiscovery[17],
// cmd 19) — in-game activation moved from the SELECT shortcut to the
// "Wireless Play" bag item, which an autopilot can't navigate to reliably.
void apWirelessCtl(melonDS::NDS* nds, melonDS::u32 discBase, int on)
{
    if (!discBase) return;
    melonDS::u32 inbox = apRd32(nds, discBase + 17*4);
    if (!inbox) return;
    apWr32(nds, inbox + 12, 19);            // cmd
    apWr32(nds, inbox + 16, on ? 1 : 0);    // arg[0]
    apWr32(nds, inbox + 4, apRd32(nds, inbox + 8) + 1);   // seq = ackSeq+1
}

static volatile unsigned int g_apLoopSeq = 0;   // emu-loop heartbeat
static int apInField = 0;        // latched once the overworld is running
static melonDS::u32 apFieldFC = 0;

melonDS::u32 apApply(melonDS::NDS* nds, melonDS::u32 maskIn)
{
    using namespace melonDS;
    u32 mask = maskIn;

    g_apLoopSeq++;   // RunFrame from the previous iteration returned

    if (apMode == -2)
    {
        const char* e = getenv("MELONDS_AP");
        apMode = (!e) ? -1 : (!strcmp(e, "host")) ? 0 : (!strcmp(e, "join")) ? 1 : -1;
        const char* ip = getenv("MELONDS_AP_IP");
        if (ip) apJoinIP = ip;
    }
    if (apMode < 0) return mask;

    apFrame++;

    // ---- transport bring-up ----
    // The mailbox bridge now rides a direct TCP MpNet (the SAME wire protocol
    // as the DeSmuME and BizHawk forks), NOT melonDS's ENet LAN — so all three
    // emulators interoperate in one session.  The TCP transport self-starts
    // inside BridgePump from the MELONDS_AP env; nothing to do here.

    // ---- input script ----
    // MELONDS_AP_HOLD: boot to the overworld and start the LAN side, but
    // do NOT activate in-game multiplayer and run no movement patterns.
    // Activation fires when a file named "melonds_go.txt" appears in the
    // working directory (SELECT, then A on "Activate Multiplayer").
    static int apHold = -1;
    static melonDS::u32 apGoFrame = 0;
    if (apHold < 0) apHold = getenv("MELONDS_AP_HOLD") ? 1 : 0;

    u32 press = 0;
    if (apFrame >= 700 && apFrame < 2000 && !apInField)
    {
        if ((apFrame % 40) < 8) press |= (1 << 0);            // tap A (title/file select)
    }
    else if (!apHold && apFrame == 2120)
    {
        apWirelessCtl(nds, apDiscBase, 1);   // debug-inbox activation (cmd 19)
    }
    else if (apHold && apFrame > 2100)
    {
        if (!apGoFrame && (apFrame % 30) == 0)
        {
            char gn[64]; snprintf(gn, sizeof(gn), "melonds_go_%s.txt", (apMode==0)?"host":"join");
            FILE* gf = fopen(gn, "r");
            if (gf)
            {
                fclose(gf);
                remove(gn);   // consume: re-armed for next touch
                apGoFrame = apFrame;
            }
        }
        if (apGoFrame)
        {
            u32 gf = apFrame - apGoFrame;
            if (gf == 10)        apWirelessCtl(nds, apDiscBase, 1);  // inbox activation
            else if (gf >= 300)  apGoFrame = 0;                      // re-arm
        }
    }

    // ---- discovery scan + telemetry ----
    if (apFrame >= 600 && apDiscBase == 0 && (apFrame % 60) == 0)
    {
        for (u32 off = 0; off < 0x400000 - 16; off += 4)
        {
            if (*(u32*)&nds->MainRAM[off] == 0xCAFE1234
                && *(u32*)&nds->MainRAM[off+4] == 0x5678CAFE)
            {
                apDiscBase = 0x02000000 + off;
                printf("[AP] discovery at %08X\n", apDiscBase);
                break;
            }
        }
    }

    if (apDiscBase)
    {
        u32 pOwExp = apRd32(nds, apDiscBase + 14*4);
        u32 pOwImp = apRd32(nds, apDiscBase + 15*4);
        u32 pDiag  = apRd32(nds, apDiscBase + 30*4);
        u8 wmState = pDiag ? apRd8(nds, pDiag + 4) : 0xFF;
        u8 peerMask = pDiag ? apRd8(nds, pDiag + 7) : 0;

        // field detection: overworld frameCounter advancing == in-field.
        // Latches the boot A-mash OFF so it cannot leak into a counter menu.
        if (!apInField)
        {
            u32 fc = apRd32(nds, pOwExp + 0x0C);
            if (fc != 0 && apFieldFC != 0 && fc != apFieldFC) apInField = 1;
            apFieldFC = fc;
        }

        if (!apConnFrame && peerMask) 
        {
            apConnFrame = apFrame;
            printf("[AP] connected at frame %u\n", apFrame);
        }

        // Remote movement commands (hold mode): a file "melonds_move.txt"
        // containing tokens like "L1 U20 R3 D2" walks the player tile-
        // exactly: each token holds its direction until the live grid
        // coordinate (read from the export block) reaches the target, with
        // a timeout guard for walls.  File is consumed on pickup.
        {
            static char mvSeq[256];
            static int mvLen = 0, mvPos = 0, mvActive = 0, mvBit = 0;
            static int mvTX = 0, mvTZ = 0;
            static int mvMode = 0;      // 0=walk 1=tap 2=wait
            static int mvTimer = 0;
            static int mvSprint = 0;
            static melonDS::u32 mvDeadline = 0;

            if (apFrame > 2100)
            {
                int curX = apRd16s(nds, pOwExp + 4);
                int curZ = apRd16s(nds, pOwExp + 6);

                if ((apFrame % 10) == 0 && (mvActive || mvPos < mvLen))
                {
                    // abort: a command file starting with '!' cancels everything
                    char an[64]; snprintf(an, sizeof(an), "melonds_move_%s.txt", (apMode==0)?"host":"join");
                    FILE* af = fopen(an, "r");
                    if (af)
                    {
                        int c0 = fgetc(af);
                        fclose(af);
                        if (c0 == '!')
                        {
                            remove(an);
                            mvActive = 0; mvLen = 0; mvPos = 0;
                        }
                    }
                }
                if (!mvActive && mvPos >= mvLen && (apFrame % 10) == 0)
                {
                    char mn[64]; snprintf(mn, sizeof(mn), "melonds_move_%s.txt", (apMode==0)?"host":"join");
                    FILE* mf = fopen(mn, "r");
                    if (mf)
                    {
                        mvLen = (int)fread(mvSeq, 1, 255, mf);
                        if (mvLen < 0) mvLen = 0;
                        mvSeq[mvLen] = 0;
                        fclose(mf);
                        remove(mn);
                        mvPos = 0;
                    }
                }
                if (!mvActive && mvPos < mvLen)
                {
                    while (mvPos < mvLen && (mvSeq[mvPos] == ' ' || mvSeq[mvPos] == 10 || mvSeq[mvPos] == 13 || mvSeq[mvPos] == 9)) mvPos++;
                    if (mvPos < mvLen)
                    {
                        char d = mvSeq[mvPos++];
                        int n = 0;
                        while (mvPos < mvLen && mvSeq[mvPos] >= '0' && mvSeq[mvPos] <= '9') n = n * 10 + (mvSeq[mvPos++] - '0');
                        char dl = (char)(d | 0x20);

                        if (n == 0 && (dl=='l'||dl=='r'||dl=='u'||dl=='d'))
                        {
                            // bare direction = one TAP (face/menu navigation)
                            switch (dl)
                            {
                            case 'l': mvBit = 5; break;
                            case 'r': mvBit = 4; break;
                            case 'u': mvBit = 6; break;
                            default:  mvBit = 7; break;
                            }
                            mvMode = 1; mvTimer = 34;
                            mvActive = 1;
                        }
                        else if (n > 0 && (dl=='l'||dl=='r'||dl=='u'||dl=='d'))
                        {
                            // movement: uppercase walks, lowercase sprints
                            mvSprint = (d >= 'a');
                            mvTX = curX; mvTZ = curZ;
                            switch (dl)
                            {
                            case 'l': mvBit = 5; mvTX = curX - n; break;
                            case 'r': mvBit = 4; mvTX = curX + n; break;
                            case 'u': mvBit = 6; mvTZ = curZ - n; break;
                            default:  mvBit = 7; mvTZ = curZ + n; break;
                            }
                            mvDeadline = apFrame + (melonDS::u32)n * 60 + 240;
                            mvMode = 0;
                            mvActive = 1;
                        }
                        else if (dl=='w' && n > 0)
                        {
                            mvMode = 2; mvTimer = n; mvActive = 1;
                        }
                        else if (dl=='a'||dl=='b'||dl=='x'||dl=='y'||dl=='s'||dl=='c')
                        {
                            // button tap: A B X Y S(tart) C(select)
                            switch (dl)
                            {
                            case 'a': mvBit = 0; break;
                            case 'b': mvBit = 1; break;
                            case 'c': mvBit = 2; break;
                            case 's': mvBit = 3; break;
                            case 'x': mvBit = 10; break;
                            default:  mvBit = 11; break;
                            }
                            mvMode = 1; mvTimer = 40;   // 12f press + gap
                            mvActive = 1;
                        }
                    }
                }
                if (mvActive)
                {
                    if (mvMode == 0)
                    {
                        int done = (apFrame > mvDeadline)
                            || (mvBit == 5 && curX <= mvTX) || (mvBit == 4 && curX >= mvTX)
                            || (mvBit == 6 && curZ <= mvTZ) || (mvBit == 7 && curZ >= mvTZ);
                        if (done) mvActive = 0;
                        else
                        {
                            press |= (1u << mvBit);
                            if (mvSprint) press |= (1u << 1);
                        }
                    }
                    else if (mvMode == 1)
                    {
                        if (mvTimer > 28) press |= (1u << mvBit);   // first 12f pressed
                        if (--mvTimer <= 0) mvActive = 0;
                    }
                    else
                    {
                        if (--mvTimer <= 0) mvActive = 0;
                    }
                }
            }
        }

        // movement phases relative to connect.
        // MELONDS_AP_PATTERN selects the stress pattern:
        //   (unset)/walk : hold a direction, alternating every 90f
        //   run          : same, with B held (sprint — double tile rate)
        //   circle       : B held, cycle RIGHT->DOWN->LEFT->UP every 32f
        //                  (running in a loop: direction-change stress)
        if (apConnFrame && !apHold)
        {
            static int patMode = -1;
            if (patMode < 0)
            {
                const char* pat = getenv("MELONDS_AP_PATTERN");
                patMode = (!pat) ? 0 : (!strcmp(pat, "run")) ? 1
                        : (!strcmp(pat, "circle")) ? 2
                        : (!strcmp(pat, "waggle")) ? 3
                        : (!strcmp(pat, "ugclient")) ? 4
                        : (!strcmp(pat, "ughost")) ? 5 : 0;
            }
            u32 cf = apFrame - apConnFrame;
            u32 winLo = (apMode == 0) ? 300u : 1700u;
            u32 winHi = (apMode == 0) ? 1500u : 2900u;

            if (patMode >= 4)
            {
                // Underground leave/re-enter repro (the user's manual recipe):
                //   both: Y (registered Explorer Kit) then A every 1s x30 -> descend
                //   leaver only: X menu, Down x5, A x4 (2s apart) -> "Go up"
                //   wait ~15s, then Y + A every 1s x30 -> descend again
                // ugclient: the JOIN instance leaves; ughost: the HOST leaves.
                bool leaver = (patMode == 4) ? (apMode == 1) : (apMode == 0);
                // 4-player runs: several instances share apMode=join, but only
                // ONE should exercise the leave/re-enter arc.  Extra joiners
                // set MELONDS_AP_NOLEAVE=1 and just descend + stand.
                static int apNoLeave = -1;
                if (apNoLeave < 0) apNoLeave = getenv("MELONDS_AP_NOLEAVE") ? 1 : 0;
                if (apNoLeave) leaver = false;

                if (cf >= 300 && cf < 308)
                    press |= (1u << 11);                                   // Y: Explorer Kit
                else if (cf >= 360 && cf < 360 + 30*60 && ((cf - 360) % 60) < 8)
                    press |= (1u << 0);                                    // A x30 (1/s)

                if (leaver)
                {
                    const u32 L = 2400;                                    // below by now
                    const u32 R = L + 240 + 4*120 + 900;                   // after up + ~15s
                    if (cf >= L && cf < L + 8)
                        press |= (1u << 10);                               // X: menu
                    else if (cf >= L + 60 && cf < L + 60 + 5*24 && ((cf - L - 60) % 24) < 8)
                        press |= (1u << 7);                                // Down x5
                    else if (cf >= L + 240 && cf < L + 240 + 4*120 && ((cf - L - 240) % 120) < 8)
                        press |= (1u << 0);                                // A x4 (2s apart)
                    else if (cf >= R && cf < R + 8)
                        press |= (1u << 11);                               // Y again
                    else if (cf >= R + 60 && cf < R + 60 + 30*60 && ((cf - R - 60) % 60) < 8)
                        press |= (1u << 0);                                // A x30: re-descend
                }
            }
            else if (cf >= winLo && cf < winHi)
            {
                if (patMode == 3)
                {
                    // waggle: sprint direction-flips every 8f (240f), then a
                    // sustained sprint (180f, direction alternates per cycle),
                    // then a full stop (90f); repeat.  Host on the L/R axis,
                    // join on U/D.
                    u32 pc = (cf - winLo) % 510;
                    int bitA = (apMode == 0) ? 5 : 6;   // LEFT / UP
                    int bitB = (apMode == 0) ? 4 : 7;   // RIGHT / DOWN
                    if (pc < 240)
                    {
                        press |= (1 << 1);
                        press |= (1 << (((pc / 8) & 1) ? bitA : bitB));
                    }
                    else if (pc < 420)
                    {
                        press |= (1 << 1);
                        press |= (1 << ((((cf - winLo) / 510) & 1) ? bitB : bitA));
                    }
                    // else: stopped
                }
                else if (patMode == 2)
                {
                    static const int circleBit[4] = { 4, 7, 5, 6 };  // R,D,L,U
                    press |= (1 << 1);                               // B: run
                    press |= (1 << circleBit[(cf / 32) & 3]);
                }
                else
                {
                    if (patMode == 1)
                        press |= (1 << 1);                           // B: run
                    if (apMode == 0)
                        press |= (1 << (((cf / 90) & 1) ? 5 : 4));   // RIGHT/LEFT
                    else
                        press |= (1 << (((cf / 90) & 1) ? 7 : 6));   // UP/DOWN
                }
            }
        }

        // GAME-HANG PC SAMPLER: if the game's own frame counter stops
        // advancing while connected, the ARM9 is wedged (emulator still
        // fine).  Sample R15/R14 for 32 frames and dump — the exact hang
        // site, resolvable against the linker map.
        {
            static u32 lastFC = 0, lastChangeAt = 0, dumped = 0;
            u32 fcNow = apRd32(nds, pOwExp + 0x0C);
            if (fcNow != lastFC) { lastFC = fcNow; lastChangeAt = apFrame; dumped = 0; }
            else if (apConnFrame && apFrame - lastChangeAt > 180 && dumped < 64)
            {
                char hn[64];
                snprintf(hn, sizeof(hn), "melonds_hangpc_%s.txt", (apMode==0)?"host":"join");
                FILE* hf = fopen(hn, "a");
                if (hf)
                {
                    fprintf(hf, "f=%u FCstuck=%u R15=%08X R14=%08X R13=%08X R12=%08X CPSR=%08X IE=%08X IF=%08X IME=%u\n",
                        apFrame, fcNow,
                        nds->ARM9.R[15], nds->ARM9.R[14], nds->ARM9.R[13], nds->ARM9.R[12],
                        nds->ARM9.CPSR, nds->IE[0], nds->IF[0], (unsigned)nds->IME[0]);
                    fclose(hf);
                    dumped++;
                }
            }
        }

        if (!apCsv)
        {
            char name[64];
            snprintf(name, sizeof(name), "melonds_ap_%s.csv", (apMode == 0) ? "host" : "join");
            apCsv = fopen(name, "w");
            if (apCsv) fprintf(apCsv, "frame,wmState,peerMask,ownX,ownZ,ownFC,i0X,i0Z,i0FC,i1X,i1Z,i1FC,epoch,bumps,rew,recOw,recBlk,recParty,recPkt,lastSrcType,ovf,gaps,subf,rxqd,pexp,pcontig,bcnCalls,bcnApplies,bcnSeq,txF,rxF\n");
        }
        if (apCsv && pOwExp && pOwImp)
        {
            fprintf(apCsv, "%u,%u,%u,%d,%d,%u,%d,%d,%u,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                apFrame, wmState, peerMask,
                apRd16s(nds, pOwExp+4), apRd16s(nds, pOwExp+6), apRd32(nds, pOwExp+0x0C),
                apRd16s(nds, pOwImp+4), apRd16s(nds, pOwImp+6), apRd32(nds, pOwImp+0x0C),
                apRd16s(nds, pOwImp+48+4), apRd16s(nds, pOwImp+48+6), apRd32(nds, pOwImp+48+0x0C),
                apRd8(nds, pDiag+8), apRd8(nds, pDiag+40), apRd32(nds, pDiag+28),
                apRd8(nds, pDiag+36), apRd8(nds, pDiag+37), apRd8(nds, pDiag+38), apRd8(nds, pDiag+39),
                apRd8(nds, pDiag+41), apRd8(nds, pDiag+11), apRd8(nds, pDiag+42), apRd8(nds, pDiag+43), apRd8(nds, pDiag+44), (apRd32(nds, pDiag+32)>>16)&0xFFFF, apRd32(nds, pDiag+32)&0xFFFF, apRd8(nds, pDiag+45), apRd8(nds, pDiag+46), apRd8(nds, pDiag+47), apRd32(nds, pDiag+12), apRd32(nds, pDiag+16));
            if ((apFrame & 255) == 0) fflush(apCsv);
        }
    }

    return mask & ~press;
}

}
// ---------------------------------------------------------------------------

namespace melonDS {
extern volatile unsigned int g_wifiCrumb;
extern volatile unsigned int g_wifiCrumbSeq;
extern volatile unsigned int g_asyncSpin;
}

// Freeze watchdog: a detached thread (survives an emu-thread hang) that
// records the last wifi breadcrumb whenever the emu thread stops making
// progress.  Enabled by MELONDS_AP (autopilot) so normal runs pay nothing.
static void MpWatchdogStart(const char* role)
{
    std::string r = role;
    std::thread([r]() {
        std::string name = "melonds_watchdog_" + r + ".txt";
        unsigned int lastLoop = 0;
        int stuckMs = 0;
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            unsigned int loop = g_apLoopSeq;
            if (loop == lastLoop)
            {
                // emu loop has not advanced a frame: RunFrame is wedged.
                stuckMs += 200;
                if (stuckMs == 1000 || stuckMs == 4000 || stuckMs == 12000)
                {
                    FILE* f = fopen(name.c_str(), "a");
                    if (f) {
                        // wifiSeq advancing while loop is stuck => spinning
                        // inside a wifi loop; asyncSpin identifies phase-14
                        fprintf(f, "STUCK %dms loop=%u wifiCrumb=%u wifiSeq=%u asyncSpin=%u\n",
                            stuckMs, loop, melonDS::g_wifiCrumb,
                            melonDS::g_wifiCrumbSeq, melonDS::g_asyncSpin);
                        fclose(f);
                    }
                }
            }
            else { stuckMs = 0; lastLoop = loop; }
        }
    }).detach();
}


// ---------------------------------------------------------------------------
// Fork-embedded mailbox bridge.  When the loaded ROM publishes the bridge
// control block (discovery slot 31) and the player has a LAN session, the
// ROM's field menu can request a bridge session: this pump then syncs the
// ROM's multiplayer mailboxes across the LAN link at frame rate (reliable
// ENet channel, packet type 4).  The emulated radio is never touched, so
// native wireless features keep full ownership of it.
// ---------------------------------------------------------------------------
namespace
{

// ---------------------------------------------------------------------------
// Cross-emulator TCP transport.  Identical wire protocol to the DeSmuME and
// BizHawk forks — [u16 len LE][tag u8][role u8][size u16 LE][payload], plus a
// [len=2][0xFF][role] control frame the host sends to assign each joiner its
// role — so a melonDS host/joiner interoperates with DeSmuME and BizHawk
// instances in the same session.  Host listens :7820 and relays; joiners
// connect.  Non-blocking; drained once per frame from BridgePump.
// ---------------------------------------------------------------------------
namespace mpnet
{
const int PORT = 7820;
const melonDS::u32 MAXFRAME = 8192;

#ifdef _WIN32
// Windows Firewall helper for hosting (same scheme as the DeSmuME fork's
// mp_bridge.cpp).  Router port-forwarding alone can't make a host reachable:
// inbound traffic must also pass this PC's own firewall, and declining the
// first-launch security alert leaves a permanent block rule for the exe.
// A melonDS host needs BOTH its native LAN port (UDP 7064 + discovery) and
// the bridge port (TCP 7820) open, so the rule is program-scoped.  When our
// rule is absent we ASK the player, then run the standard netsh repair
// visibly with one admin prompt.
const wchar_t* FW_RULE = L"melonDS (Project PM)";

static void fwLower(std::wstring& s)
{
    for (size_t i = 0; i < s.size(); i++) s[i] = towlower(s[i]);
}

static bool fwRulePresent(const wchar_t* exe)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\FirewallRules",
        0, KEY_READ, &k) != ERROR_SUCCESS) return false;

    // rule values are pipe-delimited token strings: v2.x|Action=Allow|...|App=path|...|Name=...|
    std::wstring wantName = L"|name="; wantName += FW_RULE; wantName += L"|";
    std::wstring wantApp  = L"|app=";  wantApp  += exe;     wantApp  += L"|";
    fwLower(wantName); fwLower(wantApp);

    bool found = false;
    wchar_t name[256]; BYTE data[4096];
    for (DWORD i = 0; !found; i++)
    {
        DWORD nl = 256, dl = sizeof(data) - 2, type = 0;
        LONG r = RegEnumValueW(k, i, name, &nl, NULL, &type, data, &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;
        data[dl] = 0; data[dl + 1] = 0;
        std::wstring v((const wchar_t*)data);
        fwLower(v);
        found = v.find(wantName) != std::wstring::npos
             && v.find(wantApp)  != std::wstring::npos
             && v.find(L"|action=allow|") != std::wstring::npos
             && v.find(L"|dir=in|")       != std::wstring::npos
             && v.find(L"|active=true|")  != std::wstring::npos;
    }
    RegCloseKey(k);
    return found;
}

static void ensureFirewall()
{
    static bool once = false;
    if (once) return;
    once = true;
    if (getenv("MELONDS_AP")) return;   // automated harness instances: no prompts

    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return;

    if (fwRulePresent(exe)) { printf("[BR] firewall rule ok\n"); return; }

    if (MessageBoxW(NULL,
        L"To let friends join your hosted game, Windows Firewall needs an\n"
        L"inbound rule for this emulator (without it, players outside your\n"
        L"PC usually can't connect even with the ports forwarded).\n\n"
        L"Add the rule now?  Windows will show one administrator prompt.",
        L"Project PM - enable hosting", MB_YESNO | MB_ICONQUESTION) != IDYES)
    {
        printf("[BR] firewall rule declined by user\n");
        return;
    }

    wchar_t args[1200];
    swprintf(args, 1200,
        L"/c netsh advfirewall firewall delete rule name=all program=\"%ls\" & "
        L"netsh advfirewall firewall add rule name=\"%ls\" dir=in action=allow "
        L"program=\"%ls\" enable=yes profile=any", exe, FW_RULE, exe);
    HINSTANCE rc = ShellExecuteW(NULL, L"runas", L"cmd.exe", args, NULL, SW_SHOWMINNOACTIVE);
    printf("[BR] firewall rule %s\n",
        ((INT_PTR)rc > 32) ? "repair launched" : "not added (admin prompt declined)");
}
#else
static void ensureFirewall() {}
#endif

struct Peer
{
    SOCKET s = INVALID_SOCKET;
    bool up = false;
    std::vector<melonDS::u8> rx, tx;
};

struct Net
{
    int mode = 0;                       // 0 off, 1 host, 2 join
    char joinIP[64] = "127.0.0.1";
    bool started = false;
    SOCKET listener = INVALID_SOCKET;
    Peer peers[3];                      // host: 3 client slots (slot i = role 2+i)
                                        // join: peers[0] = host link
    bool connecting = false;
    bool freshPeer = false;     // link just came up: pump resends on-change
                                // channels (party/pkt) — a peer connecting
                                // after our first send never got our party
    melonDS::u32 retryAt = 0;
    int assignedRole = 0;               // join: role handed out by the host

    int myRole() const { return (mode == 1) ? 1 : (assignedRole ? assignedRole : 2); }
    bool anyUp() const { for (int i=0;i<3;i++) if (peers[i].up) return true; return false; }

    static void setNonBlock(SOCKET s)
    {
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
        BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
    }

    void startHost()
    {
        ensureFirewall();
        WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
        mode = 1; started = true;
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener == INVALID_SOCKET) { mode = 0; return; }
        BOOL yes = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(PORT);
        if (bind(listener, (sockaddr*)&a, sizeof(a)) != 0 || listen(listener, 3) != 0)
        {
            closesocket(listener); listener = INVALID_SOCKET; mode = 0; return;
        }
        setNonBlock(listener);
        printf("[BR] hosting on :%d\n", PORT);
    }

    void startJoin(const char* ip)
    {
        WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
        if (ip && ip[0]) { strncpy(joinIP, ip, sizeof(joinIP)-1); joinIP[sizeof(joinIP)-1] = 0; }
        mode = 2; started = true;
    }

    // LAN discovery beacon (UDP :7821, shared cross-emulator format) so a
    // melonDS host also shows up in DeSmuME / BizHawk auto-find lists.
    SOCKET beaconTx = INVALID_SOCKET;
    melonDS::u32 lastBeacon = 0;
    void beaconTick(melonDS::u32 frame, melonDS::u8 players)
    {
        if (mode != 1) return;
        if (beaconTx == INVALID_SOCKET)
        {
            beaconTx = socket(AF_INET, SOCK_DGRAM, 0);
            if (beaconTx == INVALID_SOCKET) return;
            BOOL b = TRUE; setsockopt(beaconTx, SOL_SOCKET, SO_BROADCAST, (const char*)&b, sizeof(b));
        }
        if (frame - lastBeacon < 60) return;
        lastBeacon = frame;
        melonDS::u8 buf[32]; memset(buf, 0, sizeof(buf));
        memcpy(buf, "PLATMP", 6); buf[6] = 1; buf[7] = players;
        char nm[24]; DWORD n = 24; if (!GetComputerNameA(nm, &n)) { strcpy(nm, "melonDS"); n = 7; }
        memcpy(buf + 8, nm, (n < 24) ? n : 23);
        sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons(7821); a.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(beaconTx, (const char*)buf, 32, 0, (sockaddr*)&a, sizeof(a));
    }

    void startFromEnv()
    {
        started = true;
        const char* e = getenv("MELONDS_AP");
        if (e && !strcmp(e, "host")) startHost();
        else if (e && !strcmp(e, "join")) startJoin(getenv("MELONDS_AP_IP"));
        else mode = 0;
    }

    void dropPeer(int i)
    {
        Peer& p = peers[i];
        if (p.s != INVALID_SOCKET) closesocket(p.s);
        p.s = INVALID_SOCKET; p.up = false; p.rx.clear(); p.tx.clear();
        if (mode == 2) connecting = false;
    }

    void tick(melonDS::u32 frame)
    {
        if (mode == 0) return;
        if (mode == 1 && listener != INVALID_SOCKET)
        {
            for (int i = 0; i < 3; i++)
            {
                if (peers[i].up) continue;
                SOCKET s = accept(listener, NULL, NULL);
                if (s == INVALID_SOCKET) break;
                setNonBlock(s);
                peers[i].s = s; peers[i].up = true; freshPeer = true;
                melonDS::u8 ctl[4] = { 2, 0, 0xFF, (melonDS::u8)(2 + i) };
                peers[i].tx.insert(peers[i].tx.end(), ctl, ctl + 4);
                printf("[BR] peer accepted -> role %d\n", 2 + i);
            }
        }
        else if (mode == 2 && !peers[0].up)
        {
            Peer& h = peers[0];
            if (connecting)
            {
                fd_set wr, ex; FD_ZERO(&wr); FD_ZERO(&ex);
                FD_SET(h.s, &wr); FD_SET(h.s, &ex);
                timeval tv = {0,0};
                int r = select(0, NULL, &wr, &ex, &tv);
                if (r > 0 && FD_ISSET(h.s, &wr)) { h.up = true; connecting = false; freshPeer = true; printf("[BR] connected to %s\n", joinIP); }
                else if (r > 0 && FD_ISSET(h.s, &ex)) { dropPeer(0); retryAt = frame + 120; }
            }
            else if (frame >= retryAt)
            {
                h.s = socket(AF_INET, SOCK_STREAM, 0);
                if (h.s == INVALID_SOCKET) { retryAt = frame + 120; return; }
                setNonBlock(h.s);
                sockaddr_in a; memset(&a, 0, sizeof(a));
                a.sin_family = AF_INET; a.sin_port = htons(PORT);
                inet_pton(AF_INET, joinIP, &a.sin_addr);
                int r = ::connect(h.s, (sockaddr*)&a, sizeof(a));
                if (r == 0) { h.up = true; freshPeer = true; printf("[BR] connected to %s\n", joinIP); }
                else if (WSAGetLastError() == WSAEWOULDBLOCK) connecting = true;
                else { dropPeer(0); retryAt = frame + 120; }
            }
        }
        for (int i = 0; i < 3; i++) { flush(i); pumpRecv(i); }
    }

    void enqueue(int i, const melonDS::u8* p, melonDS::u32 n)
    {
        Peer& pr = peers[i];
        if (!pr.up || n == 0 || n > MAXFRAME) return;
        if (pr.tx.size() > 512*1024) return;
        melonDS::u8 hdr[2] = { (melonDS::u8)(n & 0xFF), (melonDS::u8)(n >> 8) };
        pr.tx.insert(pr.tx.end(), hdr, hdr + 2);
        pr.tx.insert(pr.tx.end(), p, p + n);
        flush(i);
    }
    void sendAll(const melonDS::u8* p, melonDS::u32 n) { for (int i=0;i<3;i++) enqueue(i, p, n); }

    void flush(int i)
    {
        Peer& p = peers[i];
        if (!p.up || p.tx.empty()) return;
        int r = ::send(p.s, (const char*)p.tx.data(), (int)p.tx.size(), 0);
        if (r > 0) p.tx.erase(p.tx.begin(), p.tx.begin() + r);
        else if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) dropPeer(i);
    }

    void pumpRecv(int i)
    {
        Peer& p = peers[i];
        if (!p.up) return;
        char tmp[16384];
        for (;;)
        {
            int r = ::recv(p.s, tmp, sizeof(tmp), 0);
            if (r > 0) { p.rx.insert(p.rx.end(), tmp, tmp + r); if (p.rx.size() > 1024*1024) { dropPeer(i); return; } }
            else if (r == 0) { dropPeer(i); return; }
            else { if (WSAGetLastError() != WSAEWOULDBLOCK) dropPeer(i); return; }
        }
    }

    melonDS::u32 recvFrame(int i, melonDS::u8* out, melonDS::u32 outMax)
    {
        Peer& p = peers[i];
        if (p.rx.size() < 2) return 0;
        melonDS::u32 n = (melonDS::u32)p.rx[0] | ((melonDS::u32)p.rx[1] << 8);
        if (n == 0 || n > MAXFRAME) { dropPeer(i); return 0; }
        if (p.rx.size() < 2 + n) return 0;
        melonDS::u32 c = (n <= outMax) ? n : outMax;
        memcpy(out, p.rx.data() + 2, c);
        p.rx.erase(p.rx.begin(), p.rx.begin() + 2 + n);
        return c;
    }

    void shutdown()
    {
        for (int i=0;i<3;i++) dropPeer(i);
        if (beaconTx != INVALID_SOCKET) { closesocket(beaconTx); beaconTx = INVALID_SOCKET; }
        if (listener != INVALID_SOCKET) { closesocket(listener); listener = INVALID_SOCKET; }
        mode = 0;
    }
};
Net gNet;
}

struct BridgeSt
{
    melonDS::u32 frame = 0;
    melonDS::u32 disc = 0, ctl = 0;
    melonDS::u32 exportBlk = 0, importBlk = 0, partyExp = 0, partyImp = 0;
    melonDS::u32 pktExp = 0, pktImp = 0, owExp = 0, owImp = 0;
    melonDS::u32 blkN = 0, partyN = 0;
    melonDS::u32 blkSize = 0, partySize = 0, pktSize = 0;
    melonDS::u8 beat = 0;
    std::vector<melonDS::u8> lastParty, lastPkt;
    melonDS::u32 roleSeenAt[5] = {0,0,0,0,0};
    melonDS::u32 dbgTx = 0, dbgRx = 0, dbgPktTx = 0, dbgPktRx = 0;

    melonDS::u8 FreshPeerMask(int myRole) const
    {
        melonDS::u8 m = 0;
        for (int r = 1; r <= 4; r++)
            if (r != myRole && roleSeenAt[r] != 0 && frame - roleSeenAt[r] <= 180)
                m |= (melonDS::u8)(1 << (r - 1));
        return m;
    }
};
BridgeSt gBr;

void apWr8(melonDS::NDS* nds, melonDS::u32 addr, melonDS::u8 v)
{
    nds->MainRAM[addr & nds->MainRAMMask] = v;
}
melonDS::u8* apPtr(melonDS::NDS* nds, melonDS::u32 addr)
{
    return &nds->MainRAM[addr & nds->MainRAMMask];
}

void BridgePump(melonDS::NDS* nds)
{
    using namespace melonDS;
    gBr.frame++;

    if (!mpnet::gNet.started) mpnet::gNet.startFromEnv();
    if (mpnet::gNet.mode == 0)
    {
        // No env config (a real player): follow the melonDS LAN lobby the
        // player used.  Hosting a LAN game starts the TCP bridge host;
        // joining one connects the bridge to the lobby host IP.  The ENet
        // session is left alone; the bridge rides alongside on :7820.
        if (MPInterface::GetType() == MPInterface_LAN)
        {
            LAN& lan = (LAN&)MPInterface::Get();
            if (lan.GetIsHost())
            {
                mpnet::gNet.startHost();
            }
            else if (lan.GetMyPlayerID() > 0)
            {
                melonDS::u32 ha = lan.GetHostAddress();
                if (ha)
                {
                    char ip[32];
                    snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                        ha & 0xFF, (ha >> 8) & 0xFF, (ha >> 16) & 0xFF, (ha >> 24) & 0xFF);
                    mpnet::gNet.startJoin(ip);
                }
            }
        }
        if (mpnet::gNet.mode == 0) return;
    }
    mpnet::gNet.tick(gBr.frame);

    // A peer link just came up: drop the on-change send caches so the next
    // pump resends party/pkt — a peer connecting AFTER our first send never
    // received our party (battle/trade launch waits on it forever).
    if (mpnet::gNet.freshPeer)
    {
        mpnet::gNet.freshPeer = false;
        gBr.lastParty.clear();
        gBr.lastPkt.clear();
    }

    if (!gBr.disc)
    {
        if ((gBr.frame % 60) != 0) return;
        for (u32 off = 0; off < 0x400000 - 16; off += 4)
        {
            if (*(u32*)&nds->MainRAM[off] == 0xCAFE1234
                && *(u32*)&nds->MainRAM[off+4] == 0x5678CAFE)
            {
                gBr.disc = 0x02000000 + off;
                break;
            }
        }
        if (!gBr.disc) return;

        gBr.exportBlk = apRd32(nds, gBr.disc + 2*4);
        gBr.importBlk = apRd32(nds, gBr.disc + 3*4);
        gBr.partyExp  = apRd32(nds, gBr.disc + 4*4);
        gBr.partyImp  = apRd32(nds, gBr.disc + 5*4);
        gBr.pktExp    = apRd32(nds, gBr.disc + 12*4);
        gBr.pktImp    = apRd32(nds, gBr.disc + 13*4);
        gBr.owExp     = apRd32(nds, gBr.disc + 14*4);
        gBr.owImp     = apRd32(nds, gBr.disc + 15*4);
        gBr.blkSize   = apRd32(nds, gBr.disc + 20*4) & 0xFFFF;
        gBr.blkN      = apRd32(nds, gBr.disc + 25*4);
        gBr.partyN    = apRd32(nds, gBr.disc + 26*4);
        gBr.ctl       = apRd32(nds, gBr.disc + 31*4);
    }
    if (!gBr.ctl || apRd32(nds, gBr.ctl) != 0x42524731) return;

    gBr.partySize = apRd16s(nds, gBr.ctl + 10) & 0xFFFF;
    gBr.pktSize   = apRd16s(nds, gBr.ctl + 12) & 0xFFFF;

    apWr8(nds, gBr.ctl + 6, ++gBr.beat);   // fork heartbeat

    u8 wanted = apRd8(nds, gBr.ctl + 4);
    bool inGame = (wanted && gBr.blkSize != 0 && gBr.blkSize <= 512
        && gBr.partySize != 0 && gBr.partySize <= 2048
        && gBr.pktSize != 0 && gBr.pktSize <= 2048);

    // NO early-return when !inGame: the old return left inbound frames
    // QUEUED in the peer rx vectors (1MB cap -> DropPeer after ~80s of
    // pre-activation idling) and never applied the peer's one-shot
    // on-change party send if they activated before us.  Drain + apply
    // ALWAYS (the mailboxes are inert BSS until the ROM activates); only
    // the outbound bundles and the status/peerMask writes gate on inGame.
    if (!inGame)
    {
        apWr8(nds, gBr.ctl + 7, 0);
        apWr8(nds, gBr.ctl + 9, 0);
    }

    int myRole = mpnet::gNet.myRole();
    apWr8(nds, gBr.ctl + 8, (u8)myRole);

    if (inGame && mpnet::gNet.anyUp())
    {
        u8 buf[2100];

        // bundle: [1][role][blkSize u16][block][ow 48] — every frame
        buf[0] = 1; buf[1] = (u8)myRole;
        buf[2] = (u8)(gBr.blkSize & 0xFF); buf[3] = (u8)(gBr.blkSize >> 8);
        memcpy(buf + 4, apPtr(nds, gBr.exportBlk), gBr.blkSize);
        memcpy(buf + 4 + gBr.blkSize, apPtr(nds, gBr.owExp), 48);
        mpnet::gNet.sendAll(buf, 4 + gBr.blkSize + 48); gBr.dbgTx++;

        // party: on content change
        if (gBr.lastParty.size() != gBr.partySize
            || memcmp(gBr.lastParty.data(), apPtr(nds, gBr.partyExp), gBr.partySize) != 0)
        {
            gBr.lastParty.assign(apPtr(nds, gBr.partyExp), apPtr(nds, gBr.partyExp) + gBr.partySize);
            buf[0] = 2; buf[1] = (u8)myRole;
            buf[2] = (u8)(gBr.partySize & 0xFF); buf[3] = (u8)(gBr.partySize >> 8);
            memcpy(buf + 4, gBr.lastParty.data(), gBr.partySize);
            mpnet::gNet.sendAll(buf, 4 + gBr.partySize);
        }

        // pkt channel: on content change
        if (gBr.lastPkt.size() != gBr.pktSize
            || memcmp(gBr.lastPkt.data(), apPtr(nds, gBr.pktExp), gBr.pktSize) != 0)
        {
            gBr.lastPkt.assign(apPtr(nds, gBr.pktExp), apPtr(nds, gBr.pktExp) + gBr.pktSize);
            buf[0] = 3; buf[1] = (u8)myRole;
            buf[2] = (u8)(gBr.pktSize & 0xFF); buf[3] = (u8)(gBr.pktSize >> 8);
            memcpy(buf + 4, gBr.lastPkt.data(), gBr.pktSize);
            mpnet::gNet.sendAll(buf, 4 + gBr.pktSize); gBr.dbgPktTx++;
        }
    }

    // receive: apply peers' mailboxes (per peer link; host relays)
    {
        u8 rx[2100];
        u32 n;
        static melonDS::u8 sGameEver[5] = {0,0,0,0,0};   // ever game-active latch (strictness)
        for (int pi = 0; pi < 3; pi++)
        while ((n = mpnet::gNet.recvFrame(pi, rx, sizeof(rx))) > 0)
        {
            gBr.dbgRx++;
            if (n == 2 && rx[0] == 0xFF) { mpnet::gNet.assignedRole = rx[1]; continue; }  // host-assigned role
            if (n < 4) continue;
            int tag = rx[0], r = rx[1];
            u32 sz = (u32)(rx[2] | (rx[3] << 8));
            if (r < 1 || r > 4 || r == myRole) continue;
            if (n < 4 + sz) continue;

            // Host relay: a client bundle reaches only the host — forward it to
            // the other clients so everyone sees everyone (origin drops its own
            // echo via r == myRole).
            if (myRole == 1)
                for (int pj = 0; pj < 3; pj++)
                    if (pj != pi) mpnet::gNet.enqueue(pj, rx, n);

            {
                // Peer NEWLY game-active (activated Wireless Play or
                // reconnected): resend on-change channels — anything sent
                // before this moment predated their readiness.
                bool wasFresh = gBr.roleSeenAt[r] != 0 && gBr.frame - gBr.roleSeenAt[r] <= 180;
                if (!wasFresh)
                {
                    gBr.lastParty.clear();
                    gBr.lastPkt.clear();
                }
            }
            gBr.roleSeenAt[r] = gBr.frame;
            sGameEver[r] = 1;   // latch: this role has been game-active this session

            // LEGACY-CHANNEL PAIR ROUTING (3+ players): the single pairwise
            // import block / party buffer only accepts the CHOSEN pair
            // partner's data (ROM publishes intent at OW export +0x18;
            // 0 = unpaired keeps first-come behaviour).  Without this every
            // peer's bundle overwrote the legacy block and battle/trade/give
            // requests "broadcast" to every player.  Per-role arrays always
            // update.
            if (tag == 1 && sz == gBr.blkSize && n >= 4 + sz + 48)
            {
                // STRICT 3+ ROUTING (4P same-trainer fix, ported from the
                // DeSmuME bridge): with two or more fresh peers the legacy
                // pairwise import only accepts the CHOSEN partner — the old
                // "pairRole 0 accepts everyone, last writer wins" open door
                // let a mid-battle pair's per-turn broadcasts (same
                // trainerHash) hijack a still-unpaired third player's entry
                // stall.  Unpaired in 3+ receives nothing and keeps waiting
                // for the real partner; plain 2P keeps the open door.
                melonDS::u8 pairRole = apRd8(nds, gBr.owExp + 0x18);
                // Ever-game-active latch, NOT recent traffic: mid-battle
                // peers freeze their exports and an aging count re-opened
                // the legacy door exactly while a pair fought (4P bug).
                int gamePeers = 0;
                for (int gr = 1; gr <= 4; gr++)
                    if (gr != myRole && sGameEver[gr]) gamePeers++;
                bool strict = (gamePeers >= 2);
                rx[4 + 0x12] = (melonDS::u8)r;   // stamp playerRole (old-hub duty;
                                                 // MpPartnerIsLead dead without it)
                if ((pairRole == 0 && !strict) || r == (int)pairRole)
                    memcpy(apPtr(nds, gBr.importBlk), rx + 4, sz);
                if (gBr.blkN) memcpy(apPtr(nds, gBr.blkN + (r-1)*sz), rx + 4, sz);
                memcpy(apPtr(nds, gBr.owImp + (r-1)*48), rx + 4 + sz, 48);
            }
            else if (tag == 2 && sz == gBr.partySize)
            {
                melonDS::u8 pairRole = apRd8(nds, gBr.owExp + 0x18);
                // Ever-game-active latch, NOT recent traffic: mid-battle
                // peers freeze their exports and an aging count re-opened
                // the legacy door exactly while a pair fought (4P bug).
                int gamePeers = 0;
                for (int gr = 1; gr <= 4; gr++)
                    if (gr != myRole && sGameEver[gr]) gamePeers++;
                bool strict = (gamePeers >= 2);
                if ((pairRole == 0 && !strict) || r == (int)pairRole)
                    memcpy(apPtr(nds, gBr.partyImp), rx + 4, sz);
                if (gBr.partyN) memcpy(apPtr(nds, gBr.partyN + (r-1)*sz), rx + 4, sz);
            }
            else if (tag == 3 && sz == gBr.pktSize)
            {
                memcpy(apPtr(nds, gBr.pktImp + (r-1)*sz), rx + 4, sz);
                gBr.dbgPktRx++;
            }
        }
    }

    // PAIR REBIND / GHOST PURGE (ported from the DeSmuME bridge).  On a
    // pairRole change to a REAL role, replay that role's latest cached
    // block/party from the always-updated per-role arrays.  A change to 0
    // is left ALONE: the ROM's pair re-validate briefly flaps pairRole to 0
    // when the partner's conversion window closes and re-courts mutually the
    // next scan tick -- zeroing here killed the P3+P4 conversion
    // mid-handshake.  The stale ex-partner hazard is handled where it bites:
    // when OUR OWN battle ends (export inBattle 1->0) the import's magic
    // dies, so a frozen mid-battle partner block can't convert a later one.
    if (gBr.owExp && gBr.importBlk)
    {
        static melonDS::u8 sLastPairRole = 0xFF;
        melonDS::u8 pr = apRd8(nds, gBr.owExp + 0x18);
        if (pr != sLastPairRole)
        {
            sLastPairRole = pr;
            if (pr >= 1 && pr <= 4)
            {
                if (gBr.blkN)
                    memcpy(apPtr(nds, gBr.importBlk), apPtr(nds, gBr.blkN + (pr-1)*gBr.blkSize), gBr.blkSize);
                if (gBr.partyN && gBr.partyImp)
                    memcpy(apPtr(nds, gBr.partyImp), apPtr(nds, gBr.partyN + (pr-1)*gBr.partySize), gBr.partySize);
            }
        }
    }
    if (gBr.exportBlk && gBr.importBlk)
    {
        static melonDS::u8 sLastOwnInBattle = 0;
        melonDS::u8 ib = apRd8(nds, gBr.exportBlk + 0x10);
        if (!ib && sLastOwnInBattle)
            apWr32(nds, gBr.importBlk, 0);   // battle over: purge partner-block ghost
        sLastOwnInBattle = ib;
    }

    if ((gBr.frame % 120) == 0)
    {
        {
            melonDS::u32 shim = apRd32(nds, gBr.disc + 16*4);
            melonDS::u8 shimA = shim ? apRd8(nds, shim) : 0xFF;
            melonDS::u32 commTask = shim ? apRd32(nds, shim + 12) : 0;
            printf("[BR] f=%u role=%d wanted=%u tx=%u rx=%u pktTx=%u pktRx=%u shim=%u commTask=%08X%c",
                gBr.frame, myRole, wanted, gBr.dbgTx, gBr.dbgRx,
                gBr.dbgPktTx, gBr.dbgPktRx, shimA, commTask, 10);
        }
        fflush(stdout);
    }

    // ALWAYS-ON crash catcher: if the game's overworld frame counter stops
    // while a bridge session is active, the ARM9 is wedged/crashed — record
    // its registers so the faulting site can be mapped, regardless of how
    // this instance was launched.
    {
        static melonDS::u32 ccLastFC = 0, ccChangedAt = 0;
        static int ccDumped = 0;
        melonDS::u32 fcNow = apRd32(nds, gBr.owExp + 0x0C);
        if (fcNow != ccLastFC)
        {
            ccLastFC = fcNow;
            ccChangedAt = gBr.frame;
            ccDumped = 0;
        }
        else if (wanted && gBr.frame - ccChangedAt > 180 && ccDumped < 24)
        {
            FILE* cf = fopen("melonds_hangpc_auto.txt", "a");
            if (cf)
            {
                fprintf(cf, "role=%d f=%u FCstuck=%u R15=%08X R14=%08X R13=%08X R12=%08X CPSR=%08X%c",
                    myRole, gBr.frame, fcNow,
                    nds->ARM9.R[15], nds->ARM9.R[14], nds->ARM9.R[13], nds->ARM9.R[12],
                    nds->ARM9.CPSR, 10);
                fprintf(cf, "  regs R0-11:");
                for (int ri = 0; ri < 12; ri++) fprintf(cf, " %08X", nds->ARM9.R[ri]);
                fprintf(cf, "%c  banked SVC=%08X,%08X ABT=%08X,%08X IRQ=%08X,%08X%c",
                    10, nds->ARM9.R_SVC[0], nds->ARM9.R_SVC[1],
                    nds->ARM9.R_ABT[0], nds->ARM9.R_ABT[1],
                    nds->ARM9.R_IRQ[0], nds->ARM9.R_IRQ[1], 10);
                // stack dump from the most plausible pre-crash SP
                melonDS::u32 sp = nds->ARM9.R_ABT[0] ? nds->ARM9.R_ABT[0] : nds->ARM9.R_SVC[0];
                if (!sp || (sp >> 24) != 0x02) sp = nds->ARM9.R_SVC[0];
                if (sp && (sp >> 24) == 0x02)
                {
                    fprintf(cf, "  stack@%08X:", sp);
                    for (int si = 0; si < 32; si++)
                        fprintf(cf, " %08X", apRd32(nds, sp + si*4));
                    fprintf(cf, "%c", 10);
                }
                fclose(cf);
                ccDumped++;
            }
        }
    }

    // status + peer mask (freshness: a bundle from that role within ~3 s).
    // Only while OUR session is active — pre-activation the ROM must keep
    // seeing "searching", and the !inGame branch above wrote 0s already.
    if (inGame)
    {
        u8 mask = gBr.FreshPeerMask(myRole);
        apWr8(nds, gBr.ctl + 9, mask);
        apWr8(nds, gBr.ctl + 7, mask ? 2 : 1);
        mpnet::gNet.beaconTick(gBr.frame, mask ? 2 : 1);
    }
}

}
// ---------------------------------------------------------------------------

void EmuThread::run()
{
    {
        const char* ap = getenv("MELONDS_AP");
        if (ap) MpWatchdogStart(ap);
    }
    Config::Table& globalCfg = emuInstance->getGlobalConfig();
    u32 mainScreenPos[3];

    //emuInstance->updateConsole();
    // No carts are inserted when melonDS first boots

    mainScreenPos[0] = 0;
    mainScreenPos[1] = 0;
    mainScreenPos[2] = 0;
    autoScreenSizing = 0;

    //videoSettingsDirty = false;

    if (emuInstance->usesOpenGL())
    {
        emuInstance->initOpenGL(0);

        useOpenGL = true;
        videoRenderer = globalCfg.GetInt("3D.Renderer");
    }
    else
    {
        useOpenGL = false;
        videoRenderer = 0;
    }

    //updateRenderer();
    videoSettingsDirty = true;

    u32 nframes = 0;
    double perfCountsSec = 1.0 / SDL_GetPerformanceFrequency();
    double lastTime = SDL_GetPerformanceCounter() * perfCountsSec;
    double frameLimitError = 0.0;
    double lastMeasureTime = lastTime;

    u32 winUpdateCount = 0, winUpdateFreq = 1;
    u8 dsiVolumeLevel = 0x1F;

    char melontitle[100];

    bool fastforward = false;
    bool slowmo = false;
    emuInstance->fastForwardToggled = false;
    emuInstance->slowmoToggled = false;

    while (emuStatus != emuStatus_Exit)
    {
        if (emuInstance->instanceID == 0)
            MPInterface::Get().Process();

        emuInstance->inputProcess();

        if (emuInstance->hotkeyPressed(HK_FrameLimitToggle)) emit windowLimitFPSChange();

        if (emuInstance->hotkeyPressed(HK_Pause)) emuTogglePause();
        if (emuInstance->hotkeyPressed(HK_Reset)) emuReset();
        if (emuInstance->hotkeyPressed(HK_FrameStep)) emuFrameStep();

        if (emuInstance->hotkeyPressed(HK_FullscreenToggle)) emit windowFullscreenToggle();

        if (emuInstance->hotkeyPressed(HK_SwapScreens)) emit swapScreensToggle();
        if (emuInstance->hotkeyPressed(HK_SwapScreenEmphasis)) emit screenEmphasisToggle();

        if (emuStatus == emuStatus_Running || emuStatus == emuStatus_FrameStep)
        {
            if (emuStatus == emuStatus_FrameStep) emuStatus = emuStatus_Paused;

            if (emuInstance->hotkeyPressed(HK_SolarSensorDecrease))
            {
                int level = emuInstance->nds->GBACartSlot.SetInput(GBACart::Input_SolarSensorDown, true);
                if (level != -1)
                {
                    emuInstance->osdAddMessage(0, "Solar sensor level: %d", level);
                }
            }
            if (emuInstance->hotkeyPressed(HK_SolarSensorIncrease))
            {
                int level = emuInstance->nds->GBACartSlot.SetInput(GBACart::Input_SolarSensorUp, true);
                if (level != -1)
                {
                    emuInstance->osdAddMessage(0, "Solar sensor level: %d", level);
                }
            }

            if (emuInstance->nds->ConsoleType == 1)
            {
                DSi* dsi = static_cast<DSi*>(emuInstance->nds);
                double currentTime = SDL_GetPerformanceCounter() * perfCountsSec;

                // Handle power button
                if (emuInstance->hotkeyDown(HK_PowerButton))
                {
                    dsi->I2C.GetBPTWL()->SetPowerButtonHeld(currentTime);
                }
                else if (emuInstance->hotkeyReleased(HK_PowerButton))
                {
                    dsi->I2C.GetBPTWL()->SetPowerButtonReleased(currentTime);
                }

                // Handle volume buttons
                if (emuInstance->hotkeyDown(HK_VolumeUp))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchHeld(DSi_BPTWL::volumeKey_Up);
                }
                else if (emuInstance->hotkeyReleased(HK_VolumeUp))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchReleased(DSi_BPTWL::volumeKey_Up);
                }

                if (emuInstance->hotkeyDown(HK_VolumeDown))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchHeld(DSi_BPTWL::volumeKey_Down);
                }
                else if (emuInstance->hotkeyReleased(HK_VolumeDown))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchReleased(DSi_BPTWL::volumeKey_Down);
                }

                dsi->I2C.GetBPTWL()->ProcessVolumeSwitchInput(currentTime);
            }

            if (useOpenGL)
                emuInstance->makeCurrentGL();

            // update render settings if needed
            if (videoSettingsDirty)
            {
                emuInstance->renderLock.lock();
                if (useOpenGL)
                {
                    emuInstance->setVSyncGL(true);
                    videoRenderer = globalCfg.GetInt("3D.Renderer");
                }
#ifdef OGLRENDERER_ENABLED
                else
#endif
                {
                    videoRenderer = 0;
                }

                updateRenderer();

                videoSettingsDirty = false;
                emuInstance->renderLock.unlock();
            }

            // process input and hotkeys
            BridgePump(emuInstance->nds);
            emuInstance->nds->SetKeyMask(apApply(emuInstance->nds, emuInstance->inputMask));

            if (emuInstance->isTouching)
                emuInstance->nds->TouchScreen(emuInstance->touchX, emuInstance->touchY);
            else
                emuInstance->nds->ReleaseScreen();

            if (emuInstance->hotkeyPressed(HK_Lid))
            {
                bool lid = !emuInstance->nds->IsLidClosed();
                emuInstance->nds->SetLidClosed(lid);
                emuInstance->osdAddMessage(0, lid ? "Lid closed" : "Lid opened");
            }

            // auto screen layout
            {
                mainScreenPos[2] = mainScreenPos[1];
                mainScreenPos[1] = mainScreenPos[0];
                mainScreenPos[0] = emuInstance->nds->PowerControl9 >> 15;

                int guess;
                if (mainScreenPos[0] == mainScreenPos[2] &&
                    mainScreenPos[0] != mainScreenPos[1])
                {
                    // constant flickering, likely displaying 3D on both screens
                    // TODO: when both screens are used for 2D only...???
                    guess = screenSizing_Even;
                }
                else
                {
                    if (mainScreenPos[0] == 1)
                        guess = screenSizing_EmphTop;
                    else
                        guess = screenSizing_EmphBot;
                }

                if (guess != autoScreenSizing)
                {
                    autoScreenSizing = guess;
                    emit autoScreenSizingChange(autoScreenSizing);
                }
            }

            // RTC sync
            emuInstance->syncRTC();


            // emulate
            u32 nlines;
            if (emuInstance->nds->GPU.GetRenderer().NeedsShaderCompile())
            {
                compileShaders();
                nlines = 1;
            }
            else
            {
                nlines = emuInstance->nds->RunFrame();
            }

            if (emuInstance->ndsSave)
                emuInstance->ndsSave->CheckFlush();

            if (emuInstance->gbaSave)
                emuInstance->gbaSave->CheckFlush();

            if (emuInstance->firmwareSave)
                emuInstance->firmwareSave->CheckFlush();

            emuInstance->drawScreen();

#ifdef MELONCAP
            MelonCap::Update();
#endif // MELONCAP

            winUpdateCount++;
            if (winUpdateCount >= winUpdateFreq && !useOpenGL)
            {
                emit windowUpdate();
                winUpdateCount = 0;
            }
            
            if (emuInstance->hotkeyPressed(HK_FastForwardToggle)) emuInstance->fastForwardToggled = !emuInstance->fastForwardToggled;
            if (emuInstance->hotkeyPressed(HK_SlowMoToggle)) emuInstance->slowmoToggled = !emuInstance->slowmoToggled;

            if (emuInstance->hotkeyPressed(HK_AudioMuteToggle)) emuInstance->toggleAudioMute();

            bool enablefastforward = emuInstance->hotkeyDown(HK_FastForward) | emuInstance->fastForwardToggled;
            bool enableslowmo = emuInstance->hotkeyDown(HK_SlowMo) | emuInstance->slowmoToggled;

            if (useOpenGL)
            {
                // when using OpenGL: when toggling fast-forward or slowmo, change the vsync interval
                if ((enablefastforward || enableslowmo) && !(fastforward || slowmo))
                {
                    emuInstance->setVSyncGL(false);
                }
                else if (!(enablefastforward || enableslowmo) && (fastforward || slowmo))
                {
                    emuInstance->setVSyncGL(true);
                }
            }

            fastforward = enablefastforward;
            slowmo = enableslowmo;
            emuInstance->updateFastForwardMute(fastforward);

            if (slowmo) emuInstance->curFPS = emuInstance->slowmoFPS;
            else if (fastforward) emuInstance->curFPS = emuInstance->fastForwardFPS;
            else if (!emuInstance->doLimitFPS && !emuInstance->doAudioSync) emuInstance->curFPS = 1000.0;
            else emuInstance->curFPS = emuInstance->targetFPS;

            if (emuInstance->audioDSiVolumeSync && emuInstance->nds->ConsoleType == 1)
            {
                DSi* dsi = static_cast<DSi*>(emuInstance->nds);
                u8 volumeLevel = dsi->I2C.GetBPTWL()->GetVolumeLevel();
                if (volumeLevel != dsiVolumeLevel)
                {
                    dsiVolumeLevel = volumeLevel;
                    emit syncVolumeLevel();
                }

                emuInstance->audioVolume = volumeLevel * (256.0 / 31.0);
            }

            if (emuInstance->doAudioSync && !(fastforward || slowmo))
                emuInstance->audioSync();

            double frametimeStep = nlines / (emuInstance->curFPS * 263.0);

            if (frametimeStep < 0.001) frametimeStep = 0.001;

            if (emuInstance->doLimitFPS)
            {
                double curtime = SDL_GetPerformanceCounter() * perfCountsSec;

                frameLimitError += frametimeStep - (curtime - lastTime);
                if (frameLimitError < -frametimeStep)
                    frameLimitError = -frametimeStep;
                if (frameLimitError > frametimeStep)
                    frameLimitError = frametimeStep;

                if (round(frameLimitError * 1000.0) > 0.0)
                {
                    SDL_Delay(round(frameLimitError * 1000.0));
                    double timeBeforeSleep = curtime;
                    curtime = SDL_GetPerformanceCounter() * perfCountsSec;
                    frameLimitError -= curtime - timeBeforeSleep;
                }

                lastTime = curtime;
            }

            nframes++;
            if (nframes >= 30)
            {
                double time = SDL_GetPerformanceCounter() * perfCountsSec;
                double dt = time - lastMeasureTime;
                lastMeasureTime = time;

                u32 fps = round(nframes / dt);
                nframes = 0;

                float fpstarget = 1.0/frametimeStep;

                winUpdateFreq = fps / (u32)round(fpstarget);
                if (winUpdateFreq < 1)
                    winUpdateFreq = 1;
                    
                double actualfps = (59.8261 * 263.0) / nlines;
                snprintf(melontitle, sizeof(melontitle), "[%d/%.0f] melonDS " MELONDS_VERSION, fps, actualfps);
                changeWindowTitle(melontitle);
            }
        }
        else
        {
            // paused
            nframes = 0;
            lastTime = SDL_GetPerformanceCounter() * perfCountsSec;
            lastMeasureTime = lastTime;

            emit windowUpdate();

            snprintf(melontitle, sizeof(melontitle), "melonDS " MELONDS_VERSION);
            changeWindowTitle(melontitle);

            SDL_Delay(75);

            emuInstance->drawScreen();
        }

        handleMessages();
    }
}

void EmuThread::sendMessage(Message msg)
{
    msgMutex.lock();
    msgQueue.enqueue(msg);
    msgMutex.unlock();
}

void EmuThread::waitMessage(int num)
{
    if (QThread::currentThread() == this) return;
    msgSemaphore.acquire(num);
}

void EmuThread::waitAllMessages()
{
    if (QThread::currentThread() == this) return;
    while (!msgQueue.empty())
        msgSemaphore.acquire();
}

void EmuThread::handleMessages()
{
    bool glborrow = false;

    msgMutex.lock();
    while (!msgQueue.empty())
    {
        Message msg = msgQueue.dequeue();
        switch (msg.type)
        {
        case msg_Exit:
            emuStatus = emuStatus_Exit;
            emuPauseStack = emuPauseStackRunning;

            emuInstance->audioDisable();
            MPInterface::Get().End(emuInstance->instanceID);
            break;

        case msg_EmuRun:
            emuStatus = emuStatus_Running;
            emuPauseStack = emuPauseStackRunning;
            emuActive = true;

            emuInstance->audioEnable();
            emit windowEmuStart();
            break;

        case msg_EmuPause:
            emuPauseStack++;
            if (emuPauseStack > emuPauseStackPauseThreshold) break;

            prevEmuStatus = emuStatus;
            emuStatus = emuStatus_Paused;

            if (prevEmuStatus != emuStatus_Paused)
            {
                emuInstance->audioDisable();
                emit windowEmuPause(true);
                emuInstance->osdAddMessage(0, "Paused");
            }
            break;

        case msg_EmuUnpause:
            if (emuPauseStack < emuPauseStackPauseThreshold) break;

            emuPauseStack--;
            if (emuPauseStack >= emuPauseStackPauseThreshold) break;

            emuStatus = prevEmuStatus;

            if (emuStatus != emuStatus_Paused)
            {
                emuInstance->audioEnable();
                emit windowEmuPause(false);
                emuInstance->osdAddMessage(0, "Resumed");
            }
            break;

        case msg_EmuStop:
            if (msg.param.value<bool>())
                emuInstance->nds->Stop();
            emuStatus = emuStatus_Paused;
            emuActive = false;

            emuInstance->audioDisable();
            emit windowEmuStop();
            break;

        case msg_EmuFrameStep:
            emuStatus = emuStatus_FrameStep;
            break;

        case msg_EmuReset:
            emuInstance->reset();

            emuStatus = emuStatus_Running;
            emuPauseStack = emuPauseStackRunning;
            emuActive = true;

            emuInstance->audioEnable();
            emit windowEmuReset();
            emuInstance->osdAddMessage(0, "Reset");
            break;

        case msg_InitGL:
            emuInstance->initOpenGL(msg.param.value<int>());
            useOpenGL = true;
            break;

        case msg_DeInitGL:
            emuInstance->deinitOpenGL(msg.param.value<int>());
            if (msg.param.value<int>() == 0)
                useOpenGL = false;
            break;

        case msg_BorrowGL:
            emuInstance->releaseGL();
            glborrow = true;
            break;

        case msg_BootROM:
            msgResult = 0;
            if (!emuInstance->loadROM(msg.param.value<QStringList>(), true, msgError))
                break;

            assert(emuInstance->nds != nullptr);
            emuInstance->nds->Start();
            msgResult = 1;
            break;

        case msg_BootFirmware:
            msgResult = 0;
            if (!emuInstance->bootToMenu(msgError))
                break;

            assert(emuInstance->nds != nullptr);
            emuInstance->nds->Start();
            msgResult = 1;
            break;

        case msg_InsertCart:
            msgResult = 0;
            if (!emuInstance->loadROM(msg.param.value<QStringList>(), false, msgError))
                break;

            msgResult = 1;
            break;

        case msg_EjectCart:
            emuInstance->ejectCart();
            break;

        case msg_InsertGBACart:
            msgResult = 0;
            if (!emuInstance->loadGBAROM(msg.param.value<QStringList>(), msgError))
                break;

            msgResult = 1;
            break;

        case msg_InsertGBAAddon:
            msgResult = 0;
            emuInstance->loadGBAAddon(msg.param.value<int>(), msgError);
            msgResult = 1;
            break;

        case msg_EjectGBACart:
            emuInstance->ejectGBACart();
            break;

        case msg_SaveState:
            msgResult = emuInstance->saveState(msg.param.value<QString>().toStdString());
            break;

        case msg_LoadState:
            msgResult = emuInstance->loadState(msg.param.value<QString>().toStdString());
            break;

        case msg_UndoStateLoad:
            emuInstance->undoStateLoad();
            msgResult = 1;
            break;

        case msg_ImportSavefile:
            {
                msgResult = 0;
                auto f = Platform::OpenFile(msg.param.value<QString>().toStdString(), Platform::FileMode::Read);
                if (!f) break;

                u32 len = FileLength(f);

                std::unique_ptr<u8[]> data = std::make_unique<u8[]>(len);
                Platform::FileRewind(f);
                Platform::FileRead(data.get(), len, 1, f);

                assert(emuInstance->nds != nullptr);
                emuInstance->nds->SetNDSSave(data.get(), len);

                CloseFile(f);
                msgResult = 1;
            }
            break;

        case msg_EnableCheats:
            emuInstance->enableCheats(msg.param.value<bool>());
            break;
        }

        msgSemaphore.release();
    }
    msgMutex.unlock();

    if (glborrow)
    {
        glBorrowMutex.lock();
        glBorrowCond.wait(&glBorrowMutex);
        glBorrowMutex.unlock();
    }
}

void EmuThread::changeWindowTitle(char* title)
{
    emit windowTitleChange(QString(title));
}

void EmuThread::initContext(int win)
{
    sendMessage({.type = msg_InitGL, .param = win});
    waitMessage();
}

void EmuThread::deinitContext(int win)
{
    sendMessage({.type = msg_DeInitGL, .param = win});
    waitMessage();
}

void EmuThread::borrowGL()
{
    sendMessage(msg_BorrowGL);
    waitMessage();
}

void EmuThread::returnGL()
{
    glBorrowMutex.lock();
    glBorrowCond.wakeAll();
    glBorrowMutex.unlock();
}

void EmuThread::emuRun()
{
    sendMessage(msg_EmuRun);
    waitMessage();
}

void EmuThread::emuPause(bool broadcast)
{
    sendMessage(msg_EmuPause);
    waitMessage();

    if (broadcast)
        emuInstance->broadcastCommand(InstCmd_Pause);
}

void EmuThread::emuUnpause(bool broadcast)
{
    sendMessage(msg_EmuUnpause);
    waitMessage();

    if (broadcast)
        emuInstance->broadcastCommand(InstCmd_Unpause);
}

void EmuThread::emuTogglePause(bool broadcast)
{
    if (emuStatus == emuStatus_Paused)
        emuUnpause(broadcast);
    else
        emuPause(broadcast);
}

void EmuThread::emuStop(bool external)
{
    sendMessage({.type = msg_EmuStop, .param = external});
    waitMessage();
}

void EmuThread::emuExit()
{
    sendMessage(msg_Exit);
    waitAllMessages();
}

void EmuThread::emuFrameStep()
{
    if (emuPauseStack < emuPauseStackPauseThreshold)
        sendMessage(msg_EmuPause);
    sendMessage(msg_EmuFrameStep);
    waitAllMessages();
}

void EmuThread::emuReset()
{
    sendMessage(msg_EmuReset);
    waitMessage();
}

bool EmuThread::emuIsRunning()
{
    return emuStatus == emuStatus_Running;
}

bool EmuThread::emuIsActive()
{
    return emuActive;
}

int EmuThread::bootROM(const QStringList& filename, QString& errorstr)
{
    sendMessage({.type = msg_BootROM, .param = filename});
    waitMessage();
    if (!msgResult)
    {
        errorstr = msgError;
        return msgResult;
    }

    sendMessage(msg_EmuRun);
    waitMessage();
    errorstr = "";
    return msgResult;
}

int EmuThread::bootFirmware(QString& errorstr)
{
    sendMessage(msg_BootFirmware);
    waitMessage();
    if (!msgResult)
    {
        errorstr = msgError;
        return msgResult;
    }

    sendMessage(msg_EmuRun);
    waitMessage();
    errorstr = "";
    return msgResult;
}

int EmuThread::insertCart(const QStringList& filename, bool gba, QString& errorstr)
{
    MessageType msgtype = gba ? msg_InsertGBACart : msg_InsertCart;

    sendMessage({.type = msgtype, .param = filename});
    waitMessage();
    errorstr = msgResult ? "" : msgError;
    return msgResult;
}

void EmuThread::ejectCart(bool gba)
{
    sendMessage(gba ? msg_EjectGBACart : msg_EjectCart);
    waitMessage();
}

int EmuThread::insertGBAAddon(int type, QString& errorstr)
{
    sendMessage({.type = msg_InsertGBAAddon, .param = type});
    waitMessage();
    errorstr = msgResult ? "" : msgError;
    return msgResult;
}

int EmuThread::saveState(const QString& filename)
{
    sendMessage({.type = msg_SaveState, .param = filename});
    waitMessage();
    return msgResult;
}

int EmuThread::loadState(const QString& filename)
{
    sendMessage({.type = msg_LoadState, .param = filename});
    waitMessage();
    return msgResult;
}

int EmuThread::undoStateLoad()
{
    sendMessage(msg_UndoStateLoad);
    waitMessage();
    return msgResult;
}

int EmuThread::importSavefile(const QString& filename)
{
    sendMessage(msg_EmuReset);
    sendMessage({.type = msg_ImportSavefile, .param = filename});
    waitMessage(2);
    return msgResult;
}

void EmuThread::enableCheats(bool enable)
{
    sendMessage({.type = msg_EnableCheats, .param = enable});
    waitMessage();
}

void EmuThread::updateRenderer()
{
    auto nds = emuInstance->nds;

    if (videoRenderer != lastVideoRenderer)
    {
        switch (videoRenderer)
        {
            case renderer3D_Software:
                nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
                break;
            case renderer3D_OpenGL:
                nds->SetRenderer(std::make_unique<GLRenderer>(*nds, false));
                break;
            case renderer3D_OpenGLCompute:
                nds->SetRenderer(std::make_unique<GLRenderer>(*nds, true));
                break;
            default: __builtin_unreachable();
        }
    }
    lastVideoRenderer = videoRenderer;

    auto& cfg = emuInstance->getGlobalConfig();
    melonDS::RendererSettings settings = {
        .ScaleFactor = cfg.GetInt("3D.GL.ScaleFactor"),
        .Threaded = cfg.GetBool("3D.Soft.Threaded"),
        .HiresCoordinates = cfg.GetBool("3D.GL.HiresCoordinates"),
        .BetterPolygons = cfg.GetBool("3D.GL.BetterPolygons")
    };

    nds->GetRenderer().SetRenderSettings(settings);
}

void EmuThread::compileShaders()
{
    auto& renderer = emuInstance->nds->GPU.GetRenderer();
    int currentShader, shadersCount;
    u64 startTime = SDL_GetPerformanceCounter();
    // kind of hacky to look at the wallclock, though it is easier than
    // than disabling vsync
    do
    {
        renderer.ShaderCompileStep(currentShader, shadersCount);
    }
    while (renderer.NeedsShaderCompile() &&
             (SDL_GetPerformanceCounter() - startTime) * perfCountsSec < 1.0 / 6.0);
    emuInstance->osdAddMessage(0, "Compiling shader %d/%d", currentShader+1, shadersCount);
}
