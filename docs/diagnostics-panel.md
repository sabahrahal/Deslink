# Panel de diagnóstico

Mide en vivo la batería, el enlace USB y el WiFi, y recomienda en lenguaje simple qué cambiar.

## Por qué la tablet se descarga aunque esté "cargando"

Un puerto USB normal del PC entrega unos 2,5 W (USB 2.0) o 4,5 W (USB 3.0). La tablet usada como pantalla consume más (estimado 5–9 W, a confirmar con medición). La carga neta es negativa. La Lenovo Idea Tab Pro admite 45 W, así que el límite es el puerto del PC.

## Qué mide

| Medida | Dónde | Cómo |
| --- | --- | --- |
| Potencia neta (W) | Tablet | `BatteryManager` (corriente y voltaje), calibrado con `CHARGE_COUNTER` |
| Tiempo hasta vacía o llena | Tablet | Promedio móvil sobre 10.200 mAh |
| Tipo de alimentación | Tablet | `EXTRA_PLUGGED`: USB, AC o ninguno |
| Margen térmico | Tablet | `PowerManager.getThermalHeadroom` |
| Velocidad del enlace USB | PC | `IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2` (detecta USB 3 conectado a 480 Mbps) |
| Ancho de banda y RTT | Ambos | Prueba de 3 s por transporte al conectar |
| Calidad del WiFi | Ambos | Banda, RSSI, velocidad del enlace, PC por cable o no |

## Qué recomienda

| Situación | Mensaje | Acción |
| --- | --- | --- |
| Potencia neta negativa por USB | "Este puerto no carga lo suficiente: tu tablet pierde un 12 % por hora" | Sugerir USB-C del PC, cargador de pared + WiFi, o modo ahorro |
| Enlace USB a 480 Mbps | "Estás en un puerto o cable USB 2.0; funciona, pero un puerto azul o USB-C es más estable" | Indicar qué puerto buscar |
| WiFi 2,4 GHz o señal débil | "Tu WiFi es lento para esto; acércate al router o usa 5 o 6 GHz" | Bajar bitrate automáticamente |
| Tablet caliente | "La tablet se está calentando; bajamos a 60 Hz para cuidarla" | Reducir fps y bitrate |
| Todo óptimo | "Conexión excelente: 18 ms de retardo" | Ninguna |

Los perfiles de batería y el modo «adaptar a la carga» por USB están en [battery.md](battery.md).
