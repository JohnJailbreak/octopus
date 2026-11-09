"""Host tests for the update path's C code: octopushelper (Octopus/RootHelper/main.c), built with -DOCT_HELPER_TEST so
it starts nothing and puts every device path under a temporary root, and the spawn bridge the app uses to start it
(Octopus/App/Spawn/oct_spawn.c), built as is. Run by `make test` and `make sanitize`; CC and HELPER_CFLAGS choose the
compiler and flags."""
import json
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CC = os.environ.get('CC', 'cc')
CFLAGS = shlex.split(os.environ.get('HELPER_CFLAGS', '-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror'))
DOWNLOADS = 'var/mobile/Library/Caches/Octopus'
TMP_DOWNLOADS = 'var/tmp/Octopus'
CONTAINERS = 'private/var/containers/Bundle/Application'


def build(name, sources, defines=()):
    out = Path(tempfile.gettempdir()) / f'octopus-{name}-{os.getpid()}'
    cmd = [CC, *CFLAGS, *defines, *[str(ROOT / s) for s in sources], '-o', str(out)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise AssertionError(f'{" ".join(cmd)}\n{result.stderr}')
    return out


class HelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper = build('helper', ['Octopus/RootHelper/main.c'], ['-DOCT_HELPER_TEST'])

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / DOWNLOADS).mkdir(parents=True)
        (self.root / TMP_DOWNLOADS).mkdir(parents=True)
        (self.root / CONTAINERS).mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def container(self, name, bundle_id, helper=True):
        folder = self.root / CONTAINERS / name
        folder.mkdir()
        if bundle_id is not None:
            (folder / '.com.apple.mobile_container_manager.metadata.plist').write_text(bundle_id + '\n')
        if helper:
            (folder / 'TrollStore.app').mkdir()
            (folder / 'TrollStore.app/trollstorehelper').write_bytes(b'')
        return folder

    def download(self, relative='Octopus-update.tipa', folder=DOWNLOADS):
        path = self.root / folder / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'PK\x03\x04')
        return path

    def run_helper(self, *args, **env):
        environment = dict(os.environ, OCTOPUS_ROOT=str(self.root), **env)
        result = subprocess.run([str(self.helper), *map(str, args)], capture_output=True, text=True, env=environment)
        return result.returncode, result.stdout.splitlines()

    def errors(self, lines):
        return [line.split('\t', 1)[1] for line in lines if line.startswith('ERR\t')]

    # Commands and usage

    def test_version(self):
        code, lines = self.run_helper('version')
        self.assertEqual((code, lines), (0, ['LOG\toctopushelper 1.0']))

    def test_usage_errors(self):
        self.assertEqual(self.run_helper()[0], 2)
        self.assertEqual(self.run_helper('frobnicate')[0], 2)
        self.assertEqual(self.run_helper('install-update', '--deb', 'x')[0], 2)
        code, lines = self.run_helper('install-update')
        self.assertEqual((code, self.errors(lines)), (2, ['Nothing to install']))

    # Installing

    def test_installs_with_trollstore_and_removes_the_download(self):
        self.container('AAAA', 'com.example.other')
        self.container('BBBB', None, helper=False)            # no metadata at all
        trollstore = self.container('CCCC', 'com.opa334.TrollStore')
        tipa = self.download()
        code, lines = self.run_helper('install-update', '--tipa', tipa)
        self.assertEqual(code, 0, lines)
        self.assertIn(f'LOG\t(test) would run {trollstore}/TrollStore.app/trollstorehelper in its own session', lines)
        self.assertIn('LOG\t(test)   install', lines)
        self.assertIn(f'LOG\t(test)   {tipa}', lines)
        self.assertEqual(lines[-1], 'LOG\tUpdate installed')
        self.assertFalse(tipa.exists(), 'the download is removed once used')

    def test_the_fallback_download_folder_and_ipa_are_accepted(self):
        self.container('CCCC', 'com.opa334.TrollStore')
        ipa = self.download('Octopus-update.ipa', TMP_DOWNLOADS)
        self.assertEqual(self.run_helper('install-update', '--tipa', ipa)[0], 0)

    def test_files_outside_the_download_folders_are_refused(self):
        self.container('CCCC', 'com.opa334.TrollStore')
        elsewhere = self.download('Octopus.tipa', 'var/mobile/Documents')
        nested = self.download('sub/Octopus.tipa')
        wrong_type = self.download('Octopus.zip')
        dotdot = self.root / DOWNLOADS / '..' / 'Octopus.tipa'
        dotdot.write_bytes(b'x')
        for path in (elsewhere, nested, wrong_type, dotdot, self.root / DOWNLOADS / 'missing.tipa'):
            code, lines = self.run_helper('install-update', '--tipa', path)
            self.assertEqual((code, self.errors(lines)), (4, ["The downloaded update wasn't found"]), path)
        self.assertTrue(elsewhere.exists(), 'a refused file is left alone')

    def test_links_are_refused(self):
        self.container('CCCC', 'com.opa334.TrollStore')
        target = self.download('Octopus.tipa', 'var/mobile/Documents')
        link = self.root / DOWNLOADS / 'Octopus-update.tipa'
        link.symlink_to(target)
        self.assertEqual(self.run_helper('install-update', '--tipa', link)[0], 4)
        # A download folder that is itself a link to somewhere else.
        (self.root / TMP_DOWNLOADS).rmdir()
        (self.root / TMP_DOWNLOADS).symlink_to(target.parent)
        self.assertEqual(self.run_helper('install-update', '--tipa', self.root / TMP_DOWNLOADS / 'Octopus.tipa')[0], 4)
        self.assertTrue(target.exists())

    def test_without_trollstore(self):
        self.container('AAAA', 'com.example.other')
        tipa = self.download()
        code, lines = self.run_helper('install-update', '--tipa', tipa)
        self.assertEqual((code, self.errors(lines)), (5, ["TrollStore's installer wasn't found. Is TrollStore installed?"]))
        self.container('CCCC', 'com.opa334.TrollStore', helper=False)   # TrollStore without its installer
        self.assertEqual(self.run_helper('install-update', '--tipa', tipa)[0], 5)

    def test_installer_that_cannot_start_or_fails(self):
        self.container('CCCC', 'com.opa334.TrollStore')
        tipa = self.download()
        code, lines = self.run_helper('install-update', '--tipa', tipa, OCTOPUS_TEST_NOSTART='trollstorehelper')
        self.assertEqual(code, 6)
        self.assertTrue(self.errors(lines)[0].startswith("Couldn't start TrollStore's installer: "))
        self.assertFalse(tipa.exists())
        tipa = self.download()
        code, lines = self.run_helper('install-update', '--tipa', tipa, OCTOPUS_TEST_FAIL='trollstorehelper')
        self.assertEqual((code, self.errors(lines)), (7, ["TrollStore's installer returned 1"]))
        self.assertFalse(tipa.exists())


class SpawnTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.driver = build('spawn', ['Octopus/App/Spawn/oct_spawn.c', 'Octopus/Tests/HelperTests/spawn_driver.c'])

    def spawn(self, *args):
        out = subprocess.run([str(self.driver), *args], capture_output=True).stdout
        head, _, rest = out.partition(b'\n')
        length_line, _, body = rest.partition(b'\n')
        code, length = int(head.split()[1]), int(length_line.split()[1])
        self.assertEqual(len(body), length)
        return code, body.decode('utf-8', 'replace')

    def test_collects_stdout_and_stderr_and_the_exit_code(self):
        code, text = self.spawn('/bin/sh', '-c', 'printf "LOG\\tone\\n"; printf "ERR\\ttwo\\n" >&2; exit 3')
        self.assertEqual(code, 3)
        self.assertEqual(text.splitlines(), ['LOG\tone', 'ERR\ttwo'])

    def test_a_program_that_cannot_start(self):
        code, text = self.spawn('/nonexistent/octopushelper', 'version')
        self.assertEqual(code, -1)
        self.assertTrue(text.startswith("ERR\tCouldn't start the update helper: "), text)

    def test_a_program_killed_by_a_signal(self):
        self.assertEqual(self.spawn('/bin/sh', '-c', 'kill -9 $$')[0], -2)

    def test_output_is_capped(self):
        code, text = self.spawn('/bin/sh', '-c', 'yes x | head -c 600000')
        self.assertEqual((code, len(text)), (0, 262144))

    def test_the_child_gets_a_minimal_environment(self):
        code, text = self.spawn('/bin/sh', '-c', 'echo "$PATH"; echo "${HOME:-none}"')
        self.assertEqual(text.splitlines(), ['/usr/bin:/bin:/usr/sbin:/sbin', 'none'])

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux has no persona spawn; macOS has one, for root only')
    def test_root_needs_persona_spawn(self):
        # Without TrollStore's persona spawn, starting as root is refused rather than run unprivileged.
        code, text = self.spawn('--root', '/bin/sh', '-c', 'echo ran')
        self.assertEqual(code, -1)
        self.assertNotIn('ran', text)
        self.assertIn("can't start the update helper as root", text)


class ZipTests(unittest.TestCase):
    """The diagnostics export's ZIP writer (Octopus/Core/oct_zip.c), read back by an independent reader."""

    @classmethod
    def setUpClass(cls):
        cls.driver = build('zip', ['Octopus/Tests/HelperTests/zip_driver.c', 'Octopus/Core/oct_zip.c',
                                   'Octopus/Protocol/oct_ptp.c', 'Octopus/Core/oct_store.c',
                                   'Octopus/Core/oct_backend_memory.c', 'Octopus/Core/oct_backend_directory.c'])

    def test_python_reads_every_entry(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'octopus-diagnostics.zip'
            self.assertEqual(subprocess.run([str(self.driver), str(path)]).returncode, 0)
            with zipfile.ZipFile(path) as archive:
                self.assertIsNone(archive.testzip())  # every CRC matches
                self.assertEqual(archive.namelist(), ['results.json', 'logs/octopus-debug.log', 'binary.bin',
                                                      'empty.txt', 'caf\u00e9.txt'])
                self.assertEqual(json.loads(archive.read('results.json')), {'ok': True})
                self.assertEqual(archive.read('binary.bin'), bytes((i * 31 + 7) & 0xFF for i in range(70000)))
                self.assertEqual(archive.read('empty.txt'), b'')
                self.assertTrue(archive.getinfo('caf\u00e9.txt').flag_bits & 0x800)  # UTF-8 name
                self.assertEqual(archive.getinfo('empty.txt').date_time, (1980, 1, 1, 0, 0, 0))
                self.assertEqual(archive.getinfo('results.json').date_time[:3], (2025, 11, 8))
            self.assertEqual(sorted(p.name for p in Path(folder).iterdir()), ['octopus-diagnostics.zip'])

    def test_the_archive_falls_back_when_the_preferred_folder_fails(self):
        """The iPhone's case: the app's temporary folder can't be created; the Octopus cache folder works."""
        with tempfile.TemporaryDirectory() as folder:
            blocker = Path(folder) / 'container'
            blocker.write_text('not a folder')
            preferred = blocker / 'tmp'
            fallback = Path(folder) / 'Caches' / 'Octopus' / 'Diagnostics'
            run = subprocess.run([str(self.driver), '--first', str(preferred), str(fallback)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout)
            written, reasons = run.stdout.splitlines()[:2]
            self.assertEqual(written, str(fallback / 'octopus-diagnostics.zip'))
            self.assertIn(str(preferred), reasons)
            self.assertIn('Not a directory', reasons)
            with zipfile.ZipFile(written) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(len(archive.namelist()), 5)
            self.assertEqual([p.name for p in fallback.iterdir()], ['octopus-diagnostics.zip'])

    def test_every_refusal_is_reported_when_no_folder_works(self):
        with tempfile.TemporaryDirectory() as folder:
            blocker = Path(folder) / 'file'
            blocker.write_text('x')
            run = subprocess.run([str(self.driver), '--first', str(blocker / 'a'), str(blocker / 'b')],
                                 capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            reasons = run.stdout.splitlines()[1]
            self.assertIn(str(blocker / 'a'), reasons)
            self.assertIn(str(blocker / 'b'), reasons)
            self.assertEqual(reasons.count('Not a directory'), 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
