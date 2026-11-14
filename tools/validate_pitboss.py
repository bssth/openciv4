#!/usr/bin/env python3
"""Exercise PitBoss with an owned BtS 3.19 install, without a display.

Runs two clients, timer expiry with nobody connected, late join/rejoin, pause,
credential rejection, SIGKILL recovery and graceful shutdown. Artifacts stay local.
"""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--civ4", required=True, type=Path)
    parser.add_argument("--build", type=Path, default=ROOT / "build/linux-release")
    parser.add_argument("--out", type=Path, default=ROOT / "build/pitboss-validation")
    parser.add_argument("--mod", default="")
    args = parser.parse_args()
    exe = args.build.resolve() / "bin/oc4core"
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", OC4_SOUND="0")
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    results = {}
    with tempfile.TemporaryDirectory(prefix="oc4-pb-", dir="/tmp") as private:
        state = Path(private) / "state"
        password = Path(private) / "password"
        password.write_text("validation-password\n")
        password.chmod(0o600)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        config = Path(private) / "server.ini"
        config.write_text(f"""[server]
civ4={args.civ4.resolve()}
mod={args.mod}
state_dir={state}
port={port}
turn_hours=0.001
[game]
humans=0,1
players=3
seed=7
size=WORLDSIZE_DUEL
""")
        for seat in (0, 1):
            subprocess.run([str(exe), "--pitboss-password", str(state), str(seat)],
                           input="validation-password\n", text=True, env=env, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        def admin(*command):
            return subprocess.check_output([str(exe), "--pitboss-admin", str(state), *command],
                                           env=env, text=True, timeout=15).strip()

        def status():
            return dict((k, int(v)) for k, v in re.findall(r"(\w+)=(\d+)", admin("status")))

        streams = []
        processes = []

        def launch(name, arguments):
            local = dict(env, OC4_DOCUMENTS=str(Path(private) / name))
            stream = (out / (name + ".log")).open("w")
            streams.append(stream)
            process = subprocess.Popen([str(exe), *map(str, arguments)], env=local,
                                       stdout=stream, stderr=subprocess.STDOUT)
            processes.append(process)
            return process

        def start(name):
            server = launch(name, ["--pitboss", config])
            until = time.monotonic() + 45
            while time.monotonic() < until:
                text = (out / (name + ".log")).read_text(errors="replace")
                if "PitBoss listening" in text:
                    return server
                if server.poll() is not None:
                    raise RuntimeError("Server failed: " + str(out / (name + ".log")))
                time.sleep(0.05)
            raise RuntimeError("Server startup timed out")

        def client(name, seat, seconds=4, script=None):
            arguments = ["--civ4", args.civ4.resolve(), "--pitboss-join", f"127.0.0.1:{port}",
                         "--seat", seat, "--password-file", password, "--pitboss-check", seconds]
            if args.mod:
                arguments += ["--mod", args.mod]
            if script:
                arguments += ["--exec-first", script]
            return launch(name, arguments)

        def passed(name, process, expected=0):
            code = process.wait(timeout=60)
            text = (out / (name + ".log")).read_text(errors="replace")
            if code != expected or "Traceback (most recent call last)" in text:
                raise RuntimeError("Validation failed: " + str(out / (name + ".log")))
            if expected == 0 and "PitBoss check passed" not in text:
                raise RuntimeError("Client did not finish: " + name)
            reason = {"bad_password": "authentication failed", "wrong_actor": "unauthorized command",
                      "invalid_city_task": "unauthorized command"}.get(name)
            if reason and reason not in text:
                raise RuntimeError("Expected rejection reason missing: " + name)
            results[name] = "passed"
            print(name, "passed", flush=True)

        try:
            server = start("server")
            # The standard Python command path reaches the authenticated seat validator.
            script = Path(private) / "orders.py"
            script.write_text("from CvPythonExtensions import *\n"
                              "CyMessageControl().sendResearch(CyGlobalContext().getInfoTypeForString('TECH_MINING'), False)\n"
                              "CyMessageControl().sendTurnComplete()\n")
            first = client("client0", 0, 10, script)
            second = client("client1", 1, 10, script)
            passed("client0", first)
            passed("client1", second)
            before = status()
            time.sleep(5)
            after = status()
            if after["turn"] <= before["turn"] or after["connected_mask"]:
                raise RuntimeError("Offline turn timer did not advance")
            results["offline_timer"] = "passed"
            passed("late_join", client("late_join", 1))
            admin("pause")
            paused = status()
            time.sleep(0.3)
            if status() != paused:
                raise RuntimeError("Paused game advanced")
            server.kill()
            server.wait(timeout=10)
            time.sleep(0.3)
            server = start("recovered")
            if status() != paused:
                raise RuntimeError("Recovery changed the paused game or consumed downtime")
            passed("recovered_client", client("recovered_client", 0, 1))
            results["paused_recovery"] = "passed"
            admin("resume")
            passed("rejoined", client("rejoined", 0, 3))
            # Crash during a turn (after a short client run, without an operator save).
            server.kill()
            server.wait(timeout=10)
            server = start("journal_recovered")
            passed("journal_client", client("journal_client", 1, 3))
            results["journal_recovery"] = "passed"
            password.write_text("incorrect-password\n")
            passed("bad_password", client("bad_password", 0, 1), expected=1)
            password.write_text("validation-password\n")
            script.write_text("from CvPythonExtensions import *\n"
                              "CyMessageControl().sendEmpireSplit(1, 0)\n")
            passed("wrong_actor", client("wrong_actor", 0, 1, script), expected=1)
            script.write_text("from CvPythonExtensions import *\n"
                              "CyMessageControl().sendDoTask(-100, 99999, 0, 0, False, False, False, False)\n")
            passed("invalid_city_task", client("invalid_city_task", 0, 1, script), expected=1)
            admin("stop")
            if server.wait(timeout=30):
                raise RuntimeError("Server shutdown failed")
            results["graceful_stop"] = "passed"
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
            for stream in streams:
                stream.close()
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print("Results:", out / "results.json")


if __name__ == "__main__":
    main()
