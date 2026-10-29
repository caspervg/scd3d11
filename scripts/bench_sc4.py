# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow", "psutil"]
# ///
"""Run SimCity 4 unattended in an isolated user directory and drive it through the game's own command
listener (-NetCommandGenerator:enabled, HTTP on port 50020).

The test user directory holds only the DLL under test. --mirror-plugins also links in a real plugin
folder (directories as junctions, big files as hard links, small files copied) without its SCD3D11 and
autosave files. Plugins that write into their own folders then write into the real one.

Each benchmark city is copied alone into its own region, so LoadCity with an empty name opens it.
Nothing is ever saved. The log, command replies and snapshots land in runtime-captures/bench-<time>.

  uv run scripts/bench_sc4.py "C:/.../City - Senneterre.sc4" Zoom5 "sleep 3" ScrollRight "sleep 6" ScrollRightStop

A command may contain {capture}, which becomes the capture folder (for TakeSnapshot). Two commands are the
script's own: "focus" brings the game to the foreground (SC4 drops to about 20 frames per second without
it), "screenshot <name>" focuses the game and saves what its window shows on screen as <name>.png in the
capture folder, and "measure <seconds>" does that and then reports the frame rate from GetFrameCount and
the CPU time of the game's busiest thread (the one that renders) and of SCD3D12Server over that many seconds,
and the game's private memory at the end. Mouse and keys go to the game's window as window messages, in client
pixels: "mdrag <x0> <y0> <x1> <y1>" drags with the middle button, "wheel <x> <y> <notches>" scrolls (positive
zooms in), "click <x> <y>" clicks the left button and "key <letter>" presses a key.
"""

import argparse
import ctypes
import ctypes.wintypes
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from datetime import datetime
from pathlib import Path

import psutil

REPO = Path(__file__).resolve().parent.parent
GAME_EXE = Path(r"C:\Program Files (x86)\GOG Galaxy\Games\SimCity 4 Deluxe Edition\Apps\SimCity 4.exe")
LIVE_PLUGINS = Path(r"C:\Users\caspe\OneDrive - Maplix\SimCity 4\Plugins")
SKIP = re.compile(r"^(SCD3D11\..*|SC4AutoSave\..*|.*\.(log|csv|bak))$", re.IGNORECASE)


def send(command: str, timeout: float = 60) -> str | None:
    """POST one command. The listener wants it to end with a newline."""
    request = urllib.request.Request("http://127.0.0.1:50020/", data=(command + "\n").encode("ascii"), method="POST")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read().decode("utf-8", "replace").strip()
    except OSError:
        return None


def focus(pid: int) -> bool:
    """Brings the process's largest visible window to the foreground. Windows only lets the foreground
    thread hand the foreground over, so this thread joins its input queue for the call."""
    user32 = ctypes.windll.user32
    user32.GetForegroundWindow.restype = ctypes.c_void_p
    windows: list[tuple[int, int]] = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def collect(hwnd, _):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            rect = (ctypes.c_long * 4)()
            user32.GetWindowRect(ctypes.c_void_p(hwnd), rect)
            windows.append(((rect[2] - rect[0]) * (rect[3] - rect[1]), hwnd))
        return True

    user32.EnumWindows(collect, None)
    if not windows:
        return False
    window = ctypes.c_void_p(max(windows)[1])
    foreground_thread = user32.GetWindowThreadProcessId(ctypes.c_void_p(user32.GetForegroundWindow()), None)
    own_thread = ctypes.windll.kernel32.GetCurrentThreadId()
    user32.AttachThreadInput(own_thread, foreground_thread, True)
    user32.ShowWindow(window, 9)  # SW_RESTORE
    user32.SetForegroundWindow(window)
    user32.AttachThreadInput(own_thread, foreground_thread, False)
    return user32.GetForegroundWindow() == window.value


def screenshot(pid: int, path: Path) -> bool:
    """Saves the game window's client area as the screen shows it, which is what the player sees whichever
    renderer drew it."""
    from PIL import ImageGrab
    user32 = ctypes.windll.user32
    user32.SetProcessDPIAware()
    user32.GetForegroundWindow.restype = ctypes.c_void_p
    focus(pid)
    time.sleep(0.5)
    window = ctypes.c_void_p(user32.GetForegroundWindow())
    rect = (ctypes.c_long * 4)()
    origin = (ctypes.c_long * 2)()
    if not user32.GetClientRect(window, rect) or not user32.ClientToScreen(window, origin):
        return False
    box = (origin[0], origin[1], origin[0] + rect[2], origin[1] + rect[3])
    ImageGrab.grab(bbox=box, all_screens=True).save(path)
    return True


def game_window(pid: int) -> int:
    """The process's largest visible window."""
    user32 = ctypes.windll.user32
    windows: list[tuple[int, int]] = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def collect(hwnd, _):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            rect = (ctypes.c_long * 4)()
            user32.GetWindowRect(ctypes.c_void_p(hwnd), rect)
            windows.append(((rect[2] - rect[0]) * (rect[3] - rect[1]), hwnd))
        return True

    user32.EnumWindows(collect, None)
    return max(windows)[1] if windows else 0


def mouse(pid: int, command: str) -> bool:
    """Posts mouse and key messages to the game's window, as the player's input would arrive."""
    user32 = ctypes.windll.user32
    user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
    window = game_window(pid)
    if not window:
        return False
    post = lambda message, w, x, y: user32.PostMessageW(window, message, w, (y & 0xFFFF) << 16 | (x & 0xFFFF))
    words = command.split()
    if words[0] == "mdrag":
        x0, y0, x1, y1 = map(int, words[1:5])
        post(0x200, 0, x0, y0)  # WM_MOUSEMOVE
        time.sleep(0.1)
        post(0x207, 0x10, x0, y0)  # WM_MBUTTONDOWN with MK_MBUTTON
        steps = 30
        for i in range(1, steps + 1):
            time.sleep(0.03)
            post(0x200, 0x10, x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        time.sleep(0.1)
        post(0x208, 0, x1, y1)  # WM_MBUTTONUP
    elif words[0] == "wheel":
        x, y, notches = int(words[1]), int(words[2]), int(words[3])
        point = ctypes.wintypes.POINT(x, y)
        user32.ClientToScreen(ctypes.c_void_p(window), ctypes.byref(point))
        for _ in range(abs(notches)):
            # WM_MOUSEWHEEL carries screen coordinates.
            post(0x20A, ((120 if notches > 0 else -120) & 0xFFFF) << 16, point.x, point.y)
            time.sleep(0.05)
    elif words[0] == "click":
        x, y = int(words[1]), int(words[2])
        post(0x200, 0, x, y)
        time.sleep(0.2)
        post(0x201, 1, x, y)  # WM_LBUTTONDOWN
        time.sleep(0.05)
        post(0x202, 0, x, y)  # WM_LBUTTONUP
    elif words[0] == "key":
        code = ord(words[1].upper())
        post(0x100, code, 0, 0)  # WM_KEYDOWN
        post(0x102, ord(words[1]), 0, 0)  # WM_CHAR
        time.sleep(0.05)
        post(0x101, code, 0, 0)  # WM_KEYUP
    return True


def thread_times(pid: int) -> dict[int, float]:
    return {thread.id: thread.user_time + thread.system_time for thread in psutil.Process(pid).threads()}


def server_time(pid: int) -> float:
    """CPU seconds of the game's own SCD3D12Server, if it runs one."""
    total = 0.0
    for process in psutil.Process(pid).children(recursive=True):
        try:
            if process.name() == "SCD3D12Server.exe":
                times = process.cpu_times()
                total += times.user + times.system
        except psutil.Error:
            pass
    return total


def frame_count() -> int:
    reply = send("GetFrameCount", timeout=10) or ""
    values = reply.split("Result:")[-1].split()
    return int(float(values[0])) if values else 0


def measure(pid: int, seconds: float) -> str:
    focused = focus(pid)
    time.sleep(1)
    threads, server, frames, start = thread_times(pid), server_time(pid), frame_count(), time.perf_counter()
    time.sleep(seconds)
    threads_after, server_after = thread_times(pid), server_time(pid)
    frames_after, elapsed = frame_count(), time.perf_counter() - start
    count = max(frames_after - frames, 1)
    busiest = max(threads_after[tid] - threads.get(tid, 0.0) for tid in threads_after)
    total = sum(threads_after.values()) - sum(threads.values())
    private = psutil.Process(pid).memory_info().private >> 20
    return (f"{count / elapsed:.1f} fps ({elapsed * 1000 / count:.2f} ms per frame), render thread "
            f"{busiest * 1000 / count:.2f} ms CPU per frame ({busiest / elapsed:.0%}), whole game "
            f"{total / elapsed:.0%}, server {(server_after - server) * 1000 / count:.2f} ms CPU per frame, "
            f"game private memory {private} MB{'' if focused else ', NOT in the foreground'}")


def game_running() -> bool:
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq SimCity 4.exe", "/NH"], capture_output=True, text=True)
    return "SimCity 4.exe" in out.stdout


def mirror_plugins(source: Path, plugins: Path) -> None:
    marker = plugins / ".mirrored"
    if marker.exists():
        return
    for item in source.iterdir():
        if SKIP.match(item.name):
            continue
        target = plugins / item.name
        if item.is_dir():
            subprocess.run(["cmd", "/c", "mklink", "/J", str(target), str(item)], check=True, capture_output=True)
        elif item.stat().st_size > 16 << 20:
            os.link(item, target)
        else:
            shutil.copy2(item, target)
    marker.touch()


def prepare_region(user_dir: Path, city: Path) -> str:
    name = "Bench " + re.sub(r"[^A-Za-z0-9 -]", "_", city.stem.removeprefix("City - "))
    region = user_dir / "Regions" / name
    shutil.rmtree(region, ignore_errors=True)
    region.mkdir(parents=True)
    shutil.copy2(city.parent / "config.bmp", region)
    (region / "region.ini").write_text(
        f"[Regional Settings]\r\nName = {name}\r\nTerrain type = 0\r\nWater Min = 60\r\nWater Max = 100\r\n")
    shutil.copy2(city, region)
    return name


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("city", type=Path)
    parser.add_argument("commands", nargs="*", help='game commands, or "sleep <seconds>"')
    parser.add_argument("--city-name", help="the name saved in the city, when it differs from the file name")
    parser.add_argument("--delay", type=float, default=3.0, help="seconds after each command")
    parser.add_argument("--size", default="1920x1080")
    parser.add_argument("--dll", type=Path, default=REPO / "cmake-build-release" / "SCD3D11.dll")
    parser.add_argument("--server", type=Path, default=REPO / "cmake-build-server-release" / "SCD3D12Server.exe",
                        help="copied into Plugins/SCD3D12, for Renderer=server")
    parser.add_argument("--ini", default="VSync=off", help="SCD3D11.ini lines, separated by ;")
    parser.add_argument("--arg", action="append", default=["-DrawStats"], help="extra game argument")
    parser.add_argument("--mirror-plugins", action="store_true")
    parser.add_argument("--plugin-source", type=Path, default=LIVE_PLUGINS)
    parser.add_argument("--user-dir", type=Path, default=Path(os.environ["LOCALAPPDATA"]) / "SCD3D12Bench")
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()

    if game_running():
        sys.exit("SimCity 4 is already running")
    plugins = args.user_dir / "Plugins"
    plugins.mkdir(parents=True, exist_ok=True)
    if args.mirror_plugins:
        mirror_plugins(args.plugin_source, plugins)
    shutil.copy2(args.dll, plugins / "SCD3D11.dll")
    (plugins / "SCD3D12Server.exe").unlink(missing_ok=True)
    if args.server.exists():
        (plugins / "SCD3D12").mkdir(exist_ok=True)
        shutil.copy2(args.server, plugins / "SCD3D12" / "SCD3D12Server.exe")
    (plugins / "SCD3D11.ini").write_text("[SCD3D11]\r\n" + "\r\n".join(args.ini.split(";")) + "\r\n")
    region = prepare_region(args.user_dir, args.city)

    capture = REPO / "runtime-captures" / f"bench-{datetime.now():%Y%m%d-%H%M%S}"
    capture.mkdir(parents=True)
    log = args.user_dir / "Logs" / "SCD3D11.log"
    server_log = args.user_dir / "Logs" / "SCD3D12Server.log"
    log.unlink(missing_ok=True)
    server_log.unlink(missing_ok=True)
    transcript: list[str] = []

    def run(command: str) -> str | None:
        reply = send(command)
        transcript.append(f"{command} -> {reply!r}")
        print(transcript[-1], flush=True)
        return reply

    def wait_for(what: str, condition) -> None:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            if condition():
                return
            if game.poll() is not None:
                raise RuntimeError(f"SimCity 4 exited while waiting for {what}")
            time.sleep(1)
        raise RuntimeError(f"timed out waiting for {what}")

    width, height = args.size.split("x")
    # One exact string: list quoting would escape the quotes around -UserDir, and SC4 would then fall back
    # to the player's own user directory.
    # --arg=-f asks for exclusive fullscreen instead of a window.
    window = "-f" if "-f" in args.arg else "-w"
    command_line = " ".join([f'"{GAME_EXE}"', "-NetCommandGenerator:enabled", window, "-Intro:off",
                             "-CustomResolution:enabled", f"-r{width}x{height}x32", f'-UserDir:"{args.user_dir}\\"',
                             *args.arg])
    game = subprocess.Popen(command_line, cwd=GAME_EXE.parent)
    try:
        # Replies look like "Command: GetAppState Result: 1 0" over several lines. The first value is the
        # state: 0 still starting, 1 region view, 2 city view.
        def state() -> str:
            reply = send("GetAppState", timeout=10) or ""
            values = reply.split("Result:")[-1].split()
            return values[0] if "Result:" in reply and values else ""
        wait_for("the listener", lambda: state() != "")
        if not log.exists():
            raise RuntimeError(f"SC4 is not using {args.user_dir}, quitting before it touches anything else")
        wait_for("the region view", lambda: state() == "1")
        run(f'LoadRegion "{region}"')
        time.sleep(5)
        # An empty name opens whatever tile comes first, often an empty one, so ask for the city by name.
        city_name = args.city_name or args.city.stem.removeprefix("City - ")
        run(f'LoadCity "{city_name}" any full')
        wait_for("the city view", lambda: state() == "2")
        time.sleep(5)
        loaded = (run("GetCityName") or "").split("Result:")[-1].strip()
        if city_name not in loaded:
            raise RuntimeError(f"loaded {loaded!r} instead of {city_name!r}")
        for command in args.commands:
            if match := re.fullmatch(r"sleep ([\d.]+)", command, re.IGNORECASE):
                time.sleep(float(match[1]))
                continue
            if command == "focus":
                transcript.append(f"focus -> {focus(game.pid)}")
                print(transcript[-1], flush=True)
                continue
            if match := re.fullmatch(r"screenshot (\S+)", command):
                transcript.append(f"{command} -> {screenshot(game.pid, capture / (match[1] + '.png'))}")
                print(transcript[-1], flush=True)
                continue
            if re.fullmatch(r"(mdrag|wheel|click|key) .+", command):
                transcript.append(f"{command} -> {mouse(game.pid, command)}")
                print(transcript[-1], flush=True)
                time.sleep(1)
                continue
            if match := re.fullmatch(r"measure ([\d.]+)", command):
                transcript.append(f"{command} -> {measure(game.pid, float(match[1]))}")
                print(transcript[-1], flush=True)
                continue
            run(command.replace("{capture}", str(capture)))
            time.sleep(args.delay)
        run("GetFrameRate")
    finally:
        # Nothing is ever saved, so a kill is safe if the quit dialog blocks.
        # The first argument skips the confirmation dialog, the second saves.
        transcript.append(f"QuitGame 1 0 -> {send('QuitGame 1 0', timeout=10)!r}")
        try:
            game.wait(10)
        except subprocess.TimeoutExpired:
            game.kill()
        (capture / "commands.txt").write_text("\n".join(transcript) + "\n")
        for path in (log, server_log):
            if path.exists():
                shutil.copy2(path, capture)
        print(capture)
    return 0


if __name__ == "__main__":
    sys.exit(main())
