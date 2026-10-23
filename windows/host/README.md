# windows/host

Servicio de Windows que captura una salida, la codifica por hardware y la envía por UDP al cliente Android (protocolo DLP, ver `docs/protocol-dlp.md`).

Por defecto duplica un monitor existente con DXGI Desktop Duplication. Con `--virtual` crea un monitor virtual (driver de `windows/driver/`, ver `docs/fase-3.md`) y captura ese.

## Compilar

Requiere Visual Studio 2022 (carga de trabajo C++) y CMake 3.16 o superior.

```
cmake -S windows/host -B build/host -G "Visual Studio 17 2022" -A x64
cmake --build build/host --config Release
build\host\Release\extendesk-host.exe --fps 60 --bitrate 50 --codec hevc
```

## Opciones

| Opción | Por defecto | Qué hace |
| --- | --- | --- |
| `--port` | 47800 | Puerto UDP en el que escucha |
| `--output` | 0 | Índice de la salida (monitor) que se captura |
| `--fps` | 60 | Fotogramas por segundo máximos |
| `--bitrate` | 50 | Mbps (CBR) |
| `--codec` | hevc | `hevc` o `h264` |
| `--encoder` | auto | `auto`: codificación directa según la GPU que captura, NVENC con NVIDIA y AMF con AMD (menos retardo que sus codificadores de Media Foundation), y si no arranca o la GPU es de otra marca (Intel), Media Foundation; `nvenc` o `amf` lo mismo solo para esa marca; `mf` siempre Media Foundation |
| `--virtual` | apagado | Monitor virtual en vez de `--output` (requiere el driver) |
| `--size` | la de la tablet | Resolución del monitor virtual. Sin `--size` se usa la que pide la tablet al conectar (o la última recordada hasta que conecte) |
| `--pace` | 100 | Reparte cada frame a estos Mbps por UDP (0 = de golpe); por cable USB no se usa |
| `--no-usb` | apagado | No escucha en TCP 127.0.0.1 ni pone `adb reverse` (cable USB, ver `docs/usb.md`) |
| `--adb` | se busca | Ruta de `adb.exe` (por defecto: junto al .exe, PATH o SDK de Android) |
| `--refine` | 4 | Repeticiones lentas (cada 100 ms) al quedarse quieta la imagen, para que el codificador la afine; 0 las desactiva |
| `--touch` / `--mouse` | lo guardado (táctil) | Fuerza el modo del táctil solo para esta vez (ver abajo) |
| `--no-touch` | apagado | Ignora los toques de la tablet |
| `--status` | apagado | Líneas `@listo` y `@estado …` (una por segundo) para la app de bandeja en vez de las estadísticas; envía PING a la tablet para medir el RTT |
| `--stop-event` | ninguno | Nombre de un evento de Windows; al activarse, el host para limpiamente (lo usa la app de bandeja) |
| `--no-audio` | apagado | No envía nunca el sonido del PC, aunque la tablet lo pida |
| `--audio-bitrate` | 128 | kbps de Opus para el sonido (32–256) |
| `--profile` | maxima | Perfil de batería si la tablet no manda el suyo (`ahorro`, `equilibrado`, `maxima`). La app manda siempre el elegido y ese manda; `--fps` y `--bitrate` siguen siendo el techo. Ver `docs/battery.md` |

Lo normal es abrirlo con `extendesk.exe` (la app de bandeja, `windows/app/`); desde una consola sigue funcionando igual.

La app de la tablet puede pedir otra resolución, fps, tasa o códec desde sus Ajustes (ver «Configuración de la tablet» en `docs/protocol-dlp.md`); lo que pide manda sobre `--fps`, `--bitrate` y `--codec`.

Cada segundo imprime fps capturados, fps codificados, Mbps y la latencia de codificación media (desde que el frame llega al codificador hasta que sale el flujo).

## Táctil

- **Táctil directo** (por defecto): los dedos de la tablet son toques reales de Windows, con multitáctil y presión, y el cursor no se mueve. Tocar, arrastrar, pellizcar para hacer zoom, desplazar con dos dedos y mantener pulsado para el clic derecho los interpreta Windows como en cualquier pantalla táctil. Usa los punteros sintéticos de Windows 10 1809 o posterior (`InjectTouchInput` en anteriores). Si Windows rechaza los toques, el host lo explica en la consola, pasa a modo ratón y se lo dice a la tablet.
- **Ratón**: para apps que no entienden el táctil. Un toque es un clic (dos seguidos, doble clic), un dedo que se mueve arrastra, un dedo quieto medio segundo es clic derecho, dos dedos desplazan (rueda), pellizcar es Ctrl + rueda y tocar con dos dedos es clic derecho.

El modo se guarda en `%APPDATA%\Extendesk\input-mode.txt` (`touch` o `mouse`). Lo cambia la tablet en marcha (paquete `INPUT_MODE`, ver `docs/protocol-dlp.md`) o cualquier app escribiendo ese fichero antes de arrancar el host. Las ventanas de administrador (p. ej. el Administrador de tareas) no aceptan toques de un host sin elevar.

El cortafuegos de Windows pedirá permiso la primera vez: hay que permitir redes privadas. El cable USB no pasa por el cortafuegos.

## Sonido

Si en la app de la tablet está activado «Sonido del PC en la tablet», el host captura lo que suena en la salida de audio predeterminada de Windows (WASAPI loopback), lo codifica en Opus (trozos de 10 ms) y lo manda por el mismo camino que el vídeo (WiFi o cable USB). Si cambias de altavoces o auriculares en Windows, sigue a la nueva salida. El PC lo sigue oyendo también por sus altavoces: bájales el volumen o silencia el PC desde los altavoces físicos si solo quieres oírlo en la tablet. En la tablet, el volumen es el de multimedia (botones de volumen).
