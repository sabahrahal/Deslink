# windows/driver

Monitor virtual de DeskLink: un driver de pantalla indirecta (IddCx, modo usuario / UMDF) que hace que Windows vea la tablet como una pantalla más, para extender el escritorio en vez de duplicarlo.

| Carpeta | Contenido |
| --- | --- |
| `idd/` | El driver (`desklink-idd.dll` + `.inf`). Se compila con MSBuild y el WDK. |
| `setup/` | `desklink-driver.exe instalar \| desinstalar` y los `.bat` que lo lanzan. |
| `include/` | Interfaz compartida con el host (GUID, IOCTL). |

## Cómo funciona

1. `desklink-driver instalar` crea un dispositivo `Root\DeskLinkIdd` y le instala el driver. El dispositivo no muestra ningún monitor por sí solo.
2. `desklink-host --virtual` abre el driver y le pide un monitor del tamaño de la tablet (`IOCTL_DESKLINK_PLUG`). El driver lo crea con un EDID propio llamado «DeskLink».
3. El host busca ese monitor por su nombre, espera a que Windows lo active en modo «extender» y lo captura con Desktop Duplication, igual que un monitor físico.
4. Mientras corre, el host avisa al driver cada segundo. Si el host se cierra (o se cuelga), el driver quita el monitor a los 5 s: no queda una pantalla fantasma.

El driver no copia ni procesa las imágenes: las devuelve al sistema según llegan, así que su coste en GPU es casi nulo.

## Firma

Windows sólo instala drivers firmados. Como es un driver de **modo usuario**, no hace falta el modo de prueba ni desactivar Secure Boot: basta con que el equipo confíe en el certificado que lo firma.

- La CI crea un certificado nuevo en cada compilación, firma el driver y lo destruye. La clave privada no se puede exportar y muere con la máquina de la CI, así que nadie puede usarla para firmar otra cosa.
- El paquete trae la parte pública (`desklink-idd.cer`). `instalar` la añade a «Entidades de certificación raíz de confianza» y «Editores de confianza» del equipo; `desinstalar` la quita.
- Cada compilación lleva un certificado distinto. El instalador de DeskLink (`windows/installer/`) sólo reinstala el driver cuando cambia su código; con el paquete suelto hay que volver a ejecutar `instalar-driver.bat` del paquete nuevo (sustituye el certificado anterior).

Para distribuirlo a otras personas hará falta firmarlo con un certificado de Microsoft (fase 6, instalador firmado).

## Instalar y probar

Ver [docs/fase-3.md](../../docs/fase-3.md).
