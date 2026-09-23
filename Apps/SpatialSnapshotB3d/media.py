# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode pixels once, retaining one PNG per *presented* sample (including VFR)."""

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

from .native import SpatialError
from .timing import ticks_to_ns

CACHE_VERSION = 1


@dataclass
class Tools:
    ffmpeg: str = ""
    ffprobe: str = ""
    heif_library: str = ""


def executable(name, configured=""):
    if configured:
        candidate = Path(configured).expanduser()
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate.resolve())
        raise SpatialError(f"実行ファイルが見つかりません: {configured}")
    found = shutil.which(name)
    if found:
        return found
    for directory in ("/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"):
        candidate = Path(directory) / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    raise SpatialError(f"{name} が必要です. インストール後, アドオン設定で実行ファイルを指定してください.")


def run(arguments):
    # No shell, downloads, or commands taken from the media / .blend file.
    try:
        result = subprocess.run(arguments, capture_output=True, text=True, timeout=1800)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SpatialError(f"メディアの復号を完了できませんでした: {error}") from error
    if result.returncode:
        raise SpatialError(f"{Path(arguments[0]).name}: {result.stderr[-3000:] or result.stdout[-3000:]}")
    return result.stdout


def png_size(path):
    with Path(path).open("rb") as file:
        header = file.read(24)
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise SpatialError(f"PNG キャッシュが不正です: {path}")
    return struct.unpack(">II", header[16:24])


def frame_path(directory, index):
    return Path(directory) / f"frame-{index:08d}.png"


def decode_movie(source, directory, info, samples, tools):
    ffprobe = executable("ffprobe", tools.ffprobe)
    streams = json.loads(run([ffprobe, "-v", "error", "-show_streams", "-of", "json", str(source)]))["streams"]
    matching = [s for s in streams if s.get("codec_type") == "video" and int(s.get("id", "-1"), 0) == info.media_id]
    if len(matching) != 1:
        raise SpatialError("SSPS に結び付いた video track を FFprobe で特定できません.")
    stream = matching[0]
    frames = json.loads(run([ffprobe, "-v", "error", "-select_streams", str(stream["index"]),
        "-show_frames", "-show_entries", "frame=pts,best_effort_timestamp", "-of", "json", str(source)]))["frames"]
    try:
        timestamps = [ticks_to_ns(frame.get("pts", frame.get("best_effort_timestamp")), stream["time_base"]) for frame in frames]
    except (ValueError, TypeError, KeyError) as error:
        raise SpatialError("MOV の presentation timestamp を読めません.") from error
    if timestamps != [sample[0] for sample in samples]:
        raise SpatialError("復号フレームの PTS と SSPS カメラ時刻が一致しません.")
    run([executable("ffmpeg", tools.ffmpeg), "-nostdin", "-v", "error", "-noautorotate", "-i", str(source),
         "-map", f"0:{stream['index']}", "-an", "-sn", "-dn", "-fps_mode", "passthrough",
         "-start_number", "0", "-pix_fmt", "rgb24", str(directory / "frame-%08d.png")])


def decode_still(source, directory, tools):
    output = frame_path(directory, 0)
    if sys.platform == "darwin" and not tools.heif_library:
        run(["/usr/bin/sips", "-s", "format", "png", str(source), "--out", str(output)])
        return
    # Select the primary explicitly, including HEIF files with multiple images / grids.
    from .heif import decode_primary
    decode_primary(source, output, tools.heif_library)


def prepare_cache(source, cache_root, info, samples, tools, *, force=False):
    source, cache_root = Path(source).resolve(), Path(cache_root).resolve()
    with source.open("rb") as file:
        digest = hashlib.file_digest(file, "sha256").hexdigest()
    identity = {"version": CACHE_VERSION, "sha256": digest, "width": info.raster_width,
                "height": info.raster_height, "timestamps_ns": [s[0] for s in samples]}
    directory = cache_root / f"v{CACHE_VERSION}-{digest}"
    if directory.is_symlink():
        raise SpatialError("キャッシュディレクトリにシンボリックリンクは使用できません.")

    def valid():
        try:
            return (json.loads((directory / "index.json").read_text()) == identity and
                all(png_size(frame_path(directory, i)) == (info.raster_width, info.raster_height) for i in range(len(samples))))
        except (OSError, ValueError, SpatialError):
            return False

    if not force and valid():
        return directory
    cache_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="decode-", dir=cache_root) as temporary:
        staging = Path(temporary)
        if info.kind == 1:
            decode_still(source, staging, tools)
        else:
            decode_movie(source, staging, info, samples, tools)
        images = sorted(staging.glob("frame-*.png"))
        if len(images) != len(samples) or any(png_size(frame_path(staging, i)) !=
                (info.raster_width, info.raster_height) for i in range(len(samples))):
            raise SpatialError("復号画像の数またはサイズが SSPS と一致しません.")
        (staging / "index.json").write_text(json.dumps(identity), encoding="utf-8")
        if directory.exists():
            shutil.rmtree(directory)
        # Rename a child so TemporaryDirectory still owns an existing empty directory.
        ready = staging / "ready"
        ready.mkdir()
        for image in images + [staging / "index.json"]:
            image.rename(ready / image.name)
        ready.rename(directory)
    return directory
