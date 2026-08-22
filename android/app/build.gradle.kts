plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.miniclaw.app"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.miniclaw.app"
        minSdk = 24          // matches the NDK build (ANDROID_PLATFORM=android-24)
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    // Prebuilt native libs in src/main/jniLibs (libminiclaw_core.so,
    // libminiclaw_jni.so) are packaged as-is; no externalNativeBuild here.
    packaging {
        jniLibs.useLegacyPackaging = true
    }
}

dependencies {
    // Intentionally zero external dependencies: the UI uses framework widgets
    // (ListView, EditText) and the engine is reached via JNI only.
}

