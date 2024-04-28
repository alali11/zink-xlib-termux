# [Mesa] (https://mesa3d.org) - The 3D Graphics Library
----

### Usage Zink in Termux
----

To use Zink with proprietary Vulkan driver:

   pkg install vulkan-loader-android

To use Zink with Turnip Freedreno driver:

   pkg install mesa-vulkan-icd-freedreno

You also may compile it yourself.

### Zink 3D Tests
----
Tests were run in the [Wine Hangover 9.3](https://github.com/alexvorxx/hangover-termux) in Termux.
Device - Poco F1 (Adreno 630). A proprietary Vulkan driver was used with Zink.
* glxgears and WineD3D tests
![](https://github.com/alexvorxx/zink-xlib-termux/blob/main/docs/glxgears&WineD3D.jpg?raw=true)
* GPU Caps Viewer
![](https://github.com/alexvorxx/zink-xlib-termux/blob/main/docs/GPUCapsViewer1.jpg?raw=true)
![](https://github.com/alexvorxx/zink-xlib-termux/blob/main/docs/GPUCapsViewer2.jpg?raw=true)

### Build deps
----

Taken from [here] (https://github.com/termux/termux-packages/issues/10103#issuecomment-1333002785).

   pkg update && pkg upgrade

   pkg install -y x11-repo

   pkg install -y clang lld binutils cmake autoconf automake libtool '*ndk*' make python git libandroid-shmem-static ninja llvm bison flex libx11 xorgproto libdrm libpixman libxfixes libjpeg-turbo xtrans libxxf86vm xorg-xrandr xorg-font-util xorg-util-macros libxfont2 libxkbfile libpciaccess xcb-util-renderutil xcb-util-image xcb-util-keysyms xcb-util-wm xorg-xkbcomp xkeyboard-config libxdamage libxinerama libxshmfence

   pip install meson mako

### Build Mesa Zink
----

Build Mesa Zink using [this repository] (https://github.com/alexvorxx/zink-xlib-termux).

Go to the folder with Mesa code and run the commands:

   LDFLAGS='-l:libandroid-shmem.a -llog' meson . build -Dgallium-va=disabled -Dgallium-drivers=virgl,zink,swrast -Ddri3=disabled -Dvulkan-drivers= -Dglx=xlib -Dplatforms=x11 -Dllvm=disabled -Dbuildtype=release

   ninja -C build install


### Source
----

This repository lives at https://gitlab.freedesktop.org/mesa/mesa.

Other repositories are likely forks, and code found there is not supported.
