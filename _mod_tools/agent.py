"""Executes install and remove requests made from inside the game.

The actions language the catalogue screen is written in has no file or network
access - its whole vocabulary is UI and animation - so the buttons cannot run
the installer themselves. What they can do is write a line to the client log.
This tails that log and carries the request out.

    python agent.py             # follow the log and act on requests
    python agent.py --once      # process what is already there and exit
    python agent.py --probe     # report which channel is arriving
    python agent.py --autostart # start it now and register it with Windows

build_catalog.py calls --autostart for you, so there is nothing to launch by
hand.

A request names a card index rather than a mod id, because the index is what
the button has. cache/catalog_index.json maps it back, and is written by the
same build_catalog run that laid the cards out, so the two cannot disagree.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import install

HERE = Path(__file__).resolve().parent
INDEX = HERE / "cache" / "catalog_index.json"
STATE = HERE / "cache" / "agent_state.json"
LOGS = (Path.home() / "AppData" / "Local" / "wotblitz" / "DAVAProject")
# Two shapes for the same request.
#
# The client filters debug-level output - the log holds error, warning and info
# lines and nothing below - so Log() from the UI may never arrive. What does
# arrive is the engine complaining about a resource it cannot find, and that is
# an error. The catalogue therefore asks for a sprite whose path encodes the
# request, and the resulting "File ... not found" line is the channel.
SPRITE_REQUEST = re.compile(r"BLITZFORGE/(\d)/(\d+)-(\d+)")
LOG_REQUEST = re.compile(r"BLITZFORGE:(install|remove|restart):(\d+)")
VERBS = {"1": "install", "2": "remove", "3": "restart"}
POLL_SECONDS = 2

STEAM_APP_ID = "444200"
OPEN_ON_LOAD = HERE / "cache" / "open_catalog_on_load"


PID_FILE = HERE / "cache" / "agent.pid"
STARTUP = (Path.home() / "AppData" / "Roaming" / "Microsoft" / "Windows"
           / "Start Menu" / "Programs" / "Startup" / "blitzforge-agent.vbs")


def _is_running(pid: int) -> bool:
    result = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"],
                            capture_output=True, text=True, check=False)
    return str(pid) in result.stdout


def already_running() -> bool:
    if not PID_FILE.exists():
        return False
    try:
        return _is_running(int(PID_FILE.read_text(encoding="utf-8").strip()))
    except (OSError, ValueError):
        return False


def claim_pid() -> None:
    PID_FILE.parent.mkdir(parents=True, exist_ok=True)
    PID_FILE.write_text(str(os.getpid()), encoding="utf-8")


def _pythonw() -> str:
    """pythonw runs without a console window, so the agent stays out of sight."""
    candidate = Path(sys.executable).with_name("pythonw.exe")
    return str(candidate) if candidate.exists() else sys.executable


def ensure_autostart() -> None:
    """Start the agent now if it is not up, and arrange for it to start with
    Windows.

    Nothing about the catalogue works without the agent: the buttons still
    light up and still write their request, and it sits in the log unread.
    Leaving that to a batch file the user has to remember made a working
    feature look broken, so it is set up as a side effect of building the
    screen instead.
    """
    script = f'''Set s = CreateObject("WScript.Shell")
s.Run """{_pythonw()}"" ""{HERE / 'agent.py'}""", 0, False
'''
    STARTUP.parent.mkdir(parents=True, exist_ok=True)
    if not STARTUP.exists() or STARTUP.read_text(encoding="utf-8") != script:
        STARTUP.write_text(script, encoding="utf-8")
        print(f"agent registered to start with Windows: {STARTUP.name}")

    if already_running():
        return
    subprocess.Popen([_pythonw(), str(HERE / "agent.py")],
                     creationflags=getattr(subprocess, "DETACHED_PROCESS", 0))
    print("agent started in the background")


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


def restart_client() -> None:
    """Relaunch the client with the catalogue already open.

    The screen cannot be opened from outside the game, so the flag makes the
    next build start with it visible; build_catalog consumes the flag, which
    keeps it from sticking on every later launch.
    """
    OPEN_ON_LOAD.parent.mkdir(parents=True, exist_ok=True)
    OPEN_ON_LOAD.write_text("1", encoding="utf-8")
    subprocess.run([sys.executable, str(HERE / "build_catalog.py")], check=False)

    subprocess.run(["taskkill", "/IM", "wotblitz.exe", "/F"],
                   capture_output=True, check=False)
    time.sleep(2)
    subprocess.run(["cmd", "/c", "start", "", f"steam://rungameid/{STEAM_APP_ID}"],
                   check=False)
    print("  restarting client with the catalogue open")


def handle(verb: str, index: int) -> None:
    if verb == "restart":
        restart_client()
        return

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
        return

    # The card captions come from the ledger, so the screen has to be rebuilt
    # or the button still offers the action that was just carried out.
    subprocess.run([sys.executable, str(HERE / "build_catalog.py")],
                   capture_output=True, check=False)
    print("  catalogue rebuilt; the new state shows after a restart")


def scan(path: Path, offset: int, act: bool = True) -> tuple[int, int]:
    """Read from offset, handle any requests, return (new offset, count)."""
    with path.open("r", encoding="utf-8", errors="replace") as handle_:
        handle_.seek(offset)
        text = handle_.read()
        new_offset = handle_.tell()

    requests = [(VERBS[v], int(i)) for v, i, _seq in SPRITE_REQUEST.findall(text)]
    requests += [(v, int(i)) for v, i in LOG_REQUEST.findall(text)]

    for verb, index in requests:
        if act:
            handle(verb, index)
    return new_offset, len(requests)


def probe() -> int:
    log = newest_log()
    if not log:
        print(f"no client log under {LOGS}")
        return 1
    text = log.read_text(encoding="utf-8", errors="replace")
    sprite = SPRITE_REQUEST.findall(text)
    logged = LOG_REQUEST.findall(text)
    print(f"log: {log.name} ({len(text)} chars)")
    print(f"  sprite channel: {len(sprite)} request(s)  {sprite[:4]}")
    print(f"  Log() channel:  {len(logged)} request(s)  {logged[:4]}")

    levels = sorted(set(re.findall(r"\[(info|debug|warning|error|trace)\]", text)))
    print(f"  levels in this log: {levels}")
    if "debug" not in levels:
        print("  -> debug output is filtered, so Log() from the UI cannot"
              " reach this file; the sprite channel is the one that matters")

    if sprite or logged:
        return 0
    print("\nNo request found. Press a button in the catalogue once,"
          " then run this again.")
    return 2


def main() -> None:
    if "--probe" in sys.argv:
        sys.exit(probe())
    if "--autostart" in sys.argv:
        ensure_autostart()
        return

    if already_running():
        print("another agent is already watching; exiting")
        return
    claim_pid()

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
