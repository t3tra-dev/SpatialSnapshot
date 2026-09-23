#!/usr/bin/env python3
"""Audit checked-in fixtures with independent media decoders; preserve all fixture bytes.

Build Tests/Bindings/AppleMediaProbe.swift first. Apple video/Quick Look services require
execution outside a sandbox that denies their XPC connections. A valid H.264 control is
needed before interpreting Apple decoder failures as evidence about the files.
"""
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import argparse
import collections
import hashlib
import json
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command, timeout=8):
    try:
        p = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                           timeout=timeout, errors="replace")
        return {"exit_code": p.returncode, "stdout": p.stdout, "stderr": p.stderr}
    except subprocess.TimeoutExpired:
        return {"exit_code": None, "timeout": timeout, "stdout": "", "stderr": "timeout"}


def parsed(result):
    try:
        result["json"] = json.loads(result["stdout"])
    except (ValueError, TypeError):
        pass
    return result


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("--apple-probe", type=Path, required=True)
    args.add_argument("--validator", type=Path, default=ROOT / "build/ssvalidate")
    args.add_argument("--output", type=Path, default=ROOT / "build/fixture-audit/results.json")
    args.add_argument("--extra", type=Path, action="append", default=[])
    config = args.parse_args()
    fixture_root = ROOT / "Fixtures/bindings"
    manifest = json.loads((fixture_root / "manifest.json").read_text())
    paths = sorted(p for p in fixture_root.rglob("*") if p.suffix in (".heif", ".heic", ".mov", ".ssps"))
    paths += [p.resolve() for p in config.extra]

    def audit(path):
        relative = str(path.relative_to(ROOT))
        expected = manifest.get(str(path.relative_to(fixture_root))) if path.is_relative_to(fixture_root) else None
        data = path.read_bytes()
        row = {"file": relative, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
               "expected": expected}
        flags = [] if path.suffix == ".ssps" else ["--quicktime" if path.suffix == ".mov" else "--heif"]
        row["binding"] = parsed(run([config.validator, "--json", *flags, path]))
        observed = row["binding"].get("json", {})
        if expected:
            domains = {d["domain"] for d in observed.get("diagnostics", [])}
            row["expected_matches"] = observed.get("status") == expected["status"] and set(expected["domains"]) <= domains
        if path.suffix == ".ssps":
            row["media"] = "spatial stream; not image/video"
            return row
        mode = "video" if path.suffix == ".mov" else "image"
        row["apple"] = parsed(run([config.apple_probe, mode, path]))
        row["quicklook"] = parsed(run([config.apple_probe, "quicklook", path], timeout=5))
        if mode == "video":
            row["ffprobe"] = parsed(run(["ffprobe", "-v", "error", "-max_alloc", "33554432",
                "-read_intervals", "%+#64", "-show_error", "-show_streams", "-show_format",
                "-show_frames", "-of", "json", path]))
            probe = row["ffprobe"].get("json", {})
            decoded = [f for f in probe.get("frames", []) if f.get("media_type") == "video"]
            video = next((s for s in probe.get("streams", []) if s.get("codec_type") == "video"), {})
            declared = video.get("nb_frames")
            row["software_decode"] = {"frames": len(decoded), "declared_frames": declared,
                "decoded": row["ffprobe"]["exit_code"] == 0 and bool(decoded) and
                    (declared is None or int(declared) == len(decoded))}
            row["ffmpeg"] = run(["ffmpeg", "-nostdin", "-v", "error", "-xerror",
                "-err_detect", "explode", "-max_alloc", "33554432", "-threads", "1",
                "-i", path, "-map", "0:v:0", "-frames:v", "64", "-fps_mode", "passthrough",
                "-f", "framemd5", "-"])
            frames = [line for line in row["ffmpeg"]["stdout"].splitlines() if line and not line.startswith("#")]
            row["ffmpeg"]["frames"] = len(frames)
            # This also tests an output muxer. Non-monotonic output DTS is a pipeline
            # failure, not proof of an input codec failure; software_decode is separate.
            row["ffmpeg"]["pipeline_ok"] = row["ffmpeg"]["exit_code"] == 0 and bool(frames)
        else:
            row["heif_info"] = run(["heif-info", path])
            with tempfile.TemporaryDirectory(prefix="ss-fixture-decode-") as directory:
                image = Path(directory) / "decoded.png"
                row["libheif"] = run(["heif-convert", path, image])
                row["libheif"]["decoded"] = row["libheif"]["exit_code"] == 0 and image.is_file()
        return row

    rows = []
    with ThreadPoolExecutor(max_workers=4) as executor:
        futures = [executor.submit(audit, path) for path in paths]
        for future in as_completed(futures):
            rows.append(future.result())
            if len(rows) % 20 == 0 or len(rows) == len(paths):
                print(f"Audited {len(rows)}/{len(paths)} files", flush=True)
    rows.sort(key=lambda row: row["file"])
    changed = [r["file"] for r in rows if hashlib.sha256((ROOT / r["file"]).read_bytes()).hexdigest() != r["sha256"]]
    result = {"environment": {"macOS": platform.mac_ver()[0], "machine": platform.machine(),
                "ffmpeg": run(["ffmpeg", "-version"])["stdout"].splitlines()[0],
                "libheif": run(["heif-info", "--version"])["stdout"]},
              "changed_inputs": changed, "files": rows}
    config.output.parent.mkdir(parents=True, exist_ok=True)
    config.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print("Expected status matches:", collections.Counter(r.get("expected_matches") for r in rows))
    for suffix in (".heif", ".heic", ".mov"):
        subset = [r for r in rows if r["file"].endswith(suffix)]
        print(suffix, "files", len(subset), "Apple decoded", sum(r["apple"].get("json", {}).get("decoded", False) for r in subset),
              "Quick Look", sum(r["quicklook"].get("json", {}).get("thumbnail", False) for r in subset))
    print("Saved", config.output)
    if changed or any(r.get("expected_matches") is False for r in rows):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
