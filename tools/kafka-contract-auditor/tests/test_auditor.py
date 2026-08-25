"""Unit tests for kafka-contract-auditor.

Run: python3 tools/kafka-contract-auditor/tests/test_auditor.py
or:  python3 -m unittest tools/kafka-contract-auditor/tests/test_auditor.py
"""

from pathlib import Path
import io
import textwrap
import tempfile
import unittest
import contextlib
import importlib.util


def import_check_module(path: Path):
    spec = importlib.util.spec_from_file_location("kafka_check", str(path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


CHECK_PATH = Path(__file__).resolve().parents[1] / "check.py"
auditor = import_check_module(CHECK_PATH)


class FakeRepoMixin:
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "infra" / "kafka").mkdir(parents=True)
        (self.root / "docs" / "06-api" / "messaging").mkdir(parents=True)
        (self.root / "docs" / "02-system" / "features").mkdir(parents=True)
        (self.root / "cpp").mkdir(parents=True)

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, rel: str, content: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(content), encoding="utf-8")

    def run_main(self) -> tuple[int, str]:
        original_root = auditor.ROOT
        auditor.ROOT = self.root
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                rc = auditor.main()
        finally:
            auditor.ROOT = original_root
        return rc, buf.getvalue()


class TestTopicHasDoc(FakeRepoMixin, unittest.TestCase):
    def test_missing_doc_errors(self):
        self.write("infra/kafka/create_topics.sh", """
            create_topic my.topic "retention"
        """)
        self.write("docs/06-api/messaging/topics.md", "| `my.topic` |\n")
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("has no doc", out)

    def test_individual_doc_passes(self):
        self.write("infra/kafka/create_topics.sh", """
            create_topic my.topic "retention"
        """)
        self.write("docs/06-api/messaging/topics.md", "| `my.topic` |\n")
        self.write("docs/06-api/messaging/my-topic.md", "# my.topic\n")
        rc, out = self.run_main()
        self.assertEqual(rc, 0)

    def test_bundled_doc_passes(self):
        self.write("infra/kafka/create_topics.sh", """
            create_topic my.topic1 "r"
            create_topic my.topic2 "r"
        """)
        self.write("docs/06-api/messaging/topics.md", "| `my.topic1` | `my.topic2` |\n")
        self.write("docs/06-api/messaging/my-topics.md", """
            # Bundled docs
            Covers `my.topic1` and `my.topic2`.
        """)
        rc, out = self.run_main()
        self.assertEqual(rc, 0)


class TestCatalogMention(FakeRepoMixin, unittest.TestCase):
    def test_catalog_missing_is_warn_not_error(self):
        self.write("infra/kafka/create_topics.sh", "create_topic my.topic r\n")
        self.write("docs/06-api/messaging/my-topic.md", "# my.topic\n")
        # topics.md empty / does not mention my.topic
        self.write("docs/06-api/messaging/topics.md", "# Topics catalog\n")
        rc, out = self.run_main()
        # Doc exists, just missing from catalog — WARN not ERROR.
        self.assertEqual(rc, 0)
        self.assertIn("not mentioned in", out)


class TestFeatureReference(FakeRepoMixin, unittest.TestCase):
    def test_feature_references_missing_topic_errors(self):
        self.write("infra/kafka/create_topics.sh", "create_topic existing r\n")
        self.write("docs/06-api/messaging/existing.md", "# existing\n")
        self.write("docs/06-api/messaging/topics.md", "| `existing` |\n")
        self.write("docs/02-system/features/F-99-demo/feature.yaml", """
            feature:
              id: F-99
              name: Demo
            kafkaTopics:
              produces:
                - phantom.topic
        """)
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("phantom.topic", out)
        self.assertIn("missing in infra/kafka/create_topics.sh", out)


class TestCppLiteralDrift(FakeRepoMixin, unittest.TestCase):
    def test_cpp_produces_unknown_topic(self):
        self.write("infra/kafka/create_topics.sh", "create_topic existing r\n")
        self.write("docs/06-api/messaging/existing.md", "# existing\n")
        self.write("docs/06-api/messaging/topics.md", "| `existing` |\n")
        (self.root / "cpp" / "matching" / "src").mkdir(parents=True, exist_ok=True)
        self.write("cpp/matching/src/foo.cpp", '''
            #include <something>
            int x() { producer.produce("missing.topic", key, data); return 0; }
        ''')
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("missing.topic", out)
        self.assertIn("produced by cpp code", out)

    def test_cpp_consumes_unknown_topic(self):
        self.write("infra/kafka/create_topics.sh", "create_topic existing r\n")
        self.write("docs/06-api/messaging/existing.md", "# existing\n")
        self.write("docs/06-api/messaging/topics.md", "| `existing` |\n")
        (self.root / "cpp" / "ledger" / "src").mkdir(parents=True, exist_ok=True)
        self.write("cpp/ledger/src/c.cpp", '''
            void f() { consumer.subscribe({"phantom.consume"}); }
        ''')
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("phantom.consume", out)
        self.assertIn("consumed by cpp code", out)


class TestInfraTopicsExemption(FakeRepoMixin, unittest.TestCase):
    def test_logs_metrics_skip_doc_check(self):
        self.write("infra/kafka/create_topics.sh", """
            create_topic logs r
            create_topic metrics r
        """)
        # No docs and no mentions — should be silent (infra exemption).
        self.write("docs/06-api/messaging/topics.md", "")
        rc, out = self.run_main()
        self.assertEqual(rc, 0)


class TestTopicDocFilename(unittest.TestCase):
    def test_basic(self):
        self.assertEqual(auditor.topic_doc_filename("foo"), "foo.md")

    def test_dotted(self):
        self.assertEqual(auditor.topic_doc_filename("foo.bar"), "foo-bar.md")

    def test_multi_dotted(self):
        self.assertEqual(auditor.topic_doc_filename("a.b.c"), "a-b-c.md")


if __name__ == "__main__":
    unittest.main()
