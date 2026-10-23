# android/app

App Android de Extendesk (Kotlin + Jetpack Compose, minSdk 26). Ver `docs/fase-1.md` y `docs/design.md`.

```
cd android
./gradlew :app:testDebugUnitTest :app:assembleDebug
```

La APK queda en `app/build/outputs/apk/debug/app-debug.apk`. Requiere JDK 17 y el SDK de Android (compileSdk 34).

## Pantallas

- **Inicio** (`MainActivity`, `ui/HomeScreen.kt`): busca PCs solo mientras está a la vista (cable USB y red, con
  DISCOVER), los lista con su nombre y conecta con un toque. Recuerda el último PC y, al abrir la app, se conecta
  solo a él si lo encuentra (se puede cancelar o desactivar en Ajustes).
- **Ajustes** (`ui/SettingsScreen.kt`): el único sitio donde se configura Extendesk (ver [ajustes](../../docs/ajustes.md)).
  A la vista: perfil (Calidad, Equilibrado, Ahorro), sonido del PC y táctil (directo, ratón o solo ver). Plegado en
  «Avanzado»: resolución, calidad, fluidez, códec, adaptar a la carga, estadísticas, pantalla encendida y conexión
  automática. Se guardan en `SettingsStore` y se mandan al PC al conectar.
- **Sesión** (`StreamActivity`): vídeo a pantalla completa (también bajo la cámara), capa de estado con
  reintentar/volver, atrás dos veces para desconectar, botón ••• de la esquina (se puede arrastrar) para el menú rápido.
