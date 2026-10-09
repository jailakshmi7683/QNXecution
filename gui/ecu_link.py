"""ecu_link.py - SSH adapter between the Windows GUI and the QNX ECU monitor.

The adapter starts the monitor itself over one SSH channel, so only one monitor
exists. Commands go to the monitor's stdin; events and CLI output come back on
stdout. All parsing of monitor output lives in this file. The GUI can only send
a fixed whitelist of monitor commands, never arbitrary shell input.
"""
import queue
import re
import shlex
import threading
import time
import socket

import paramiko

SERVICES = ["wheel_speed", "abs", "traction_control", "dashboard"]
FAULTS = ("crash", "stuck", "overrun", "deadlock", "clear")

# CONFIRMED: copied from dependency_graph[] in common.h
DEPENDS_ON = {
    "wheel_speed": [],
    "abs": ["wheel_speed"],
    "traction_control": ["wheel_speed"],
    "dashboard": ["abs", "traction_control", "wheel_speed"],
}

LOG_RE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\] (\S+) \| (\S+) \| (.*)$")
STATUS_RE = re.compile(r"^(\w+)\s+(\d+)\s+([A-Z_]+)\s+(-?\d+)\s+(-|\d+ ms ago)\s+(\d+)/(\d+)$")
STATE_RE = re.compile(r"^(\w+): (\w+) -> (\w+)$")
SPAWNED_RE = re.compile(r"^(\w+) started \(pid=(\d+)\)")
RECOVERED_RE = re.compile(
    r"^(\w+) healthy again: recovery_time=(\d+) ms, spawn_to_progress=(\d+) ms, "
    r"attempts=(\d+)/(\d+), budget=(\d+) ms: (MET|MISSED)")
ROOT_RE = re.compile(r"^(\w+) is ROOT CAUSE")
SYMPTOM_RE = re.compile(r"^(\w+) (?:depends on|is a SYMPTOM of upstream) (\w+)")
FIRST_WORD_RE = re.compile(r"^(\w+):")


def _blank():
    return {"state": "UNKNOWN", "pid": None, "tier": None, "last_progress_ms": None,
            "attempts": 0, "max_retries": None, "spawns": 0, "restarts": 0,
            "role": None, "upstream": None, "last_recovery_ms": None,
            "budget_ms": None, "budget_result": None}


class EcuLink:
    def __init__(self, host="10.0.0.1", user="qnxuser", bin_dir="/data/home/qnxuser/ecu"):
        self.host, self.user, self.bin_dir = host, user, bin_dir
        self.events = queue.Queue()          # ("raw", line) and ("event", dict) for the GUI
        self._lock = threading.Lock()
        self._ssh = None
        self._chan = None
        self._poll_stop = threading.Event()
        self._last_rx = 0.0
        self._last_inject = {}
        self._last_inject_any = 0.0
        self.services = {n: _blank() for n in SERVICES}
        self.stats = {"injections": 0, "recoveries": 0, "met": 0, "missed": 0, "degraded": 0}
        self.recovery_ms = []

    # ---------------- connection ----------------
    def connect(self, password, trust_new_host=False):
        """Host key is verified against ~/.ssh/known_hosts unless trust_new_host=True
        (trust-on-first-use: accepts whatever key the Pi shows once; only use on a direct link)."""
        ssh = paramiko.SSHClient()
        ssh.load_system_host_keys()
        ssh.set_missing_host_key_policy(
            paramiko.AutoAddPolicy() if trust_new_host else paramiko.RejectPolicy())
        ssh.connect(self.host, username=self.user, password=password, timeout=8,
                    banner_timeout=15, auth_timeout=15,
                    look_for_keys=False, allow_agent=False)
        ssh.get_transport().set_keepalive(5)
        self._ssh = ssh

    def _run(self, cmd, timeout=8):
        _, out, _ = self._ssh.exec_command(cmd, timeout=timeout)
        return out.read().decode("utf-8", "replace")

    def monitor_pids(self):
        pids = []
        for line in self._run("pidin").splitlines():
            p = line.split()
            if len(p) >= 3 and p[0].isdigit() and (p[2] == "monitor" or p[2].endswith("/monitor")):
                pids.append(int(p[0]))
        return pids

    def kill_monitor(self):
        """SIGTERM: the monitor shuts its services down cleanly."""
        self._run("slay monitor")

    @property
    def running(self):
        c = self._chan
        return c is not None and not c.closed and not c.exit_status_ready()

    def start_monitor(self):
        if self.monitor_pids():
            raise RuntimeError("a monitor is already running on the target")
        chan = self._ssh.get_transport().open_session()
        chan.set_combine_stderr(True)
        chan.settimeout(1.0)
        d = shlex.quote(self.bin_dir)
        chan.exec_command("export ECU_BIN_DIR=%s; cd %s && exec ./monitor" % (d, d))
        self._chan = chan
        self._last_rx = time.monotonic()
        threading.Thread(target=self._read_loop, daemon=True).start()

    def start_polling(self, period=1.0):
        self._poll_stop.clear()

        def run():
            while not self._poll_stop.is_set() and self.running:
                try:
                    self._chan.send("status\n")
                except Exception:
                    break
                self._poll_stop.wait(period)
        threading.Thread(target=run, daemon=True).start()

    def stop(self, wait=4.0):
        """Ask the monitor to quit (it stops its own services), then close the link."""
        self._poll_stop.set()
        try:
            if self.running:
                self._chan.send("quit\n")
            t0 = time.monotonic()
            while self.running and time.monotonic() - t0 < wait:
                time.sleep(0.1)
        except Exception:
            pass
        self.close()

    def close(self):
        self._poll_stop.set()
        for obj in (self._chan, self._ssh):
            try:
                if obj:
                    obj.close()
            except Exception:
                pass

    # ---------------- commands (whitelist only) ----------------
    def request(self, what):
        if what not in ("status", "metrics", "timeline 30", "timeline all"):
            raise ValueError("command not allowed")
        self._chan.send(what + "\n")

    def reset(self, svc):
        if svc != "all" and svc not in SERVICES:
            raise ValueError("unknown service")
        self._chan.send("reset %s\n" % svc)

    def inject(self, svc, fault, persist=False, ms=None):
        if svc not in SERVICES or fault not in FAULTS:
            raise ValueError("unsupported injection")
        if not self.running:
            raise RuntimeError("not connected to a monitor")
        now = time.monotonic()
        if now - self._last_inject.get(svc, 0.0) < 2.0 or now - self._last_inject_any < 0.5:
            raise RuntimeError("injection ignored: too soon after the previous one")
        cmd = "inject %s %s" % (svc, fault)
        if persist:
            cmd += " persist"
        if ms is not None:
            ms = int(ms)
            if not 100 <= ms <= 60000:
                raise ValueError("duration must be 100..60000 ms")
            cmd += " %d" % ms
        self._last_inject[svc] = self._last_inject_any = now
        self._chan.send(cmd + "\n")
        return cmd

    # ---------------- parsing ----------------
    def _read_loop(self):
        buf = ""
        while True:
            try:
                data = self._chan.recv(4096)
            except socket.timeout:
                continue
            except Exception as e:
                if type(e).__name__ == "timeout":
                    continue
                break
            if not data:
                break
            buf += data.decode("utf-8", "replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                self._handle_line(line.rstrip("\r"))
        self.events.put(("event", {"ts": "", "event": "LINK_CLOSED",
                                   "text": "monitor connection closed"}))

    def _handle_line(self, line):
        self._last_rx = time.monotonic()
        self.events.put(("raw", line))
        m = LOG_RE.match(line)
        if m:
            self._on_event(*m.groups())
            return
        m = STATUS_RE.match(line.strip())
        if m:
            self._on_status(*m.groups())

    def _on_status(self, name, tier, state, pid, age, attempts, max_r):
        if name not in self.services:
            return
        with self._lock:
            s = self.services[name]
            s.update(state=state, pid=int(pid) if int(pid) > 0 else None, tier=int(tier),
                     attempts=int(attempts), max_retries=int(max_r),
                     last_progress_ms=None if age == "-" else int(age.split()[0]))

            self._clear_stale_affected()

    def _on_event(self, ts, sender, ev, text):
        with self._lock:
            sv = self.services
            if ev == "STATE":
                m = STATE_RE.match(text)
                if m and m.group(1) in sv:
                    sv[m.group(1)]["state"] = m.group(3)
                    if m.group(3) == "HEALTHY":
                        sv[m.group(1)]["role"] = sv[m.group(1)]["upstream"] = None
            elif ev == "SPAWNED":
                m = SPAWNED_RE.match(text)
                if m and m.group(1) in sv:
                    s = sv[m.group(1)]
                    s["pid"] = int(m.group(2))
                    s["spawns"] += 1
                    s["restarts"] = s["spawns"] - 1
            elif ev == "ROOT_CAUSE":
                m = ROOT_RE.match(text)
                if m and m.group(1) in sv:
                    sv[m.group(1)]["role"] = "root"
            elif ev == "SYMPTOM":
                m = SYMPTOM_RE.match(text)
                if m and m.group(1) in sv:
                    sv[m.group(1)]["role"] = "affected"
                    sv[m.group(1)]["upstream"] = m.group(2)
            elif ev in ("SYMPTOM_CLEARED", "ESCALATE"):
                m = FIRST_WORD_RE.match(text)
                if m and m.group(1) in sv:
                    sv[m.group(1)]["role"] = None
                if ev == "ESCALATE":
                    self.stats["degraded"] += 1
            elif ev == "RECOVERED":
                m = RECOVERED_RE.match(text)
                if m and m.group(1) in sv:
                    s = sv[m.group(1)]
                    s["last_recovery_ms"] = int(m.group(2))
                    s["budget_ms"] = int(m.group(6))
                    s["budget_result"] = m.group(7)
                    s["role"] = s["upstream"] = None
                    self.stats["recoveries"] += 1
                    self.stats["met" if m.group(7) == "MET" else "missed"] += 1
                    self.recovery_ms.append(int(m.group(2)))
            elif ev == "INJECT":
                self.stats["injections"] += 1
            self._clear_stale_affected()
        self.events.put(("event", {"ts": ts, "event": ev, "text": text}))

    def _clear_stale_affected(self):          # call only while self._lock is held
        for s in self.services.values():
            up = s["upstream"]
            if s["role"] == "affected" and up in self.services \
                    and self.services[up]["state"] == "HEALTHY":
                s["role"] = s["upstream"] = None

    def snapshot(self):
        with self._lock:
            return {"services": {k: dict(v) for k, v in self.services.items()},
                    "stats": dict(self.stats), "recovery_ms": list(self.recovery_ms),
                    "live": self.running and (time.monotonic() - self._last_rx) < 4.0}