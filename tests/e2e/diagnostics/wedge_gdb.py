"""Solo-daemon #58 probe: any start() outcome other than success within WEDGE_WAIT_S
is a wedge (the client RPC errors ~20s before the daemon's 30s semaphore releases), so
attach gdb to every container process and dump backtraces to WEDGE_OUT_DIR. Exits 0."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time

from logoscore import LogoscoreDockerDaemon

MODULE = "delivery_module"
CONFIG = json.dumps({
    "logLevel": "DEBUG", "listenAddress": "0.0.0.0", "tcpPort": 60000,
    "clusterId": "198", "numShardsInNetwork": 1,
    "relay": True, "store": False, "filter": False, "lightpush": False,
    "peerExchange": False, "discv5Discovery": False, "reliabilityEnabled": True,
})

IMAGE = os.environ.get("LOGOSCORE_IMAGE", "logoscore:smoke-portable")
MODULES_DIR = os.environ.get("LOGOS_MODULES_DIR", "result/modules")
CLI = os.environ.get("LOGOSCORE_CLI", "logoscore")
OUT_DIR = os.environ.get("WEDGE_OUT_DIR", "wedge-diagnostics")
WAIT_S = float(os.environ.get("WEDGE_WAIT_S", "25"))
LOG_TAIL_S = float(os.environ.get("WEDGE_LOG_TAIL_S", "35"))
WANT_CORE = os.environ.get("WEDGE_CORE") == "1"
GDB_DEADLINE = "180"


def run(*a: str) -> subprocess.CompletedProcess:
    return subprocess.run(a, capture_output=True, text=True)


def write(name: str, text: str) -> None:
    with open(os.path.join(OUT_DIR, name), "w") as f:
        f.write(text)


def container_pids(cname: str) -> list[tuple[str, str]]:
    lines = run("docker", "top", cname, "-eo", "pid,comm").stdout.strip().splitlines()
    pids = []
    for line in lines[1:]:
        parts = line.split(None, 1)
        if parts and parts[0].isdigit():
            pids.append((parts[0], parts[1] if len(parts) > 1 else "?"))
    return pids


def gdb_cmd(pid: str) -> list[str]:
    exprs = [
        "set pagination off",
        f"set sysroot /proc/{pid}/root",
        "info sharedlibrary",
        "info threads",
        "thread apply all bt",
    ]
    if WANT_CORE:
        exprs.append(f"generate-core-file {os.path.join(OUT_DIR, 'core.' + pid)}")
    cmd = ["sudo", "timeout", "-s", "KILL", GDB_DEADLINE, "gdb", "-p", pid, "-batch"]
    for e in exprs:
        cmd += ["-ex", e]
    return cmd


def dump_wedge(cname: str) -> int:
    pids = container_pids(cname)
    maps, kstacks = [], []
    for pid, comm in pids:
        head = f"===== pid {pid} ({comm}) =====\n"
        maps.append(head + run("sudo", "cat", f"/proc/{pid}/maps").stdout)
        kstacks.append(head + run("sudo", "sh", "-c",
                                   f"for t in /proc/{pid}/task/*; do echo \"== $t ==\"; cat $t/stack 2>/dev/null; done").stdout)
    write("proc_maps.txt", "\n\n".join(maps))
    write("kernel_stacks.txt", "\n\n".join(kstacks))

    running = [(pid, comm, subprocess.Popen(gdb_cmd(pid), stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE, text=True))
               for pid, comm in pids]
    bts = []
    for pid, comm, p in running:
        out, err = p.communicate()
        bts.append(f"===== pid {pid} ({comm}) =====\n{out}" + (f"\n--- gdb stderr ---\n{err}" if err else ""))
    write("start_backtraces.txt", "\n\n".join(bts))
    return len(pids)


def inspect_artifacts() -> None:
    md = os.path.join(MODULES_DIR, MODULE)
    plugin = os.path.join(md, "delivery_module_plugin.so")
    lines = [f"modules subdir: {md}", "== ls ==", run("ls", "-l", md).stdout]
    if os.path.isdir(md):
        for f in sorted(os.listdir(md)):
            if ".so" in f:
                lines.append(run("sha256sum", os.path.join(md, f)).stdout.strip())
    grep = run("sh", "-c",
               f"strings {plugin} | grep -E 'about to invoke|invoke returned startResult|callApiRetVoid callback fired' "
               "|| echo NO_INSTRUMENTATION_STRINGS")
    lines += ["== instrumentation literals in loaded plugin ==", grep.stdout.strip() or grep.stderr.strip()]
    ldd_p = run("ldd", plugin)
    lines += ["== ldd plugin ==", ldd_p.stdout + ldd_p.stderr]
    lib = os.path.join(md, "liblogosdelivery.so")
    if os.path.exists(lib):
        ldd_l = run("ldd", lib)
        lines += ["== ldd liblogosdelivery ==", ldd_l.stdout + ldd_l.stderr]
    write("artifacts.txt", "\n".join(lines))


def main() -> int:
    os.makedirs(OUT_DIR, exist_ok=True)
    summary = [f"image={IMAGE}", f"modules_dir={MODULES_DIR}", f"wait_s={WAIT_S}"]

    with LogoscoreDockerDaemon(image=IMAGE, modules_dir=MODULES_DIR) as daemon:
        client = daemon.client(binary=CLI)
        client.load_module(MODULE)
        summary.append(f"createNode: {client.call(MODULE, 'createNode', CONFIG, timeout=90)}")

        cname = daemon.container_name
        summary.append(f"container={cname}")
        summary.append("procs: " + ", ".join(f"{p}:{c}" for p, c in container_pids(cname)))

        start_out: dict = {}

        def do_start() -> None:
            try:
                start_out["r"] = client.call(MODULE, "start", timeout=120)
            except Exception as e:
                start_out["e"] = repr(e)

        t_start = time.monotonic()
        threading.Thread(target=do_start, daemon=True).start()
        while time.monotonic() - t_start < WAIT_S and not start_out:
            time.sleep(0.5)

        r = start_out.get("r")
        good = isinstance(r, dict) and r.get("success") is True
        summary.append(f"WEDGED={not good} start_out={start_out or 'NONE (start never returned)'}")

        if not good:
            try:
                n = dump_wedge(cname)
                summary.append(f"gdb backtraces captured for {n} container procs")
            except Exception as e:
                summary.append(f"gdb dump failed: {e!r}")
            try:
                inspect_artifacts()
                summary.append("artifact inspection -> artifacts.txt")
            except Exception as e:
                summary.append(f"artifact inspection failed: {e!r}")
            probes: dict = {}
            for m in ("version", "getAvailableConfigs"):
                pt = time.monotonic()
                try:
                    probes[m] = {"ok": client.call(MODULE, m, timeout=15), "s": round(time.monotonic() - pt, 1)}
                except Exception as e:
                    probes[m] = {"err": repr(e), "s": round(time.monotonic() - pt, 1)}
            summary.append(f"post-wedge RPC probes: {probes}")
            while time.monotonic() - t_start < LOG_TAIL_S:
                time.sleep(1)
            summary.append(f"start_out after {LOG_TAIL_S}s: {start_out}")
        else:
            summary.append("start returned success (good build) — gdb skipped")

        logs = run("docker", "logs", cname)
        write("daemon.log", logs.stdout + logs.stderr)

    write("summary.txt", "\n".join(summary) + "\n")
    print("\n".join(summary))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print(f"wedge_gdb diagnostic error: {e!r}")
        sys.exit(0)
