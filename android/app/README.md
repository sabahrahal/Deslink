# android/app

App Android de DeskLink (Kotlin + Jetpack Compose, minSdk 26). Ver `docs/fase-1.md` y `docs/design.md`.

```
cd android
./gradlew :app:testDebugUnitTest :app:assembleDebug
```

La APK queda en `app/build/outputs/apk/debug/app-debug.apk`. Requiere JDK 17 y el SDK de Android (compileSdk 34).

## Pantallas

- **Inicio** (`MainActivity`, `ui/HomeScreen.kt`): busca PCs solo mientras está a la vista (cable USB y red, con
  DISCOVER), los lista con su nombre y conecta con un toque. Recuerda el último PC y, al abrir la app, se conecta
  solo a él si lo encuentra (se puede cancelar o desactivar en Ajustes).
- **Ajustes** (`ui/SettingsScreen.kt`): resolución del monitor, calidad, fluidez, códec, estadísticas, pantalla
  encendida y conexión automática. Se guardan en `SettingsStore`; los de imagen viajan al PC en el HELLO.
- **Sesión** (`StreamActivity`): vídeo a pantalla completa (también bajo la cámara), capa de estado con
  reintentar/volver, atrás dos veces para desconectar, cuatro dedos para ver u ocultar las estadísticas.
