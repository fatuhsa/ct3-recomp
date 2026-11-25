import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Pass -Pdebuggable on Release builds for unrooted simpleperf profiling with -O2 native code:
//   ./gradlew assembleRelease -Pdebuggable
val isProfileDebuggable = project.hasProperty("debuggable")

android {
    namespace = "com.ct2recomp.android"
    compileSdk = 35
    ndkVersion = "27.1.12297006"

    defaultConfig {
        applicationId = "com.ct2recomp.android"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_PLATFORM=android-29",
                    "-DCT2_ENABLE_PRELOAD=ON"
                )
            }
        }
    }

    buildTypes {
        debug {
            isDebuggable = true
            externalNativeBuild {
                cmake {
                    // Always keep -O2 Release optimization on native/recomp code even in debug APKs;
                    // gate debug verbosity via a dedicated flag instead.
                    arguments += listOf(
                        "-DCMAKE_BUILD_TYPE=Release",
                        "-DCT2_VERBOSE_LOG=ON"
                    )
                }
            }
        }
        release {
            isMinifyEnabled = false
            isDebuggable = isProfileDebuggable
            externalNativeBuild {
                cmake {
                    arguments += listOf(
                        "-DCMAKE_BUILD_TYPE=Release",
                        "-DCT2_VERBOSE_LOG=OFF"
                    )
                }
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            version = "3.22.1"
        }
    }

    packaging {
        jniLibs {
            // Keep .so uncompressed and 16 KB aligned inside APK for Android 15+ compatibility.
            useLegacyPackaging = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-ktx:1.9.3")
}
