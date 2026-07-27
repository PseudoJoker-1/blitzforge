"""Executes install and remove requests made from inside the game.

The actions language the catalogue screen is written in has no file or network
access - its whole vocabulary is UI and animation - so the buttons cannot run
the installer themselves. What they can do is write a line to the client log.
This tails that log and carries the request out.

    python agent.py            # follow the log and act on requests
    python agent.py --once     # process what is already there and exit
    python agent.py --probe    # report whether the log channel works at all

A request names a card index rather than a mod id, because the index is what
the button has. cache/catalog_index.json maps it back, and is written by the
same build_catalog run that laid the cards out, so the two cannot disagree.
"""
from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path

import install

HERE = Path(__file__).resolve().parent
INDEX = HERE / "cache" / "catalog_index.json"
STATE = HERE / "cache" / "agent_state.json"
LOGS = (Path.home() / "AppData" / "Local" / "wotblitz" / "DAVAProject")
REQUEST = re.compile(r"BLITZFORGE:(install|remove):(\d+)")
POLL_SECONDS = 2


def newest_log() -> Path | None:
    logs = sorted(LOGS.glob("blitz-logs_*.txt"),
                  key=lambda p: p.stat().st_mtime, reverse=True)
    return logs[0] if logs else None


def load_index() -> list[str]:
    if not INDEX.exists():
        return []
    try:
        return json.loads(INDEX.read_text(encoding="utf-8"))["order"]
    except (OSError, json.JSONDecodeError, KeyError):
        return []


def load_state() -> dict:
    if not STATE.exists():
        return {}
    try:
        return json.loads(STATE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def save_state(state: dict) -> None:
    STATE.parent.mkdir(parents=True, exist_ok=True)
    STATE.write_text(json.dumps(state, indent=2), encoding="utf-8")


def handle(verb: str, index: int) -> None:
    order = load_index()
    if index >= len(order):
        print(f"  request for card {index}, but the catalogue has {len(order)}; "
              f"rebuild it so the index matches")
        return
    mod_id = order[index]
    print(f"  {verb} {mod_id}")
    try:
        if verb == "install":
            install.install(mod_id)
        else:
            install.remove(mod_id)
    except SystemExit as error:
        # A refused install is a normal outcome - a hash mismatch, a conflict -
        # and must not take the agent down with it.
        print(f"  refused: {' '.join(str(error).split())}")


def scan(path: Path, offset: int, act: bool = True) -> tuple[int, int]:
    """Read from offset, handle any requests, return (new offset, count)."""
    with path.open("r", encoding="utf-8", errors="replace") as handle_:
        handle_.seek(offset)
        text = handle_.read()
        new_offset = handle_.tell()

    found = 0
    for verb, index in REQUEST.findall(text):
        found += 1
        if act:
            handle(verb, int(index))
    return new_offset, found


def probe() -> int:
    log = newest_log()
    if not log:
        print(f"no client log under {LOGS}")
        return 1
    text = log.read_text(encoding="utf-8", errors="replace")
    hits = REQUEST.findall(text)
    print(f"log: {log.name} ({len(text)} chars)")
    if hits:
        print(f"channel works: {len(hits)} request(s) found -> {hits[:5]}")
        return 0
    print("no BLITZFORGE lines in this log.\n"
          "Either no button was pressed in this session, or the client does\n"
          "not route UI Log() calls to this file. Press an install button\n"
          "once, then run this again.")
    return 2


def main() -> None:
    if "--probe" in sys.argv:
        sys.exit(probe())

    state = load_state()
    once = "--once" in sys.argv

    print(f"watching {LOGS}")
    while True:
        log = newest_log()
        if log:
            key = log.name
            # A new session writes a new file, so an unseen name starts at zero
            # rather than inheriting the previous file's offset.
            offset = state.get(key, 0)
            if offset > log.stat().st_size:
                offset = 0
            offset, found = scan(log, offset)
            if found:
                print(f"  handled {found} request(s) from {key}")
            state[key] = offset
            save_state(state)
        if once:
            return
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
