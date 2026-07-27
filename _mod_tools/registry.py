"""Client for the BlitzForge mod registry.

The catalogue screen is generated from whatever this returns, so a registry
that is unreachable must not stop the hangar from building. Every successful
response is cached to disk and the cache is used when the network fails.

    python registry.py                 # fetch and show what the registry has
    python registry.py --offline       # read the cache only
"""
from __future__ import annotations

import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
CACHE = HERE / "cache" / "registry.json"
DEFAULT_URL = "https://backend-pseudojoker-1s-projects.vercel.app"
TIMEOUT = 10


class RegistryError(RuntimeError):
    pass


def _get(url: str) -> dict:
    request = urllib.request.Request(
        url, headers={"Accept": "application/json", "User-Agent": "blitzforge-tools"})
    with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
        if response.status != 200:
            raise RegistryError(f"{url} -> HTTP {response.status}")
        return json.loads(response.read().decode("utf-8"))


def _read_cache() -> dict | None:
    if not CACHE.exists():
        return None
    try:
        return json.loads(CACHE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def _write_cache(payload: dict) -> None:
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    # Write to a temporary file and replace, so an interrupted run cannot
    # leave a half-written cache that the next offline build would trust.
    temporary = CACHE.with_suffix(".tmp")
    temporary.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                         encoding="utf-8")
    temporary.replace(CACHE)


def fetch(base_url: str = DEFAULT_URL, offline: bool = False) -> tuple[list[dict], str]:
    """Return (mods, source) where source is 'registry', 'cache' or 'empty'."""
    if not offline:
        try:
            payload = _get(base_url.rstrip("/") + "/api/mods")
            if not isinstance(payload.get("mods"), list):
                raise RegistryError("response has no mods array")
            _write_cache(payload)
            return payload["mods"], "registry"
        except (urllib.error.URLError, OSError, json.JSONDecodeError,
                RegistryError) as error:
            print(f"registry unreachable ({error}); falling back to cache",
                  file=sys.stderr)

    cached = _read_cache()
    if cached and isinstance(cached.get("mods"), list):
        return cached["mods"], "cache"
    return [], "empty"


def main() -> None:
    mods, source = fetch(offline="--offline" in sys.argv)
    print(f"source: {source}   mods: {len(mods)}")
    for mod in mods:
        artifact = mod.get("artifact")
        state = "artifact" if artifact else "metadata only"
        print(f"  {mod['id']:16} {mod['version']:8} {mod['type']:9} {state}")


if __name__ == "__main__":
    main()
