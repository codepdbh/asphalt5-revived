import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]


def hash_stream(stream):
    return hashlib.file_digest(stream, "sha256").hexdigest()


def inventory_file(path, root):
    with path.open("rb") as stream:
        header = stream.read(16)
        stream.seek(0)
        return dict(path=path.relative_to(root).as_posix(), size=path.stat().st_size,
                    header_hex=header.hex(), sha256=hash_stream(stream))


def extract_apk(apk, destination):
    with ZipFile(apk) as archive:
        entries = archive.infolist()
        seen = set()
        for info in entries:
            name = info.filename.rstrip("/")
            parts = name.split("/")
            if any(not re.fullmatch(r"[^\\/:]+", part) or part in {".", ".."}
                   or part.rstrip(" .") != part or PurePosixPath(part).is_reserved()
                   for part in parts):
                raise ValueError(f"Unsafe APK member {info.filename!r}")
            key = name.casefold()
            if key in seen:
                raise ValueError(f"Duplicate APK member {name!r}")
            seen.add(key)
        for info in entries:
            path = destination / info.filename
            if info.is_dir():
                path.mkdir(parents=True, exist_ok=True)
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as source, path.open("xb") as output:
                shutil.copyfileobj(source, output)


def run_tool(tool, *args, check=True):
    command = [str(tool), *map(str, args)]
    if tool.suffix.lower() == ".bat":
        command = ["cmd.exe", "/d", "/c", *command]
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", check=check)
    return result.returncode, result.stdout + result.stderr


def write_json(path, payload):
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, default=ROOT / "a5")
    parser.add_argument("--sdk", type=Path, default=os.environ.get("ANDROID_HOME") or
                        os.environ.get("ANDROID_SDK_ROOT") or
                        Path(os.environ.get("LOCALAPPDATA", ".")) / "Android/Sdk")
    parser.add_argument("--build-tools", default="36.0.0")
    parser.add_argument("--ndk", default="28.2.13676358")
    args = parser.parse_args()
    input_root = args.input.resolve()
    outputs = [ROOT / name for name in ("workspace", "research")]
    for path in outputs:
        if path.resolve().is_relative_to(input_root) or input_root.is_relative_to(path.resolve()):
            parser.error("Input and output directories must be disjoint")
        path.mkdir(exist_ok=True)
    paths = sorted(path for path in input_root.rglob("*") if path.is_file())
    apks = [path for path in paths if path.suffix.lower() == ".apk"]
    if len(apks) != 1:
        parser.error(f"Expected one APK, found {len(apks)}")
    apk = apks[0]
    sdk_tools = args.sdk / "build-tools" / args.build_tools
    toolchains = args.sdk / "ndk" / args.ndk / "toolchains/llvm/prebuilt"
    llvm = next(path / "bin" for path in toolchains.iterdir() if path.is_dir())
    exe = ".exe" if os.name == "nt" else ""
    (_, badging) = run_tool(sdk_tools / ("aapt" + exe), "dump", "badging", apk)
    (_, manifest) = run_tool(sdk_tools / ("aapt" + exe), "dump", "xmltree", apk, "AndroidManifest.xml")
    signature_code, signature = run_tool(
        sdk_tools / ("apksigner.bat" if exe else "apksigner"),
        "verify", "--verbose", "--print-certs", apk, check=False)
    package = re.search(r"^package: name='([^']+)' versionCode='([^']+)' versionName='([^']+)'",
                        badging, re.M)
    if package is None:
        raise ValueError("Missing package metadata")
    assets = [inventory_file(path, input_root) for path in paths if path != apk]
    apk_entry = inventory_file(apk, input_root)
    work = Path(tempfile.mkdtemp(prefix="audit-", dir=outputs[0]))
    unpacked = work / "apk"
    extract_apk(apk, unpacked)
    for filename, report in (("badging.txt", badging), ("manifest.txt", manifest),
                             ("signature.txt", signature)):
        (work / filename).write_text(report, encoding="utf-8")

    import lief

    libraries = []
    for library in sorted(unpacked.rglob("*.so")):
        binary = lief.ELF.parse(str(library))
        if binary is None:
            raise ValueError(f"Cannot read {library}")
        entry = inventory_file(library, unpacked)
        entry.update(
            machine=binary.header.machine_type.name,
            elf_class=binary.header.identity_class.name,
            needed=binary.libraries,
            imports=sorted({item.name for item in binary.dynamic_symbols if
                            item.shndx == 0 and item.name}),
            exports=sorted({item.name for item in binary.exported_symbols}),
            relocations=sorted({item.type.name for item in binary.relocations}),
        )
        libraries.append(entry)
        _, dump = run_tool(llvm / ("llvm-readelf" + exe), "-h", "-l", "-d", "-s", "-r", "-A",
                           "--unwind", library)
        (work / (library.name + ".readelf.txt")).write_text(dump, encoding="utf-8")

    natives = []
    for dex in sorted(unpacked.glob("classes*.dex")):
        _, dump = run_tool(sdk_tools / ("dexdump" + exe), "-d", dex)
        (work / (dex.name + ".txt")).write_text(dump, encoding="utf-8")
        for block in re.split(r"^Class #", dump, flags=re.M)[1:]:
            owner = re.search(r"Class descriptor\s*:\s*'([^']+)'", block)
            if not owner:
                continue
            for match in re.finditer(
                r"name\s*:\s*'([^']+)'\s*\n\s*type\s*:\s*'([^']+)'"
                r"\s*\n\s*access\s*:\s*[^\n]*\bNATIVE\b",
                block,
            ):
                natives.append(dict(owner=owner[1], name=match[1], descriptor=match[2], dex=dex.name))

    profile = dict(zip(("package", "version_code", "version_name"), package.groups()))
    profile.update(apk_sha256=apk_entry["sha256"], engine_candidates=libraries,
                   signature_verified=signature_code == 0, input_directory=input_root.name)
    outputs = outputs[1]
    for name, payload in {
        "apk_inventory": dict(apk=apk_entry, entries=[
            inventory_file(path, unpacked) for path in sorted(unpacked.rglob("*")) if path.is_file()
        ]),
        "native_symbols": libraries,
        "jni_methods": natives,
        "asset_manifest": assets,
        "game_profile": profile,
    }.items():
        write_json(outputs / (name + ".json"), payload)
    print(f"{profile['package']} {profile['version_name']}: {len(assets)} external files, "
          f"{len(libraries)} native libraries, {len(natives)} DEX native declarations")
    print(work.relative_to(ROOT))


if __name__ == "__main__":
    main()
