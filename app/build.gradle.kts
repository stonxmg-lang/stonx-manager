plugins {
    id("com.android.application")
    id("com.google.gms.google-services")
}

android {
    namespace  = "com.stonx.manager"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.stonx.manager"
        minSdk        = 31
        targetSdk     = 35
        versionCode   = 1
        versionName   = "1.0.0"
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        externalNativeBuild {
            cmake {
                cppFlags  += "-std=c++17 -O2 -fvisibility=hidden"
                arguments += listOf("-DANDROID_PLATFORM=android-31", "-DANDROID_STL=c++_shared")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path    = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        debug   { isDebuggable = true }
        release {
            isMinifyEnabled   = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    implementation(platform("com.google.firebase:firebase-bom:33.3.0"))
    implementation("com.google.firebase:firebase-database")
}
