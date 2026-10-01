"""Player accounts: an offline player name, or a Microsoft account.

Microsoft sign-in uses the OAuth 2.0 device-code flow (the user opens
microsoft.com/link and types a short code), then exchanges the token through
Xbox Live and XSTS for a Minecraft services token and profile:

    Microsoft  ->  Xbox Live (XBL)  ->  XSTS  ->  Minecraft services  ->  profile

It needs an Azure app (client) ID that Mojang has approved for the Minecraft
API. Set it with the ``STARLITE_MS_CLIENT_ID`` environment variable or the
``ms_client_id`` key in ``launcher.json``.
"""

from __future__ import annotations

import hashlib
import json
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import asdict, dataclass
from typing import Optional

from .minecraft import USER_AGENT

MS_AUTHORITY = "https://login.microsoftonline.com/consumers/oauth2/v2.0"
MS_SCOPE = "XboxLive.signin offline_access"
XBL_AUTH_URL = "https://user.auth.xboxlive.com/user/authenticate"
XSTS_AUTH_URL = "https://xsts.auth.xboxlive.com/xsts/authorize"
MC_LOGIN_URL = "https://api.minecraftservices.com/authentication/login_with_xbox"
MC_PROFILE_URL = "https://api.minecraftservices.com/minecraft/profile"

PLAYER_NAME_RE = re.compile(r"^[A-Za-z0-9_]{3,16}$")

XSTS_ERRORS = {
    2148916233: "This Microsoft account has no Xbox profile. Sign in once at xbox.com to create one.",
    2148916235: "Xbox Live is not available in your country/region.",
    2148916236: "This account needs adult verification (South Korea).",
    2148916237: "This account needs adult verification (South Korea).",
    2148916238: "This is a child account. An adult must add it to a Microsoft family first.",
}


class AuthError(Exception):
    """Sign-in failed; the message is shown to the user."""


class AuthCancelled(Exception):
    """The user cancelled sign-in."""


@dataclass
class Account:
    type: str  # "offline" or "microsoft"
    name: str
    uuid: str
    refresh_token: Optional[str] = None
    access_token: Optional[str] = None  # Minecraft services token, kept in memory only

    def to_settings(self) -> dict:
        data = asdict(self)
        data.pop("access_token")
        return {k: v for k, v in data.items() if v is not None}

    @classmethod
    def from_settings(cls, data: Optional[dict]) -> Optional["Account"]:
        if not data or data.get("type") not in ("offline", "microsoft") or not data.get("name"):
            return None
        return cls(type=data["type"], name=data["name"], uuid=data.get("uuid", ""),
                   refresh_token=data.get("refresh_token"))


# -- offline player name ------------------------------------------------------

def validate_player_name(name: str) -> Optional[str]:
    """Return an error message, or None if the name is a valid Minecraft name."""
    if not 3 <= len(name) <= 16:
        return "Name must be 3-16 characters."
    if not PLAYER_NAME_RE.match(name):
        return "Use only letters, numbers and _."
    return None


def offline_uuid(name: str) -> str:
    """Same UUID the game uses for offline players: UUID v3 of ``OfflinePlayer:<name>``."""
    digest = bytearray(hashlib.md5(f"OfflinePlayer:{name}".encode("utf-8")).digest())
    digest[6] = (digest[6] & 0x0F) | 0x30
    digest[8] = (digest[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(digest)))


def offline_account(name: str) -> Account:
    error = validate_player_name(name)
    if error:
        raise AuthError(error)
    return Account(type="offline", name=name, uuid=offline_uuid(name))


# -- Microsoft account --------------------------------------------------------

@dataclass
class DeviceCode:
    user_code: str
    verification_uri: str
    device_code: str
    interval: int
    expires_at: float


def _request(url: str, *, form: Optional[dict] = None, body: Optional[dict] = None,
             token: Optional[str] = None) -> tuple[int, dict]:
    headers = {"User-Agent": USER_AGENT, "Accept": "application/json"}
    data = None
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    elif body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            raw = response.read()
            return response.status, json.loads(raw) if raw else {}
    except urllib.error.HTTPError as err:
        raw = err.read()
        try:
            return err.code, json.loads(raw) if raw else {}
        except ValueError:
            return err.code, {}
    except urllib.error.URLError as err:
        raise AuthError(f"Network error: {err.reason}") from err


def _short(description: str) -> str:
    """First sentence of an Azure error, without trace/correlation IDs."""
    return description.splitlines()[0].split(" Trace ID:")[0]


class MicrosoftAuth:
    def __init__(self, client_id: str):
        if not client_id:
            raise AuthError("Microsoft sign-in isn't configured (no client ID). See README.")
        self.client_id = client_id

    # Step 1: device code
    def start(self) -> DeviceCode:
        status, data = _request(f"{MS_AUTHORITY}/devicecode",
                                form={"client_id": self.client_id, "scope": MS_SCOPE})
        if status != 200:
            raise AuthError(_short(data.get("error_description", "Could not start Microsoft sign-in.")))
        return DeviceCode(data["user_code"], data["verification_uri"], data["device_code"],
                          int(data.get("interval", 5)), time.time() + int(data["expires_in"]))

    # Step 2: wait for the user to approve in the browser
    def wait(self, code: DeviceCode, cancel: threading.Event) -> Account:
        interval = code.interval
        while time.time() < code.expires_at:
            if cancel.wait(interval):
                raise AuthCancelled()
            status, data = _request(f"{MS_AUTHORITY}/token", form={
                "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
                "client_id": self.client_id,
                "device_code": code.device_code,
            })
            if status == 200:
                return self._minecraft_login(data["access_token"], data.get("refresh_token"))
            error = data.get("error")
            if error == "authorization_pending":
                continue
            if error == "slow_down":
                interval += 5
                continue
            if error == "authorization_declined":
                raise AuthError("Sign-in was declined.")
            if error == "expired_token":
                break
            raise AuthError(_short(data.get("error_description", "Microsoft sign-in failed.")))
        raise AuthError("The sign-in code expired. Please try again.")

    # Silent sign-in with a stored refresh token
    def refresh(self, refresh_token: str) -> Account:
        status, data = _request(f"{MS_AUTHORITY}/token", form={
            "grant_type": "refresh_token",
            "client_id": self.client_id,
            "refresh_token": refresh_token,
            "scope": MS_SCOPE,
        })
        if status != 200:
            raise AuthError("Your Microsoft session expired. Please sign in again.")
        return self._minecraft_login(data["access_token"], data.get("refresh_token", refresh_token))

    # Steps 3-6: Xbox Live -> XSTS -> Minecraft -> profile
    def _minecraft_login(self, ms_token: str, refresh_token: Optional[str]) -> Account:
        status, xbl = _request(XBL_AUTH_URL, body={
            "Properties": {"AuthMethod": "RPS", "SiteName": "user.auth.xboxlive.com",
                           "RpsTicket": f"d={ms_token}"},
            "RelyingParty": "http://auth.xboxlive.com",
            "TokenType": "JWT",
        })
        if status != 200:
            raise AuthError("Xbox Live sign-in failed.")
        user_hash = xbl["DisplayClaims"]["xui"][0]["uhs"]

        status, xsts = _request(XSTS_AUTH_URL, body={
            "Properties": {"SandboxId": "RETAIL", "UserTokens": [xbl["Token"]]},
            "RelyingParty": "rp://api.minecraftservices.com/",
            "TokenType": "JWT",
        })
        if status != 200:
            raise AuthError(XSTS_ERRORS.get(xsts.get("XErr"), "Xbox Live authorization failed."))

        status, mc = _request(MC_LOGIN_URL, body={
            "identityToken": f"XBL3.0 x={user_hash};{xsts['Token']}",
        })
        if status != 200:
            if status == 403:
                raise AuthError("This app ID isn't approved for the Minecraft API yet. See README.")
            raise AuthError("Minecraft sign-in failed.")

        status, profile = _request(MC_PROFILE_URL, token=mc["access_token"])
        if status == 404:
            raise AuthError("This Microsoft account doesn't own Minecraft: Java Edition.")
        if status != 200:
            raise AuthError("Could not load your Minecraft profile.")

        raw_id = profile["id"]
        return Account(type="microsoft", name=profile["name"], uuid=str(uuid.UUID(raw_id)),
                       refresh_token=refresh_token, access_token=mc["access_token"])
