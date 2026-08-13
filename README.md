<p align="center"><img src="https://raw.githubusercontent.com/melonDS-emu/melonDS/master/res/icon/melon_128x128.png"></p>
<h2 align="center"><b>melonDS</b></h2>
<p align="center">
<a href="http://melonds.kuribo64.net/" alt="melonDS website"><img src="https://img.shields.io/badge/website-melonds.kuribo64.net-%2331352e.svg"></a>
<a href="http://melonds.kuribo64.net/downloads.php" alt="Release: 1.1"><img src="https://img.shields.io/badge/release-1.1-%235c913b.svg"></a>
<a href="https://www.gnu.org/licenses/gpl-3.0" alt="License: GPLv3"><img src="https://img.shields.io/badge/License-GPL%20v3-%23ff554d.svg"></a>
<a href="https://kiwiirc.com/client/irc.badnik.net/?nick=IRC-Source_?#melonds" alt="IRC channel: #melonds"><img src="https://img.shields.io/badge/IRC%20chat-%23melonds-%23dd2e44.svg"></a>
<a href="https://discord.gg/pAMAtExcqV" alt="Discord"><img src="https://img.shields.io/badge/Discord-Kuribo64-7289da?logo=discord&logoColor=white"></a>
<br>
<a href="https://github.com/fdelavega02/melonDS-Project-PM/actions/workflows/build-ubuntu.yml?query=branch%3Alinux-native"><img src="https://github.com/fdelavega02/melonDS-Project-PM/actions/workflows/build-ubuntu.yml/badge.svg?branch=linux-native" alt="Linux AppImage build" /></a>
</p>
DS emulator, sorta

The goal is to do things right and fast, akin to blargSNES (but hopefully better). But also to, you know, have a fun challenge :)
<hr>

## Project PM fork

This is an in-progress community fork maintained by **M4doesstuff** and
**Hermy**. Its goal is to make the Project PM multiplayer build of melonDS
available as a native Linux x86_64 AppImage, rather than requiring Wine.

### v1.3 Beta: online room-code multiplayer

The current Linux prerelease, **Project PM Linux v1.3 Beta**, brings in the
upstream room-code relay mode. It is the simplest way to play with someone
over the internet: it needs no port forwarding or router configuration, and
works even when either player is behind CGNAT. See [Hosting over the
internet](#hosting-over-the-internet) for the Host Online Game / Join Online
Game steps.

Download the current native Linux build from the repository's
[Releases](https://github.com/fdelavega02/melonDS-Project-PM/releases) page.
On CachyOS, install `fuse2`, mark the AppImage executable, and launch it:

```fish
sudo pacman -S --needed fuse2
chmod +x melonDS*.AppImage
./melonDS*.AppImage
```

If AppImage mounting is unavailable, use
`APPIMAGE_EXTRACT_AND_RUN=1 ./melonDS*.AppImage` instead.

The `linux-native` branch is this fork's default branch and is the source of
the published Linux builds. It contains the Project PM multiplayer bridge from
upstream's `platinum-mp` branch. Tags beginning with
`project-pm-linux-v` create a GitHub Release with the AppImage attached.

The [`platinum-mp`](https://github.com/ComicartOlie/melonDS-Project-PM/tree/platinum-mp)
branch carries the embedded multiplayer bridge for
**Project PM**, a co-op multiplayer romhack of Pokémon Platinum. Hosting or
joining a LAN game also syncs the romhack's multiplayer mailboxes with the
other players. The upstream sibling DeSmuME port of the same bridge lives at
[ComicartOlie/Desmume-Project-PM](https://github.com/ComicartOlie/Desmume-Project-PM).
For native Linux DeSmuME work, see [M4doesstuff's DeSmuME Project PM Linux
port](https://github.com/fdelavega02/Desmume-Project-PM), on its default
`linux-native` branch. It has native Host, Join, and Disconnect controls, and
has passed both local multiplayer and a real remote internet multiplayer test
through TCP 7820 port forwarding.
All credit for the emulator itself goes to the melonDS team.

### Prepare a Project PM ROM

Project PM is a patch for Pokémon Platinum. The Project PM multiplayer bridge
works with the **patched** game, not with an unmodified Platinum ROM. Obtain a
clean ROM by dumping a copy of the game you own, then apply the Project PM
`.xdelta` patch supplied by the Project PM project.

On CachyOS, install `xdelta3` and place the clean ROM and patch in the same
directory. The following example uses simple filenames to avoid shell quoting
problems:

```fish
sudo pacman -S --needed xdelta3
mkdir -p ~/Games/Project-PM
cd ~/Games/Project-PM
# Copy your legally dumped base ROM here as: Pokemon Platinum.nds
# Copy the Project PM patch here as: project-pm.xdelta
xdelta3 -d -s "Pokemon Platinum.nds" project-pm.xdelta "Project PM Multiplayer.nds"
```

`-d` tells xdelta3 to decode/apply the patch. `-s` selects the clean base ROM,
and the final filename is the new patched ROM. Open `Project PM
Multiplayer.nds` in melonDS. If xdelta3 reports that the source file does not
match, do not force it: the base ROM is the wrong regional release or revision
for that patch. Use the exact base ROM specified with the Project PM patch.

### Hosting over the internet

The easy way is **online relay mode**: no port forwarding, no router setup,
and nobody sees anyone else's IP address.

One player picks **Host Online Game...** in melonDS's Multiplayer menu. The
relay server field comes pre-set to the community relay, so just click
through: the emulator shows a 5 character room code. Share the code, and
everyone else picks **Join Online Game...** and enters it. Both sides dial
*out* to the relay, which splices the two connections together, so it works
from behind any normal router and from behind CGNAT. The room code stays on
screen for the whole session.

The relay only passes bytes through: it never sees your IP as anything more
than a connection to itself, and players never exchange addresses with each
other.

The relay server software is a small open source Python script
(`tools/relay/pm_relay.py` in the Project PM repo), so anyone can run their
own. Tick **Custom Relay Server** in the Multiplayer menu and the online
dialogs gain a relay server field; whatever you last used is remembered.

#### LAN and direct IP (unchanged)

On the same LAN or a VPN (Hamachi, Radmin, ZeroTier, Tailscale), one player
hosts with "Host LAN game" and everyone else joins with the host's IP. That
path works exactly as it always has and needs no setup.

To host over the open internet **without** the relay (direct IP), three
things must all be true on the **host's** side. Joiners never need any of
this, and online relay mode needs none of it either:

1. **Router port forwards**: melonDS needs **two** ports forwarded to the
   host PC: **UDP 7064** (melonDS's LAN session) and **TCP 7820** (the mod's
   sync bridge). Forwarding only 7820 is the most common mistake; the
   session can never form without 7064.
2. **Linux firewall**: if a firewall is enabled on the host, allow both
   **UDP 7064** and **TCP 7820**. For example, with UFW:
   `sudo ufw allow 7064/udp` and `sudo ufw allow 7820/tcp`.
3. **A real public IP**: if your router's WAN address (in its admin page)
   is different from what whatismyip.com shows, or starts with
   100.64-100.127, your ISP has you behind CGNAT and no amount of port
   forwarding will work. Use a VPN like Hamachi/ZeroTier, or have a friend
   with a real IP host.

## How to use

Firmware boot (not direct boot) requires a BIOS/firmware dump from an original DS or DS Lite.
DS firmwares dumped from a DSi or 3DS aren't bootable and only contain configuration data, thus they are only suitable when booting games directly.

### Possible firmware sizes

 * 128KB: DSi/3DS DS-mode firmware (reduced size due to lacking bootcode)
 * 256KB: regular DS firmware
 * 512KB: iQue DS firmware

DS BIOS dumps from a DSi or 3DS can be used with no compatibility issues. DSi BIOS dumps (in DSi mode) are not compatible. Or maybe they are. I don't know.

As for the rest, the interface should be pretty straightforward. If you have a question, don't hesitate to ask, though!

## How to build
See [BUILD.md](./BUILD.md) for build instructions.

## TODO LIST

 * better DSi emulation
 * better OpenGL rendering
 * netplay
 * the impossible quest of pixel-perfect 3D graphics
 * support for rendering screens to separate windows
 * emulating some fancy addons
 * other non-core shit (debugger, graphics viewers, etc)

### TODO LIST FOR LATER (low priority)

 * big-endian compatibility (Wii, etc)
 * LCD refresh time (used by some games for blending effects)
 * any feature you can eventually ask for that isn't outright stupid

## Credits

 * Martin for GBAtek, a good piece of documentation
 * Cydrak for the extra 3D GPU research
 * limittox for the icon
 * All of you comrades who have been testing melonDS, reporting issues, suggesting shit, etc

## Licenses

[![GNU GPLv3 Image](https://www.gnu.org/graphics/gplv3-127x51.png)](http://www.gnu.org/licenses/gpl-3.0.en.html)

melonDS is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

### External
* Images used in the Input Config Dialog - see `src/frontend/qt_sdl/InputConfig/resources/LICENSE.md`
