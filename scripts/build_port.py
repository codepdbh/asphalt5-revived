"""Build the ARM64 Android port of Asphalt 5 HD 3.4.1.

Pipeline (incremental, cached under workspace/build):
  1. Verify the reference translator revision and the audited engine SHA-256.
  2. Translate libasphalt5.so (ARM32) to C once per engine/translator version
     and apply the CMatrix scalar replacement from the reference port.
  3. Compile changed units with the NDK for aarch64-linux-android31.
  4. Link libasphalt5.so (app) or the standalone boot probe (--boot-probe).
  5. Package, sign and optionally install the APK, push game data and launch.
"""
import argparse
import hashlib
import importlib.util
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from zipfile import ZIP_STORED, ZipFile

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "references/Asphalt5-Native"
REVISION = "f977989fa74faeb2442831f1241b4943d5459322"
ENGINE_SHA256 = "2966c986c73e71e7d2fa791a2f3a371749c7db183d7c40ff6641a57f01b78206"
PACKAGE = "com.androidevolved.asphalt5"
DEVICE_DATA_PARENT = "/sdcard/gameloft/games"   # data folder: <parent>/asphalt5
BUILD = ROOT / "workspace/build"
APP = ROOT / "app/src/main"
GUEST_PATCH = ROOT / "patches/asphalt5-native-android.patch"

GENERATED_FLAGS = ["-O1", "-fwrapv", "-fno-strict-aliasing", "-w"]
RUNTIME_FLAGS = ["-O2", "-fwrapv", "-fno-strict-aliasing", "-Wall", "-Wextra",
                 "-Wno-unused-parameter", "-Wno-sign-compare"]


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(*paths):
    digest = hashlib.sha256()
    for path in paths:
        digest.update(Path(path).read_bytes())
    return digest.hexdigest()


def run(command, **kwargs):
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


def load_engine(apk):
    with ZipFile(apk) as archive:
        engine = archive.read("lib/armeabi/libasphalt5.so")
    if sha256(engine) != ENGINE_SHA256:
        raise SystemExit("The engine SHA-256 does not match the audited 3.4.1 library")
    return engine


def check_reference():
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REFERENCE, text=True).strip()
    if revision != REVISION:
        raise SystemExit(f"Expected references/Asphalt5-Native at {REVISION}, found {revision}")


def apply_cmatrix_patch(generated):
    """CMatrix::Mult/PreMult/SetMult/GetQuaternion use the VFPv2 short-vector
    form; the reference replaces them with exact scalar host code."""
    spec = importlib.util.spec_from_file_location(
        "a5_runtime_patches", REFERENCE / "tools/apply_runtime_patches.py")
    patches = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(patches)
    targets = dict(patches.VERSIONS["3.4.1"]["cmatrix"])
    for path in sorted(generated.glob("functions_*.c")):
        source = path.read_text(encoding="utf-8")
        changed = False
        for function, op in list(targets.items()):
            source, did = patches.patch_cmatrix(source, function, op)
            if did is not None:
                targets.pop(function)
                changed = changed or did
        if changed:
            path.write_text(source, encoding="utf-8", newline="\n")
    if targets:
        raise SystemExit("CMatrix patch targets not found: " + ", ".join(sorted(targets)))


def translate(engine):
    """Returns the cached generated/ directory for this engine and translator."""
    key = sha256(b"".join([
        engine, REVISION.encode(),
        (REFERENCE / "tools/armrecomp.py").read_bytes(),
        (REFERENCE / "tools/apply_runtime_patches.py").read_bytes(),
        b"cmatrix-v1",
    ]))[:16]
    generated = BUILD / f"generated-{key}"
    if (generated / ".complete").is_file():
        return generated
    BUILD.mkdir(parents=True, exist_ok=True)
    for stale in BUILD.glob("generated-*"):
        shutil.rmtree(stale, ignore_errors=True)
    library = BUILD / "libasphalt5-armeabi.so"
    library.write_bytes(engine)
    work = Path(tempfile.mkdtemp(prefix="generated-tmp-", dir=BUILD))
    print("translating ARM32 engine to C (one-time)...", flush=True)
    run([sys.executable, "tools/armrecomp.py", "--so", library, "--out", work], cwd=REFERENCE)
    apply_cmatrix_patch(work)
    (work / ".complete").write_text(key)
    work.rename(generated)
    return generated


def patched_guest(source_dir):
    guest = source_dir / "guest.c"
    stamp = source_dir / "guest.c.key"
    key = file_hash(REFERENCE / "src/core/guest.c", GUEST_PATCH)
    if guest.is_file() and stamp.is_file() and stamp.read_text() == key:
        return guest
    source_dir.mkdir(parents=True, exist_ok=True)
    guest.write_bytes((REFERENCE / "src/core/guest.c").read_bytes().replace(b"\r\n", b"\n"))
    run(["git", "apply", "--no-index", "--unsafe-paths", GUEST_PATCH], cwd=source_dir)
    stamp.write_text(key)
    return guest


def toolchain(ndk):
    prebuilt = Path(ndk) / "toolchains/llvm/prebuilt"
    bins = [path / "bin" for path in prebuilt.iterdir() if (path / "bin").is_dir()]
    if len(bins) != 1:
        raise SystemExit(f"Cannot find the NDK LLVM toolchain under {prebuilt}")
    suffix = ".exe" if os.name == "nt" else ""
    return bins[0] / f"clang{suffix}", bins[0] / f"clang++{suffix}"


def compile_all(clang, clangxx, units, include_dirs, obj_dir, jobs):
    obj_dir.mkdir(parents=True, exist_ok=True)
    target = ["--target=aarch64-linux-android31", "-fPIC", "-D_GNU_SOURCE", "-g0"]
    includes = [f"-I{d}" for d in include_dirs]

    def compile_unit(item):
        source, flags = item
        obj = obj_dir / (source.stem + ".o")
        signature = obj.with_suffix(".flags")
        command_flags = " ".join(flags)
        if (obj.is_file() and signature.is_file() and signature.read_text() == command_flags
                and obj.stat().st_mtime >= source.stat().st_mtime
                and obj.stat().st_mtime >= max(h.stat().st_mtime for h in headers)):
            return obj, False
        compiler = clangxx if source.suffix == ".cpp" else clang
        language = ["-std=c++17"] if source.suffix == ".cpp" else ["-std=c11"]
        run([compiler, *target, *language, *flags, *includes, "-c", source, "-o", obj])
        signature.write_text(command_flags)
        return obj, True

    headers = [p for d in include_dirs for p in Path(d).glob("*.h")]
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(compile_unit, units))
    rebuilt = sum(1 for _, changed in results if changed)
    print(f"compiled {rebuilt} of {len(results)} units", flush=True)
    return [obj for obj, _ in results]


def build_native(args, engine):
    check_reference()
    generated = translate(engine)
    guest = patched_guest(BUILD / "src")
    clang, clangxx = toolchain(args.ndk)
    native = ROOT / "native"
    runtime = [guest, *(native / name for name in (
        "imports_android.c", "net_android.c", "gles_bridge.c", "guest_jni.c", "game_host.c", "texture_upload.cpp")),
        ROOT / "third_party/powervr/PVRTDecompress.cpp"]
    if args.boot_probe:
        runtime.append(native / "boot_probe.c")
    units = [(path, RUNTIME_FLAGS) for path in runtime]
    units += [(path, GENERATED_FLAGS) for path in sorted(generated.glob("*.c"))]
    includes = [REFERENCE / "src/core", generated, native, ROOT / "third_party/powervr"]
    objects = compile_all(clang, clangxx, units, includes, BUILD / "obj", args.jobs)
    output = BUILD / ("asphalt5-boot-probe" if args.boot_probe else "libasphalt5.so")
    link = [clangxx, "--target=aarch64-linux-android31", *objects, "-static-libstdc++",
            "-Wl,--no-undefined", "-Wl,-z,max-page-size=16384", "-Wl,--gc-sections",
            "-lEGL", "-lGLESv1_CM", "-llog", "-lm", "-o", output]
    if not args.boot_probe:
        link.insert(2, "-shared")
    run(link)
    header = output.read_bytes()[:20]
    if header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != 183:
        raise SystemExit("Link did not produce a little-endian AArch64 ELF64")
    print(output, flush=True)
    return output


def package(args, library, engine):
    sdk = Path(args.sdk).resolve()
    build_tools = sdk / "build-tools" / args.build_tools
    android_jar = sdk / "platforms/android-36/android.jar"
    suffix = ".exe" if os.name == "nt" else ""
    java_home = os.environ.get("JAVA_HOME")
    java_bin = (lambda name: str(Path(java_home) / "bin" / (name + suffix))) if java_home else (lambda name: name)
    work = BUILD / "apk"
    shutil.rmtree(work, ignore_errors=True)
    classes = work / "classes"
    classes.mkdir(parents=True)
    run([java_bin("javac"), "--release", "11", "-Xlint:-options", "-classpath", android_jar,
         "-d", classes, *sorted((APP / "java").rglob("*.java"))])
    run([java_bin("java"), "-cp", build_tools / "lib/d8.jar", "com.android.tools.r8.D8",
         "--min-api", "31", "--lib", android_jar, "--output", work,
         *sorted(classes.rglob("*.class"))])
    unsigned, aligned = work / "unsigned.apk", work / "aligned.apk"
    signed = BUILD / "asphalt5-arm64.apk"
    resources = work / "res.zip"
    run([build_tools / f"aapt2{suffix}", "compile", "--dir", APP / "res", "-o", resources])
    run([build_tools / f"aapt2{suffix}", "link", "-I", android_jar, "--manifest",
         APP / "AndroidManifest.xml", "-R", resources, "--auto-add-overlay", "-o", unsigned])
    with ZipFile(unsigned, "a", compression=ZIP_STORED) as archive:
        archive.write(work / "classes.dex", "classes.dex")
        archive.writestr("assets/engine.bin", engine)
        archive.write(library, "lib/arm64-v8a/libasphalt5.so")
    run([build_tools / f"zipalign{suffix}", "-f", "-P", "16", "4", unsigned, aligned])
    keystore = ROOT / "workspace/native-probe-debug.jks"
    if not keystore.exists():
        run([java_bin("keytool"), "-genkeypair", "-keystore", keystore, "-storepass", "android",
             "-keypass", "android", "-alias", "androiddebugkey", "-dname", "CN=Asphalt5 Local Debug",
             "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000"])
    signer = [java_bin("java"), "-jar", build_tools / "lib/apksigner.jar"]
    run([*signer, "sign", "--ks", keystore, "--ks-key-alias", "androiddebugkey",
         "--ks-pass", "pass:android", "--key-pass", "pass:android", "--out", signed, aligned])
    run([*signer, "verify", signed])
    print(signed, flush=True)
    return signed


def adb_command(args):
    return [args.adb, *(["-s", args.serial] if args.serial else [])]


def deploy(args, apk):
    adb = adb_command(args)
    run([*adb, "install", "--no-incremental", "-r", apk])
    # Data lives on shared internal storage; grant "All files access" for testing
    # (on a phone the app asks for it on first launch).
    run([*adb, "shell", "appops", "set", "--uid", PACKAGE, "MANAGE_EXTERNAL_STORAGE", "allow"])
    if args.push_data:
        run([*adb, "shell", "mkdir", "-p", DEVICE_DATA_PARENT])
        run([*adb, "push", "--sync", Path(args.push_data), f"{DEVICE_DATA_PARENT}/"])
    if args.launch:
        run([*adb, "logcat", "-c"])
        run([*adb, "shell", "am", "start", "-S", "-W", "-n", f"{PACKAGE}/.GameActivity"])


def run_boot_probe(args, probe, engine):
    adb = adb_command(args)
    remote = "/data/local/tmp/asphalt5-boot"
    library = BUILD / "libasphalt5-armeabi.so"
    library.write_bytes(engine)
    run([*adb, "shell", "mkdir", "-p", remote])
    try:
        run([*adb, "push", probe, f"{remote}/probe"])
        run([*adb, "push", library, f"{remote}/engine.so"])
        run([*adb, "shell", "chmod", "700", f"{remote}/probe"])
        run([*adb, "shell", f"{remote}/probe", f"{remote}/engine.so"])
    finally:
        run([*adb, "shell", "rm", "-rf", remote])


def main():
    sdk_default = Path(os.environ.get("LOCALAPPDATA", "")) / "Android/Sdk"
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("apk", type=Path, help="Original Asphalt 5 HD 3.4.1 APK")
    parser.add_argument("--ndk", type=Path, default=sdk_default / "ndk/28.2.13676358")
    parser.add_argument("--sdk", type=Path, default=sdk_default)
    parser.add_argument("--build-tools", default="36.0.0")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial")
    parser.add_argument("--boot-probe", action="store_true",
                        help="Build the standalone engine boot probe and run it over ADB")
    parser.add_argument("--install", action="store_true", help="Install the APK over ADB")
    parser.add_argument("--push-data", type=Path, nargs="?", const=ROOT / "a5/datos/asphalt5",
                        help="Push the game data folder to /sdcard/gameloft/games/asphalt5")
    parser.add_argument("--launch", action="store_true", help="Start the game after installing")
    args = parser.parse_args()
    if (args.push_data or args.launch) and not args.install:
        parser.error("--push-data and --launch require --install")
    if args.push_data and not (args.push_data / "gamecfg.bar").is_file():
        parser.error(f"{args.push_data} does not contain gamecfg.bar")

    engine = load_engine(args.apk)
    output = build_native(args, engine)
    if args.boot_probe:
        run_boot_probe(args, output, engine)
        return
    apk = package(args, output, engine)
    if args.install:
        deploy(args, apk)


if __name__ == "__main__":
    main()
