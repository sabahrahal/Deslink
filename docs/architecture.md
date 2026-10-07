# Arquitectura

Documento completo y editable: https://claude.ai/code/artifact/c14c732b-c555-41a5-b7fd-9430ebb261ef

## Principios

- **Cero copias a CPU en Windows**: el frame vive en la GPU desde la captura hasta que sale codificado.
- **Nada de colas**: cada etapa trabaja con el frame más reciente y descarta los viejos.
- **Un solo pipeline, transporte intercambiable**: WiFi y USB usan el mismo protocolo (`ITransport`).

## Pipeline

```
Windows (GPU)                                          Android
Monitor virtual -> Captura -> NV12 + NVENC -> DLP  ==>  Recepción -> AMediaCodec -> SurfaceView
   (IddCx)     (Desktop Dup.)  (HEVC, P1)  (UDP,FEC,     (hilo NDK)   (baja latencia)  (sin cola)
                                            AES-GCM)
      ^                                                                  |
      +-------------- toque y lápiz (UDP, InjectSyntheticPointerInput) --+
```

## Transportes (de menor a mayor latencia esperada)

| Transporte | Requisitos | Fase |
| --- | --- | --- |
| Cable USB por ADB reverse (TCP) | Depuración USB (una vez) y adb.exe junto al host | 5 |
| USB por Android Open Accessory | Driver WinUSB en Windows | descartado por ahora (ver `docs/usb.md`) |
| Anclaje USB (tethering) | Activar anclaje USB cada vez | 2 |
| WiFi 6E / 5 GHz | Misma red, mDNS `_desklink._udp` | 1 |

## Ajustes de baja latencia

- Encoder: sin B-frames, sin lookahead, intra refresh en vez de keyframes periódicos, VBV de un frame, varios slices por frame.
- Decoder: `KEY_LOW_LATENCY`, claves del fabricante, envío por slice, render directo en `SurfaceView`.
- Sistema: hilos dedicados con prioridad alta, cero asignaciones por frame, `WifiLock` de baja latencia, `Surface.setFrameRate`.

## Hoja de ruta

1. Prototipo de pipeline por WiFi (duplicando un monitor existente).
2. USB (anclaje y ADB), banco de pruebas, overlay y primera versión del panel de diagnóstico.
3. Driver IddCx con modos dinámicos.
4. Toque multipunto y lápiz con presión.
5. Android Open Accessory, emparejamiento QR y cifrado.
6. Pulido: varios dispositivos, instalador firmado, AV1.
