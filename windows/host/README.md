# windows/host

Servicio de Windows que captura una salida, la codifica por hardware y la envía por UDP al cliente Android (protocolo DLP, ver `docs/protocol-dlp.md`).

Por defecto duplica un monitor existente con DXGI Desktop Duplication. Con `--virtual` crea un monitor virtual (driver de `windows/driver/`, ver `docs/fase-3.md`) y captura ese.

## Compilar

Requiere Visual Studio 2022 (carga de trabajo C++) y CMake 3.16 o superior.

```
cmake -S windows/host -B build/host -G "Visual Studio 17 2022" -A x64
cmake --build build/host --config Release
build\host\Release\desklink-host.exe --fps 60 --bitrate 50 --codec hevc
```

## Opciones

| Opción | Por defecto | Qué hace |
| --- | --- | --- |
| `--port` | 47800 | Puerto UDP en el que escucha |
| `--output` | 0 | Índice de la salida (monitor) que se captura |
| `--fps` | 60 | Fotogramas por segundo máximos |
| `--bitrate` | 50 | Mbps (CBR) |
| `--codec` | hevc | `hevc` o `h264` |
| `--virtual` | apagado | Monitor virtual en vez de `--output` (requiere el driver) |
| `--size` | 2944x1840 | Resolución del monitor virtual |
| `--pace` | 100 | Reparte cada frame a estos Mbps por UDP (0 = de golpe); por cable USB no se usa |
| `--no-usb` | apagado | No escucha en TCP 127.0.0.1 ni pone `adb reverse` (cable USB, ver `docs/usb.md`) |
| `--adb` | se busca | Ruta de `adb.exe` (por defecto: junto al .exe, PATH o SDK de Android) |
| `--refine` | 4 | Repeticiones lentas (cada 100 ms) al quedarse quieta la imagen, para que el codificador la afine; 0 las desactiva |
| `--no-audio` | apagado | No envía nunca el sonido del PC, aunque la tablet lo pida |
| `--audio-bitrate` | 128 | kbps de Opus para el sonido (32–256) |

Cada segundo imprime fps capturados, fps codificados, Mbps y la latencia de codificación media (desde que el frame llega al codificador hasta que sale el flujo).

El cortafuegos de Windows pedirá permiso la primera vez: hay que permitir redes privadas. El cable USB no pasa por el cortafuegos.

## Sonido

Si en la app de la tablet está activado «Sonido del PC en la tablet», el host captura lo que suena en la salida de audio predeterminada de Windows (WASAPI loopback), lo codifica en Opus (trozos de 10 ms) y lo manda por el mismo camino que el vídeo (WiFi o cable USB). Si cambias de altavoces o auriculares en Windows, sigue a la nueva salida. El PC lo sigue oyendo también por sus altavoces: bájales el volumen o silencia el PC desde los altavoces físicos si solo quieres oírlo en la tablet. En la tablet, el volumen es el de multimedia (botones de volumen).
