import unittest
from pathlib import Path

from starlite.minecraft import MinecraftInstaller, rules_allow


class RulesTest(unittest.TestCase):
    def test_no_rules_allows(self):
        self.assertTrue(rules_allow(None, "windows", "x86_64"))

    def test_os_allow_only(self):
        rules = [{"action": "allow", "os": {"name": "osx"}}]
        self.assertTrue(rules_allow(rules, "osx", "arm64"))
        self.assertFalse(rules_allow(rules, "windows", "x86_64"))

    def test_disallow_overrides(self):
        rules = [{"action": "allow"}, {"action": "disallow", "os": {"name": "osx"}}]
        self.assertTrue(rules_allow(rules, "linux", "x86_64"))
        self.assertFalse(rules_allow(rules, "osx", "x86_64"))

    def test_arch(self):
        rules = [{"action": "allow", "os": {"name": "windows", "arch": "arm64"}}]
        self.assertTrue(rules_allow(rules, "windows", "arm64"))
        self.assertFalse(rules_allow(rules, "windows", "x86_64"))


class CollectTasksTest(unittest.TestCase):
    def test_collects_client_libs_assets_logging(self):
        inst = MinecraftInstaller(Path("/tmp/x"))
        inst.os_name, inst.arch = "windows", "x86_64"
        version = {
            "id": "1.0",
            "downloads": {"client": {"url": "u/c", "sha1": "a", "size": 1}},
            "libraries": [
                {"downloads": {"artifact": {"url": "u/l1", "path": "l/1.jar", "sha1": "b", "size": 2}}},
                {"downloads": {"artifact": {"url": "u/l2", "path": "l/2.jar", "sha1": "c", "size": 3}},
                 "rules": [{"action": "allow", "os": {"name": "osx"}}]},
            ],
            "logging": {"client": {"file": {"id": "log.xml", "url": "u/log", "sha1": "d", "size": 4}}},
        }
        assets = {"objects": {"a": {"hash": "ab" + "0" * 38, "size": 5},
                              "dup": {"hash": "ab" + "0" * 38, "size": 5}}}
        tasks = inst.collect_tasks(version, assets)
        urls = sorted(t.url for t in tasks)
        self.assertEqual(len(tasks), 4)  # client, 1 windows lib, 1 unique asset, log config
        self.assertIn("u/l1", urls)
        self.assertNotIn("u/l2", urls)


if __name__ == "__main__":
    unittest.main()
