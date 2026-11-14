plugins {
    id("com.android.application")
}

android {
    namespace = "com.zbotw.recomp.probe"
    compileSdk = 36
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "com.zbotw.recomp.probe"
        minSdk = 31  // (Build.SOC_MODEL)
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.30.4"
        }
    }

    // adrenotools loads its hooks from nativeLibraryDir: the libraries must be extracted from the APK
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
