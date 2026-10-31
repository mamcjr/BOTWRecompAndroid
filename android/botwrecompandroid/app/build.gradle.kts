import java.util.Properties
import java.security.MessageDigest

plugins {
    id("com.android.application")
}

val repo: File = rootDir.resolve("../..").canonicalFile
val deps: File = repo.resolve("android/deps/out")  // android/deps/build_deps.sh
val gen = layout.buildDirectory.dir("generated/botwrecompandroid")
// The release key: outside the repository, in ~/.botwrecompandroid/keystore.properties (storeFile, storePassword, keyAlias,
// keyPassword); without it, release builds are signed with the debug key
val releaseKey: File = File(System.getProperty("user.home"), ".botwrecompandroid/keystore.properties")

android {
    namespace = "com.botwrecompandroid"
    compileSdk = 36
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "com.botwrecomp.android"
        minSdk = 30
        targetSdk = 36
        versionCode = 26
        versionName = "0.2.6-beta"
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += "-DBOTW_GEN_DIR=${gen.get().asFile.resolve("inc").absolutePath.replace('\\', '/')}"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.30.4"
        }
    }

    sourceSets {
        getByName("main") {
            java.srcDirs("src/main/java", deps.resolve("java"))  // (SDL's Java side, the same version as libSDL3.so)
            jniLibs.srcDirs(gen.get().asFile.resolve("jniLibs"))
            assets.srcDirs(gen.get().asFile.resolve("assets"))
            res.srcDirs("src/main/res", gen.get().asFile.resolve("res"))
        }
    }

    // aapt's default without "<dir>_*": the game code's builder needs Python's folders named _* (zipfile/_path, ...)
    androidResources {
        ignoreAssetsPattern = "!.svn:!.git:!.ds_store:!*.scc:.*:!CVS:!thumbs.db:!picasa.ini:!*~"
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true  // adrenotools loads its hooks from nativeLibraryDir
            pickFirsts += listOf("**/libSDL3.so", "**/libshaderc_shared.so", "**/libavcodec.so", "**/libavformat.so",
                                 "**/libavutil.so", "**/libswscale.so")
        }
    }

    signingConfigs {
        if (releaseKey.exists()) create("release") {
            val p = Properties().apply { releaseKey.inputStream().use { load(it) } }
            storeFile = file(p.getProperty("storeFile"))
            storePassword = p.getProperty("storePassword")
            keyAlias = p.getProperty("keyAlias")
            keyPassword = p.getProperty("keyPassword")
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release") ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// What the native build and the APK need: the SMAA shader as a C string (smaa_hlsl.inc),
// the prebuilt libraries, and the shipped resources (the start screen's font, the icon).
val prepareBotwRecompAndroid by tasks.registering {
    val smaa = repo.resolve("third_party/smaa/SMAA.hlsl")
    val font = repo.resolve("third_party/fonts/Baloo2/Baloo2-SemiBold.ttf")
    val icon = repo.resolve("assets/artwork/triforce-icon-v2.png")
    val background = repo.resolve("assets/artwork/great-plateau-background-v2.png")
    val touchSkin = repo.resolve("assets/artwork/sheikah-touch-atlas-v1.png")
    val libs = listOf("libSDL3.so", "libshaderc_shared.so", "libavcodec.so", "libavformat.so", "libavutil.so", "libswscale.so")
    inputs.files(smaa, font, icon, background, touchSkin)
    inputs.files(libs.map { deps.resolve("lib/$it") }.filter { it.exists() })
    if (deps.resolve("gen").exists()) inputs.dir(deps.resolve("gen"))  // (the game code's builder, below)
    inputs.files(listOf("tools/gen.py", "tools/lifter.py", "tools/analyze.py", "tools/nso.py", "tools/passes.py", "tools/patches.py",
                        "tools/simd_c.py", "tools/gamelib_entry.c", "runtime/cpu.h",
                        "runtime/simd.h", "runtime/simd_fp.h", "runtime/gamelib.h", "runtime/hle/hle_symbols.txt").map { repo.resolve(it) })
    val hotLists = repo.resolve("tools").listFiles { f -> f.name.matches(Regex("hot_functions_\\d+\\.txt")) }?.toList() ?: listOf()
    inputs.files(hotLists)
    outputs.dir(gen)
    doLast {
        val out = gen.get().asFile
        // CR dropped, non-ASCII bytes as '?', backslashes and quotes escaped, every line a "...\n" literal
        val bytes = smaa.readBytes().filter { it != '\r'.code.toByte() }.map { if (it < 0) '?'.code.toByte() else it }
        val lines = String(bytes.toByteArray(), Charsets.US_ASCII).split('\n').let { if (it.last().isEmpty()) it.dropLast(1) else it }
        val inc = lines.joinToString("") { "\"" + it.replace("\\", "\\\\").replace("\"", "\\\"") + "\\n\"\n" }
        out.resolve("inc").mkdirs()
        out.resolve("inc/smaa_hlsl.inc").writeText(inc)
        for (lib in libs)  // (FFmpeg's only once android/deps/build_ffmpeg.sh made them)
            if (deps.resolve("lib/$lib").exists()) deps.resolve("lib/$lib").copyTo(out.resolve("jniLibs/arm64-v8a/$lib"), overwrite = true)
        font.copyTo(out.resolve("assets/res/Baloo2-SemiBold.ttf"), overwrite = true)
        deps.resolve("fonts/DejaVuSans.ttf").copyTo(out.resolve("assets/res/DejaVuSans.ttf"), overwrite = true)
        icon.copyTo(out.resolve("assets/res/icon.png"), overwrite = true)
        background.copyTo(out.resolve("assets/res/launcher-background.png"), overwrite = true)
        touchSkin.copyTo(out.resolve("assets/res/sheikah-touch-atlas.png"), overwrite = true)
        icon.copyTo(out.resolve("res/mipmap-xxxhdpi/ic_launcher.png"), overwrite = true)
        // the game code's builder (docs/las/ANDROID-PORT.md, Fase 6; android/deps/build_gen_deps.sh): its programs and
        // libraries as native libraries (Android runs binaries only from nativeLibraryDir), the rest as assets/gen
        // (runtime/gamebuild.c; unpacked by BOTWRecompAndroidActivity)
        val g = deps.resolve("gen")
        if (g.resolve("zig/libzig.so").exists()) {
            val jni = out.resolve("jniLibs/arm64-v8a")
            for (f in listOf(g.resolve("zig/libzig.so"), g.resolve("python/libpymain.so"), g.resolve("python/libpython3.14.so")) +
                     (g.resolve("lib").listFiles() ?: arrayOf()).filter { it.name.endsWith(".so") })
                f.copyTo(jni.resolve(f.name), overwrite = true)
            val a = out.resolve("assets/gen")
            a.deleteRecursively()
            g.resolve("python/lib").copyRecursively(a.resolve("python/lib"), overwrite = true)
            g.resolve("site").copyRecursively(a.resolve("site"), overwrite = true)
            g.resolve("zig/lib").copyRecursively(a.resolve("zig/lib"), overwrite = true)
            for (n in listOf("gen.py", "lifter.py", "analyze.py", "nso.py", "passes.py", "patches.py", "simd_c.py"))
                repo.resolve("tools/$n").copyTo(a.resolve("tools/$n"), overwrite = true)
            for (n in listOf("cpu.h", "simd.h", "simd_fp.h", "gamelib.h"))
                repo.resolve("runtime/$n").copyTo(a.resolve("gamelib-src/$n"), overwrite = true)
            repo.resolve("tools/gamelib_entry.c").copyTo(a.resolve("gamelib-src/gamelib_entry.c"), overwrite = true)
            repo.resolve("runtime/hle/hle_symbols.txt").copyTo(a.resolve("gamelib-src/hle_symbols.txt"), overwrite = true)
            // (runtime/gamebuild.c hot / cold: only these functions at -O1, one list per game version; not part of the
            // game code's id)
            for (f in hotLists) f.copyTo(a.resolve("gamelib-src/${f.name}"), overwrite = true)
            val digest = MessageDigest.getInstance("SHA-256")
            val buffer = ByteArray(65536)
            for (f in a.walkTopDown().filter { it.isFile }.sortedBy { it.relativeTo(a).invariantSeparatorsPath }) {
                digest.update(f.relativeTo(a).invariantSeparatorsPath.toByteArray(Charsets.UTF_8))
                digest.update(0.toByte())
                digest.update(f.length().toString().toByteArray(Charsets.US_ASCII))
                digest.update(0.toByte())
                f.inputStream().use { input ->
                    while (true) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        digest.update(buffer, 0, n)
                    }
                }
            }
            out.resolve("assets/gen.id").writeText(digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) })
        }
    }
}
tasks.named("preBuild") { dependsOn(prepareBotwRecompAndroid) }
tasks.matching { it.name.startsWith("configureCMake") || it.name.startsWith("buildCMake") }.configureEach {
    dependsOn(prepareBotwRecompAndroid)
}
