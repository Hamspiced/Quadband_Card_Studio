"""Static integration guards complement actionlint without contacting GitHub."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PagesWorkflowTests(unittest.TestCase):
    def test_third_party_actions_are_pinned(self):
        for name in ("build.yml", "release.yml", "pages.yml"):
            text = (ROOT/".github/workflows"/name).read_text()
            for action in re.findall(r"uses:\s+(\S+)", text):
                with self.subTest(workflow=name, action=action):
                    self.assertTrue(action.startswith("./") or
                                    re.fullmatch(r"[A-Za-z0-9_./-]+@[0-9a-f]{40}", action))

    def test_pages_waits_for_successful_stable_release_publication(self):
        text = (ROOT/".github/workflows/release.yml").read_text()
        pages = text.split("\n  pages:\n", 1)[1]
        self.assertIn("needs: [prepare, build, publish]", pages)
        self.assertIn("needs.prepare.outputs.prerelease == 'false'", pages)
        self.assertIn("needs.publish.outputs.tag == needs.prepare.outputs.tag", pages)
        self.assertIn("source-commit: ${{ needs.prepare.outputs.commit }}", pages)
        self.assertNotIn("contents: write", pages)
        self.assertEqual(text.count("contents: write"), 1)
        self.assertIn("types: [published]", text)
        self.assertNotIn("pull_request_target", text)

    def test_unprivileged_site_build_and_serialized_latest_only_deploy(self):
        text = (ROOT/".github/workflows/pages.yml").read_text()
        site, deploy = text.split("\n  deploy:\n", 1)
        self.assertNotIn("pages: write", site)
        self.assertNotIn("id-token: write", site)
        self.assertIn("name: Quadband-CoreS3-web-payload", site)
        self.assertIn("scripts/prepare_pages.py", site)
        self.assertEqual(text.count("github.rest.repos.getLatestRelease"), 2)
        self.assertEqual(text.count("release.tag_name === process.env.RELEASE_TAG"), 2)
        self.assertIn("group: github-pages", deploy)
        self.assertIn("cancel-in-progress: false", deploy)
        self.assertIn("pages: write", deploy)
        self.assertIn("id-token: write", deploy)
        self.assertNotIn("npm ", deploy)

    def test_browser_tests_gate_reusable_build(self):
        text = (ROOT/".github/workflows/build.yml").read_text()
        self.assertIn("python -m unittest discover -s firmware/cores3/tests -p 'test_web_export.py'", text)
        self.assertIn("npm run test:browser", text)
        self.assertIn("npm audit --audit-level=high", text)
        self.assertNotIn("--allow-dirty", text)
        self.assertIn("uses: ./.github/workflows/build.yml", (ROOT/".github/workflows/release.yml").read_text())

    def test_application_builds_are_manual_or_explicit_reusable_calls(self):
        for name in ("build.yml", "cardputerzero.yml"):
            text = (ROOT/".github/workflows"/name).read_text()
            triggers = text.split("\npermissions:", 1)[0]
            self.assertIn("\n  workflow_dispatch:", triggers)
            self.assertNotIn("\n  push:", triggers)
            self.assertNotIn("\n  pull_request:", triggers)
        self.assertIn("\n  workflow_call:", (ROOT/".github/workflows/build.yml").read_text())

    def test_windows_upload_retains_executable_folder_and_release_zip(self):
        text = (ROOT/".github/workflows/build.yml").read_text()
        windows = text.split("name: Quadband-Windows\n", 1)[1].split("\n      - name:", 1)[0]
        self.assertIn("desktop/dist/QuadbandCardStudio/", windows)
        self.assertIn("desktop/dist/*.zip", windows)
        self.assertIn("dist/Quadband-Card-Studio-*-macOS-universal.zip", text)

    def test_firmware_container_trusts_only_the_checked_out_workspace(self):
        text = (ROOT/".github/workflows/build.yml").read_text()
        firmware = text.split("\n  cores3-firmware:\n",1)[1].split("\n  portable:\n",1)[0]
        command = 'git config --global --add safe.directory "$GITHUB_WORKSPACE"'
        self.assertIn(command,firmware)
        self.assertLess(firmware.index(command),firmware.index("name: Read release metadata"))
        self.assertNotIn("safe.directory '*'",firmware)
        self.assertNotIn('safe.directory "*"',firmware)


if __name__ == "__main__":
    unittest.main()
