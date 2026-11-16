import tempfile
import unittest
from pathlib import Path
import importlib.util

SPEC = importlib.util.spec_from_file_location(
    "weread_perf_log_summary", Path(__file__).with_name("weread_perf_log_summary.py")
)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PerfLogSummaryTest(unittest.TestCase):
    def test_aggregates_pipeline_counters(self):
        log = """I (1) WEREAD: cache chapter tar index=2 bytes=2048 limit=4096 free_psram=8192 elapsed_ms=70 ok=1
I (2) WEREAD: cache chapter tar index=3 bytes=0 limit=4096 free_psram=8192 elapsed_ms=10 ok=0
I (3) WEREAD: cache chapter book=123 index=2/3 html_bytes=1200 fetch_ms=80 transform_ms=30 image_ms=2 elapsed_ms=110 result=1
I (4) WEREAD: cache prefetch hits=2 wait_ms=50 enabled=1
I (5) WEREAD: cache image reuse hits=4 stored=9 tar_images=7 tar_fallbacks=1
I (6) WEREAD: cache shard keep-alive hits=12 fallbacks=1 disabled=1
I (7) WEREAD: cache profile book=123 chapters=3 fetch_ms=300 transform_ms=200 image_ms=10 chapter_wall_ms=500
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "serial.log"
            path.write_text(log, encoding="utf-8")
            result = MODULE.summarize(path)
        self.assertEqual(result["tar_summary"], {
            "requests": 2, "successful_requests": 1, "bytes": 2048, "elapsed_ms": 80
        })
        self.assertEqual(result["prefetch"][0]["hits"], 2)
        self.assertEqual(result["images"][0]["tar_images"], 7)
        self.assertEqual(result["keepalive"][0]["fallbacks"], 1)
        self.assertEqual(result["profiles"][0]["chapters"], 3)
        self.assertEqual(result["chapter_summary"]["successful"], 1)


if __name__ == "__main__":
    unittest.main()
