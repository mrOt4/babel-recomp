# Babel Recomp — Illusion of Gaia

Port nativo para PC y consolas de **Illusion of Gaia** (SNES, USA), construido
sobre un framework de recompilación estática, con idiomas, banda sonora MSU-1,
escalado HD, New Game+, RetroAchievements y mucho más.

[Read in English](README.md)

[![Apóyame en Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/mr0ta)

> **No se incluye ningún dato del juego.** Ni este repositorio ni sus
> versiones contienen la ROM, gráficos, música ni código generado a partir del
> juego. Necesitas tu propia copia legal; todo se lee de ella al jugar.

| Plataforma | Paquete | Estado |
|---|---|---|
| Linux x86-64 | `.zip` | Desarrollado y probado aquí |
| Windows x86-64 | `.zip` | Compila; aún sin probar en Windows |
| Nintendo Switch | `.nro` | Probado en consola, de principio a fin |
| Android arm64 | `.apk` | Compila; aún sin probar en un dispositivo |
| macOS (Apple Silicon) | `.zip` | Lo compila el CI; sin probar |
| Nintendo 3DS / 2DS | `.cia`, `.3dsx` | Funciona en Azahar; sin probar en consola; se recomienda New 3DS |
| PlayStation Vita | `.vpk` | Funciona en Vita3K; sin probar en consola |

## Qué necesitas

- La ROM de **Illusion of Gaia (USA)**, sin modificar, 2 MB (si tiene cabecera
  de copiadora de 512 bytes se ignora). El juego la comprueba antes de arrancar:

  | | |
  |---|---|
  | SHA-256 | `32adc048fd89228a4310c03df243e83de7d436cdb2460b4cc83ade20d359b2bd` |
  | SHA-1 | `38de0c84dd2bc3bdfc0a3d7ab0220c79b70887c3` |
  | CRC32 | `1c3848c0` |

- *Opcional:* tus ROMs europeas (*Illusion of Time*: España, Francia,
  Alemania) para jugar en castellano, francés o alemán.
- *Opcional:* un pack MSU-1 de Illusion of Gaia (archivos `.pcm`).
- *Opcional:* el parche `.ips` de Apocalypse Gaia (ver [MODS.md](MODS.md)).

## Características

**El juego**
- El juego completo, jugable de principio a fin, ejecutándose de forma nativa.
- Partidas compatibles con el original (`.srm`), 10 ranuras de estado con
  miniatura, rebobinado y avance rápido.
- **Idiomas:** textos y fuente en castellano, francés y alemán sacados de tu
  propia ROM europea, intercambiables en partida. Las frases que solo existen
  en la versión USA están traducidas a mano.
- **Banda sonora MSU-1:** música con calidad de CD desde cualquier pack MSU-1,
  sin parchear la ROM; los efectos de sonido siguen siendo los del juego.
- **New Game+:** en la pantalla END el juego ofrece guardar la partida como
  completada. Al continuarla empiezas de nuevo con las estadísticas y
  habilidades de Will y enemigos más duros (daño ×2, ×3 desde la segunda vuelta).
- **Porcentaje de completado:** salas superadas, Joyas Rojas y final.
- **Comodidades:** botón de correr, cambio rápido de objeto (X + L/R),
  apilado de Joyas Rojas/Hierbas/Cristales, marca del objeto equipado,
  mantener pulsado para avanzar el texto.
- **Trucos:** vida infinita, DP, estadísticas, las 50 Joyas Rojas (desactivan
  los logros).
- **Apocalypse Gaia:** el jefe final recortado, restaurado (con tu parche).
- **Randomizer:** juega semillas de Illusion of Gaia Randomizer (IoGR).

**Gráficos**
- **Escalado HD:** píxeles nítidos a cualquier resolución o bordes suavizados
  (xBR), hasta 4K.
- **Modo 7 en HD:** el mapa del mundo y las escenas en Modo 7 a alta resolución.
- **Packs HD:** arte de sustitución para tiles y sprites (1×/2×/4×).
- **Shaders GLSL de RetroArch:** presets multipaso, LUTs, parámetros.
- Pantalla completa, contador de FPS, opciones de relación de aspecto.

**Utilidades**
- **Lanzador:** selector de ROM con verificación, ajustes de vídeo, sonido y
  controles, reasignación de botones y página de Mods.
- **Menú de ajustes en partida** sobre el juego en pausa (Esc, o ZL + R3).
- **Pantalla complementaria** (I, o ZL + L3): inventario, estado, estados
  guardados y ajustes con el arte del propio juego, también táctil. Los modos
  *Dual* y *Flip Grip* la muestran junto al juego en marcha.
- **RetroAchievements** (softcore); los logros sin conexión se envían después.
- **Registros y crash logs** en `logs/` en todas las plataformas, para
  adjuntarlos al informar de un fallo.
- **Controles táctiles** en Android: un mando en pantalla junto a la imagen,
  que se oculta al conectar un mando físico.

### Modos de pantalla

- **Panorámico 16:9.** La imagen se amplía por los lados con el mapa real (lo
  que la SNES nunca mostraba), sin estirarla; más allá del borde del mapa las
  columnas extra quedan en negro. En los ajustes de pantalla del lanzador elige
  **16:9 (Widescreen)**, o pon `Widescreen = 1` en `[Graphics]` de `config.ini`
  (en 3DS y Vita, que no tienen lanzador). Desactivado, la imagen 4:3 es
  idéntica a la original.
- **Pantalla compañera.** I en el teclado, ZL + L3 en un mando: el juego se
  pausa en una segunda pantalla dibujada con el arte del propio juego: tu
  inventario sobre el pedestal de piedra (toca o pulsa A para equipar), la
  página de estado, los estados guardados con miniatura y los ajustes.
  Funciona también con el tacto.
- **Doble pantalla.** *Dual* pone el juego y la pantalla compañera uno al lado
  del otro, ambos en marcha; **Flip Grip** y **Flip Grip R** los apilan y giran
  la imagen un cuarto de vuelta para jugar con la consola en vertical (Switch
  con Flip Grip, móviles, tablets). Menú de ajustes → *Pantalla compañera*.

Todos los mods y opciones están descritos en [MODS.md](MODS.md).

## Instalación y uso

Descarga el paquete de tu plataforma en [Releases](https://github.com/mrOt4/babel-recomp/releases) o
compílalo tú mismo (más abajo). En todos los casos, lo que añades va **junto
al ejecutable**:

```
babel-recomp(.exe / .nro)
assets/  i18n/  mods/       (vienen en el paquete)
rom.sfc                     tu ROM USA (cualquier nombre si la eliges)
languages/                  tus ROMs europeas (opcional)
msu1/                       los .pcm de tu pack MSU-1 (opcional)
shaders/                    shaders GLSL de RetroArch (opcional)
```

### Dónde va cada cosa

| Plataforma | Carpeta del juego (ROM, `languages/`, `msu1/`, `mods/`, `shaders/`, `saves/`, `logs/`, `config.ini`) |
|---|---|
| Windows | la carpeta donde lo descomprimiste, junto a `babel-recomp.exe` |
| Linux | la carpeta donde lo descomprimiste, junto a `babel-recomp` |
| macOS | la carpeta donde lo descomprimiste, junto a `babel-recomp` |
| Switch | `sdmc:/switch/babel-recomp/` (los shaders también se leen de `sdmc:/retroarch/shaders/shaders_glsl/`) |
| Android | `Android/data/com.mr0ta.babelrecomp/files/` en el almacenamiento interno |
| 3DS | `sdmc:/3ds/babel-recomp/` (firmware DSP: `sdmc:/3ds/dspfirm.cdc`) |
| Vita | `ux0:data/babel-recomp/` (la aplicación se instala como `ux0:app/BABL00001`) |

Dentro de esa carpeta:

| Ruta | Contenido |
|---|---|
| `rom.sfc` | tu ROM USA |
| `languages/` | tus ROMs europeas, con cualquier nombre |
| `msu1/` | los `.pcm` de un pack MSU-1 |
| `mods/` | mods descomprimidos (carpeta con `manifest.toml`) y el catálogo incluido |
| `shaders/` | shaders GLSL de RetroArch (PC, Switch, Android) |
| `saves/save.srm` | la partida de batería del juego (compatible con el original) |
| `saves/save0.sav` … | estados guardados, con miniaturas `.thumb` |
| `logs/` | el registro de depuración y los informes de cuelgues |
| `config.ini`, `keybinds.ini` | ajustes y controles |
| `env.txt` | solo consolas, opcional: opciones de diagnóstico, una `CLAVE=valor` por línea |

En PC, si la carpeta es de solo lectura, los registros van a la carpeta de
datos del usuario (`%APPDATA%\babel-recomp`, `~/.local/share/babel-recomp`,
`~/Library/Application Support/babel-recomp`); la segunda línea del registro
indica la ruta.

### Windows

1. Descomprime `babel-recomp-<versión>-windows-x86_64.zip` en una carpeta
   donde puedas escribir.
2. Abre `babel-recomp.exe`. Se abre el lanzador: elige tu ROM y pulsa
   **Play**. (O ponla junto al ejecutable como `rom.sfc`.)

El ejecutable no necesita DLLs. SmartScreen puede avisar de un programa sin
firmar: *Más información → Ejecutar de todas formas*.

### Linux

1. Descomprime `babel-recomp-<versión>-linux-x86_64.zip`.
2. `./babel-recomp` y elige la ROM en el lanzador, o
   `./babel-recomp /ruta/a/rom.sfc` para entrar directamente.

### macOS

1. Descomprime `babel-recomp-<versión>-macos-arm64.zip`.
2. No está firmado: ejecuta una vez
   `xattr -dr com.apple.quarantine babel-recomp-*` y luego `./babel-recomp`.

### Nintendo Switch

Necesitas una Switch con firmware personalizado y el Homebrew Menu.

1. Copia la carpeta `switch/babel-recomp/` del paquete a la raíz de la SD:
   `sdmc:/switch/babel-recomp/babel-recomp.nro`.
2. Pon tu ROM ahí como `rom.sfc` (`sdmc:/switch/babel-recomp/rom.sfc`).
   Opcional: `languages/`, `msu1/`.
3. Abre *Babel Recomp* desde el Homebrew Menu. Se recomienda entrar al
   Homebrew Menu a través de un juego (manteniendo R al abrirlo) en lugar del
   Álbum, para tener toda la memoria.

Los controles son los de SNES en los Joy-Con o el mando Pro. ZL + R3 abre los
ajustes y ZL + L3 la pantalla complementaria. Mantén ZL + R3 al arrancar para
un arranque seguro (vídeo por defecto, sin shader).

### Android

1. Instala `babel-recomp-<versión>-android-arm64.apk` (permite instalar
   desde orígenes desconocidos). Android 7.0 o superior, ARM de 64 bits.
2. Ábrelo una vez y ciérralo: así crea su carpeta.
3. Copia tu ROM como `rom.sfc` en
   `Android/data/com.mr0ta.babelrecomp/files/` (por cable USB, o con
   `adb push rom.sfc /sdcard/Android/data/com.mr0ta.babelrecomp/files/`).
   `languages/` y `msu1/` van en la misma carpeta.
4. Ábrelo de nuevo. Los controles táctiles aparecen junto a la imagen; al
   conectar un mando (Bluetooth o USB) se ocultan.

### Nintendo 3DS / 2DS

Necesitas firmware personalizado (Luma3DS). Funciona en todos los modelos; se
recomienda una **New 3DS / New 2DS XL**, porque los modelos originales tienen
mucha menos CPU (y no incluyen la banda sonora MSU-1).

1. Copia la carpeta `3ds/babel-recomp/` del paquete a la raíz de la SD y pon
   allí tu ROM como `rom.sfc` (`sdmc:/3ds/babel-recomp/rom.sfc`). Opcional:
   `languages/`, `msu1/`.
2. Instala `babel-recomp.cia` con FBI (aparece en el menú HOME), o abre
   `babel-recomp.3dsx` desde el Homebrew Launcher.
3. El sonido necesita el firmware DSP de la consola, como todo el homebrew de
   3DS: ejecuta **DSP1** una vez (crea `sdmc:/3ds/dspfirm.cdc`).

El juego se muestra en la pantalla superior, sin escalar. Si la consola no
puede dibujar todos los fotogramas, se saltan algunos para que el juego
mantenga su velocidad.

### PlayStation Vita

Necesitas HENkaku/Ensō con VitaShell.

1. Instala `babel-recomp-<versión>-vita.vpk` con VitaShell.
2. Pon tu ROM como `ux0:data/babel-recomp/rom.sfc` (la carpeta se crea en el
   primer arranque; `languages/` y `msu1/` también van ahí).

### Rendimiento en portátiles

Todas las versiones emulan el hardware de SNES con exactitud (los mismos
fotogramas y el mismo sonido que en PC, comprobado fotograma a fotograma). La
3DS y la Vita tienen mucha menos CPU que un móvil o una Switch: saltan
fotogramas cuando hace falta para mantener la velocidad del juego y usan
automáticamente los ajustes más rápidos. Según lo medido en emulador, una New
3DS movería el juego a unos tres cuartos de su velocidad completa; se agradecen
datos de consolas reales (activa el contador de FPS en el menú de ajustes).

## Controles

La disposición de SNES, por **posición** en cualquier mando: en consolas de
Nintendo coinciden las letras (A derecha, B abajo, X arriba, Y izquierda); en
otros mandos, el botón en esa posición hace esa función. Todo se puede
reasignar en el lanzador.

| SNES | Teclado | Nintendo (Switch, 3DS) | Xbox | PlayStation / Vita |
|---|---|---|---|---|
| Cruceta | Flechas | Cruceta / stick izquierdo | Cruceta / stick izquierdo | Cruceta / stick izquierdo |
| A | X | A | B | Círculo |
| B | Z | B | A | Equis |
| X | S | X | Y | Triángulo |
| Y | A | Y | X | Cuadrado |
| L / R | C / V | L / R | LB / RB | L1 / R1 (Vita: L / R) |
| Start / Select | Intro / Mayús dcha. | + / − (3DS: Start / Select) | Menu / View | Options / Share (Vita: Start / Select) |

**Atajos.** En un mando, mantén **ZL** (LT, L2) como botón de función; el juego
nunca lo usa:

| Acción | Teclado | Mando |
|---|---|---|
| Menú de ajustes | Esc | ZL + R3 |
| Pantalla compañera | I | ZL + L3 |
| Menú de estados guardados | F11 | ZL + Start |
| Guardar / cargar rápido (ranura 1) | Mayús+F1 / F1 (ranuras 1-10: F1-F10) | ZL + R / ZL + L |
| Rebobinar | F12 | ZL + Select |
| Avance rápido (mantener) | Tab | ZL + ZR |
| Indicador de rendimiento | — | ZL + X |
| Volumen | — | ZL + Arriba / Abajo |
| Pantalla completa | Alt+Intro | — |
| Siguiente idioma | Ctrl+L | — |

**Por plataforma.**

- **Switch:** Joy-Con (acoplados o por parejas) o mando Pro. La pantalla táctil
  funciona en la pantalla compañera y en los modos Dual / Flip Grip.
- **Android:** el mando en pantalla (cruceta, A/B/X/Y, L/R, Select/Start y un
  botón ☰ para los ajustes) mientras no haya un mando conectado; un mando
  Bluetooth o USB lo oculta y usa la tabla de arriba.
- **3DS:** los botones de la consola (Circle Pad = cruceta). **Toca la pantalla
  táctil** para abrir el menú de ajustes; los estados guardados están dentro.
- **Vita:** los botones de la consola como en la columna de PlayStation. **Toca
  la pantalla** para abrir el menú de ajustes; los estados guardados están
  dentro.

## Compilar desde el código

Necesitas CMake ≥ 3.20, un compilador C11/C++17, Python 3 y Ninja (o Make).
SDL3 se descarga y compila automáticamente si el sistema no lo tiene.

```sh
git clone https://github.com/mrOt4/babel-recomp.git
cd babel-recomp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/babel-recomp
```

La compilación por defecto es **solo intérprete**: el código del juego se lee
de tu ROM al ejecutarse, así que nada derivado de la ROM se compila dentro y no
hace falta ninguna ROM para compilar. (En este juego funciona igual e igual de
rápido; se comparó fotograma a fotograma.) Quien quiera el código
recompilado estáticamente puede configurar con
`-DSNESRECOMP_INTERPRETER_ONLY=OFF` y generarlo desde su propia ROM con
`bash tools/regen.sh --rom /ruta/a/rom.sfc`; ese resultado (`src/gen/`) no se
debe subir ni compartir nunca.

| Destino | Cómo |
|---|---|
| Windows (cruzado desde Linux) | `scripts/toolchains/setup_windows.sh` y configurar con `-DCMAKE_TOOLCHAIN_FILE=scripts/toolchains/windows-llvm-mingw.cmake` |
| Android | `scripts/toolchains/setup_android.sh`, `source ~/toolchains/android-env.sh` y `cd android && ./gradlew assembleRelease` |
| Switch | devkitPro (`switch-dev`) y Mesa: `scripts/toolchains/setup_switch.sh`, luego `scripts/toolchains/stage_switch.sh` para la carpeta de la SD |
| 3DS | `scripts/toolchains/setup_3ds.sh` y configurar con `-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake` (genera el `.3dsx` y el `.cia`) |
| Vita | `scripts/toolchains/setup_vita.sh` y configurar con `-DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake` (genera el `.vpk`) |
| Paquete | `scripts/package_release.sh <carpeta-build> <plataforma>` |

Los detalles están en [scripts/toolchains/README.md](scripts/toolchains/README.md).
El workflow de GitHub Actions compila Linux, Windows, macOS y Android y los
publica al subir una etiqueta `v*`.

## Informar de un problema

Abre una issue y adjunta la carpeta `logs/` que hay junto al juego
(`sdmc:/switch/babel-recomp/logs/` y `babel-recomp.log` en Switch). Cuenta qué
estabas haciendo y qué mods tenías activados.

## Créditos y agradecimientos

Este port se apoya en el trabajo de mucha gente. Gracias a todos.

- **Quintet / Enix**: el *Illusion of Gaia* original (1993). Todo el contenido
  del juego pertenece a sus propietarios.
- **snesrecomp**: el framework de recompilación estática de SNES, de Matthew
  Stanley y la comunidad R.A.I.D.
  ([repositorio oficial](https://github.com/RetroPortingToolKit/snesrecomp)).
  Este port está construido sobre él. Incluye código e investigación de:
  - **snesrev** (ports de zelda3 / smw por ingeniería inversa, MIT): el núcleo
    del hardware de SNES;
  - **LakeSnes**, **snes9x**, **bsnes**, **ares** (Cx4), **IsoFrieze**
    (SMWDisX), **perplexes** (recompiler-rs), **DerrickGold** (ar-recomp) y
    otros citados en
    [snesrecomp/THIRD_PARTY_ATTRIBUTION.md](snesrecomp/THIRD_PARTY_ATTRIBUTION.md).
- **recomp-ui**: el lanzador, de Matthew Stanley (MIT), con **Dear ImGui**
  (Omar Cornut), **stb** (Sean Barrett) y las fuentes Lato, Noto Sans Symbols 2
  y OpenMoji (ver sus `NOTICE.md`).
- **GaiaLabs**, de Azarem ([GaiaLabs](https://github.com/Azarem/GaiaLabs)):
  las herramientas de ROM hacking, el desensamblado y la base de datos de
  Illusion of Gaia: símbolos, formato de texto y los parches de referencia del
  botón de correr, New Game+, transiciones de música y más. Sin estas
  herramientas este port no existiría.
- **Illusion of Gaia Randomizer (IoGR)**: DontBaguMe y colaboradores
  ([iogr.app](https://www.iogr.app)); el parche **Apocalypse Gaia**, de un
  miembro de esa comunidad, y sus testers.
- **Autores de packs MSU-1**, en especial **Conn** (foro Zeldix), cuya
  numeración de pistas sigue este port.
- **Aleks** (Alexgg1014), autor de
  [ALEKS Ultimate NX](https://github.com/Alexgg1014/The-Legend-of-Zelda-A-Link-To-The-Past-Alek-s-Ultimate-NX-Edition): por la
  pantalla dual (los modos Dual y Flip Grip de la pantalla complementaria) y
  por su ayuda durante todo el proyecto. De su port se adaptaron, con permiso,
  el arranque seguro, el informe de cuelgues y el almacenamiento de
  RetroAchievements. ALEKS Ultimate NX se basa en
  [zelda-alttp-3ds](https://github.com/EstebanPdN/zelda-alttp-3ds), de **EstebanPdN**.
- **RetroAchievements** y **rcheevos**.
- **SDL** (libsdl.org), **devkitPro** (devkitA64, libnx, devkitARM, libctru),
  **Mesa** para Switch (danfromtico, NaGaa95), **VitaSDK**, **Project_CTR**
  (makerom) y **bannertool**.
- Los ports de **zelda3** para 3DS (ProbablyRohan) y Vita (Rinnegatamante), cuyas
  decisiones de compilación orientaron las versiones portátiles.
- **RetroArch / libretro**: el formato de shaders GLSL y `libretro.h`.
- Todos los que han probado versiones y enviado logs, capturas y vídeos.

## Proyecto asistido por IA

Este proyecto se ha desarrollado con ayuda de inteligencia artificial (Claude,
de Anthropic, mediante Claude Code), dirigido, probado y revisado por
**mr0ta**. Los fallos son nuestros; por favor, infórmanos.

## Licencia

El código fuente está bajo la
[PolyForm Noncommercial License 1.0.0](LICENSE): libre para usar, estudiar,
modificar y compartir con cualquier fin no comercial. Los frameworks mantienen
sus propias licencias (`snesrecomp/LICENSE`, `recomp-ui/LICENSE` y los avisos
de terceros citados). Ninguna cubre el juego, que no se distribuye aquí.

Las donaciones por Ko-fi son voluntarias y no son una venta del juego ni de
este software: todo es gratuito.

*Illusion of Gaia* es una marca de sus respectivos propietarios. Este proyecto
no está afiliado ni respaldado por Nintendo, Square Enix ni Quintet.
