"""Unit tests for feature-yaml-checker.

Run: python3 tools/feature-yaml-checker/tests/test_checker.py
or:  python3 -m unittest tools/feature-yaml-checker/tests/test_checker.py

Создаёт временный repo-like layout, кладёт в него фейковые feature.yaml /
feature-component-map.yaml / create_topics.sh с известными drift'ами, и
проверяет что main() возвращает 1 и выводит ожидаемые errors.
"""

from pathlib import Path
import io
import sys
import textwrap
import tempfile
import unittest
import contextlib
import importlib.util


def import_check_module(path: Path):
    spec = importlib.util.spec_from_file_location("feature_yaml_checker_check", str(path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


CHECK_PATH = Path(__file__).resolve().parents[1] / "check.py"
checker = import_check_module(CHECK_PATH)


class FakeRepoMixin:
    """Build a minimal repo layout under self.root."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "specs" / "domain").mkdir(parents=True)
        (self.root / "docs" / "02-system" / "features").mkdir(parents=True)
        (self.root / "docs" / "06-api" / "messaging").mkdir(parents=True)
        (self.root / "infra" / "kafka").mkdir(parents=True)
        (self.root / "cpp" / "matching" / "src").mkdir(parents=True)
        (self.root / "contracts" / "proto" / "fob" / "common" / "v1").mkdir(parents=True)

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, rel: str, content: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(content), encoding="utf-8")

    def run_main(self) -> tuple[int, str]:
        """Pivot ROOT, capture stdout, call main()."""
        original_root = checker.ROOT
        checker.ROOT = self.root
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                rc = checker.main()
        finally:
            checker.ROOT = original_root
        return rc, buf.getvalue()


class TestStatusSync(FakeRepoMixin, unittest.TestCase):
    def test_status_mismatch_is_error(self):
        self.write("specs/domain/feature-component-map.yaml", """
            features:
              F-99:
                name: Demo
                docsPath: docs/02-system/features/F-99-demo
                status: not-implemented
                primaryComponents: [matching]
                protoContracts: []
                codePaths: []
        """)
        self.write("docs/02-system/features/F-99-demo/feature.yaml", """
            feature:
              id: F-99
              name: Demo
              status: in-progress-impl
        """)
        self.write("infra/kafka/create_topics.sh", "# none\n")
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("status mismatch", out)
        self.assertIn("F-99", out)

    def test_status_aligned_passes(self):
        self.write("specs/domain/feature-component-map.yaml", """
            features:
              F-99:
                name: Demo
                docsPath: docs/02-system/features/F-99-demo
                status: in-progress-impl
                primaryComponents: [matching]
                protoContracts: []
                codePaths: []
        """)
        self.write("docs/02-system/features/F-99-demo/feature.yaml", """
            feature:
              id: F-99
              name: Demo
              status: in-progress-impl
        """)
        self.write("infra/kafka/create_topics.sh", "# none\n")
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("OK", out)


class TestKafkaTopicDrift(FakeRepoMixin, unittest.TestCase):
    def test_topic_missing_in_create_topics(self):
        self.write("specs/domain/feature-component-map.yaml", """
            features:
              F-99:
                name: Demo
                docsPath: docs/02-system/features/F-99-demo
                status: in-progress-impl
                primaryComponents: [matching]
                protoContracts: []
                codePaths: []
        """)
        self.write("docs/02-system/features/F-99-demo/feature.yaml", """
            feature:
              id: F-99
              name: Demo
              status: in-progress-impl
            kafkaTopics:
              produces:
                - my.topic
        """)
        # Empty create_topics.sh
        self.write("infra/kafka/create_topics.sh", "# none\n")
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("missing in infra/kafka/create_topics.sh", out)

    def test_topic_present_no_doc(self):
        self.write("specs/domain/feature-component-map.yaml", """
            features:
              F-99:
                name: Demo
                docsPath: docs/02-system/features/F-99-demo
                status: in-progress-impl
                primaryComponents: [matching]
                protoContracts: []
                codePaths: []
        """)
        self.write("docs/02-system/features/F-99-demo/feature.yaml", """
            feature:
              id: F-99
              name: Demo
              status: in-progress-impl
            kafkaTopics:
              produces:
                - my.topic
        """)
        self.write("infra/kafka/create_topics.sh", "create_topic my.topic\n")
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("no doc", out)
        self.assertIn("my-topic.md", out)

    def test_topic_with_doc_passes(self):
        self.write("specs/domain/feature-component-map.yaml", """
            features:
              F-99:
                name: Demo
                docsPath: docs/02-system/features/F-99-demo
                status: in-progress-impl
                primaryComponents: [matching]
                protoContracts: []
                codePaths: []
        """)
        self.write("docs/02-system/features/F-99-demo/feature.yaml", """
            feature:
              id: F-99
              name: Demo
              status: in-progress-impl
            kafkaTopics:
              produces:
                - my.topic
        """)
        self.write("infra/kafka/create_topics.sh", "create_topic my.topic\n")
        self.write("docs/06-api/messaging/my-topic.md", "# my.topic\n")
        rc, out = self.run_main()
        self.assertEqual(rc, 0)


class TestCodePathExistence(FakeRepoMixin, unittest.TestCase):
    def test_missing_code_path_errors(self):
        self.write("specs/domain/feature-component-map.yaml", """
            features:
              F-99:
                name: Demo
                docsPath: docs/02-system/features/F-99-demo
                status: in-progress-impl
                primaryComponents: [matching]
                protoContracts: []
                codePaths:
                  - cpp/matching/src/missing_file.cpp
        """)
        self.write("docs/02-system/features/F-99-demo/feature.yaml", """
            feature:
              id: F-99
              name: Demo
              status: in-progress-impl
        """)
        self.write("infra/kafka/create_topics.sh", "# none\n")
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("missing on disk", out)


class TestProtoContractExistence(FakeRepoMixin, unittest.TestCase):
    def test_missing_proto_errors(self):
        self.write("specs/domain/feature-component-map.yaml", """
            features:
              F-99:
                name: Demo
                docsPath: docs/02-system/features/F-99-demo
                status: in-progress-impl
                primaryComponents: [matching]
                protoContracts:
                  - contracts/proto/fob/common/v1/no_such.proto
                codePaths: []
        """)
        self.write("docs/02-system/features/F-99-demo/feature.yaml", """
            feature:
              id: F-99
              name: Demo
              status: in-progress-impl
        """)
        self.write("infra/kafka/create_topics.sh", "# none\n")
        rc, out = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("protoContracts missing", out)


class TestFlattenCodePaths(unittest.TestCase):
    def test_list(self):
        self.assertEqual(checker.flatten_code_paths(["a", "b"]), ["a", "b"])

    def test_nested_dict(self):
        nested = {"service": ["a"], "app": {"sub": ["b", "c"]}}
        self.assertEqual(set(checker.flatten_code_paths(nested)), {"a", "b", "c"})

    def test_none_returns_empty(self):
        self.assertEqual(checker.flatten_code_paths(None), [])


class TestTopicDocFilename(unittest.TestCase):
    def test_dot_to_dash(self):
        self.assertEqual(checker.topic_doc_filename("batch.outputs"), "batch-outputs.md")

    def test_plain(self):
        self.assertEqual(checker.topic_doc_filename("fills"), "fills.md")

    def test_multi_dot(self):
        self.assertEqual(
            checker.topic_doc_filename("venue.liquidity.fob"), "venue-liquidity-fob.md"
        )


if __name__ == "__main__":
    unittest.main()
