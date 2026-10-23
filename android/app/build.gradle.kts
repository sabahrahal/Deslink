plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "app.extendesk"
    compileSdk = 36

    defaultConfig {
        applicationId = "app.extendesk"
        minSdk = 26
        targetSdk = 36
        // La CI pasa un número que siempre sube (el de la ejecución) para que cada APK nueva se instale encima
        // de la anterior. En local queda en 1.
        versionCode = System.getenv("EXTENDESK_VERSION_CODE")?.toIntOrNull() ?: 1
        versionName = System.getenv("EXTENDESK_VERSION_NAME") ?: "0.1.0"
    }

    // Una sola clave para todas las APK (depuración y release, en local y en la CI): así una APK nueva se instala
    // encima de la anterior sin desinstalar. Se toma de EXTENDESK_KEYSTORE (la CI la saca de un secreto) o de
    // android/keystore/desklink-dev.jks. Sin ninguna de las dos se usa la clave de depuración de cada máquina
    // (ver android/keystore/README.md). El archivo, su contraseña y el alias «desklink» conservan el nombre antiguo:
    // van dentro de la clave y cambiarlos obligaría a firmar con otra.
    val keystore = System.getenv("EXTENDESK_KEYSTORE")?.let { file(it) }
        ?: rootProject.file("keystore/desklink-dev.jks").takeIf { it.exists() }
    val extendeskSigning = keystore?.let { ks ->
        signingConfigs.create("extendesk") {
            storeFile = ks
            storePassword = System.getenv("EXTENDESK_KEYSTORE_PASSWORD") ?: "desklink-dev"
            keyAlias = System.getenv("EXTENDESK_KEY_ALIAS") ?: "desklink"
            keyPassword = System.getenv("EXTENDESK_KEY_PASSWORD") ?: "desklink-dev"
        }
    }

    buildTypes {
        debug {
            extendeskSigning?.let { signingConfig = it }
        }
        release {
            isMinifyEnabled = false
            // Sin clave propia, la release se firma con la de depuración para que siempre salga una APK instalable.
            signingConfig = extendeskSigning ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
    }
    testOptions {
        // Las pruebas de pantalla (Compose) corren en el ordenador con Robolectric, sin tablet.
        unitTests.isIncludeAndroidResources = true
    }
    composeOptions {
        // Debe ir a la par con la versión de Kotlin (1.9.24 -> 1.5.14).
        kotlinCompilerExtensionVersion = "1.5.14"
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.06.00")
    implementation(composeBom)
    implementation("androidx.activity:activity-compose:1.9.0")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.12.2")
    testImplementation("androidx.test.ext:junit:1.1.5")
    testImplementation(composeBom)
    testImplementation("androidx.compose.ui:ui-test-junit4")
    // La actividad vacía donde corren las pruebas de pantalla: solo en debug (las pruebas corren en esa variante).
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
