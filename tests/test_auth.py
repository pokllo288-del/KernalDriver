import threading
import time
import unittest
import uuid
from unittest import mock

from starlite import auth
from starlite.auth import (Account, AuthError, DeviceCode, MicrosoftAuth, offline_account,
                           offline_uuid, validate_player_name)


class OfflineTest(unittest.TestCase):
    def test_validate(self):
        self.assertIsNone(validate_player_name("Steve_123"))
        self.assertIsNotNone(validate_player_name("ab"))
        self.assertIsNotNone(validate_player_name("a" * 17))
        self.assertIsNotNone(validate_player_name("bad name"))

    def test_offline_uuid_is_v3_and_stable(self):
        u = uuid.UUID(offline_uuid("Steve"))
        self.assertEqual(u.version, 3)
        self.assertEqual(offline_uuid("Steve"), offline_uuid("Steve"))
        self.assertNotEqual(offline_uuid("Steve"), offline_uuid("Alex"))

    def test_offline_account(self):
        acc = offline_account("Steve")
        self.assertEqual((acc.type, acc.name), ("offline", "Steve"))
        with self.assertRaises(AuthError):
            offline_account("x")

    def test_settings_roundtrip_drops_access_token(self):
        acc = Account("microsoft", "Steve", "u", refresh_token="r", access_token="secret")
        data = acc.to_settings()
        self.assertNotIn("access_token", data)
        self.assertEqual(Account.from_settings(data).refresh_token, "r")
        self.assertIsNone(Account.from_settings({"type": "bogus", "name": "x"}))


class MicrosoftFlowTest(unittest.TestCase):
    def test_requires_client_id(self):
        with self.assertRaises(AuthError):
            MicrosoftAuth("")

    def _responses(self, profile_status=200):
        return {
            auth.MS_AUTHORITY + "/token": (200, {"access_token": "ms", "refresh_token": "rt"}),
            auth.XBL_AUTH_URL: (200, {"Token": "xbl", "DisplayClaims": {"xui": [{"uhs": "hash"}]}}),
            auth.XSTS_AUTH_URL: (200, {"Token": "xsts"}),
            auth.MC_LOGIN_URL: (200, {"access_token": "mc"}),
            auth.MC_PROFILE_URL: (profile_status, {"id": "069a79f444e94726a5befca90e38aaf5",
                                                   "name": "Notch"}),
        }

    def _run(self, responses):
        calls = []

        def fake(url, **kw):
            calls.append((url, kw))
            return responses[url]

        code = DeviceCode("ABCD", "https://microsoft.com/link", "dc", 0, time.time() + 60)
        with mock.patch.object(auth, "_request", side_effect=fake):
            return MicrosoftAuth("cid").wait(code, threading.Event()), calls

    def test_full_chain(self):
        acc, calls = self._run(self._responses())
        self.assertEqual((acc.type, acc.name, acc.refresh_token), ("microsoft", "Notch", "rt"))
        self.assertEqual(acc.uuid, "069a79f4-44e9-4726-a5be-fca90e38aaf5")
        xbl_body = dict(calls)[auth.XBL_AUTH_URL]["body"]
        self.assertEqual(xbl_body["Properties"]["RpsTicket"], "d=ms")
        self.assertEqual(dict(calls)[auth.MC_LOGIN_URL]["body"]["identityToken"], "XBL3.0 x=hash;xsts")

    def test_not_owned(self):
        with self.assertRaisesRegex(AuthError, "doesn't own"):
            self._run(self._responses(profile_status=404))

    def test_no_xbox_profile(self):
        r = self._responses()
        r[auth.XSTS_AUTH_URL] = (401, {"XErr": 2148916233})
        with self.assertRaisesRegex(AuthError, "no Xbox profile"):
            self._run(r)


if __name__ == "__main__":
    unittest.main()
