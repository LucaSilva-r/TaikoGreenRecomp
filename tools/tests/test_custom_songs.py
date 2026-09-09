import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("custom_songs", Path(__file__).parents[1] / "custom_songs.py")
custom = importlib.util.module_from_spec(spec)
spec.loader.exec_module(custom)
from tja2fumen.parsers import parse_fumen


class CustomSongsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "audio.ogg").write_bytes(b"audio stays untouched")
        self.chart = self.root / "test.tja"

    def write(self, measures=2, encoding="utf-8-sig"):
        self.chart.write_text("TITLE:テスト\nSUBTITLE:--サブタイトル\nBPM:120\nOFFSET:0\nWAVE:audio.ogg\nDEMOSTART:0.5\n"
                              "COURSE:Oni\nLEVEL:7\n#START\n" + "1000,\n" * measures + "#END\n",
                              encoding=encoding)

    def test_shift_jis_and_big_endian_timing(self):
        self.write(encoding="shift_jis")
        entry = custom.inspect(self.chart, self.root)
        self.assertEqual(entry["title"], "テスト")
        self.assertEqual(entry["subtitle"], "サブタイトル")
        self.assertEqual(entry["mask"], 8)
        self.assertEqual(entry["preview_ms"], 500)
        output = self.root / "cache"
        custom.convert(self.chart, output, entry["revision"])
        self.assertEqual((output / "ready").read_text().splitlines()[1], "2000")
        fumen = parse_fumen(str(output / "m.bin"))
        self.assertEqual(fumen.header.order, ">")
        self.assertAlmostEqual(fumen.measures[0].offset_start + 240000 / fumen.measures[0].bpm, 2000)
        self.assertEqual((self.root / "audio.ogg").read_bytes(), b"audio stays untouched")

    def test_measure_limit_rejects_before_publication(self):
        self.write(measures=301)
        entry = custom.inspect(self.chart, self.root)
        with self.assertRaisesRegex(ValueError, "300 measures"):
            custom.convert(self.chart, self.root / "cache", entry["revision"])
        self.assertFalse((self.root / "cache/ready").exists())

    def test_changed_chart_requires_refresh(self):
        self.write()
        entry = custom.inspect(self.chart, self.root)
        self.write(measures=3)
        with self.assertRaisesRegex(ValueError, "changed since discovery"):
            custom.convert(self.chart, self.root / "cache", entry["revision"])

    def test_missing_wave_is_not_indexed(self):
        self.write()
        (self.root / "audio.ogg").unlink()
        with self.assertRaisesRegex(ValueError, "audio is missing"):
            custom.inspect(self.chart, self.root)


if __name__ == "__main__":
    unittest.main()
