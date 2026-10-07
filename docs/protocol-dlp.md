# Protocolo DLP v1

Protocolo propio sobre UDP, el mismo para WiFi y USB. Implementado en `protocol/` (C++, usado por el host de Windows) y en `android/app/.../Dlp.kt` y `FrameReassembler.kt` (Kotlin, fase 1). Las pruebas de ambos cubren los mismos casos.

## Cabecera (24 bytes, little-endian)

| Bytes | Campo | Significado |
| --- | --- | --- |
| 0–1 | magic | `0x4C44` ("DL") |
| 2 | version | 1 |
| 3 | type | tipo de paquete (tabla siguiente) |
| 4 | flags | bit 0: frame clave; bit 1: último paquete del frame |
| 5 | reservado | 0 |
| 6–9 | frame_id | número de frame; crece de uno en uno |
| 10–11 | packet_index | posición del paquete dentro del frame |
| 12–13 | packet_count | paquetes del frame |
| 14–21 | timestamp_us | reloj del emisor, en microsegundos |
| 22–23 | payload_size | bytes de payload que siguen a la cabecera |

Tamaño máximo de paquete: 1200 bytes (1176 de payload), para no fragmentar en UDP. Todos los paquetes de un frame salvo el último van llenos.

## Tipos

| Valor | Nombre | Sentido | Payload |
| --- | --- | --- | --- |
| 1 | VIDEO | host a cliente | trozo del frame codificado (H.264 o HEVC, formato Annex B) |
| 2 | HELLO | cliente a host | vacío. Registra al cliente como destino y pide frame clave |
| 3 | KEYFRAME_REQUEST | cliente a host | vacío |
| 4 | PING | cliente a host | vacío; el timestamp es el del cliente |
| 5 | PONG | host a cliente | vacío; devuelve el timestamp del PING |
| 6 | STREAM_INFO | host a cliente | ancho u16, alto u16, fps u16, códec u8 (1 H.264, 2 HEVC) |

## Flujo

1. El cliente envía HELLO cada segundo hasta recibir STREAM_INFO.
2. El host responde STREAM_INFO, fuerza un frame clave y empieza a enviar VIDEO a la dirección de origen del HELLO.
3. El cliente reensambla. Sin colas: al completarse un frame se descartan los anteriores incompletos.
4. Si se pierde un frame (o hay un hueco en `frame_id`), el cliente pide KEYFRAME_REQUEST y no decodifica frames hasta recibir uno clave.
5. El cliente envía PING cada segundo; el RTT es la diferencia entre el reloj local y el timestamp devuelto.

## Pendiente (no está en la fase 1)

- FEC para pérdidas sueltas sin esperar a un frame clave.
- Cifrado AES-GCM y emparejamiento por QR. **Ahora el tráfico va en claro y el host acepta a cualquier cliente de la red**: usar sólo en redes de confianza.
- Canal de entrada (toque y lápiz) y control de tasa adaptativo.
