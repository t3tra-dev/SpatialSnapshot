#!/usr/bin/env python3
"""Build the native core and an installable, platform-specific Blender extension ZIP."""

import argparse
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / ".build/b3d")
    args = parser.parse_args()
    machine = platform.machine().lower()
    architecture = "arm64" if machine in ("aarch64", "arm64") else "x64" if machine in ("amd64", "x86_64") else None
    system = {"darwin": "macos", "linux": "linux", "win32": "windows"}.get(sys.platform)
    if not architecture or not system:
        parser.error("Unsupported OS / architecture")
    output, native = args.output.resolve(), ROOT / ".build/b3d/native"
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(native), "-DCMAKE_BUILD_TYPE=Release",
                    "-DBUILD_SHARED_LIBS=ON", "-DBUILD_TESTING=OFF", "-DSS_BUILD_TOOLS=OFF"], check=True)
    subprocess.run(["cmake", "--build", str(native), "--config", "Release", "--parallel"], check=True)
    filename = {"darwin": "libspatialsnapshot.dylib", "win32": "spatialsnapshot.dll"}.get(sys.platform, "libspatialsnapshot.so")
    library = native / filename
    if not library.exists():
        library = native / "Release" / filename
    if not library.exists():
        raise SystemExit(f"Native library not found: {library}")
    with tempfile.TemporaryDirectory(prefix="b3d-package-") as temporary:
        stage = Path(temporary)
        addon = ROOT / "Apps/SpatialSnapshotB3d"
        for file in addon.iterdir():
            if file.is_file() and (file.suffix in (".py", ".toml", ".md") or file.name == "LICENSE"):
                shutil.copy2(file, stage / file.name)
        manifest = stage / "blender_manifest.toml"
        text = manifest.read_text().replace('[permissions]', f'platforms = ["{system}-{architecture}"]\n\n[permissions]')
        manifest.write_text(text)
        (stage / "lib").mkdir()
        shutil.copy2(library.resolve(), stage / "lib" / filename)
        shutil.copy2(ROOT / "LICENSE", stage / "LICENSE-SpatialSnapshot.txt")
        # Ship the exact C source used for this binary, independently of Git hosting.
        source = stage / "native-source"
        shutil.copytree(ROOT / "Sources/SpatialSnapshotC", source / "Sources/SpatialSnapshotC")
        shutil.copytree(ROOT / "cmake", source / "cmake")
        shutil.copytree(ROOT / "Tools", source / "Tools")
        for file in ("LICENSE", "CMakeLists.txt"):
            shutil.copy2(ROOT / file, source / file)
        archive = output / f"spatial_snapshot-0.1.0-{system}-{architecture}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            for file in sorted(stage.rglob("*")):
                if file.is_file():
                    bundle.write(file, file.relative_to(stage))
    print(archive)


if __name__ == "__main__":
    main()
