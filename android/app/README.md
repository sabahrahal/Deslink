# android/app

App Android de DeskLink (Kotlin, sin dependencias externas, minSdk 26). Ver `docs/fase-1.md`.

```
cd android
./gradlew :app:testDebugUnitTest :app:assembleDebug
```

La APK queda en `app/build/outputs/apk/debug/app-debug.apk`. Requiere JDK 17 y el SDK de Android (compileSdk 34).
