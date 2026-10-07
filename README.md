# DeskLink

Usa un móvil o una tablet Android como segundo monitor de un PC con Windows, por WiFi o por USB, con la menor latencia posible.

## Cómo funciona

1. Un driver de pantalla virtual (IddCx) crea un monitor extra en Windows.
2. El host captura ese monitor en la GPU y lo codifica con el encoder hardware (NVENC, AMF o Quick Sync).
3. El vídeo viaja por un protocolo UDP propio (DLP) sobre WiFi o USB.
4. La app Android lo decodifica por hardware y lo pinta directo en pantalla.
5. El toque y el lápiz vuelven al PC por el mismo canal.

Objetivo: menos de 35 ms de cristal a cristal por WiFi y menos de 25 ms por USB.

## Estructura

| Carpeta | Contenido |
| --- | --- |
| `protocol/` | Especificación DLP y librería C++ compartida (paquetes, FEC, cifrado) |
| `windows/driver/` | Indirect Display Driver (IddCx, UMDF) |
| `windows/host/` | Servicio C++20: captura, encoders, transportes, entrada, diagnóstico |
| `windows/app/` | App de bandeja, emparejamiento y panel de diagnóstico (WinUI 3 / .NET 8) |
| `android/app/` | App Kotlin + Jetpack Compose (UI, emparejamiento QR, panel de diagnóstico) |
| `android/native/` | C++ NDK: red, FEC, AMediaCodec |
| `tools/bench/` | `desklink-bench`: comparación de latencia entre transportes |

## Documentación

- [Arquitectura](docs/architecture.md)
- [Fase 1: prototipo y cómo probarlo](docs/fase-1.md)
- [Fase 3: monitor virtual](docs/fase-3.md)
- [Protocolo DLP](docs/protocol-dlp.md)
- [Asesor de latencia](docs/latency-advisor.md)
- [Panel de diagnóstico](docs/diagnostics-panel.md)

## Hardware de referencia (pruebas de desarrollo)

- PC: Windows, NVIDIA RTX 4060
- Tablet: Lenovo Idea Tab Pro (Dimensity 8300, 2944x1840 a 144 Hz, USB-C 3.2, WiFi 6E)
