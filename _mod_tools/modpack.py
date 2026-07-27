"""The BlitzForge artifact format: build, inspect and apply a mod package.

An artifact is a zip:

    blitzforge.json     what the mod changes, and the hashes that prove it
    patches/*.diff      unified diffs against client resources
    files/**            whole files the mod author owns
    native/*.dll        native mod binaries

Two rules shape the format.

*Patches carry no original content.* Diffs are generated with zero context
lines, so a patch holds only the lines the mod adds and removes. A mod that
edits Hangar.yaml therefore ships its own additions, never a copy of
Wargaming's file. Whole-file entries under files/ are for content the author
actually owns.

*A patch names the file it was made against.* Every patch records the sha256
of the pristine original and of the expected result. Applying to a client
whose file has changed - a game update, another mod - fails loudly instead of
producing something that is neither.

    python modpack.py build   <source-dir> <out.zip>
    python modpack.py inspect <artifact.zip>
    python modpack.py apply   <artifact.zip>
"""
from __future__ import annotations

import difflib
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path

import patch_dvpl
from dvpl import unpack

HERE = Path(__file__).resolve().parent
SCHEMA = 1
HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


# ----------------------------------------------------------------- line model

def to_lines(data: bytes) -> list[str]:
    """Split on LF only.

    Client resources are CRLF, and splitting on b"\\n" leaves the CR at the end
    of each line where join puts it back untouched. Round-tripping is exact,
    which matters because the result is hash-checked.
    """
    return [line.decode("utf-8") for line in data.split(b"\n")]


def from_lines(lines: list[str]) -> bytes:
    return b"\n".join(line.encode("utf-8") for line in lines)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------- diffs

def make_diff(old: bytes, new: bytes, target: str) -> str:
    return "\n".join(difflib.unified_diff(
        to_lines(old), to_lines(new),
        fromfile=f"a/{target}", tofile=f"b/{target}",
        n=0, lineterm="")) + "\n"


def apply_diff(old: bytes, diff: str) -> bytes:
    """Apply a zero-context unified diff.

    Removed lines are checked against the source as they are consumed. The
    hash check afterwards would catch a mismatch anyway, but failing at the
    offending hunk says which edit did not fit.
    """
    lines = to_lines(old)
    result: list[str] = []
    cursor = 0

    diff_lines = diff.split("\n")
    index = 0
    while index < len(diff_lines):
        line = diff_lines[index]
        match = HUNK.match(line)
        if not match:
            index += 1
            continue

        old_start = int(match.group(1))
        old_count = int(match.group(2) or 1)
        # With a count of zero the header points at the line to insert after,
        # so the cut sits past it; otherwise it is a 1-based line number.
        cut = old_start if old_count == 0 else old_start - 1
        if cut < cursor:
            raise ValueError(f"hunk out of order at line {old_start}")
        result.extend(lines[cursor:cut])

        index += 1
        removed = 0
        while index < len(diff_lines):
            body = diff_lines[index]
            if body.startswith("-"):
                expected = body[1:]
                position = cut + removed
                if position >= len(lines) or lines[position] != expected:
                    found = lines[position] if position < len(lines) else "<eof>"
                    raise ValueError(
                        f"patch does not fit at line {position + 1}: "
                        f"expected {expected!r}, found {found!r}")
                removed += 1
            elif body.startswith("+"):
                result.append(body[1:])
            else:
                break
            index += 1

        if removed != old_count:
            raise ValueError(
                f"hunk at {old_start} declares {old_count} removals, has {removed}")
        cursor = cut + old_count

    result.extend(lines[cursor:])
    return from_lines(result)


# ------------------------------------------------------------------ artifacts

def _pristine(target: str) -> bytes:
    """The untouched client file, whatever is installed right now."""
    live, backup = patch_dvpl._slots(target)
    source = backup if backup.exists() else live
    if not source.exists():
        raise SystemExit(f"no such client resource: {target}")
    return unpack(source.read_bytes())


def build(source_dir: Path, out_path: Path) -> None:
    """Package a mod.

    source_dir holds manifest.yaml plus a patched/ tree whose paths mirror
    Data/. Each patched file is diffed against the pristine original.
    """
    manifest_path = source_dir / "manifest.yaml"
    if not manifest_path.exists():
        raise SystemExit(f"no manifest.yaml in {source_dir}")

    from build_catalog import read_manifest
    manifest = read_manifest(manifest_path)

    patches, files = [], []
    diffs: dict[str, str] = {}

    patched_root = source_dir / "patched"
    if patched_root.is_dir():
        for path in sorted(patched_root.rglob("*")):
            if not path.is_file():
                continue
            target = path.relative_to(patched_root).as_posix()
            original = _pristine(target)
            modified = path.read_bytes()
            if original == modified:
                print(f"  skipped (identical): {target}")
                continue
            diff = make_diff(original, modified, target)
            if apply_diff(original, diff) != modified:
                raise SystemExit(f"diff does not reproduce {target}; refusing to ship it")
            name = f"patches/{len(patches)}.diff"
            diffs[name] = diff
            patches.append({
                "target": target,
                "diff": name,
                "source_sha256": sha256(original),
                "result_sha256": sha256(modified),
            })
            print(f"  patch: {target}")

    files_root = source_dir / "files"
    if files_root.is_dir():
        for path in sorted(files_root.rglob("*")):
            if path.is_file():
                files.append(path.relative_to(files_root).as_posix())

    metadata = {
        "schema": SCHEMA,
        "id": manifest["id"],
        "version": manifest.get("version", "0.0.0"),
        "type": manifest.get("type", "resource"),
        "patches": patches,
        "files": files,
    }

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("blitzforge.json",
                         json.dumps(metadata, ensure_ascii=False, indent=2))
        for name, diff in diffs.items():
            archive.writestr(name, diff)
        for relative in files:
            archive.write(files_root / relative, f"files/{relative}")

    data = out_path.read_bytes()
    print(f"built {out_path.name}: {len(patches)} patch(es), {len(files)} file(s), "
          f"{len(data)} B\n  sha256 {sha256(data)}")


def read_metadata(artifact: Path) -> dict:
    with zipfile.ZipFile(artifact) as archive:
        metadata = json.loads(archive.read("blitzforge.json").decode("utf-8"))
    if metadata.get("schema") != SCHEMA:
        raise SystemExit(f"unsupported artifact schema: {metadata.get('schema')}")
    return metadata


def inspect(artifact: Path) -> None:
    metadata = read_metadata(artifact)
    print(f"{metadata['id']} {metadata['version']} ({metadata['type']})")
    print(f"sha256 {sha256(artifact.read_bytes())}")
    with zipfile.ZipFile(artifact) as archive:
        for patch in metadata["patches"]:
            diff = archive.read(patch["diff"]).decode("utf-8")
            added = sum(1 for l in diff.split("\n") if l.startswith("+") and l[1:2] != "+")
            removed = sum(1 for l in diff.split("\n") if l.startswith("-") and l[1:2] != "-")
            print(f"  patch {patch['target']}  +{added} -{removed}")
    for name in metadata["files"]:
        print(f"  file  {name}")


def apply(artifact: Path) -> dict:
    """Apply an artifact to the client. Every patch is checked before any is written."""
    metadata = read_metadata(artifact)
    staged: list[tuple[str, bytes]] = []

    with zipfile.ZipFile(artifact) as archive:
        for patch in metadata["patches"]:
            target = patch["target"]
            original = _pristine(target)
            if sha256(original) != patch["source_sha256"]:
                raise SystemExit(
                    f"{target} is not the file this mod was built against.\n"
                    f"  expected {patch['source_sha256'][:16]}…\n"
                    f"  found    {sha256(original)[:16]}…\n"
                    f"The client was updated or another mod owns this file.")
            diff = archive.read(patch["diff"]).decode("utf-8")
            result = apply_diff(original, diff)
            if sha256(result) != patch["result_sha256"]:
                raise SystemExit(f"{target}: patched result does not match its hash")
            staged.append((target, result))

        # Nothing is written until every patch has been verified, so a mod that
        # fails halfway cannot leave the client half-modified.
        for target, result in staged:
            temporary = HERE / "work" / f"_apply_{Path(target).name}"
            temporary.parent.mkdir(parents=True, exist_ok=True)
            temporary.write_bytes(result)
            patch_dvpl.install(target, temporary)
            temporary.unlink()

        for relative in metadata["files"]:
            destination = HERE.parent / "Data" / relative
            payload = archive.read(f"files/{relative}")
            temporary = HERE / "work" / f"_file_{Path(relative).name}"
            temporary.parent.mkdir(parents=True, exist_ok=True)
            temporary.write_bytes(payload)
            live, _ = patch_dvpl._slots(relative)
            (patch_dvpl.install if live.exists() else patch_dvpl.create)(relative, temporary)
            temporary.unlink()

    return metadata


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    command = sys.argv[1]
    if command == "build":
        build(Path(sys.argv[2]), Path(sys.argv[3]))
    elif command == "inspect":
        inspect(Path(sys.argv[2]))
    elif command == "apply":
        apply(Path(sys.argv[2]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
