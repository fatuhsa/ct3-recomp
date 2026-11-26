plugins {
    id("com.android.application")
}

// Pass -Pdebuggable on Release builds for unrooted simpleperf profiling with -O2 native code:
//   gradle -p android assembleRelease -Pdebuggable
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
                val hasSdkAndLifted =
                    file("../../sdk/runtime").exists() &&
                        file("../../generated/game/x86_func_mapping.cpp").exists()
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_PLATFORM=android-29",
                    "-DCT2_ENABLE_PRELOAD=ON",
                    "-DCT3_ENABLE_XBE_SDK=${if (hasSdkAndLifted) "ON" else "OFF"}"
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
                    // keep verbose disk logging off so hot loops stay stutter-free.
                    arguments += listOf(
                        "-DCMAKE_BUILD_TYPE=Release",
                        "-DCT2_VERBOSE_LOG=OFF"
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

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-ktx:1.9.3")
}
