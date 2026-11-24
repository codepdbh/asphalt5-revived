# Port nativo de Asphalt 5 HD 3.4.1

El motor ARM32 original se traduce a C y el NDK compila ese C para AArch64. La app carga una biblioteca ELF64 normal (`libasphalt5.so`), sin emulador, JIT ni generador de código en tiempo de ejecución. Las direcciones del motor conservan 32 bits: son desplazamientos dentro de una reserva de 4 GiB.

La traducción usa [Asphalt5-Native](https://github.com/mrbaazzman/Asphalt5-Native/tree/f977989fa74faeb2442831f1241b4943d5459322) en la revisión fijada por el script (traductor, núcleo `guest.c` y la corrección de `CMatrix`). Toda la capa de plataforma es propia de Android.

## Arquitectura

| Pieza | Archivo | Qué hace |
|---|---|---|
| Núcleo invitado | `guest.c` de la referencia + `patches/asphalt5-native-android.patch` | Reserva de memoria con `mmap`/`mprotect` y `MAP_NORESERVE`, cargador ELF, relocaciones, heap de 32 bits |
| Importaciones | `native/imports_android.c` | Las ~110 importaciones de libc, stdio, libm, EABI, pthread y red del motor, cada una registrada directamente en su dirección (sin búsqueda por nombre) |
| GLES 1.1 | `native/gles_bridge.c`, `native/texture_upload.cpp` | Llamadas directas al driver de Android; escalado del lienzo lógico 854×480 a la pantalla con bandas; PVRTC1 descomprimido por software si el driver no lo soporta |
| JNI invitado | `native/guest_jni.c` | JNIEnv y JavaVM completos para el motor; recursos leídos en C; el resto de métodos se reenvía a `GameBridge` por nombre y firma |
| Red e hilos | `native/net_android.c` | Sockets reales para el multijugador local (broadcast UDP, TCP no bloqueante, `select`, `ioctl` de interfaces) e hilos reales con su propia pila invitada |
| Ciclo de vida | `native/game_host.c` | Arranque del motor y la misma secuencia que hacían `Asphalt5`, `Asphalt5Renderer`, `GLMediaPlayer` y `GLResLoader` |
| App | `app/src/main/java/.../GameActivity.java`, `GameBridge.java`, `GameNative.java` | `GLSurfaceView` GLES1, multitáctil, teclas, acelerómetro, pausa, audio con `SoundPool` y `MediaPlayer` |

Mapa de memoria del invitado: imagen ELF en `0x00000000`, pila en `0x10000000-0x10FFFFFF`, heap en `0x11000000-0x3FFFFFFF` (comprometido entero, como en la referencia, porque el empaquetador de geometría lee un poco más allá de los bloques de vértices y el driver GL desreferencia esos punteros), zona auxiliar en `0x40000000` y datos del host (errno, `_ctype_`, `__sF`, JNIEnv) en `0x70000000`.

## Detalles de ABI que corrige esta versión

- `_ctype_` es un `OBJECT` de 4 bytes en bionic antiguo: un puntero a la tabla de 257 entradas, no la tabla. `__sF` son tres `FILE` de 84 bytes; `stdout` y `stderr` van a logcat.
- Los argumentos variables siguen AAPCS softfp: los `double` ocupan un par de registros par o 8 bytes alineados en pila. Se aplica a `printf`, `sprintf`, `vsprintf`, `swprintf`, `__android_log_print` y a las llamadas JNI `Call*Method`, `Call*MethodV` y `Call*MethodA`.
- `GLMediaPlayer.isSoundLoaded` e `isSoundLoadedBig` devuelven 0 si el sonido está cargado y -1 si no (contrato del Java original).
- Las referencias JNI locales mueren al volver la llamada nativa más externa o con `DeleteLocalRef`, y pasan por una cuarentena antes de liberarse porque 3.4.1 lee algunas cadenas después de borrarlas.
- Al pedir un vídeo (`loadMovie`) se libera `Game+0x1d72` tras el fotograma, como hacía la finalización de `MyVideoView`. De momento los vídeos no se reproducen.
- `__cxa_guard_*` usan el bit 0 del guard (ABI C++ de ARM).
- El código generado se compila con `-O1 -fwrapv -fno-strict-aliasing`, igual que la referencia. Sin `-fwrapv` el desbordamiento con signo de la aritmética traducida es comportamiento indefinido.

## Compilar, instalar y ejecutar

Requisitos: Python con `capstone==5.0.9` y `pyelftools==0.32`, Android NDK r28, SDK de plataforma 36, Build Tools 36.0.0, un JDK con `javac` y el checkout de referencia en `references/Asphalt5-Native`.

```powershell
python -m pip install capstone==5.0.9 pyelftools==0.32
# Prueba de arranque por ADB (constructores + crc32 del motor):
python scripts/build_port.py a5/05011-Asphalt-5-HD-v3.4.1.apk --boot-probe
# APK del juego, instalar, copiar los datos y lanzar:
python scripts/build_port.py a5/05011-Asphalt-5-HD-v3.4.1.apk --install --push-data --launch
adb logcat -s Asphalt5 A5Guest
```

La traducción se hace una vez y queda en `workspace/build/generated-<hash>`; después solo se recompilan los archivos que cambian. El APK resultante es `workspace/build/asphalt5-arm64.apk`. Contiene la biblioteca original como recurso, así que debe quedarse en local.

Los datos del juego (la carpeta `asphalt5`, 326 archivos) van en la memoria interna, en `/sdcard/gameloft/games/asphalt5/`, la misma ruta que usaba el juego original. Para leerla, Android 11+ exige el permiso "Acceso a todos los archivos" (`MANAGE_EXTERNAL_STORAGE`). La app lo pide en el primer arranque y `build_port.py --install` lo concede por ADB. Las partidas guardadas se escriben en el almacenamiento privado de la app. `--push-data` usa `adb push --sync`, así que solo copia lo que falta.

## Resolución

El juego original recibía la resolución real de cada dispositivo y ajusta su interfaz a ella. La app le pasa un lienzo de 480 líneas con el ancho según la proporción de la pantalla (entre 640 y 1280; por ejemplo 1040×480 en 19,5:9) y el puente GL lo escala a la resolución nativa sin deformar. La escena 3D se renderiza a la resolución real del panel.

- Carrera y HUD se adaptan solos: el HUD se ancla a las esquinas.
- El escaparate 3D del menú principal usa un viewport fijo de 738×442 (función que vuelve a `0x000fb3bc`). El puente lo ensancha hasta el panel lateral y estrecha la proyección por el mismo factor para no estirar el coche.
- En lienzos de más de 854 de ancho, el blanco de borrado de las pantallas 2D se cambia a negro, porque sus bitmaps solo cubren 854 columnas.
- La portada es un bitmap de 854 px: queda alineada a la izquierda con una franja negra a la derecha.

## Hitos

1. Traducir y ejecutar código del motor en un dispositivo ARM64. Completado.
2. Memoria de 32 bits, relocaciones y constructores. Completado (6.208 relocaciones, 83 constructores, crc32 `0xcbf43926`).
3. Importaciones y JNI completos hasta la inicialización del motor con los datos reales. Completado.
4. Presentar fotogramas en pantalla y llegar al menú original. Completado: portada, menú principal 3D, pantalla de carga y carrera rápida (HUD, rivales, minimapa) en el Redmi Note 8 (API 37, 2340×1080).
5. Pendiente de verificar: audio audible, control táctil y acelerómetro en carrera, completar una carrera, guardar, reiniciar y comprobar la persistencia. Medir FPS. Probar en hardware con páginas de 16 KiB.

## Multijugador WiFi local

El motor trae el multijugador local completo de Gameloft (`CAndroidSocket`): descubrimiento por broadcast UDP (puerto 7891), partida por TCP (puerto 7790), sockets no bloqueantes con `select` y la IP propia mediante `ioctl(SIOCGIFCONF)`.

- `native/net_android.c` usa descriptores reales del sistema y convierte las estructuras que cambian entre ARM32 y AArch64: `timeval`, `ifconf`/`ifreq` (32 bytes por entrada en el invitado), `hostent` y `addrinfo`. `sockaddr`, `socklen_t` y `fd_set` son idénticos y se pasan tal cual. Los envíos usan `MSG_NOSIGNAL`.
- `pthread_create` crea hilos reales, cada uno con su `CPU` y 1 MiB de pila en el heap invitado; si llaman a JNI se adjuntan a la JVM.
- La app llama a `Asphalt5.nativeCheckwifi` como el Java original (al arrancar y en cada cambio de red), mantiene un `MulticastLock` para recibir broadcasts y cuenta un punto de acceso propio como WiFi activo.
- El nombre del jugador en la sala es el nombre del dispositivo (Ajustes), no "localhost"; `gethostbyname` de ese nombre devuelve la IP WiFi.
- **Menú:** la versión 3.4.1 GAND solo ofrece el botón ONLINE (acción `0x40d`), que depende de los servidores de Gameloft, cerrados. El flujo local sigue completo en el motor (`GS_MultiplayerMenu::Update` gestiona la acción `0x40c`). `game_host.c` cambia la acción de la tabla del botón (`0x1f82f4` y `0x1f830c`) a `0x40c` y envuelve `GS_MultiplayerMenu::ApplyMenuSettings` (ranura 33 de la vtable, `0x19817c`) para que la etiqueta sea la cadena original "LOCAL". Comprueba los valores esperados antes de modificar nada.

Verificado en el Redmi Note 8: MULTIPLAYER → LOCAL → LOCAL WI-FI (CREATE/JOIN) → selección de coche y pista → sala "CONNECTED DEVICES" esperando jugador, con TCP escuchando en 7790 y UDP en 7891. Falta probar una partida entre dos teléfonos.

## Pendiente conocido

- Vídeos (`loadMovie`): se omiten.
- `glCopyTexImage2D` copia el origen escalado pero con el tamaño lógico; si algún efecto lo usa a pantalla completa se verá recortado. Se registra en logcat la primera vez que se usa.
- Si el driver no expone `GL_OES_matrix_palette`, las mallas con skinning no se deforman. El Redmi Note 8 sí lo expone.
- La pérdida del contexto EGL no se recupera. Se usa `setPreserveEGLContextOnPause(true)`.
