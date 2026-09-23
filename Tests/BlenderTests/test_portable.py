"""Run without bpy on macOS and Linux against the built C shared library."""
import os
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Apps"))
from SpatialSnapshotB3d.native import Library, SpatialError  # noqa: E402
from SpatialSnapshotB3d.timing import sample_index, end_frame, ticks_to_ns  # noqa: E402


class TimelineTests(unittest.TestCase):
    def test_irregular_samples_and_project_fps(self):
        times = [0, 83_333_331, 199_456_789]
        self.assertEqual([sample_index(times, f, 0, 1, 24, 1) for f in range(1, 7)], [0, 0, 1, 1, 1, 2])
        self.assertEqual(sample_index(times, 3, 0, 1, 60, 1), 0)
        self.assertEqual(sample_index(times, 3, 0, 1, 10, 1), 2)
        self.assertEqual(sample_index(times, 13, 0, 11, 24, 1), 1)

    def test_ntsc_base_uses_exact_ui_value(self):
        base = 1.0010000467300415
        self.assertEqual(sample_index([0, 1_001_000_000], 31, 0, 1, 30, base), 1)
        self.assertEqual(end_frame(1_001_000_000, 1, 30, base), 30)

    def test_subframes_reverse_seek_and_end_hold(self):
        times = [0, 50_000_000, 100_000_000]
        self.assertEqual(sample_index(times, 1, 0.5, 1, 10, 1), 1)
        self.assertEqual([sample_index(times, f, 0, 1, 10, 1) for f in [4, 2, -3, 3, 1]], [2, 2, 0, 2, 0])
        self.assertEqual(end_frame(3_000_000_000, 7, 24, 1), 78)
        self.assertEqual(end_frame(0, 7, 24, 1), 7)

    def test_timestamp_rounding(self):
        self.assertEqual(ticks_to_ns(1, "1/2000000000"), 1)
        self.assertEqual(ticks_to_ns(1, "1/15"), 66_666_667)
        self.assertEqual(ticks_to_ns(123, "1/1000000000"), 123)
        with self.assertRaises(ValueError):
            ticks_to_ns(-1, "1/15")


class ReaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(os.environ.get("SPATIALSNAPSHOT_C_LIBRARY", ""))

    def test_bound_still_and_owned_snapshot(self):
        doc, info, samples = self.library.open_media(ROOT / "Apps/SpatialSnapshotLab/Resources/Room.heic")
        with doc:
            self.assertEqual((info.raster_width, info.raster_height), (640, 480))
            self.assertEqual(samples, [(0, 0)])
            self.assertEqual(doc.cameras[0].translation, (0, 0, 0))
            self.assertEqual(doc.cameras[0].quaternion, (0, 0, 0, 1))
            chunks, digest = doc.snapshot(0)
            self.assertGreater(len(chunks), 0)
            raw = doc.data
        self.assertTrue(chunks[0].xyz)
        with self.library.open_ssps(raw) as reopened:
            self.assertEqual(reopened.snapshot(0), (chunks, digest))

    def test_movie_presentation_timeline(self):
        doc, info, samples = self.library.open_media(ROOT / "Apps/SpatialSnapshotLab/Resources/Room.mov")
        with doc:
            self.assertEqual(info.duration_ns, 3_000_000_000)
            self.assertEqual(len(samples), 45)
            self.assertEqual([c.timestamp_ns for c in doc.cameras], [s[0] for s in samples])
            self.assertEqual(sum(s[1] for s in samples), info.duration_ns)
            first = doc.snapshot(0)
            doc.snapshot(samples[-1][0])
            self.assertEqual(doc.snapshot(0), first)

    def test_unbound_and_malformed_inputs_are_rejected(self):
        with self.assertRaises(SpatialError):
            self.library.open_media(ROOT / "Fixtures/bindings/interop/source.heic")
        with self.assertRaises(SpatialError):
            self.library.open_ssps(b"invalid SSPS")

    def test_geometry_updates_and_empty_initial_scene(self):
        with self.library.open_ssps((ROOT / "Fixtures/v1/minimal-still.ssps").read_bytes()) as doc:
            self.assertEqual(doc.snapshot(0)[0], [])
        with self.library.open_ssps((ROOT / "Fixtures/v1/geometry-video.ssps").read_bytes()) as doc:
            states = [doc.snapshot(camera.timestamp_ns)[1] for camera in doc.cameras]
            self.assertGreater(len(set(states)), 1)
            self.assertEqual(doc.snapshot(0)[1], states[0])


if __name__ == "__main__":
    unittest.main()
