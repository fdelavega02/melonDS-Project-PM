# Building the Linux Project PM fork

This fork supports native **Linux x86_64** builds. Its default branch,
`linux-native`, includes the Project PM multiplayer bridge and is the source
for the published AppImage releases.

## CachyOS / Arch Linux

1. Install the build dependencies:

   ```fish
   sudo pacman -S --needed base-devel cmake ninja extra-cmake-modules git libpcap sdl2 qt6-{base,multimedia,svg} libarchive enet zstd faad2
   ```

2. Clone the fork and enter it:

   ```fish
   git clone --branch linux-native --single-branch https://github.com/fdelavega02/melonDS-Project-PM.git
   cd melonDS-Project-PM
   ```

3. Configure and compile a release build:

   ```fish
   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DMELONDS_EMBED_BUILD_INFO=ON
   cmake --build build -j (nproc)
   ```

4. Launch it:

   ```fish
   ./build/melonDS
   ```

## Other Linux distributions

Install a C++ toolchain, CMake, Ninja, Extra CMake Modules, SDL2, Qt 6
(Base, Multimedia, and SVG), libpcap, libarchive, ENet, zstd, and FAAD2 using
your distribution's package manager. Then use the same clone, configure, and
build commands above. On Debian/Ubuntu, the corresponding development packages
are used by the repository's Ubuntu 22.04 AppImage workflow.

## AppImage releases

If you only want to play, download the Linux x86_64 AppImage from
[Releases](https://github.com/fdelavega02/melonDS-Project-PM/releases). On
CachyOS, install `fuse2`, mark the file executable, and launch it:

```fish
sudo pacman -S --needed fuse2
chmod +x melonDS-Project-PM-linux-x86_64.AppImage
./melonDS-Project-PM-linux-x86_64.AppImage
```

If AppImage mounting is unavailable, use:

```fish
APPIMAGE_EXTRACT_AND_RUN=1 ./melonDS-Project-PM-linux-x86_64.AppImage
```

The `Linux AppImage` GitHub Actions workflow builds and validates the
distributable AppImage on every push to `linux-native`. Tags named
`project-pm-linux-v*` publish the generated AppImage to a GitHub release.
