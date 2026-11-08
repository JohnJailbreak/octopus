"""Checks that keep the repository honest, as Speaka's tests/test_build.py does: the README's links, anchors and
"Repository layout" match the files; the changelog has an entry for the version being built; and the values the update
path repeats across Swift, C and the project file agree. Run by `make test`."""
import plistlib
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def read(path):
    return (ROOT / path).read_text(encoding='utf-8')


def marketing_version():
    match = re.search(r'^\s*MARKETING_VERSION:\s*"?([0-9][0-9.]*)"?', read('project.yml'), re.M)
    assert match, 'MARKETING_VERSION is missing from project.yml'
    return match.group(1)


def github_anchor(heading):
    return re.sub(r'[^a-z0-9 _-]', '', heading.strip().lower()).replace(' ', '-')


class ReadmeTests(unittest.TestCase):
    def setUp(self):
        self.readme = read('README.md')

    def test_links_resolve(self):
        for target in re.findall(r'(?:\]\(|src="|href=")([^)"#]+?)(?:#[^)"]*)?[)"]', self.readme):
            if target.startswith(('http://', 'https://', 'mailto:')):
                continue
            self.assertTrue((ROOT / target).exists(), f'README links to {target}, which does not exist')

    def test_anchors_resolve(self):
        anchors = {github_anchor(h) for h in re.findall(r'^#{2,6} (.+)$', self.readme, re.M)}
        for anchor in re.findall(r'(?:\]\(|href=")#([^)"]+)', self.readme):
            self.assertIn(anchor, anchors, f'README links to #{anchor}, which no heading has')

    def test_repository_layout_exists(self):
        block = re.search(r'## Repository layout\n\n```text\n(.*?)```', self.readme, re.S)
        self.assertIsNotNone(block, 'README has no Repository layout tree')
        stack = []
        for line in block.group(1).splitlines():
            match = re.match(r'^((?:│   |    )*)[├└]── (\S.*?)(?:\s{2,}.*)?$', line)
            if not match:
                continue
            depth = len(match.group(1)) // 4
            names = [name.strip() for name in match.group(2).split(',')]
            stack = stack[:depth] + [names[0].rstrip('/')]
            for name in names:
                path = ROOT.joinpath(*stack[:-1], name.rstrip('/'))
                self.assertTrue(path.exists(), f'README\'s layout names {path.relative_to(ROOT)}, which does not exist')

    def test_badges_and_links_name_this_repository(self):
        for repo in re.findall(r'github\.com/([\w.-]+/[\w.-]+?)/(?:releases|actions)', self.readme):
            self.assertEqual(repo.lower(), 'm9z2w4d8vw-blip/octopus')


class ChangelogTests(unittest.TestCase):
    def test_the_version_being_built_has_an_entry(self):
        version = marketing_version()
        self.assertIn(f'\n## v{version}\n', read('docs/CHANGELOG.md'),
                      f'docs/CHANGELOG.md needs a "## v{version}" entry for MARKETING_VERSION {version}')


class UpdatePathTests(unittest.TestCase):
    def test_download_folders_match_the_helper(self):
        swift = re.search(r'static let downloadFolders = \[(.*?)\]', read('Octopus/App/Updater.swift')).group(1)
        helper = re.search(r'k_update_dirs\[\] = \{(.*?)\};', read('Octopus/RootHelper/main.c'), re.S).group(1)
        self.assertEqual(re.findall(r'"([^"]+)"', swift), re.findall(r'"([^"]+)"', helper))

    def test_background_task_is_permitted(self):
        task = re.search(r'static let taskID = "([^"]+)"', read('Octopus/App/UpdateNotifications.swift')).group(1)
        self.assertRegex(read('project.yml'), rf'BGTaskSchedulerPermittedIdentifiers:\n\s+- {re.escape(task)}\n')

    def test_the_updater_asks_this_repository(self):
        updater = read('Octopus/App/Updater.swift')
        api = re.search(r'api\.github\.com/repos/([^/]+/[^/]+)/releases', updater).group(1)
        page = re.search(r'https://github\.com/([^/]+/[^/]+)/releases"', updater).group(1)
        repository = re.search(r'repository = URL\(string: "https://github\.com/([^"]+)"\)', read('Octopus/App/Theme.swift')).group(1)
        self.assertEqual({api, page, repository}, {'m9z2w4d8vw-blip/octopus'})

    def test_entitlements_allow_starting_the_helper_as_root(self):
        app = plistlib.loads((ROOT / 'Octopus/Octopus.entitlements').read_bytes())
        helper = plistlib.loads((ROOT / 'Octopus/RootHelper/entitlements.plist').read_bytes())
        for key in ('com.apple.private.persona-mgmt', 'com.apple.private.security.no-sandbox', 'platform-application'):
            self.assertIs(app.get(key), True, key)
        self.assertIs(helper.get('com.apple.private.security.storage.AppBundles'), True)
        package = read('tools/package.sh')
        self.assertIn('ldid -SOctopus/Octopus.entitlements "$APP/Octopus"', package)
        self.assertIn('ldid -SOctopus/RootHelper/entitlements.plist "$APP/octopushelper"', package)

    def test_the_helper_is_where_the_updater_starts_it(self):
        self.assertIn('Bundle.main.bundlePath + "/octopushelper"', read('Octopus/App/Updater.swift'))
        self.assertIn('cp build/octopushelper "$APP/octopushelper"', read('tools/package.sh'))


class WindowsScriptTests(unittest.TestCase):
    """The two defects the v0.3.6 report showed (tools/windows/octopus-usb-diagnostics.ps1)."""

    script = (ROOT / 'tools' / 'windows' / 'octopus-usb-diagnostics.ps1').read_text(encoding='utf-8')

    def test_absent_properties_are_checked_before_use(self):
        self.assertNotIn('$property.Data', self.script)  # threw under StrictMode when a device lacked the property
        self.assertIn("$property.PSObject.Properties['Data']", self.script)
        self.assertIn('$value = $null', self.script)

    def test_only_the_iphone_on_usb_is_selected(self):
        self.assertIn("-match '^USB\\\\VID_05AC&'", self.script)
        self.assertNotIn("FriendlyName -match 'Apple", self.script)  # matched a Bluetooth mouse and the PPTP miniport
        self.assertNotRegex(self.script, r"-match '[^']*\bPTP\b[^']*'")


if __name__ == '__main__':
    unittest.main(verbosity=2)
