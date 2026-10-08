# Protocolo DLP v1

Protocolo propio sobre UDP, el mismo para WiFi y USB. Por cable USB (`adb reverse`, ver `docs/usb.md`) los mismos paquetes viajan por TCP, cada uno precedido de su tamaño (u16 little-endian). Implementado en `protocol/` (C++, usado por el host de Windows) y en `android/app/.../Dlp.kt` y `FrameReassembler.kt` (Kotlin, fase 1). Las pruebas de ambos cubren los mismos casos.

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
| 2 | HELLO | cliente a host | Registra al cliente como destino y pide frame clave. Payload opcional: configuración de la tablet (ver abajo); vacío = no cambia nada |
| 3 | KEYFRAME_REQUEST | cliente a host | vacío |
| 4 | PING | los dos sentidos | vacío; el timestamp es el de quien lo envía |
| 5 | PONG | los dos sentidos | vacío; devuelve el timestamp del PING |
| 6 | STREAM_INFO | host a cliente | ancho u16, alto u16, fps u16, códec u8 (1 H.264, 2 HEVC). Hosts nuevos añaden: banderas u8 (bit 0: aplica la configuración de HELLO), longitud u8 y nombre del PC en UTF-8 (máx. 64). Los clientes antiguos solo leen los 7 primeros bytes |
| 7 | BURST_REQUEST | cliente a host | count u16, size u16, pace_mbps u16 (0 = de golpe). El timestamp sirve de nonce |
| 9 | RATE_REPORT | cliente a host | frames recibidos u16, frames perdidos u16 en el último segundo. El host baja la tasa un 30 % si hay pérdidas (o si la tablet pide un frame clave), recuerda un techo por debajo de la tasa que falló y la sube un 5 % tras 8 s limpios (máximo `--bitrate`) |
| 10 | NACK | cliente a host | `frame_id` en la cabecera; payload: u16 por cada `packet_index` que falta (máx. 128). El host guarda los últimos 4096 paquetes y los reenvía al ritmo de `--pace`. La tablet reclama a ~2 RTT (mín. 4 ms), hasta 4 veces por paquete, y retiene un frame completo (15–60 ms según el RTT) mientras recupera uno anterior; si no llega, se descarta y se pide frame clave |
| 11 | TOUCH | cliente a host | estado táctil completo: u8 n, y por dedo u8 id, u16 x, u16 y (0–65535 sobre la pantalla capturada), u16 presión (0–1024). La tablet lo reenvía cada 100 ms mientras haya dedos y repite el «sin dedos» final; el host compara con el estado anterior para decidir qué dedos bajan, se mueven o suben, y los suelta tras 700 ms de silencio. Se inyecta con `InjectTouchInput` en el monitor principal; `--no-touch` lo desactiva |
| 12 | DISCOVER | cliente a host | vacío. El host responde STREAM_INFO al remitente sin tomarlo como destino del vídeo (búsqueda de PCs). Un host antiguo lo ignora: la tablet manda HELLO si nadie responde en 1 s |
| 8 | BURST_DATA | host a cliente | relleno; `packet_index`/`packet_count` numeran la ráfaga y el timestamp devuelve el nonce |

## Configuración de la tablet (payload de HELLO)

14 bytes, little-endian. 0 en un campo = lo que diga el host (sus opciones de arranque).

| Bytes | Campo | Significado |
| --- | --- | --- |
| 0–1 | width | ancho pedido para el monitor virtual (la tablet en horizontal, ya escalado por el ajuste «Resolución») |
| 2–3 | height | alto |
| 4–5 | fps | fotogramas por segundo |
| 6–7 | bitrate_mbps | tasa máxima |
| 8 | codec | 0 el del host, 1 H.264, 2 HEVC |
| 9 | reservado | 0 |
| 10–11 | density_dpi | informativo: densidad de la pantalla |
| 12–13 | refresh_hz | informativo: frecuencia máxima de la pantalla |

Campos nuevos se añaden al final: el host ignora los bytes que no conoce y toma como 0 los que falten. La tablet manda la misma configuración en cada HELLO; el host solo actúa cuando cambia. Entonces deja de responder a los HELLO, rehace el pipeline (y, con `--virtual`, vuelve a crear el monitor con el tamaño pedido salvo que se arrancara con `--size`) y manda el STREAM_INFO nuevo. fps, tasa y códec de la tablet mandan sobre `--fps`, `--bitrate` y `--codec` cuando no son 0.

## Flujo

1. El cliente envía HELLO (con su configuración) cada segundo hasta recibir STREAM_INFO.
2. El host responde STREAM_INFO, fuerza un frame clave y empieza a enviar VIDEO a la dirección de origen del HELLO.
3. El cliente reensambla. Sin colas: al completarse un frame se descartan los anteriores incompletos.
4. Si se pierde un frame (o hay un hueco en `frame_id`), el cliente pide KEYFRAME_REQUEST y no decodifica frames hasta recibir uno clave.
5. El cliente envía PING cada segundo; el RTT es la diferencia entre el reloj local y el timestamp devuelto.
   Con `--status` (la app de bandeja) el host también envía un PING por segundo al cliente para mostrar el RTT en el PC;
   el cliente que lo conteste con PONG (mismo timestamp) lo hace visible. Un cliente que no lo conteste sigue funcionando.

## Pendiente (no está en la fase 1)

- FEC para pérdidas sueltas sin esperar a un frame clave.
- Cifrado AES-GCM y emparejamiento por QR. **Ahora el tráfico va en claro y el host acepta a cualquier cliente de la red**: usar sólo en redes de confianza.
- Canal de entrada (toque y lápiz) y control de tasa adaptativo.
