#!/usr/bin/env python3
"""The release pipeline in .github/workflows/build.yml.

Two kinds of test. Structural: who may publish, when, and with which permissions, so the stable
main pipeline cannot be changed by accident. Behavioural: the prerelease job's own shell scripts,
extracted from the workflow and run against a stand-in `gh` that keeps tags and releases on disk,
so idempotent retries and tag-collision refusal are exercised, not just read.
"""
import hashlib, json, os, pathlib, subprocess, tempfile, textwrap, unittest
import yaml

ROOT = pathlib.Path(__file__).resolve().parents[3]
WORKFLOW_TEXT = (ROOT / ".github/workflows/build.yml").read_text()
WORKFLOW = yaml.safe_load(WORKFLOW_TEXT)
JOBS = WORKFLOW["jobs"]
TRIGGERS = WORKFLOW.get(True, WORKFLOW.get("on"))
SHA = "0123456789abcdef0123456789abcdef01234567"
OTHER = "fedcba9876543210fedcba9876543210fedcba98"
VERSION = "0.4.99"
DEB = f"com.parv.octopus.native_{VERSION}_iphoneos-arm64.deb"


def step(job, name):
    return next(s["run"] for s in JOBS[job]["steps"] if s.get("name") == name)


FAKE_GH = r'''#!/bin/bash
# Stand-in for gh: tags and releases live under $FAKE_GH_STATE.
s="$FAKE_GH_STATE"; echo "$*" >> "$s/log"
case "$1 $2" in
  "api "*)
    path="$2"; expr=""; [ "${3:-}" = --jq ] && expr="$4"
    case "$path" in
      */git/ref/tags/*) f="$s/tags/${path##*/git/ref/tags/}"; [ -f "$f" ] || exit 1
                        sha="$(sed -n 1p "$f")"; type="$(sed -n 2p "$f")"; type="${type:-commit}"
                        [ "$expr" = .object.type ] && echo "$type" || echo "$sha" ;;
      */git/tags/*)     cat "$s/tagobjects/${path##*/git/tags/}" ;;
      *) exit 1 ;;
    esac ;;
  "release view")   [ -d "$s/releases/$3" ] || exit 1; [ "${4:-}" = --json ] && cat "$s/releases/$3/prerelease"; exit 0 ;;
  "release upload") [ -d "$s/releases/$3" ] || exit 1; echo upload >> "$s/releases/$3/events" ;;
  "release edit")   [ -d "$s/releases/$3" ] || exit 1; echo "edit $*" >> "$s/releases/$3/events" ;;
  "release create")
    tag="$3"; shift 3; target=""; pre=false; latest=unset
    while [ $# -gt 0 ]; do case "$1" in --target) target="$2"; shift ;; --prerelease) pre=true ;; --latest=false) latest=false ;; esac; shift; done
    mkdir -p "$s/releases/$tag" "$s/tags"; echo "$pre" > "$s/releases/$tag/prerelease"; echo create >> "$s/releases/$tag/events"
    echo "$target" > "$s/tags/$tag"; echo "target=$target prerelease=$pre latest=$latest" > "$s/releases/$tag/created" ;;
  *) exit 1 ;;
esac
'''


class Structure(unittest.TestCase):
    def test_triggers(self):
        # main only: sonnet is gone, and with it the prerelease job. Everything publishes from main.
        self.assertEqual(TRIGGERS["push"]["branches"], ["main"])
        self.assertEqual(TRIGGERS["push"]["tags"], ["v*"])
        self.assertIn("pull_request", TRIGGERS)

    def test_least_privilege(self):
        self.assertEqual(WORKFLOW["permissions"], {"contents": "read"})
        writers = {n for n, j in JOBS.items() if (j.get("permissions") or {}).get("contents") == "write"}
        self.assertEqual(writers, {"release"})   # one job may write releases, and only one

    def test_release_gating(self):
        job = JOBS["release"]
        # The companion is required. A release carrying the app alone meant the DEB had to be found
        # elsewhere, and the two drifted apart; a mismatched pair cost two debugging sessions.
        self.assertEqual(job["needs"], ["core", "app", "native"])
        self.assertIn("refs/heads/main", job["if"])
        self.assertIn("refs/tags/v", job["if"])
        self.assertIn("pull_request", job["if"])                    # excluded, checked below
        self.assertIn("github.event_name != 'pull_request'", job["if"])
        self.assertNotIn("sonnet", job["if"])

    def test_the_prerelease_job_is_gone(self):
        # sonnet is deleted, so nothing may publish from it or mark a build as a prerelease.
        self.assertNotIn("prerelease", JOBS)
        self.assertNotIn("sonnet", WORKFLOW_TEXT)

    def test_no_workflow_level_concurrency(self):
        # A workflow-wide group keeps only one pending run, which could skip a stable release on main.
        self.assertNotIn("concurrency", WORKFLOW)

    def test_publish_flags(self):
        run = step("release", "Publish")
        self.assertIn('tag="v$VERSION"', run)            # every push to main is a release
        self.assertIn("--verify-tag", run)
        self.assertNotIn("--prerelease", run)            # nothing is published as a prerelease now
        self.assertIn("Octopus.tipa", run)
        self.assertIn("Octopus.ipa", run)
        self.assertIn("iphoneos-arm64.deb", run)         # the companion ships with the app

    def test_release_verifies_the_pair(self):
        verify = step("release", "Verify the paired assets")
        for needle in ("Octopus.tipa", "Octopus.ipa", "iphoneos-arm64.deb", ".sha256", ".json"):
            self.assertIn(needle, verify)

    def test_failed_native_build_keeps_diagnostics(self):
        kept = [s for s in JOBS["native"]["steps"] if s.get("if") == "failure()"]
        self.assertTrue(kept and "upload-artifact" in kept[0]["uses"])


class Behaviour(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        (self.dir / "bin").mkdir()
        gh = self.dir / "bin/gh"
        gh.write_text(FAKE_GH); gh.chmod(0o755)
        self.state = self.dir / "state"
        for d in ("tags", "releases", "tagobjects"):
            (self.state / d).mkdir(parents=True)
        (self.state / "log").write_text("")
        self.tag = f"v{VERSION}"   # every push to main publishes v<version>

    def tearDown(self):
        self.tmp.cleanup()

    def run_step(self, name):
        # The release job reads GITHUB_REF to tell a tag push from a push to main.
        env = dict(os.environ, VERSION=VERSION, GITHUB_SHA=SHA, GH_REPO="owner/octopus",
                   GITHUB_REF="refs/heads/main", GITHUB_REF_NAME="main",
                   GITHUB_STEP_SUMMARY=str(self.dir / "step-summary.md"),
                   FAKE_GH_STATE=str(self.state), PATH=f"{self.dir / 'bin'}:{os.environ['PATH']}")
        return subprocess.run(["bash", "-c", step("release", name)], cwd=self.dir, env=env,
                               capture_output=True, text=True, timeout=60)

    def assets(self, version=VERSION, corrupt=False):
        # The release job downloads the app into release/ and the companion into release/native/.
        app, native = self.dir / "release", self.dir / "release/native"
        native.mkdir(parents=True)
        (app / "Octopus.tipa").write_bytes(b"tipa"); (app / "Octopus.ipa").write_bytes(b"tipa")
        sums = "".join(f"{hashlib.sha256(b'tipa').hexdigest()}  {n}\n" for n in ("Octopus.tipa", "Octopus.ipa"))
        (app / "SHA256SUMS").write_text(sums)
        if corrupt:
            (app / "Octopus.ipa").write_bytes(b"altered")
        (native / DEB).write_bytes(b"deb")
        (native / f"{DEB}.sha256").write_text(f"{hashlib.sha256(b'deb').hexdigest()}  {DEB}\n")
        (native / f"{DEB}.json").write_text(json.dumps({"version": version}))

    def test_verify_accepts_a_matching_pair_and_sums_every_binary(self):
        self.assets()
        r = self.run_step("Verify the paired assets")
        self.assertEqual(r.returncode, 0, r.stderr)
        names = [l.split()[1] for l in (self.dir / "release/SHA256SUMS").read_text().splitlines()]
        self.assertEqual(names, ["Octopus.tipa", "Octopus.ipa", DEB])

    def test_verify_rejects_a_missing_deb(self):
        self.assets(); (self.dir / "release/native" / DEB).unlink()
        self.assertNotEqual(self.run_step("Verify the paired assets").returncode, 0)

    def test_verify_rejects_a_deb_from_another_version(self):
        self.assets(version="0.4.98")
        self.assertNotEqual(self.run_step("Verify the paired assets").returncode, 0)

    def test_verify_rejects_a_corrupted_app(self):
        self.assets(corrupt=True)
        self.assertNotEqual(self.run_step("Verify the paired assets").returncode, 0)

    def publish(self):
        (self.dir / "publish").mkdir(exist_ok=True)
        (self.dir / "release-notes.md").write_text("notes\n")
        return self.run_step("Publish")

    def test_first_publish_creates_a_release_at_this_commit(self):
        r = self.publish()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        created = (self.state / "releases" / self.tag / "created").read_text().strip()
        # A push to main releases at that commit and marks it latest. Nothing is a prerelease now:
        # sonnet is gone, so main is where everything is published from.
        self.assertEqual(created, f"target={SHA} prerelease=false latest=unset")

    def test_a_retry_for_the_same_commit_refreshes_rather_than_duplicates(self):
        self.assertEqual(self.publish().returncode, 0)
        r = self.publish()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        events = (self.state / "releases" / self.tag / "events").read_text().split("\n")
        # Created once, then the second run refreshes the same release rather than making another.
        self.assertEqual([e.split()[0] for e in events if e], ["create", "upload", "edit"])
        self.assertIn("--draft=false", (self.state / "releases" / self.tag / "events").read_text())

    def test_an_existing_release_is_refreshed_without_deleting_anything(self):
        self.assertEqual(self.publish().returncode, 0)
        self.assertEqual(self.publish().returncode, 0)
        log = (self.state / "log").read_text()
        # Drafts left by Actions are cleaned up, but nothing published is ever deleted.
        self.assertNotIn("release delete", log)

    def test_an_annotated_tag_is_resolved_to_its_commit(self):
        (self.state / "tags" / self.tag).write_text("tagobject1\ntag\n")
        (self.state / "tagobjects" / "tagobject1").write_text(SHA + "\n")
        (self.state / "releases" / self.tag).mkdir()
        (self.state / "releases" / self.tag / "prerelease").write_text("true\n")
        r = self.publish()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("upload", (self.state / "releases" / self.tag / "events").read_text())


if __name__ == "__main__":
    unittest.main()
