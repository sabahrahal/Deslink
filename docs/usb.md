# USB y diagnóstico (fase 2)

## Conectar por USB con anclaje (funciona ya)
1. Conecta la tablet al PC con un cable USB-C **de datos** (mejor uno USB 3.x o Thunderbolt, no el de carga lenta).
2. En la tablet: Ajustes → Conexiones → Punto de acceso y anclaje → **Anclaje USB**.
3. En el PC aparece un adaptador de red nuevo. Mira su IPv4 con `ipconfig` (normalmente `192.168.42.x`).
4. Escribe esa IP en DeskLink. Es el mismo protocolo UDP, sin pasar por el router.

Con el campo de IP vacío, la app busca el PC sola (broadcast por todas las redes, incluido el anclaje USB). Para comparar con WiFi: pulsa «Diagnóstico y recomendaciones» con cada IP y compara la mediana y el p95.

## ADB reverse
`adb reverse` solo reenvía TCP; DLP va sobre UDP, así que no sirve tal cual. Queda para la fase 5 junto con AOA
(Android Open Accessory), que no necesita anclaje ni depuración USB.

## Qué mide el diagnóstico
- Prueba de conexión: 100 PING/PONG de DLP (uno cada 20 ms) → mediana, p95, variación y pérdida.
- Prueba de ráfaga: el host envía 400 paquetes seguidos (como un frame clave), de golpe y repartidos a 100 Mbps. Si de golpe
  se pierden y repartidos no, el enlace no tolera ráfagas: usa `desklink-host.exe --pace 100`. Si se pierden igualmente,
  baja la calidad con `--bitrate` (el diagnóstico sugiere valores).
- WiFi: banda (2,4/5/6 GHz), señal y velocidad de enlace.
- Batería: consumo neto desde `CHARGE_COUNTER` (el signo de `CURRENT_NOW` varía según fabricante), carga por USB/red,
  temperatura. Avisa si «está conectada pero se descarga».
- Temperatura del sistema (`PowerManager`, Android 10+).

Los umbrales están en `Advisor.kt` y se ajustarán con mediciones reales. La velocidad real del puerto USB del PC
(USB 2 vs 3) llega en una fase posterior, medida desde el host de Windows.

## Si por USB no lo encuentra
1. La tablet debe mostrar el anclaje USB activo (Ajustes → Conexiones → Punto de acceso y anclaje → Anclaje USB).
2. Al fallar, la app lista las redes que ve la tablet; con el anclaje activo debe aparecer una `USB (rndis0 …)`.
3. Windows suele clasificar el adaptador del anclaje como red **pública** y bloquea UDP entrante. En PowerShell como
   administrador: `netsh advfirewall firewall add rule name="DeskLink" dir=in action=allow protocol=UDP localport=47800 profile=any`.

## Opciones del host para enlaces difíciles
`desklink-host.exe --pace <Mbps>` reparte el envío de cada frame a esa tasa (por defecto 100; `--pace 0` envía de golpe) y `--bitrate <Mbps>` baja
la calidad. La consola muestra «errores de envío» si Windows rechaza paquetes.

## Desconexión
El host olvida al cliente si pasan 5 s sin recibir nada de él y vuelve a «esperando cliente»; la tablet reenvía HELLO
si deja de oír al host durante 3 s, así que un corte breve de red se recupera solo.
