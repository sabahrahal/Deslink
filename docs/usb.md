# USB y diagnóstico

## Conectar por cable USB (recomendado)
Va por `adb reverse`: la app abre una conexión TCP a `127.0.0.1:47800` en la tablet y adb la lleva por el cable hasta el
host, que escucha en `127.0.0.1:47800` (TCP) del PC. No pasa por ninguna red ni por el cortafuegos.

Una sola vez:
1. En la tablet, activa las **Opciones de desarrollador** (Ajustes → Acerca de la tablet → toca 7 veces «Número de
   compilación») y dentro activa **Depuración USB**. Se queda activada; no hay que repetirlo como el anclaje.
2. En el PC no hay que instalar nada más: el instalador de DeskLink y el paquete `desklink-host-windows` de la CI ya
   traen `adb.exe`, `AdbWinApi.dll` y `AdbWinUsbApi.dll` junto a `desklink-host.exe` (del paquete, descomprímelo entero
   en una carpeta). Si compilas el host tú, copia esos tres archivos de las
   [platform-tools de Android](https://developer.android.com/tools/releases/platform-tools) junto al .exe (o deja adb
   en el PATH; también se busca en `%LOCALAPPDATA%\Android\Sdk\platform-tools` y con `--adb ruta`).
3. Enchufa la tablet con un cable **de datos** y acepta «¿Permitir depuración USB?» marcando «Permitir siempre».

Cada vez: abre `desklink-host.exe` (dice «Tablet … conectada por USB (ADB)») y pulsa **Conectar** en la app con la IP
vacía. La app prueba primero el cable y, si no está, busca por WiFi. Si se desenchufa o se reinicia el host, la app
reconecta sola.

Si Windows no reconoce la tablet en `adb devices`, instala el driver USB del fabricante (o el «Google USB Driver»).
Si a los pocos segundos el host dice «no veo ninguna tablet», la depuración USB está apagada o el cable es solo de carga.

Solo puede haber un host abierto a la vez (los dos querrían el puerto 47800). Si abres otro, el anterior se cierra solo
y le deja el sitio; si el puerto lo tiene otro programa, el host dice cuál.

### Por qué así (y no Android Open Accessory)
- AOA no necesita depuración USB, pero en Windows obliga a cambiar el driver del dispositivo (WinUSB/Zadig) y a
  instalar otro para el modo accesorio, porque el driver MTP no deja mandar los comandos de cambio de modo.
- ADB ya trae su driver y por USB 3 mueve de sobra 100+ Mbps con ~1 ms de sobrecoste.
- Por cable no se pierden paquetes: el host envía cada frame en una sola escritura (sin `--pace`), empieza ya a la
  tasa máxima y la tablet no reclama paquetes (NACK); despierta cada 100 ms en vez de cada 4 ms (menos CPU y batería).

Formato: los mismos paquetes DLP, cada uno precedido de su tamaño (u16 little-endian). Un cliente a la vez; una
conexión nueva sustituye a la anterior. `--no-usb` lo desactiva.

## Conectar por USB con anclaje
1. Conecta la tablet al PC con un cable USB-C **de datos** (mejor uno USB 3.x o Thunderbolt, no el de carga lenta).
2. En la tablet: Ajustes → Conexiones → Punto de acceso y anclaje → **Anclaje USB**.
3. En el PC aparece un adaptador de red nuevo. Mira su IPv4 con `ipconfig` (normalmente `192.168.42.x`).
4. Escribe esa IP en DeskLink. Es el mismo protocolo UDP, sin pasar por el router.

Con el campo de IP vacío, la app busca el PC sola (broadcast por todas las redes, incluido el anclaje USB). Para comparar con WiFi: pulsa «Diagnóstico y recomendaciones» con cada IP y compara la mediana y el p95.

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
