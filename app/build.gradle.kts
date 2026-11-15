// MCLA Recompiled Android — :app module (Helio G85 / Mali-G52 profile)
//
// Gradle never runs CMake for the game core. The native side (libmain.so,
// librexruntime.so, GPU plugins, libSDL3.so, libc++_shared.so) is built by
// scripts/build_native.sh, which stages libraries into src/main/jniLibs.
// This module only packages them and provides the Java launcher + SDLActivity.

plugins {
    id("com.android.application")
}

val sdlJavaDir = file("src/main/sdl_java")

android {
    namespace = "com.mcla.recomp"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.mcla.recomp"
        // 28: ASharedMemory (26) for guest memory and AAudio (27) for audio.
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    sourceSets["main"].java.srcDirs("src/main/java", sdlJavaDir)

    buildTypes {
        debug {
            isDebuggable = true
            isJniDebuggable = true
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs {
            // Unpack the libraries into nativeLibraryDir: the runtime finds its
            // GPU plugins by real path next to itself, and a Mali/system driver
            // loaded through the proxy needs real files as well.
            useLegacyPackaging = true
            keepDebugSymbols += "**/*.so"
        }
    }

    lint {
        checkReleaseBuilds = false
        abortOnError = false
    }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("com.google.android.material:material:1.11.0")
}
