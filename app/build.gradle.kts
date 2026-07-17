plugins {
    id 'com.android.application'
    id 'org.jetbrains.kotlin.android'
}

android {
    namespace 'com.chrispixel.ps2recompiler'
    compileSdk 34

    defaultConfig {
        applicationId "com.chrispixel.ps2recompiler"
        minSdk 26
        targetSdk 34
        versionCode 1
        versionName "0.1.0"

        ndk {
            abiFilters 'arm64-v8a', 'armeabi-v7a'
        }

        externalNativeBuild {
            cmake {
                cppFlags "-std=c++20 -O3 -march=armv8-a+simd -ffast-math -funroll-loops"
                arguments "-DANDROID_STL=c++_shared",
                          "-DANDROID_PLATFORM=android-26"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path "src/main/cpp/CMakeLists.txt"
            version "3.22.1"
        }
    }

    buildFeatures {
        viewBinding true
    }

    buildTypes {
        release {
            minifyEnabled false
            proguardFiles getDefaultProguardFile('proguard-android-optimize.txt')
        }
        debug {
            debuggable true
            jniDebuggable true
        }
    }

    compileOptions {
        sourceCompatibility JavaVersion.VERSION_11
        targetCompatibility JavaVersion.VERSION_11
    }

    kotlinOptions {
        jvmTarget = '11'
    }
}

dependencies {
    implementation 'androidx.core:core-ktx:1.12.0'
    implementation 'androidx.appcompat:appcompat:1.6.1'
    implementation 'com.google.android.material:material:1.12.0'
    implementation 'androidx.constraintlayout:constraintlayout:2.1.4'
    implementation 'androidx.recyclerview:recyclerview:1.3.2'
    implementation 'androidx.lifecycle:lifecycle-runtime-ktx:2.7.0'
    implementation 'androidx.activity:activity-ktx:1.8.2'
    implementation 'androidx.documentfile:documentfile:1.0.1'
}
