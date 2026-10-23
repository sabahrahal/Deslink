# Asesor de latencia

Al conectar un dispositivo, Extendesk detecta el hardware de ambos lados, mide cada transporte disponible y aplica la configuración de menor latencia. Luego explica al usuario, sin tecnicismos, qué eligió y qué podría mejorar.

## Qué detecta y cómo decide

| Detección | Cómo | Regla de decisión |
| --- | --- | --- |
| Encoder del PC | Fabricante de la GPU y capacidades de NVENC, AMF o Quick Sync | Encoder hardware del fabricante; Media Foundation sólo como respaldo |
| Códecs del dispositivo | `MediaCodecList`: decoders hardware y soporte de baja latencia | HEVC si ambos lados lo tienen por hardware; si no, H.264. AV1 sólo si el banco de pruebas lo confirma más rápido |
| Pantalla del dispositivo | Resolución nativa y frecuencias soportadas | Monitor virtual a resolución nativa y la frecuencia más alta que el encoder sostenga (60/90/120/144 Hz) |
| Transportes disponibles | Cable USB, anclaje activo, red WiFi compartida | Prueba de 3 s en cada uno; gana el de menor latencia con pérdida < 1 % |
| Calidad de red y enlace | Banda WiFi, señal, velocidad del enlace USB | Bitrate inicial según el ancho de banda medido |
| Energía y temperatura | Panel de diagnóstico | Bajar fps antes que calidad si la batería o la temperatura lo exigen |

## Qué ve el usuario

Un indicador (Excelente, Buena, Mejorable) con la latencia medida y, si hay margen, una recomendación con su ganancia estimada, calculada con mediciones reales:

- "Conecta por cable USB: bajarás de 32 a 19 ms"
- "Tu router tiene 5 GHz: úsalo para ganar estabilidad"

## Orden esperado de transportes (de menor a mayor latencia)

USB por AOA, anclaje USB, WiFi 6 GHz, WiFi 5 GHz, WiFi 2,4 GHz.

## Ejemplo: equipo de referencia

RTX 4060 + Lenovo Idea Tab Pro: NVENC con HEVC, monitor virtual 2944x1840 empezando a 120 Hz, transporte USB y WiFi 6E a 6 GHz como alternativa.
