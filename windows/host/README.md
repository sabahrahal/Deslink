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

Cada segundo imprime fps capturados, fps codificados, Mbps y la latencia de codificación media (desde que el frame llega al codificador hasta que sale el flujo).

El cortafuegos de Windows pedirá permiso la primera vez: hay que permitir redes privadas. El cable USB no pasa por el cortafuegos.
