# Toolchains por plataforma

Todo se instala en `~/toolchains` (cambia la ruta con `TOOLCHAINS=...`). Solo
los pasos de devkitPro (Switch, 3DS) necesitan `sudo`.

| Plataforma | Script | Root | Estado |
|---|---|---|---|
| Linux x86-64 | (el del sistema) | no | se compila y prueba aquí |
| Windows x86-64 | `setup_windows.sh` (llvm-mingw UCRT) + `windows-llvm-mingw.cmake` | no | compila (`.exe` estático, sin DLLs) |
| Android arm64 | `setup_android.sh` (JDK 21, SDK 35, NDK 27, CMake) | no | compila el `.apk` (`android/`, Gradle) |
| Nintendo Switch | `setup_switch.sh devkitpro` → `rust` → `mesa` | **sí** (devkitPro) | compila `babel-recomp.nro`; probado en consola |
| Nintendo 3DS | `setup_3ds.sh` (devkitPro `3ds-dev` + makerom + bannertool) | **sí** (devkitPro) | compila `.3dsx` y `.cia`; probado en Azahar |
| PlayStation Vita | `setup_vita.sh` (VitaSDK) | no | compila el `.vpk`; probado en Vita3K |
| macOS | — | — | lo compila el CI de GitHub (macos-14) |

## Windows

```sh
scripts/toolchains/setup_windows.sh
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=scripts/toolchains/windows-llvm-mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
```

SDL3 se compila desde el código fuente fijado (`SNESRECOMP_SDL3_FETCH`), enlazado
estáticamente: el `.exe` no necesita DLLs.

## Android

```sh
scripts/toolchains/setup_android.sh
source ~/toolchains/android-env.sh
```

Acepta las licencias del SDK de Android (están en `~/toolchains/android-sdk/licenses`).
El juego usa el presentador OpenGL ES 3.0 (por defecto en Android), que es el
que dibuja el escalado HD suavizado (xBR).

## Nintendo Switch

1. `scripts/toolchains/setup_switch.sh devkitpro`: imprime los comandos con
   `sudo` para añadir el repositorio de devkitPro e instalar devkitA64, libnx,
   SDL2, Mesa y el resto. Ejecútalos tú y abre una terminal nueva.
2. `scripts/toolchains/setup_switch.sh rust`: Rust y el target aarch64 que
   necesita mesa-switch.
3. `scripts/toolchains/setup_switch.sh mesa`: clona y compila
   [mesa-switch](https://github.com/NaGaa95/mesa-switch) (EGL + OpenGL ES 3 sobre
   la Tegra X1, driver nvc0 o Zink/NVK). devkitPro ya trae `switch-mesa`, más
   antiguo; probaremos los dos.

Objetivos en Switch: 1280x720 a 60 fps en portátil y 1920x1080 a 60 fps en
dock. El escalado HD (nítido o xBR) no tiene coste apreciable; un pack HD
con líneas `fx` sí (efectos por CPU a 4x).

### Compilar y preparar la SD

```sh
export DEVKITPRO=/opt/devkitpro
cmake -S . -B build-switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
      -DCMAKE_BUILD_TYPE=Release -DSNESRECOMP_SDL_BACKEND=SDL2 \
      -DSNESRECOMP_SWITCH_MESA_DIR=$HOME/toolchains/mesa-switch-dft/mesa-install/opt/devkitpro/portlibs/switch
cmake --build build-switch
scripts/toolchains/stage_switch.sh        # -> dist/switch/babel-recomp/
```

RetroAchievements en Switch necesita la red de devkitPro:
`sudo dkp-pacman -S switch-curl` (y volver a configurar `build-switch`). Sin
ella el juego compila igual y la tarjeta dice "not available".

Icono del menú homebrew: `res/icon_256.jpg` (original del proyecto); otro JPEG
de 256x256 con `-DSNESRECOMP_SWITCH_ICON=/ruta/icono.jpg`.

Sin `SNESRECOMP_SWITCH_MESA_DIR` se usa el `switch-mesa` de devkitPro (Mesa 20.1).
Mesa para Switch: [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch)
(Mesa 26.2, nvc0/Zink, también Vulkan/NVK) en `~/toolchains/mesa-switch-dft`.

Estructura en la SD (copia `dist/switch` a la raíz):

```
/switch/babel-recomp/babel-recomp.nro
/switch/babel-recomp/rom.sfc                 tu ROM USA
/switch/babel-recomp/languages/rom_xx.sfc    tus ROMs europeas (es, fr, de)
/switch/babel-recomp/msu1/                   pack MSU-1 (.pcm)
/switch/babel-recomp/mods/modhd/             mods sueltos (carpeta con manifest.toml)
/switch/babel-recomp/shaders/                shaders GLSL de RetroArch (.glslp)
```

La misma estructura vale en PC: junto al ejecutable `babel-recomp`.

## Nintendo 3DS

```sh
scripts/toolchains/setup_3ds.sh            # makerom + bannertool en ~/toolchains/ctr-tools
export DEVKITPRO=/opt/devkitpro
cmake -S . -B build-3ds -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-3ds                    # -> babel-recomp.3dsx y babel-recomp.cia
scripts/package_release.sh build-3ds 3ds   # zip con 3ds/babel-recomp/ y el .cia
```

En consola todo vive en `sdmc:/3ds/babel-recomp/`. Un `env.txt` ahí (líneas
`CLAVE=valor`) hace de entorno, p. ej. `SNESRECOMP_FPS=1`.

## PlayStation Vita

```sh
scripts/toolchains/setup_vita.sh
export VITASDK=~/toolchains/vitasdk
cmake -S . -B build-vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita                   # -> babel-recomp.vpk
```

En consola todo vive en `ux0:data/babel-recomp/` (también `env.txt`).

