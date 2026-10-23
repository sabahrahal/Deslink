# android/keystore

Todas las APK de Extendesk (depuración y release, en local y en la CI) se firman con **una sola clave fija**. Antes cada
ejecución de la CI firmaba con una clave de depuración distinta, así que Android no dejaba instalar la APK nueva
encima de la vieja y había que desinstalar cada vez. Con una clave fija, `adb install -r` (o abrir la APK en la tablet)
actualiza la app y conserva sus ajustes.

De dónde sale la clave, por orden:

1. CI: el secreto `DESKLINK_KEYSTORE_B64` (el `.jks` en base64), con `DESKLINK_KEYSTORE_PASSWORD` y
   `DESKLINK_KEY_PASSWORD` si no son `desklink-dev`. En local: la variable `DESKLINK_KEYSTORE` (ruta al `.jks`).
2. `android/keystore/desklink-dev.jks`, si está en el repo (clave de desarrollo de este proyecto personal; contraseña
   `desklink-dev`, alias `desklink`).
3. Si no hay ninguna, la clave de depuración de cada máquina (la CI avisa: esas APK no se instalan encima).

Al pasar de una APK firmada con la clave de depuración antigua a una firmada con la fija hay que desinstalar **una
vez**.

## La clave de publicación (secretos de GitHub)

La CI firma con la clave guardada en tres secretos del repositorio (Settings → Secrets and variables → Actions):
`DESKLINK_KEYSTORE_B64` (el `.jks` en base64), `DESKLINK_KEYSTORE_PASSWORD` y `DESKLINK_KEY_PASSWORD`. El alias es
`desklink`. El `.jks` **no** está en el repositorio: la única copia segura es la del gestor de contraseñas de quien
mantiene el proyecto. Es también la clave de subida de Google Play (el `.aab` de cada versión sale firmado con ella);
si se pierde, Play obliga a pedir un cambio de clave de subida, que tarda días.

Para comprobar con qué clave se firmó una APK descargada: `keytool -printcert -jarfile Extendesk.apk` (la huella SHA-256
debe ser siempre la misma).
