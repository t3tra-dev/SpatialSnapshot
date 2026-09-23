# SPDX-License-Identifier: GPL-3.0-or-later
"""Presentation-time sampling. Source FPS is never used as a frame-index shortcut."""

from bisect import bisect_right
from fractions import Fraction
import math

NANOSECONDS = 1_000_000_000


def seconds_per_frame(fps, fps_base):
    if fps <= 0 or not math.isfinite(fps_base) or fps_base <= 0:
        raise ValueError("FPS と FPS Base は正の値にしてください.")
    # Blender stores fps_base as float32; recover UI values such as 1001/1000.
    return Fraction(str(round(fps_base, 6))) / int(fps)


def sample_index(timestamps, frame, subframe, start_frame, fps, fps_base):
    if not timestamps:
        raise ValueError("カメラサンプルがありません.")
    elapsed = (Fraction(frame - start_frame) + Fraction(str(subframe))) * seconds_per_frame(fps, fps_base)
    return max(0, min(len(timestamps) - 1, bisect_right(timestamps, elapsed * NANOSECONDS) - 1))


def end_frame(duration_ns, start_frame, fps, fps_base):
    count = math.ceil(Fraction(duration_ns, NANOSECONDS) / seconds_per_frame(fps, fps_base))
    return start_frame + max(1, count) - 1


def ticks_to_ns(ticks, time_base):
    value = int(ticks) * Fraction(time_base) * NANOSECONDS
    if value < 0:
        raise ValueError("負の presentation timestamp は SSPS v1 では使用できません.")
    return (2 * value.numerator + value.denominator) // (2 * value.denominator)
