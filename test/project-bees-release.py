"""Exercise the release command using a temporary Git remote and GitHub CLI stub."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "project-bees-release.py"
REPO = "example/project-bees"


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        workspace = tempfile.TemporaryDirectory()
        self.addCleanup(workspace.cleanup)
        root = Path(workspace.name)
        self.remote = root / "origin.git"
        self.checkout = root / "checkout"
        self.checkout.mkdir()
        self.git("init", "--bare", "--initial-branch=main", str(self.remote))
        self.git("init", "--initial-branch=main")
        self.git("config", "user.name", "Release Test")
        self.git("config", "user.email", "release-test@example.invalid")
        self.git("remote", "add", "origin", str(self.remote))
        self.git("commit", "--allow-empty", "-m", "Published source")
        self.git("tag", "-a", "v0.1.5", "-m", "Previous release")
        self.git("tag", "v1.0.0-rc.1")
        self.git("commit", "--allow-empty", "-m", "Unreleased source")
        self.git("push", "--tags", "origin", "main")
        self.source = self.git("rev-parse", "HEAD")

        self.calls = root / "gh-calls"
        self.release = root / "gh-release"
        self.reject_release = root / "gh-reject"
        self.move_main = root / "gh-move-main"
        gh = root / "gh"
        gh.write_text(
            "#!/usr/bin/env python3\n"
            "from pathlib import Path\n"
            "import os, subprocess, sys\n"
            "calls, release, reject, move_main = (Path(os.environ[key]) for key in "
            "('GH_TEST_CALLS', 'GH_TEST_RELEASE', 'GH_TEST_REJECT', 'GH_TEST_MOVE_MAIN'))\n"
            "args = sys.argv[1:]\n"
            "with calls.open('a') as log:\n"
            "    log.write(' '.join(args) + '\\n')\n"
            "if args[0] == 'api':\n"
            "    if move_main.exists():\n"
            "        subprocess.run(('git', 'push', 'origin', 'new-main:main'), check=True)\n"
            "    if release.exists():\n"
            "        print(release.read_text())\n"
            "elif args[:2] == ['release', 'create'] and '--verify-tag' in args:\n"
            "    if reject.exists():\n"
            "        sys.exit(1)\n"
            "    url = 'https://example.invalid/releases/' + args[2]\n"
            "    release.write_text(url + '\\tfalse\\tfalse')\n"
            "    print(url)\n"
            "else:\n"
            "    sys.exit(2)\n"
        )
        gh.chmod(0o755)
        self.env = {
            **os.environ,
            "PATH": f"{root}{os.pathsep}{os.environ['PATH']}",
            "GH_TEST_CALLS": str(self.calls),
            "GH_TEST_RELEASE": str(self.release),
            "GH_TEST_REJECT": str(self.reject_release),
            "GH_TEST_MOVE_MAIN": str(self.move_main),
        }

    def git(self, *args):
        return subprocess.check_output(("git", *args), cwd=self.checkout, text=True).strip()

    def refs(self):
        return self.git("--git-dir", self.remote, "show-ref")

    def run_release(self, bump="patch", run_id="42", tag=None):
        args = [sys.executable, str(SCRIPT), bump, self.source, run_id]
        if tag is not None:
            args.extend(("--publish-tag", tag, "--repo", REPO))
        return subprocess.run(args, cwd=self.checkout, env=self.env, capture_output=True, text=True)

    def assert_stopped(self, result, message):
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(message, result.stderr)

    def test_preview_only_computes_the_stable_version(self):
        before = self.refs()
        for bump, expected in (("patch", "v0.1.6"), ("minor", "v0.2.0"), ("major", "v1.0.0")):
            with self.subTest(bump=bump):
                result = self.run_release(bump=bump)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)
        self.assertEqual(self.refs(), before)
        self.assertEqual(self.git("tag", "--list"), "v0.1.5\nv1.0.0-rc.1")
        self.assertFalse(self.calls.exists())

    def test_version_order_is_numeric(self):
        self.git("tag", "v0.1.10", "HEAD^")
        self.git("push", "origin", "refs/tags/v0.1.10")
        result = self.run_release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "v0.1.11")

    def test_rejects_stale_main(self):
        self.git("commit", "--allow-empty", "-m", "New main tip")
        self.git("push", "origin", "main")
        self.git("checkout", "--detach", self.source)
        before = self.refs()
        self.assert_stopped(self.run_release(tag="v0.1.6"), "tip of origin/main")
        self.assertEqual(self.refs(), before)
        self.assertFalse(self.calls.exists())

    def test_rejects_conflicting_next_tag_outside_main(self):
        self.git("checkout", "--detach", "HEAD^")
        self.git("commit", "--allow-empty", "-m", "Different source")
        self.git("tag", "v0.1.6")
        self.git("push", "origin", "refs/tags/v0.1.6")
        self.git("checkout", "main")
        before = self.refs()
        self.assert_stopped(self.run_release(tag="v0.1.6"), "outside origin/main history")
        self.assertEqual(self.refs(), before)
        self.assertFalse(self.calls.exists())

    def test_rechecks_main_before_pushing_the_tag(self):
        self.git("checkout", "-b", "new-main")
        self.git("commit", "--allow-empty", "-m", "Concurrent main update")
        self.git("checkout", "main")
        self.move_main.touch()
        self.assert_stopped(self.run_release(tag="v0.1.6"), "origin/main moved after CI")
        self.assertEqual(self.git("--git-dir", self.remote, "tag", "--list"), "v0.1.5\nv1.0.0-rc.1")
        self.assertNotIn("release create", self.calls.read_text())

    def test_rechecks_main_before_creating_a_release_on_retry(self):
        self.reject_release.touch()
        self.assert_stopped(self.run_release(tag="v0.1.6"), "Release stopped")
        self.reject_release.unlink()
        self.git("checkout", "-b", "new-main")
        self.git("commit", "--allow-empty", "-m", "Concurrent main update")
        self.git("checkout", "main")
        self.move_main.touch()
        self.assert_stopped(self.run_release(tag="v0.1.6"), "origin/main moved before")
        self.assertEqual(self.git("--git-dir", self.remote, "rev-parse", "v0.1.6^{}"), self.source)
        self.assertFalse(self.release.exists())
        self.assertEqual(self.calls.read_text().count("release create v0.1.6"), 1)

    def test_rejects_changed_plan_and_existing_draft(self):
        before = self.refs()
        self.assert_stopped(self.run_release(tag="v0.1.7"), "Planned tag changed")
        self.assertFalse(self.calls.exists())
        self.release.write_text("https://example.invalid/draft\ttrue\tfalse")
        self.assert_stopped(self.run_release(tag="v0.1.6"), "already exists on GitHub")
        self.assertEqual(self.refs(), before)
        self.assertNotIn("release create", self.calls.read_text())

    def test_publishes_tested_tag_once_and_retries_the_same_run(self):
        self.reject_release.touch()
        self.assert_stopped(self.run_release(tag="v0.1.6"), "Release stopped")
        after_tag = self.refs()
        self.assertEqual(self.git("--git-dir", self.remote, "rev-parse", "main"), self.source)
        self.assertEqual(self.git("--git-dir", self.remote, "rev-parse", "v0.1.6^{}"), self.source)
        self.assertFalse(self.release.exists())
        self.assert_stopped(self.run_release(run_id="43", tag="v0.1.6"), "already has a stable version tag")

        self.reject_release.unlink()
        for _ in range(2):
            result = self.run_release(tag="v0.1.6")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), "v0.1.6")
        self.assertEqual(self.refs(), after_tag)
        calls = self.calls.read_text()
        self.assertEqual(calls.count("release create v0.1.6"), 2)
        self.assertEqual(calls.count(f"--verify-tag --target {self.source}"), 2)


if __name__ == "__main__":
    unittest.main()
