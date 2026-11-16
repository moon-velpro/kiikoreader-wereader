#!/usr/bin/env python3
import unittest

from weread_cache_log_analyzer import classify_url, parse_lines, render_text


class WeReadCacheLogAnalyzerTest(unittest.TestCase):
    def test_url_categories(self):
        self.assertEqual(classify_url("https://weread.qq.com/book/chapterdownload?bookId=1"), "eink_archive")
        self.assertEqual(classify_url("https://weread.qq.com/web/book/chapterInfos"), "toc")
        self.assertEqual(classify_url("https://weread.qq.com/web/book/chapter/e_1"), "shard")
        self.assertEqual(classify_url("https://res.weread.qq.com/book/chapter.tar"), "tar")
        self.assertEqual(classify_url("https://weread.qq.com/web/book/read"), "reader_or_read_api")

    def test_aggregates_cache_log(self):
        lines = [
            "I (100) WEREAD: HTTP complete url=https://weread.qq.com/web/book/chapterInfos bytes=500 elapsed_ms=100 connection_reused=0",
            "I (200) WEREAD: HTTP complete url=https://weread.qq.com/web/book/chapter/e_1 bytes=1000 elapsed_ms=250 connection_reused=1",
            "I (300) WEREAD: HTTP complete url=https://res.weread.qq.com/book/chapter.tar bytes=4096 elapsed_ms=800 connection_reused=0",
            "I (400) WEREAD: cache fetch index=1 bytes=12000 elapsed_ms=900 result=1",
            "I (500) WEREAD: cache image id=2 bytes=4096 elapsed_ms=800 result=1",
            "I (600) WEREAD: cache chapter book=book index=1/1 html_bytes=12000 elapsed_ms=1100 result=1",
            "I (700) WEREAD: prefetch_wait_ms=75",
            "I (800) WEREAD: work done kind=2 ok=1 canceled=0 elapsed_ms=1500",
        ]
        report = parse_lines(lines)
        self.assertEqual(report["http_requests"], 3)
        self.assertEqual(report["http_total"]["bytes"], 5596)
        self.assertEqual(report["http_total"]["elapsed_ms"], 1150)
        self.assertEqual(report["http_by_type"]["shard"]["reused"], 1)
        self.assertEqual(report["cache_fetch"]["count"], 1)
        self.assertEqual(report["images"]["bytes"], 4096)
        self.assertEqual(report["chapters"]["count"], 1)
        self.assertEqual(report["prefetch_wait_ms"], 75)
        self.assertEqual(report["cache_work_ms"], 1500)
        self.assertEqual(report["request_time_minus_work_ms"], 0)
        self.assertNotIn("book", str(report["http_by_type"]))

    def test_new_phase_metrics(self):
        lines = [
            "I WEREAD: cache fetch index=1 bytes=12000 elapsed_ms=900 reader_ms=300 content_ms=500 attempts=1 result=1",
            "I WEREAD: cache chapter book=book index=1/1 html_bytes=12000 fetch_ms=900 package_ms=200 elapsed_ms=1100 result=1",
            "I WEREAD: cache metrics chapters=1 image_requests=2 image_ok=2 image_failed=0 image_unsupported=0 image_bytes=8192 reader_ms=300 content_ms=500 chapter_fetch_ms=900 chapter_package_ms=200 image_fetch_ms=150 image_zip_ms=20 zip_crc_ms=5 zip_write_ms=30 file_read_ms=10 total_ms=1150",
        ]
        report = parse_lines(lines)
        self.assertEqual(report["cache_fetch"]["elapsed_ms"], 900)
        self.assertEqual(report["chapters"]["fetch_ms"], 900)
        self.assertEqual(report["chapters"]["package_ms"], 200)
        self.assertEqual(report["cache_metrics"]["reader_ms"], 300)
        self.assertEqual(report["cache_metrics"]["image_ok"], 2)
        self.assertEqual(report["cache_metrics"]["total_ms"], 1150)
        summary = render_text(report)
        self.assertIn("Package stages:", summary)
        self.assertIn("image-fetch=150 ms", summary)

    def test_failed_chapter_is_counted(self):
        report = parse_lines([
            "W WEREAD: cache fetch index=3 bytes=0 elapsed_ms=2000 result=0",
            "E WEREAD: cache chapter book=id index=3/3 html_bytes=0 elapsed_ms=2000 result=0",
        ])
        self.assertEqual(report["cache_fetch"]["failures"], 1)
        self.assertEqual(report["chapters"]["failures"], 1)

    def test_failed_fetch_with_phase_breakdown(self):
        report = parse_lines([
            "W WEREAD: cache fetch failed index=3 chapter=uid elapsed_ms=2100 reader_ms=800 content_ms=900 attempts=2 error=network"
        ])
        self.assertEqual(report["cache_fetch"]["failures"], 1)
        self.assertEqual(report["cache_fetch"]["elapsed_ms"], 2100)

    def test_text_summary_is_standalone(self):
        report = parse_lines(["I WEREAD: work done kind=2 ok=1 canceled=0 elapsed_ms=1000"])
        summary = render_text(report)
        self.assertIn("Cache work: 1000 ms", summary)
        self.assertIn("Measured prefetch wait: 0 ms", summary)


if __name__ == "__main__":
    unittest.main()
