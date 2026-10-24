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
| WiFi 6E / 5 GHz | Misma red, mDNS `_extendesk._udp` | 1 |

## Ajustes de baja latencia

- Encoder: sin B-frames, sin lookahead, intra refresh en vez de keyframes periódicos, VBV de un frame, varios slices por frame.
- Decoder: `KEY_LOW_LATENCY`, claves del fabricante, envío por slice, render directo en `SurfaceView`.
- Sistema: hilos dedicados con prioridad alta, cero asignaciones por frame, `WifiLock` de baja latencia, `Surface.setFrameRate`.

## Desconexión

La pantalla extra solo existe mientras hay un dispositivo conectado: así Windows nunca se queda con un monitor que
nadie ve. El host arranca escuchando sin crearla, la crea cuando una tablet saluda (HELLO, ya a su medida) y la
quita cuando se va. Mientras no hay pantalla no captura ni codifica.

| Qué pasa | PC | Tablet |
| --- | --- | --- |
| Desconectar, Atrás o cerrar la app en la tablet | Recibe BYE (motivo 0) y quita la pantalla extra en 1 s | Vuelve al inicio |
| Bloquear la tablet o pasarla a segundo plano | Recibe BYE (motivo 1): deja de enviar y guarda la pantalla 60 s | Al desbloquear vuelve a saludar y sigue |
| Corte sin aviso (cable que parpadea, Wi-Fi) | A los 5 s olvida a la tablet y guarda la pantalla 15 s más (20 s en total; por cable se nota al momento, 15 s) | «Reconectando…»; si en 20 s no oye nada, vuelve al inicio con «Se perdió la conexión» |
| Detener o Salir en el PC | Manda BYE (motivo 0) 3 veces y quita la pantalla | «Extendesk se detuvo en el PC»; si el PC vuelve en 8 s (al aplicar cambios se reinicia) sigue sola, si no vuelve al inicio |

Mientras se guarda la pantalla, la ventana del PC dice cuánto queda (`gracia` en la línea `@estado`). Las
ventanas que había en la pantalla extra vuelven al monitor principal al quitarla, y Windows las devuelve a su sitio
cuando se vuelve a crear con la misma resolución. Duplicando un monitor no hay pantalla que quitar: solo se deja de
enviar. Ver BYE en [el protocolo](protocol-dlp.md).

## Hoja de ruta

1. Prototipo de pipeline por WiFi (duplicando un monitor existente).
2. USB (anclaje y ADB), banco de pruebas, overlay y primera versión del panel de diagnóstico.
3. Driver IddCx con modos dinámicos.
4. Toque multipunto y lápiz con presión.
5. Android Open Accessory, emparejamiento QR y cifrado.
6. Pulido: varios dispositivos, instalador firmado, AV1.
