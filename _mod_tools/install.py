"""Install and remove mods from the BlitzForge registry.

    python install.py list
    python install.py install <id>
    python install.py remove  <id>

Patches are built against the pristine client file, so two mods that edit the
same resource cannot both be applied: the second would be diffed against stock
and would quietly erase the first. Installing records which mod owns which
file, and a conflicting install is refused rather than allowed to win.
"""
from __future__ import annotations

import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

import modpack
import patch_dvpl
import registry

HERE = Path(__file__).resolve().parent
LEDGER = HERE / "cache" / "installed.json"
DOWNLOADS = HERE / "cache" / "artifacts"


def load_ledger() -> dict:
    if not LEDGER.exists():
        return {}
    try:
        return json.loads(LEDGER.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def save_ledger(ledger: dict) -> None:
    LEDGER.parent.mkdir(parents=True, exist_ok=True)
    LEDGER.write_text(json.dumps(ledger, ensure_ascii=False, indent=2),
                      encoding="utf-8")


def other_owner(ledger: dict, target: str, installing: str) -> str | None:
    """Any mod other than `installing` that already owns this file.

    Returning the first owner found is not enough: reinstalling a mod puts its
    own id in the ledger, and if that id is returned the caller reads it as
    "no conflict" and a genuine second owner goes unnoticed.
    """
    for mod_id, record in ledger.items():
        if mod_id != installing and target in record.get("targets", []):
            return mod_id
    return None


def download(url: str, expected_sha: str) -> Path:
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    destination = DOWNLOADS / Path(url).name

    if destination.exists() and modpack.sha256(destination.read_bytes()) == expected_sha:
        print(f"  cached  {destination.name}")
        return destination

    print(f"  fetching {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "blitzforge-tools"})
    with urllib.request.urlopen(request, timeout=30) as response:
        payload = response.read()

    actual = modpack.sha256(payload)
    if actual != expected_sha:
        # Never unpack an artifact whose bytes are not the reviewed bytes.
        raise SystemExit(
            f"artifact hash mismatch, refusing it\n"
            f"  registry says {expected_sha}\n"
            f"  downloaded    {actual}")

    destination.write_bytes(payload)
    print(f"  verified {len(payload)} B, sha256 ok")
    return destination


def find_mod(mod_id: str) -> dict:
    mods, source = registry.fetch()
    print(f"registry: {source}")
    for mod in mods:
        if mod["id"] == mod_id:
            return mod
    raise SystemExit(f"no mod {mod_id!r} in the registry")


def install(mod_id: str) -> None:
    mod = find_mod(mod_id)
    artifact_info = mod.get("artifact")
    if not artifact_info:
        raise SystemExit(
            f"{mod_id} is metadata only; the registry has no artifact for it yet")

    ledger = load_ledger()
    archive = download(artifact_info["url"], artifact_info["sha256"])
    metadata = modpack.read_metadata(archive)
    targets = [patch["target"] for patch in metadata["patches"]] + metadata["files"]

    for target in targets:
        other = other_owner(ledger, target, mod_id)
        if other:
            raise SystemExit(
                f"{target}\n  is already patched by {other!r}.\n"
                f"Remove it first: python install.py remove {other}")

    modpack.apply(archive)
    ledger[mod_id] = {
        "version": metadata["version"],
        "artifact_sha256": artifact_info["sha256"],
        "targets": targets,
    }
    save_ledger(ledger)
    print(f"installed {mod_id} {metadata['version']} ({len(targets)} file(s))")


def remove(mod_id: str) -> None:
    ledger = load_ledger()
    record = ledger.get(mod_id)
    if not record:
        raise SystemExit(f"{mod_id} is not installed")

    for target in record["targets"]:
        _, backup = patch_dvpl._slots(target)
        if backup.exists():
            patch_dvpl.restore(target)
        else:
            live, _ = patch_dvpl._slots(target)
            if live.exists():
                live.unlink()
                print(f"removed added file {target}")

    del ledger[mod_id]
    save_ledger(ledger)
    print(f"removed {mod_id}")


def show() -> None:
    ledger = load_ledger()
    mods, source = registry.fetch()
    print(f"registry: {source}   installed: {len(ledger)}\n")
    for mod in mods:
        record = ledger.get(mod["id"])
        if record:
            state = f"installed {record['version']}"
        elif mod.get("artifact"):
            state = "available"
        else:
            state = "no artifact"
        print(f"  {mod['id']:16} {mod['version']:8} {mod['type']:9} {state}")

    unknown = set(ledger) - {mod["id"] for mod in mods}
    for mod_id in sorted(unknown):
        print(f"  {mod_id:16} {ledger[mod_id]['version']:8} {'?':9} "
              f"installed, no longer in the registry")


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    command = sys.argv[1]
    if command == "list":
        show()
    elif command == "install":
        install(sys.argv[2])
    elif command == "remove":
        remove(sys.argv[2])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
