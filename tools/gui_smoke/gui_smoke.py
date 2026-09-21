#!/usr/bin/env python3
"""Drive the real application and check what it actually did.

The unit suites answer "is this function right"; the backend suite answers "is
this route right". Neither answers the question that broke twice this week:
*does the thing the user clicks reach the thing that was tested?* A menu entry
wired to nothing, a dialog that opens on a window that has no dataset, a result
written to a file nobody opens — all of those pass every test in the repo.

So this launches the built app on a real FITS, clicks through it, and asserts
on things that can be checked rather than looked at:

* **the backend's own job history** (`/v1/health` → `recent_tasks`): which
  operations ran, in what order, and whether they failed. This is the strongest
  oracle available — it is the backend saying what it was asked to do;
* **the workspace**: which files a tool actually produced, and what is in their
  headers;
* **the window list and the menu bar**, through macOS accessibility.

Screenshots are written next to the results for triage, but nothing is asserted
from a picture: a scenario fails on a fact, not on a pixel.

    python3 tools/gui_smoke/gui_smoke.py --list
    python3 tools/gui_smoke/gui_smoke.py --image <2d.fits> --cube <3d.fits>
    python3 tools/gui_smoke/gui_smoke.py --image <2d.fits> --only ruler,region

Requires macOS, a display, and Accessibility permission for the terminal that
runs it (System Settings → Privacy & Security → Accessibility). It is a
developer tool, not part of `ctest`: CI has no display, and a smoke test that
cannot run headless does not belong in a job that must.
"""
from __future__ import annotations

import argparse
import json
import re
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
APP = REPO / "build" / "VisIVOVisualAnalytics.app" / "Contents" / "MacOS" / "VisIVOVisualAnalytics"
PROCESS = "VisIVOVisualAnalytics"
BACKEND = "http://127.0.0.1:8000"
TOKEN_FILE = Path.home() / ".visivo_token"
WORKSPACE = Path.home() / ".visivo" / "exports"


# ── talking to the machine ───────────────────────────────────────────────────

def _clicker() -> Path:
    """The compiled CGEvent clicker: accessibility's own `click` does nothing on
    this application's custom widgets, so every click is a real event."""
    source = Path(__file__).with_name("click.swift")
    binary = Path(tempfile.gettempdir()) / "visivo_gui_smoke_click"
    if not binary.exists() or binary.stat().st_mtime < source.stat().st_mtime:
        subprocess.run(["swiftc", "-O", str(source), "-o", str(binary)], check=True)
    return binary


def _pids(pattern: str) -> list[int]:
    found = subprocess.run(["pgrep", "-f", pattern], capture_output=True, text=True)
    return [int(pid) for pid in found.stdout.split() if pid.isdigit()]


def _backend_pids() -> list[int]:
    return _pids("uvicorn app.main:app")


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def foreign_instances() -> list[str]:
    """Anything already running that this suite would have to displace."""
    running = []
    if _pids(PROCESS):
        running.append("the application")
    if _backend_pids():
        running.append("a backend (port 8000)")
    return running


class ScriptError(RuntimeError):
    """AppleScript said no. Raised rather than returned: the first version of
    this file folded stderr into the result, so a failed query came back as a
    string that parsed into zero menu items — and the scenario reported every
    entry missing instead of "I could not ask"."""


def osascript(script: str, *, check: bool = True, timeout: float = 20.0) -> str:
    # With no timeout an accessibility call that blocks blocks the whole suite:
    # the outer deadlines are only ever looked at between calls.
    try:
        result = subprocess.run(["osascript", "-e", script], capture_output=True,
                                text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        # Raised whatever `check` says. `check=False` means "AppleScript may
        # legitimately answer no" — it does not mean "a hung accessibility
        # query may be reported as an empty window list", which is how a
        # blocked query would come back as "the dialog never opened".
        raise ScriptError(f"osascript timed out after {timeout:g}s: {script}") from None
    if result.returncode != 0:
        if check:
            raise ScriptError(result.stderr.strip() or f"osascript failed: {script}")
        return ""
    return result.stdout.strip()


def system_events(body: str, *, check: bool = True) -> str:
    return osascript(
        f'tell application "System Events" to tell process "{PROCESS}" to {body}', check=check)


def activate_app() -> None:
    osascript(f'tell application "{PROCESS}" to activate', check=False)
    time.sleep(0.5)


def ensure_frontmost(attempts: int = 4) -> None:
    """Bring the application forward, and do not click until it is.

    A synthetic click into a background application is spent activating it: the
    button under the pointer never sees it. The symptom is a dialog that stays
    open and a backend that is never asked for anything — which is how the cube
    smoothing scenario failed while a screenshot showed the dialog sitting
    there with the pointer on its Apply button, and how a moment run failed
    with the editor, not the viewer, in front.
    """
    for _ in range(attempts):
        if system_events("get frontmost", check=False) == "true":
            return
        activate_app()
    raise ScriptError("the application will not come to the front")


def windows() -> list[str]:
    """The application's window titles.

    Accessibility reports an EMPTY list while the application is not frontmost
    — so a scenario that runs after anything else has stolen focus sees no
    windows, no sheets, and no buttons, and concludes that its dialog never
    opened. Every query therefore brings the application forward first; on a
    machine where nothing steals focus that costs one AppleScript call.
    """
    ensure_frontmost()
    raw = system_events("get name of every window", check=False)
    return [w.strip() for w in raw.split(",") if w.strip()] if raw else []


def sheet_count() -> int:
    """How many macOS *sheets* are open across the app's windows.

    A Qt dialog opened with a parent (`dialog->open()`) becomes a sheet on
    macOS: it is attached to its parent window and is NOT in the window list.
    Checking only `windows()` therefore reports "the dialog never opened" for
    every dialog in this application that has one — which is most of them.
    """
    total = 0
    for index in range(1, len(windows()) + 1):
        raw = system_events(f"get count of sheets of window {index}", check=False)
        with contextlib_suppress():
            total += int(raw)
    return total


def dialog_open(title_fragment: str = "") -> bool:
    """A dialog is up: either a window whose title matches, or any sheet.

    Sheets carry no title through accessibility (asked for one, they answer
    "missing value"), so a sheet can only be counted, not identified — which is
    why `open_dialog` below asks for a sheet that was NOT there a moment ago
    rather than for "a sheet".
    """
    if title_fragment and any(title_fragment.lower() in w.lower() for w in windows()):
        return True
    return sheet_count() > 0


def _containers(frontmost_only: bool = False) -> list[str]:
    """AppleScript references worth searching for a button, outermost first.

    Qt lays its dialog buttons out in a QDialogButtonBox, which reaches
    accessibility as an AXGroup — `button "Apply" of sheet 1` finds nothing, so
    each sheet's groups have to be searched too.
    """
    refs: list[str] = []
    # Window 1 is the frontmost one, which is where a modal confirmation is.
    count = 1 if frontmost_only else len(windows())
    for index in range(1, count + 1):
        window = f"window {index}"
        sheets = system_events(f"get count of sheets of {window}", check=False)
        with contextlib_suppress():
            for sheet_index in range(1, int(sheets) + 1):
                sheet = f"sheet {sheet_index} of {window}"
                refs.append(sheet)
                groups = system_events(f"get count of groups of {sheet}", check=False)
                with contextlib_suppress():
                    refs += [f"group {g} of {sheet}" for g in range(1, int(groups) + 1)]
        refs.append(window)
        groups = system_events(f"get count of groups of {window}", check=False)
        with contextlib_suppress():
            refs += [f"group {g} of {window}" for g in range(1, int(groups) + 1)]
    return refs


def button_center(name: str, sheets_only: bool = False,
                  frontmost_only: bool = False) -> tuple[float, float] | None:
    """Screen point at the middle of the named button, or None if it is not up.

    Hard-coded coordinates for a dialog button are a trap: the dialog is
    centred on a window whose size depends on the screen, so a point that hits
    Apply here misses it by 80 px on another display — which is exactly how
    this scenario first failed, silently, as "the backend never received the
    request".
    """
    for ref in _containers(frontmost_only=frontmost_only):
        if sheets_only and "sheet" not in ref:
            continue
        names = system_events(f"get name of every button of {ref}", check=False)
        entries = [n.strip() for n in names.split(",")] if names else []
        for position, entry in enumerate(entries, start=1):
            # A prefix is enough: the viewer's kind tag reads "CUBE · 167×69×402",
            # so an exact name would have to spell out the dimensions of the file
            # under test.
            if entry != name and not entry.startswith(name):
                continue
            where = system_events(f"get position of button {position} of {ref}", check=False)
            size = system_events(f"get size of button {position} of {ref}", check=False)
            with contextlib_suppress():
                x, y = (float(v) for v in where.split(", "))
                w, h = (float(v) for v in size.split(", "))
                return x + w / 2, y + h / 2
    return None


def top_menus() -> list[str]:
    raw = system_events("get name of every menu bar item of menu bar 1", check=False)
    return [n.strip() for n in raw.split(",") if n.strip()]


def focus_window_with_menu(top: str, timeout: float = 10.0) -> None:
    """Bring forward a window whose menu bar carries `top`.

    On macOS the menu bar belongs to the FRONTMOST WINDOW, and this application
    has two kinds: the Data Hub (File, Data, …) and the viewers (File, WCS,
    Tools, View). Activating the application is not enough — whichever window
    was last raised decides which menus exist, so a scenario that closes a
    dialog can find "Tools" gone. Raise windows until the menu is there.
    """
    if top in top_menus():
        return
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        # By INDEX, not by name: the Data Hub and the viewers all report the
        # title "VisIVO Visual Analytics", so `window "…"` always resolves to
        # the first one and the loop raises the same window forever.
        for index in range(1, len(windows()) + 1):
            system_events(f'perform action "AXRaise" of window {index}', check=False)
            time.sleep(0.4)
            if top in top_menus():
                return
        time.sleep(0.5)
    raise ScriptError(
        f'no window offers the "{top}" menu (windows: {windows()}, menus: {top_menus()})')


MAIN_WINDOW_TITLE = "VisIVO Visual Analytics"


def close_window(index: int) -> None:
    """Press a window's own close button.

    By SUBROLE, not by position: `button 1` is whatever accessibility happens
    to list first, and on the FITS header window that is "Edit…" — so closing
    the window opened the header EDITOR instead, which then put a modal "Do you
    want to exit?" in front of everything and every later scenario failed.
    """
    system_events('perform action "AXPress" of (first button of window '
                  f'{index} whose subrole is "AXCloseButton")', check=False)


def dismiss_confirmation() -> bool:
    """Answer a confirmation with the harmless option, if one is up.

    "No" before "Cancel", and never "Yes": the confirmation this application
    raises when a header editor is closed asks whether to EXIT.

    Only the frontmost window and its sheets are searched. A confirmation is
    modal and therefore in front; searching every window instead would let this
    press the Cancel of some unrelated long-running dialog, or a "Don't Save"
    that throws work away — so "Don't Save" is not among the answers at all.
    """
    for name in ("No", "Cancel"):
        point = button_center(name, frontmost_only=True)
        if point:
            ensure_frontmost()
            subprocess.run([str(_clicker()), str(point[0]), str(point[1])], check=False)
            time.sleep(0.5)
            return True
    return False


def close_product_windows(timeout: float = 10.0) -> None:
    """Close every window that is not a main one, and dismiss any open sheet.

    Scenarios that end with a product window still up cost the NEXT scenario
    its dialog: with a product window key, the viewer's menu action fires into
    a window that has no dataset and nothing opens — which reads, from the
    outside, as "the menu is dead". Each scenario tries to close its own
    window; this is the net under them.

    Everything here is addressed to a specific window of a specific process.
    An earlier version pressed ⌘W instead, which is sent to whatever is
    frontmost — so the moment the application lost focus the suite started
    closing windows in OTHER applications. A test tool gets no keystrokes it
    cannot address.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        extra = [index for index, title in enumerate(windows(), start=1)
                 if title != MAIN_WINDOW_TITLE]
        if not extra and sheet_count() == 0:
            return
        if extra:
            # ONE window per pass, then look again: closing a window renumbers
            # the ones after it, so a loop over a list of indices closes the
            # wrong windows from the second one on — which is how a tidy-up
            # between scenarios ended up closing the VIEWER and left every
            # later scenario reporting that the Tools menu had disappeared.
            close_window(extra[0])
            time.sleep(0.5)
            dismiss_confirmation()
        elif not dismiss_confirmation():
            # Inside the sheet only: a "Close" elsewhere on screen belongs to
            # some other window, and pressing that closes the wrong thing.
            for name in ("Close", "Done"):
                point = button_center(name, sheets_only=True)
                if point:
                    ensure_frontmost()
                    subprocess.run([str(_clicker()), str(point[0]), str(point[1])], check=False)
                    time.sleep(0.4)
                    break
        time.sleep(0.3)
    # Saying nothing here is how a scenario fails for a reason that happened
    # before it started: the next one runs into whatever is still on screen.
    raise AssertionError(
        f"could not clear the screen between scenarios: windows={windows()}, "
        f"sheets={sheet_count()}")


def menu_item(path: list[str]) -> str:
    """Click a native menu item, e.g. ["Tools", "Export", "Smooth / Regrid…"]."""
    top, *rest = path
    focus_window_with_menu(top)
    leaf = rest[-1]
    chain = f'menu 1 of menu bar item "{top}" of menu bar 1'
    for name in rest[:-1]:
        chain = f'menu 1 of menu item "{name}" of {chain}'
    return system_events(f'click menu item "{leaf}" of {chain}')


def open_dialog(path: list[str], title_fragment: str, timeout: float = 20.0,
                attempts: int = 3) -> None:
    """Click a menu entry until its dialog is actually up.

    The first menu click after a launch is sometimes eaten: the viewer is still
    building its first frame, and a click that arrives during that never
    reaches the action. Nothing is wrong with the application — but a suite
    that gives up on the first try reports a dead menu once in every few runs,
    which is worse than useless.
    """
    for attempt in range(attempts):
        before_windows, before_sheets = set(windows()), sheet_count()

        def appeared() -> bool:
            for title in set(windows()) - before_windows:
                if title_fragment.lower() in title.lower():
                    return True
            # A sheet has no title to check, so what is checked is that there
            # is one MORE of them than before this menu was clicked: any sheet
            # at all would be satisfied by a leftover from a previous scenario.
            return sheet_count() > before_sheets

        menu_item(path)
        if wait_for(appeared, timeout=timeout / attempts):
            return
    raise AssertionError(f'the "{path[-1]}" dialog never opened')


def menu_items(path: list[str]) -> list[str]:
    """The names under a menu, for --list and for asserting an entry exists."""
    top, *rest = path
    focus_window_with_menu(top)
    chain = f'menu 1 of menu bar item "{top}" of menu bar 1'
    for name in rest:
        chain = f'menu 1 of menu item "{name}" of {chain}'
    raw = system_events(f"get name of every menu item of {chain}")
    return [n.strip() for n in raw.split(",") if n.strip()]


# ── the backend as an oracle ─────────────────────────────────────────────────

def _texts(ref: str) -> list[str]:
    raw = system_events(f"get value of every static text of {ref}", check=False)
    return [value.strip() for value in raw.split(",") if value.strip()
            and value.strip() != "missing value"]


def window_texts(title_fragment: str = "") -> list[str]:
    """Every label in a window, its groups included.

    Qt's widgets do not answer `entire contents`, so the groups are walked one
    level by hand — which is enough: this application puts its readouts in a
    card inside the window, not deeper.
    """
    for index, title in enumerate(windows(), start=1):
        if title_fragment and title_fragment.lower() not in title.lower():
            continue
        values = _texts(f"window {index}")
        groups = system_events(f"get count of groups of window {index}", check=False)
        with contextlib_suppress():
            for group in range(1, int(groups) + 1):
                values += _texts(f"group {group} of window {index}")
        return values
    return []


def labelled_number(texts: list[str], label: str) -> float | None:
    """The number after `label` in a list of labels and values.

    The readout cards are label/value pairs in layout order, so the value is
    simply the next entry — and "64769 / 64769" counts as its first number.
    """
    for position, value in enumerate(texts):
        if value.rstrip(":").strip().lower() != label.rstrip(":").strip().lower():
            continue
        for candidate in texts[position + 1:position + 2]:
            with contextlib_suppress():
                return float(candidate.split("/")[0].strip())
    return None


def overlay_count(overlay: str) -> float | None:
    """The number beside an entry of the viewer's Overlays block.

    The block is a column of check boxes with a count label at the right of
    each row. Accessibility gives the check box NAMES in layout order and the
    labels in layout order — including the empty ones for the rows that have no
    count — so the count for a row is the label at the row's own position.
    That is the count the annotation code itself writes, which makes it a real
    oracle rather than "a click was accepted".
    """
    for index in range(1, len(windows()) + 1):
        groups = system_events(f"get count of groups of window {index}", check=False)
        with contextlib_suppress():
            for group in range(1, int(groups) + 1):
                ref = f"group {group} of window {index}"
                names = [n.strip() for n in
                         system_events(f"get name of every checkbox of {ref}",
                                       check=False).split(",")]
                if overlay not in names:
                    continue
                raw = system_events(f"get value of every static text of {ref}", check=False)
                values = [v.strip() for v in raw.split(",")]
                if "OVERLAYS" not in values:
                    continue
                tail = values[values.index("OVERLAYS") + 1:]
                position = names.index(overlay)
                if position < len(tail):
                    with contextlib_suppress():
                        return float(tail[position])
                return None
    return None


#: The hint the viewer shows beside the mode strip, per mode. It is the only
#: part of the armed mode accessibility can see: the strip itself is a custom
#: widget that reports neither its buttons nor which is selected.
MODE_HINTS = {
    "Pan": "drag to pan",
    "Region": "drag on the image",
    "Ruler": "click the two ends",
}


def mode_hint() -> str | None:
    """Which mode the viewer says it is in, or None if it says nothing."""
    texts = window_texts(MAIN_WINDOW_TITLE)
    for mode, hint in MODE_HINTS.items():
        if any(hint in value for value in texts):
            return mode
    return None


def _channel_map_caption(texts: list[str]) -> tuple[int, float, float] | None:
    """(panels, range low, range high) out of the channel-map window's caption.

    It is written across two labels because the comma inside the range splits
    the accessibility value list, so the labels are joined before matching.
    """
    joined = " ".join(texts)
    match = re.search(r"(\d+)\s+panels.*?range\s*\[\s*([-\d.eE+]+)[,\s]+([-\d.eE+]+)\s*\]",
                      joined)
    if not match:
        return None
    with contextlib_suppress():
        return int(match.group(1)), float(match.group(2)), float(match.group(3))
    return None


def viewer_kind(filename: str) -> str | None:
    """The kind tag ("IMAGE"/"CUBE") of the viewer showing this file.

    A file can be written correctly, opened successfully and still land in the
    wrong viewer — which is the whole point of the classifier — so a scenario
    that produces a cube asks which viewer ended up with it.
    """
    for index, title in enumerate(windows(), start=1):
        texts = _texts(f"window {index}")
        groups = system_events(f"get count of groups of window {index}", check=False)
        with contextlib_suppress():
            for group in range(1, int(groups) + 1):
                texts += _texts(f"group {group} of window {index}")
        if not any(filename in value for value in texts):
            continue
        names = [n.strip() for n in
                 system_events(f"get name of every button of window {index}",
                               check=False).split(",")]
        for name in names:
            for kind in ("CUBE", "IMAGE"):
                if name.startswith(kind):
                    return kind
    return None


def fits_axes(path: Path) -> list[int]:
    """NAXISn from a FITS primary header, read from the bytes.

    A dependency-free check that the file a tool produced is the KIND of file
    it claimed: smoothing a cube has to give back a cube. numpy and astropy are
    not necessarily installed for whatever interpreter runs this suite, and the
    header is fixed-format ASCII, so it is read directly.
    """
    axes: dict[int, int] = {}
    with path.open("rb") as handle:
        while True:
            block = handle.read(2880)
            if len(block) < 2880:
                break
            for start in range(0, 2880, 80):
                card = block[start:start + 80].decode("ascii", "replace")
                key = card[:8].strip()
                if key == "END":
                    return [axes[index] for index in sorted(axes)]
                if key.startswith("NAXIS") and key[5:].isdigit():
                    with contextlib_suppress():
                        axes[int(key[5:])] = int(card[10:].split("/")[0].strip())
    return [axes[index] for index in sorted(axes)]


def health() -> dict:
    token = TOKEN_FILE.read_text().strip() if TOKEN_FILE.exists() else ""
    request = urllib.request.Request(f"{BACKEND}/v1/health", headers={"X-Visivo-Token": token})
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.loads(response.read())
    except (urllib.error.URLError, OSError, ValueError):
        return {}


def operations() -> list[str]:
    """The backend's job history, newest first."""
    return [entry["operation"] for entry in health().get("recent_tasks", [])]


def failed_operations() -> list[str]:
    return [entry["operation"] for entry in health().get("recent_tasks", [])
            if entry.get("status") in OperationLog.BAD_STATUS]


class OperationLog:
    """Everything the backend has finished since this started.

    `/v1/health` keeps only the last few jobs — the same five the Data Hub
    shows. Asserting on that snapshot works only if nothing else runs in
    between, and when a tool opens its own result the opening pushes the tool
    off the list: the cube smoothing scenario failed for exactly that reason
    while the application had done every single thing it was asked. So the jobs
    are collected as they appear and the scenarios ask this instead.

    Identity is the weak point and is worth being explicit about. The snapshot
    carries no job id, only an operation name and `age_seconds` since the job
    FINISHED, so a job is keyed by name and reconstructed finish time. That
    reconstruction includes the request latency, so the same job can land a
    tenth of a second apart on two successive samples — which would make an old
    job look new to a later `mark()`. A new key therefore only counts as a new
    job when no job of the same name was already recorded within
    `SAME_JOB_SECONDS` of it.

    The consequences of the missing id that this cannot fix, stated rather than
    papered over: two jobs of the same name finishing within that window are one
    job here, and more than five jobs finishing between two samples lose the
    oldest of them. Both are only reachable with a much busier backend than one
    scenario produces.
    """

    #: Two finishes of the same operation closer than this, seen in DIFFERENT
    #: replies, are taken to be one job seen twice. It is as wide as the health
    #: request's own timeout, because that is how far a slow reply can push the
    #: estimate; two jobs in one reply are never merged whatever it says.
    SAME_JOB_SECONDS = 5.0

    #: What the backend calls a finished-badly job. Everything else it reports
    #: — "done" for a synchronous operation, "completed" for a task — is a
    #: success, and "running"/"pending" is simply not finished yet; treating
    #: anything that is not the word "done" as a failure marked every completed
    #: task as broken.
    BAD_STATUS = ("failed", "cancelled", "error")

    #: Jobs kept. A suite run is finite, but the log outlives a relaunch.
    LIMIT = 2000

    def __init__(self, interval: float = 0.25):
        self.interval = interval
        self._seen: dict[tuple[str, float], str] = {}
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        if self._thread:
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._poll, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        self._thread = None

    def _poll(self) -> None:
        while not self._stop.is_set():
            # The clock is read BEFORE the request: the age in the reply is
            # measured when the reply is written, so subtracting it from the
            # time the reply arrived adds the whole round trip to the estimate.
            asked_at = time.time()
            tasks = health().get("recent_tasks", [])
            entries = []
            for task in tasks:
                with contextlib_suppress():
                    entries.append((str(task["operation"]),
                                    asked_at - float(task.get("age_seconds", 0.0)),
                                    str(task.get("status", ""))))
            self._record(entries)
            self._stop.wait(self.interval)

    def _record(self, entries: list[tuple[str, float, str]]) -> None:
        """Fold one reply into the log.

        Jobs listed TOGETHER in one reply are distinct jobs by definition, so
        each one may claim at most one existing key: without that rule a failed
        job and an older successful one of the same name, half a second apart,
        merged into a single entry whose status was the successful one — and
        the failure disappeared from a scenario that was there to catch it.
        """
        with self._lock:
            claimed: set[tuple[str, float]] = set()
            # Oldest first, and each entry takes the CLOSEST free key rather
            # than the first one within the window. Taking the first let a new
            # failure at t+0.5 claim the key of the success at t that was
            # already in the log, after which the success — re-listed in the
            # same reply — was filed as something new: one job since the mark,
            # reported as successful, when there were two and one had failed.
            for operation, finished_at, status in sorted(entries, key=lambda e: e[1]):
                best, best_distance = None, self.SAME_JOB_SECONDS
                for candidate, stored in self._seen.items():
                    if candidate[0] != operation or candidate in claimed:
                        continue
                    # A job does not go from failed to done: two different
                    # terminal statuses are two different jobs, however close.
                    if (stored in self.BAD_STATUS) != (status in self.BAD_STATUS) \
                            and status and stored:
                        continue
                    distance = abs(candidate[1] - finished_at)
                    if distance <= best_distance:
                        best, best_distance = candidate, distance
                key = best
                if key is None:
                    key = (operation, round(finished_at, 1))
                    while key in claimed or key in self._seen:
                        key = (operation, round(key[1] + 0.1, 1))
                claimed.add(key)
                self._seen[key] = status
            while len(self._seen) > self.LIMIT:
                self._seen.pop(next(iter(self._seen)))

    def _snapshot(self) -> dict[tuple[str, float], str]:
        """Keys AND statuses together: reading the keys and then looking each
        status up separately can see a job that was recorded in between."""
        with self._lock:
            return dict(self._seen)

    def mark(self) -> set:
        """A token standing for "everything seen up to now"."""
        return set(self._snapshot())

    def count(self, operation: str, since: set | None = None) -> int:
        since = since or set()
        return sum(1 for key in self._snapshot() if key[0] == operation and key not in since)

    def failed(self, operation: str, since: set | None = None) -> int:
        since = since or set()
        return sum(1 for key, status in self._snapshot().items()
                   if key[0] == operation and key not in since and status in self.BAD_STATUS)

    def names(self, since: set | None = None) -> list[str]:
        since = since or set()
        return [key[0] for key in self._snapshot() if key not in since]


def wait_for(predicate, timeout: float = 30.0, interval: float = 0.5) -> bool:
    """Poll instead of sleeping blindly: a fixed sleep is either a flake or a
    waste, and usually both on a different machine."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


# ── the application under test ───────────────────────────────────────────────

class App:
    """Launch, drive and tear down one run of the application."""

    def __init__(self, artefacts: Path, verbose: bool = False):
        self.artefacts = artefacts
        self.verbose = verbose
        self.process: subprocess.Popen | None = None
        self.log: Path | None = None
        self.clicker = self._build_clicker()
        self.ops = OperationLog()
        self._owned: set[int] = set()
        self.shots = 0

    # -- setup ---------------------------------------------------------------

    def _build_clicker(self) -> Path:
        return _clicker()

    def kill_everything(self) -> None:
        """Stop the application and the backend THIS RUN started.

        Killing only the app leaves its uvicorn holding port 8000, and the next
        launch then fails with "address already in use" and hangs on the startup
        dialog — which looks exactly like the app being broken. So the backend
        goes too, but only the one this run is responsible for: matching on the
        command line alone ("uvicorn app.main:app", or any process named like
        the application) also names a developer's own server and their own open
        session, and a test tool does not get to end those.
        """
        for pid in sorted(self._owned):
            with contextlib_suppress():
                os.kill(pid, signal.SIGTERM)
        time.sleep(1.5)
        for pid in sorted(self._owned):
            if _alive(pid):
                with contextlib_suppress():
                    os.kill(pid, signal.SIGKILL)
        self._owned.clear()
        time.sleep(0.5)

    def launch(self, dataset: Path) -> None:
        self.kill_everything()
        self.log = self.artefacts / f"app-{dataset.stem[:24]}.log"
        handle = open(self.log, "w")
        self.ops.start()
        before = set(_backend_pids())
        self.process = subprocess.Popen([str(APP), str(dataset)], stdout=handle, stderr=handle)
        self._owned.add(self.process.pid)
        try:
            ready = wait_for(lambda: any("Visual Analytics" in w for w in windows())
                             and not any("Starting" in w for w in windows()),
                             timeout=90)
        finally:
            # Whatever backend appeared after our own launch is ours to stop —
            # claimed even when the launch fails, or a run that gives up on a
            # window that never appeared leaves its backend holding port 8000.
            self._owned |= set(_backend_pids()) - before
        if not ready:
            raise AssertionError(f"the window never appeared (see {self.log})")
        # The dataset is open once the backend says so.
        if not wait_for(lambda: self.ops.count("datasets/open"), timeout=60):
            raise AssertionError("the backend never saw the dataset being opened")
        # …and the viewer is usable once its own menus are on the bar: the first
        # scenario after a launch otherwise clicks a menu that is still the Data
        # Hub's and reports the dialog as never opening.
        focus_window_with_menu("Tools", timeout=30)
        self.activate()

    def activate(self) -> None:
        osascript(f'tell application "{PROCESS}" to activate')
        time.sleep(0.6)

    # -- driving -------------------------------------------------------------

    def click(self, x: float, y: float) -> None:
        ensure_frontmost()
        subprocess.run([str(self.clicker), str(x), str(y)], check=True)
        time.sleep(0.4)

    def click_button(self, name: str, timeout: float = 15.0) -> None:
        """Click a dialog button by NAME, wherever the dialog happens to be."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            point = button_center(name)
            if point:
                self.click(*point)
                return
            time.sleep(0.4)
        raise AssertionError(f'no button named "{name}" is on screen')

    def click_button_until(self, name: str, done, timeout: float = 90.0,
                           attempts: int = 3) -> None:
        """Press a button until it has visibly done something.

        Focus can be stolen between "bring the application forward" and the
        click itself — by a notification, by another application's window
        raising itself — and the click is then spent re-activating. Pressing
        again costs a second; deciding that the feature is broken costs an
        afternoon.
        """
        for attempt in range(attempts):
            if attempt and (done() or button_center(name) is None):
                # Either it worked after all, or the button is gone because it
                # worked: pressing again would submit the job a second time.
                # A missed observation is not the same as a missed click, and
                # this told them apart only after three moment jobs had been
                # queued by one press of Compute.
                break
            self.click_button(name)
            if wait_for(done, timeout=timeout / attempts):
                return
        if done():
            return
        raise AssertionError(f'pressing "{name}" never had any effect')

    def drag(self, x: float, y: float, to_x: float, to_y: float) -> None:
        ensure_frontmost()
        subprocess.run([str(self.clicker), str(x), str(y), str(to_x), str(to_y)], check=True)
        time.sleep(0.6)

    def type_text(self, text: str) -> None:
        # Keystrokes go to whatever is frontmost, exactly like clicks do.
        ensure_frontmost()
        osascript(f'tell application "System Events" to keystroke "{text}"')
        time.sleep(0.3)

    def press_return(self) -> None:
        ensure_frontmost()
        osascript('tell application "System Events" to key code 36')
        time.sleep(0.5)

    def screenshot(self, name: str) -> Path:
        self.shots += 1
        path = self.artefacts / f"{self.shots:02d}-{name}.png"
        subprocess.run(["screencapture", "-x", "-o", str(path)], capture_output=True)
        return path

    def close(self) -> None:
        self.ops.stop()
        self.kill_everything()
        self.process = None


class contextlib_suppress:
    """`contextlib.suppress(Exception)` without the import at the top of a file
    that is mostly subprocess calls."""

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return True


# ── scenarios ────────────────────────────────────────────────────────────────

@dataclass
class Scenario:
    name: str
    kind: str                     # "image" | "cube"
    run: object                   # callable(app) -> None; raises on failure
    description: str = ""


@dataclass
class Result:
    name: str
    passed: bool
    detail: str = ""
    shot: Path | None = None
    seconds: float = 0.0


SCENARIOS: list[Scenario] = []


def scenario(name: str, kind: str, description: str = ""):
    def register(function):
        SCENARIOS.append(Scenario(name, kind, function, description or function.__doc__ or ""))
        return function
    return register


# The window is laid out by Qt, so positions are read from the screen rather
# than hard-coded per widget: these are the few anchors the scenarios need, in
# screen POINTS on the app's own (maximised) window.
MODE_STRIP_Y = 141
MODE_X = {"Pan": 79, "Probe": 128, "Region": 185, "Ruler": 240, "Annotate": 300}
IMAGE_CENTRE = (860, 560)


@scenario("mode-strip", "image")
def _mode_strip(app: App) -> None:
    """Every mode arms, and the strip follows a mode armed from the menu.

    What is checked is the viewer's own hint beside the strip ("click the two
    ends", …), which is the only part of the armed mode accessibility can see:
    the strip is a custom widget that reports neither its buttons nor which of
    them is selected, so nothing here can tell whether the right one is
    highlighted — only that the viewer believes it is in that mode.
    """
    for mode in ("Region", "Ruler", "Pan"):
        app.click(MODE_X[mode], MODE_STRIP_Y)
        if not wait_for(lambda: mode_hint() == mode, timeout=5):
            raise AssertionError(f"clicking {mode} left the viewer in {mode_hint()}")
    # Armed from the MENU this time: the strip has to follow, which is the part
    # that silently breaks when a new mode is added.
    menu_item(["Tools", "Measure", "Ruler (Distance)"])
    if not wait_for(lambda: mode_hint() == "Ruler", timeout=5):
        raise AssertionError("arming the ruler from the menu did not reach the viewer")
    app.screenshot("mode-strip")


@scenario("region-readout", "image")
def _region(app: App) -> None:
    """A dragged region produces statistics that hold together.

    The numbers are read out of the window and checked against each other —
    a region with pixels in it, a mean between the extremes, a positive
    scatter. What is NOT checked here is the live readout drawn on the image
    against these: that is painted into the VTK scene, where accessibility
    cannot follow it.
    """
    app.click(MODE_X["Region"], MODE_STRIP_Y)
    app.drag(700, 400, 950, 620)
    if not wait_for(lambda: "Region Analysis" in " ".join(windows()), timeout=15):
        raise AssertionError("the region statistics window never opened")
    texts = window_texts("Region Analysis")
    valid = labelled_number(texts, "Valid pixels")
    minimum = labelled_number(texts, "Min")
    maximum = labelled_number(texts, "Max")
    mean = labelled_number(texts, "Mean")
    scatter = labelled_number(texts, "Std Dev")
    if None in (valid, minimum, maximum, mean, scatter):
        raise AssertionError(f"the statistics could not be read: {texts}")
    if valid <= 0:
        raise AssertionError("the region was reported as having no valid pixels")
    if not minimum <= mean <= maximum:
        raise AssertionError(f"mean {mean} is outside [{minimum}, {maximum}]")
    if scatter <= 0:
        raise AssertionError("a region of real data was reported with no scatter")
    app.screenshot("region-readout")
    for index, title in enumerate(windows(), start=1):
        if "Region Analysis" in title:
            close_window(index)
            break


@scenario("ruler", "image")
def _ruler(app: App) -> None:
    """The ruler stays armed across the two clicks that measure a distance.

    The measurement itself is drawn into the VTK scene and cannot be read from
    outside, so this proves the gesture reaches an armed ruler and does not
    knock the viewer out of the mode — not that the distance is right, which
    has no test of its own yet.
    """
    app.click(MODE_X["Ruler"], MODE_STRIP_Y)
    if not wait_for(lambda: mode_hint() == "Ruler", timeout=5):
        raise AssertionError("the ruler never armed")
    app.click(700, 400)
    app.click(900, 560)
    if mode_hint() != "Ruler":
        raise AssertionError(f"measuring left the viewer in {mode_hint()}")
    app.screenshot("ruler")


@scenario("annotation-overlay", "image")
def _annotation(app: App) -> None:
    """An annotation is placed and counted in the Overlays block.

    The count beside "Annotations" is the oracle — it is written by the code
    that owns the annotations, so a text placed but not registered, or
    registered but not counted, fails here.
    """
    before = overlay_count("Annotations") or 0
    menu_item(["Tools", "Annotations", "Add Text Annotation…"])
    time.sleep(1.0)
    app.type_text("smoke")
    app.press_return()
    app.click(*IMAGE_CENTRE)
    if not wait_for(lambda: (overlay_count("Annotations") or 0) > before, timeout=20):
        raise AssertionError(
            f'the Overlays block still counts {overlay_count("Annotations")} annotations')
    app.screenshot("annotation")


@scenario("histogram", "image")
def _histogram(app: App) -> None:
    """The pixel histogram opens as its own window."""
    menu_item(["Tools", "Statistics", "Pixel Histogram"])
    if not wait_for(lambda: any("Histogram" in w for w in windows()), timeout=20):
        raise AssertionError("the histogram window never opened")
    app.screenshot("histogram")
    for index, title in enumerate(windows(), start=1):
        if "Histogram" in title:
            close_window(index)
            break


@scenario("image-quality", "image")
def _image_quality(app: App) -> None:
    """A backend product: the route runs and the result window appears."""
    mark = app.ops.mark()
    menu_item(["Tools", "Products", "Image Quality / Artifacts"])
    if not wait_for(lambda: app.ops.count("products/image_quality", mark), timeout=60):
        raise AssertionError("the backend never received the image-quality request")
    if not wait_for(lambda: any("Image Quality" in w for w in windows()), timeout=30):
        raise AssertionError("the metrics window never opened")
    app.screenshot("image-quality")
    system_events('click button "Close" of window "Image Quality / Artifacts"',
                  check=False)
    if app.ops.failed("products/image_quality", mark):
        raise AssertionError("the backend reported the image-quality request as failed")


@scenario("smooth-regrid", "image")
def _smooth_image(app: App) -> None:
    """Smooth writes a new FITS into the workspace and the backend records it."""
    before = set(WORKSPACE.glob("*_smooth*.fits"))
    mark = app.ops.mark()
    open_dialog(["Tools", "Export", "Smooth / Regrid…"], "Smooth")
    app.screenshot("smooth-dialog")
    app.click_button_until("Apply", lambda: app.ops.count("image/filter", mark))
    produced = wait_for(lambda: set(WORKSPACE.glob("*_smooth*.fits")) - before, timeout=30)
    if not produced:
        raise AssertionError("no smoothed file appeared in the workspace")
    if app.ops.failed("image/filter", mark):
        raise AssertionError("the backend reported the filter as failed")
    written = sorted(set(WORKSPACE.glob("*_smooth*.fits")) - before)[0]
    axes = fits_axes(written)
    if len(axes) != 2:
        raise AssertionError(f"smoothing a 2-D image produced {len(axes)} axes: {axes}")
    app.screenshot("smooth-result")


@scenario("header-editor", "image")
def _header_editor(app: App) -> None:
    """The FITS header opens from the toolbar tag and offers Edit…."""
    # The tag lives in the VIEWER's toolbar, so the viewer has to be the front
    # window before a click means anything — and the tag's position depends on
    # how long the file path in the breadcrumb is, so it is looked up, not
    # guessed.
    focus_window_with_menu("Tools")
    app.click_button("IMAGE")
    if not wait_for(lambda: dialog_open("FITS Header"), timeout=20):
        raise AssertionError("the FITS header window never opened")
    app.screenshot("header")
    for index, title in enumerate(windows(), start=1):
        if "FITS Header" in title:
            close_window(index)
            break


@scenario("moment-map", "cube")
def _moment(app: App) -> None:
    """Compute Moment runs and registers a product."""
    mark = app.ops.mark()
    open_dialog(["Tools", "Maps", "Compute Moment…"], "Moment")
    app.screenshot("moment-dialog")
    # The oracle is the moment TASK, not "some cube route ran": the viewer
    # re-bins the cube for its own display all the time, so a scenario that
    # accepts any `cube/…` passes even when no moment is ever computed — as
    # this one did, while the registry was in fact dropping every completed
    # task on the floor.
    app.click_button_until("Compute", lambda: app.ops.count("moment", mark),
                           timeout=180, attempts=2)
    if app.ops.failed("moment", mark):
        raise AssertionError("the backend reported the moment as failed")
    time.sleep(3)
    app.screenshot("moment-result")
    # Closed BY WINDOW, not by pressing the first button called "Close" on
    # screen: the viewer has one too, and pressing that closes the viewer —
    # after which every later scenario reports that the Tools menu is gone.
    for index, title in enumerate(windows(), start=1):
        if "Moment" in title:
            close_window(index)
            break


@scenario("noise", "cube")
def _noise(app: App) -> None:
    """Estimate Noise reaches the backend and answers."""
    mark = app.ops.mark()
    open_dialog(["Tools", "Statistics", "Estimate Noise..."], "Noise")
    app.click_button_until("Estimate", lambda: app.ops.count("cube/noise", mark))
    if app.ops.failed("cube/noise", mark):
        raise AssertionError("the backend reported the noise estimate as failed")
    app.screenshot("noise")


@scenario("cube-header", "cube")
def _cube_header(app: App) -> None:
    """The cube's own FITS header opens from its toolbar tag."""
    focus_window_with_menu("Tools")
    app.click_button("CUBE")
    if not wait_for(lambda: dialog_open("FITS Header"), timeout=20):
        raise AssertionError("the FITS header window never opened")
    app.screenshot("cube-header")


@scenario("cube-histogram", "cube")
def _cube_histogram(app: App) -> None:
    """The per-slice pixel histogram opens as its own window."""
    menu_item(["Tools", "Statistics", "Pixel Histogram (current slice)"])
    if not wait_for(lambda: any("Histogram" in w for w in windows()), timeout=25):
        raise AssertionError("the histogram window never opened")
    app.screenshot("cube-histogram")


@scenario("channel-maps", "cube")
def _channel_maps(app: App) -> None:
    """Channel maps are generated: the backend re-bins the cube and the result
    window names the file it was made from."""
    mark = app.ops.mark()
    open_dialog(["Tools", "Maps", "Channel Maps…"], "Channel Maps")
    app.click_button_until(
        "Generate",
        lambda: any(op.startswith("cube/") for op in app.ops.names(mark)), timeout=120)
    if not wait_for(lambda: any(w.startswith("Channel Maps - ") for w in windows()), timeout=60):
        raise AssertionError("the channel maps were never shown")
    # The window is titled before its panels are filled and stays up to show an
    # error, so the title alone proves nothing. Its caption is written from the
    # data that came back — "64 panels · 167 × 69 px per panel · range [a, b]" —
    # so a generation that failed or returned nothing has no range in it.
    def described() -> bool:
        return bool(_channel_map_caption(window_texts("Channel Maps - ")))

    if not wait_for(described, timeout=90):
        raise AssertionError(
            f"the channel maps never described any data: {window_texts('Channel Maps - ')}")
    panels, low, high = _channel_map_caption(window_texts("Channel Maps - "))
    if panels <= 0:
        raise AssertionError("the channel map window reports no panels")
    if low == high:
        raise AssertionError(f"the panels carry no data: range [{low}, {high}]")
    if any(app.ops.failed(name, mark) for name in set(app.ops.names(mark))):
        raise AssertionError(f"a backend job failed while generating: {app.ops.names(mark)}")
    app.screenshot("channel-maps")


@scenario("cube-smooth", "cube")
def _smooth_cube(app: App) -> None:
    """Smoothing a cube writes a cube and opens it: the filter, the classifier
    and the dispatch, in one gesture."""
    before = set(WORKSPACE.glob("*_smooth*.fits"))
    mark = app.ops.mark()
    open_dialog(["Tools", "Export", "Smooth / Regrid…"], "Smooth")
    app.click_button_until("Apply", lambda: app.ops.count("image/filter", mark), timeout=120)
    produced = wait_for(lambda: set(WORKSPACE.glob("*_smooth*.fits")) - before, timeout=30)
    if not produced:
        raise AssertionError("no smoothed cube appeared in the workspace")
    if app.ops.failed("image/filter", mark):
        raise AssertionError("the backend reported the filter as failed")
    # The result has to BE a cube and be opened as one — smoothing that
    # flattens the third axis, or a classifier that calls the result an image,
    # both end with the file in the wrong viewer.
    written = sorted(set(WORKSPACE.glob("*_smooth*.fits")) - before)[0]
    axes = fits_axes(written)
    if len(axes) < 3:
        raise AssertionError(f"smoothing a cube produced {len(axes)} axes: {axes}")
    if not wait_for(lambda: app.ops.count("datasets/open", mark), timeout=60):
        raise AssertionError("the smoothed cube was never opened")
    if app.ops.failed("datasets/open", mark):
        raise AssertionError("opening the smoothed cube failed")
    if not wait_for(lambda: viewer_kind(written.name) == "CUBE", timeout=30):
        raise AssertionError(
            f"the smoothed cube was opened as {viewer_kind(written.name)}, not CUBE")
    app.screenshot("cube-smooth")


@scenario("menus", "image")
def _menus(app: App) -> None:
    """Every tool group the viewer advertises is still reachable — a menu that
    lost its entries is the cheapest possible regression to catch."""
    expected = {
        ("Tools", "Regions"): {"Box Region Stats", "Export Region (CRTF/DS9)…"},
        ("Tools", "Catalogue"): {"Overlay VLKB Compact Sources", "Overlay VLKB Bubbles",
                                 "VLKB Catalogue Query…"},
        ("Tools", "Measure"): {"Ruler (Distance)", "Angle (3 Points)"},
        ("Tools", "Export"): {"Smooth / Regrid…", "Export to Workspace as FITS…"},
    }
    missing = {}
    for path, wanted in expected.items():
        present = set(menu_items(list(path)))
        gap = wanted - present
        if gap:
            missing[" ▸ ".join(path)] = sorted(gap)
    if missing:
        raise AssertionError(f"menu entries missing: {missing}")


# ── running them ─────────────────────────────────────────────────────────────

def run_scenarios(app: App, dataset: Path, kind: str, wanted: set[str] | None) -> list[Result]:
    chosen = [s for s in SCENARIOS if s.kind == kind and (not wanted or s.name in wanted)]
    if not chosen:
        return []
    print(f"\n── {kind}: {dataset.name}")
    app.launch(dataset)
    results: list[Result] = []
    for item in chosen:
        started = time.monotonic()
        try:
            app.activate()
            close_product_windows()
            item.run(app)
            results.append(Result(item.name, True, seconds=time.monotonic() - started))
            print(f"   ✓ {item.name}  ({results[-1].seconds:.1f}s)")
        except Exception as exc:  # noqa: BLE001 — a scenario failing is the output
            shot = app.screenshot(f"FAILED-{item.name}")
            results.append(Result(item.name, False, str(exc), shot,
                                  time.monotonic() - started))
            print(f"   ✗ {item.name}: {exc}")
            print(f"     screenshot: {shot}")
    app.close()
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", type=Path, help="a 2-D FITS to run the image scenarios on")
    parser.add_argument("--cube", type=Path, help="a 3-D FITS to run the cube scenarios on")
    parser.add_argument("--only", default="", help="comma-separated scenario names")
    parser.add_argument("--artefacts", type=Path,
                        default=Path(tempfile.gettempdir()) / "visivo-gui-smoke",
                        help="where screenshots and logs are written")
    parser.add_argument("--list", action="store_true", help="list the scenarios and exit")
    parser.add_argument("--take-over", action="store_true",
                        help="stop an application or backend that is already running "
                             "(without this the suite refuses to touch either)")
    args = parser.parse_args()

    if args.list:
        for item in SCENARIOS:
            print(f"{item.kind:6}  {item.name:20}  {item.description.strip().splitlines()[0]}")
        return 0

    if sys.platform != "darwin":
        print("gui_smoke only runs on macOS (it drives the native menu bar).")
        return 2
    if not APP.exists():
        print(f"the app is not built: {APP}")
        return 2
    if not args.image and not args.cube:
        print("give --image and/or --cube (a FITS to open)")
        return 2

    running = foreign_instances()
    if running and not args.take_over:
        print(f"already running: {', '.join(running)}.\n"
              "This suite needs the application and port 8000 to itself. Close them, "
              "or pass --take-over to have them stopped for you.")
        return 2
    if running and args.take_over:
        subprocess.run(["pkill", "-f", PROCESS], capture_output=True)
        subprocess.run(["pkill", "-f", "uvicorn app.main:app"], capture_output=True)
        time.sleep(2.0)

    # A fresh subdirectory per run rather than erasing what was given: an
    # `--artefacts` pointing at a directory with anything else in it used to
    # have that deleted.
    args.artefacts.mkdir(parents=True, exist_ok=True)
    args.artefacts = args.artefacts / time.strftime("run-%Y%m%d-%H%M%S")
    args.artefacts.mkdir()
    wanted = {name.strip() for name in args.only.split(",") if name.strip()} or None

    app = App(args.artefacts)
    results: list[Result] = []
    try:
        if args.image:
            results += run_scenarios(app, args.image.expanduser(), "image", wanted)
        if args.cube:
            results += run_scenarios(app, args.cube.expanduser(), "cube", wanted)
    finally:
        app.close()

    passed = sum(1 for r in results if r.passed)
    print(f"\n{passed}/{len(results)} scenarios passed · artefacts in {args.artefacts}")
    for r in results:
        if not r.passed:
            print(f"  ✗ {r.name}: {r.detail}")
    return 0 if passed == len(results) and results else 1


if __name__ == "__main__":
    sys.exit(main())
