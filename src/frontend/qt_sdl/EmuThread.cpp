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

static volatile unsigned int g_apLoopSeq = 0;   // emu-loop heartbeat

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

    // ---- LAN bring-up ----
    if (apFrame == ((apMode == 0) ? 240u : 480u))
    {
        MPInterface::Set(MPInterface_LAN);
        MPInterface::Get().SetRecvTimeout(300);
        MPInterface::Get().SetAsyncMode(true);
        LAN& lan = (LAN&)MPInterface::Get();
        bool ok = (apMode == 0) ? lan.StartHost("AP0", 2)
                                : lan.StartClient("AP1", apJoinIP);
        printf("[AP] LAN %s -> %d\n", (apMode == 0) ? "host" : "join", ok ? 1 : 0);
    }

    // ---- input script ----
    u32 press = 0;
    if (apFrame >= 700 && apFrame < 2000)
    {
        if ((apFrame % 40) < 8) press |= (1 << 0);            // tap A (title/file select)
    }
    else if (apFrame >= 2060 && apFrame < 2072)
    {
        press |= (1 << 2);                                     // SELECT: wireless menu
    }
    else if (apFrame >= 2120 && apFrame < 2128)
    {
        press |= (1 << 0);                                     // A: Activate Multiplayer
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

        if (!apConnFrame && peerMask) 
        {
            apConnFrame = apFrame;
            printf("[AP] connected at frame %u\n", apFrame);
        }

        // movement phases relative to connect.
        // MELONDS_AP_PATTERN selects the stress pattern:
        //   (unset)/walk : hold a direction, alternating every 90f
        //   run          : same, with B held (sprint — double tile rate)
        //   circle       : B held, cycle RIGHT->DOWN->LEFT->UP every 32f
        //                  (running in a loop: direction-change stress)
        if (apConnFrame)
        {
            static int patMode = -1;
            if (patMode < 0)
            {
                const char* pat = getenv("MELONDS_AP_PATTERN");
                patMode = (!pat) ? 0 : (!strcmp(pat, "run")) ? 1
                        : (!strcmp(pat, "circle")) ? 2
                        : (!strcmp(pat, "waggle")) ? 3 : 0;
            }
            u32 cf = apFrame - apConnFrame;
            u32 winLo = (apMode == 0) ? 300u : 1700u;
            u32 winHi = (apMode == 0) ? 1500u : 2900u;

            if (cf >= winLo && cf < winHi)
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
            if (fcNow != lastFC) { lastFC = fcNow; lastChangeAt = apFrame; }
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
