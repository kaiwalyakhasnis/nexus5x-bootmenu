// The boot menu as an embedded-compose app, built from that repo with
// APP_DIR=<this directory> (scripts/build.sh does it): a static library whose
// only C export is ec_app_main(), linked with native/ (app.env: APP_NATIVE).
plugins {
    kotlin("multiplatform")
    id("org.jetbrains.kotlin.plugin.compose")
}

kotlin {
    linuxArm64 {
        compilations.getByName("main") {
            cinterops.create("sys") {
                definitionFile.set(project.file("src/nativeInterop/cinterop/sys.def"))
                includeDirs(project.file("native"))
            }
        }
        binaries.staticLib {
            baseName = "app"
        }
    }
    sourceSets {
        val linuxArm64Main by getting {
            dependencies {
                implementation(project(":core:ui"))
            }
        }
        all {
            languageSettings.optIn("kotlinx.cinterop.ExperimentalForeignApi")
        }
    }
}
