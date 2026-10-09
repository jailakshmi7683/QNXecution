"""ecu_gui.py - ECU GUARDIAN dashboard (Windows / Tkinter).
Every value shown comes from the real monitor via ecu_link.py. Nothing is simulated.
"""
import queue
import re
import threading
import time
import tkinter as tk
from datetime import datetime
from tkinter import messagebox, simpledialog, ttk

from ecu_link import DEPENDS_ON, SERVICES, EcuLink

try:                                    # crisp text on high-DPI Windows screens
    import ctypes
    ctypes.windll.shcore.SetProcessDpiAwareness(1)
except Exception:
    pass

HOST, USER, BIN_DIR = "10.0.0.1", "qnxuser", "/data/home/qnxuser/ecu"
LED_MODE = "SIMULATED"      # set to "PHYSICAL" only after the monitor really drives /dev/gpio

BG, PANEL, CARD, EDGE = "#0b1220", "#121a2b", "#16213a", "#1f2a44"
CYAN, GREEN, AMBER, RED = "#22d3ee", "#34d399", "#fbbf24", "#f87171"
GREY, TEXT, DIM = "#64748b", "#e2e8f0", "#94a3b8"
TINT = {GREEN: "#0f3d2e", AMBER: "#4a3a0c", RED: "#4a1a1a", GREY: "#1e293b"}

EVENT_COLORS = {
    "STARTED": CYAN, "INJECT": CYAN, "SCENARIO": CYAN, "RESET": CYAN,
    "FAULT_DETECTED": RED, "FAULT_UPDATE": RED, "ROOT_CAUSE": RED, "ESCALATE": RED,
    "SAFE_STATE": RED, "DEPENDENCY_LOST": RED, "SPAWN_FAILED": RED, "LINK_CLOSED": RED,
    "SYMPTOM": AMBER, "RESTARTING": AMBER, "KILL_RETRY": AMBER, "WARNING": AMBER,
    "SYMPTOM_CLEARED": GREEN, "READY": GREEN, "RECOVERED": GREEN, "STABLE": GREEN,
    "PROGRESS_RESUMED": GREEN, "SPAWNED": TEXT, "SHUTDOWN": AMBER,
}
NOISE = re.compile(r"^(SERVICE\s+TIER|(wheel_speed|abs|traction_control|dashboard)\s+\d+\s+[A-Z_]+\s+-?\d+\s)")
UNKNOWN_SVC = {"state": "UNKNOWN", "pid": None, "tier": None, "last_progress_ms": None,
               "restarts": 0, "role": None, "upstream": None, "last_recovery_ms": None,
               "budget_ms": None, "budget_result": None}


def effective(s, live):
    """(colour, headline, detail) for one service, from real data only."""
    if not live or s["state"] == "UNKNOWN":
        return GREY, "UNKNOWN", "no live data"
    st, role = s["state"], s["role"]
    if st == "HEALTHY":
        if role == "affected":
            return AMBER, "AFFECTED", "upstream: %s" % s["upstream"]
        return GREEN, "HEALTHY", ""
    detail = "ROOT CAUSE" if role == "root" else ("symptom of %s" % s["upstream"] if role == "affected" else "")
    if st in ("FAULTED", "DEGRADED"):
        return RED, st, detail
    return AMBER, st.replace("_", " "), detail


class App:
    def __init__(self, root):
        self.root = root
        self.link = None
        self.busy = False
        self.t0 = None
        root.title("ECU GUARDIAN - Automotive ECU Watchdog & Recovery")
        root.configure(bg=BG)
        root.geometry("1380x840")
        root.minsize(1100, 700)
        self._style()
        self._build()
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        root.after(250, self.tick)

    # ------------------------------------------------------------ layout
    def _style(self):
        st = ttk.Style()
        st.theme_use("clam")
        st.configure("Treeview", background=PANEL, fieldbackground=PANEL, foreground=TEXT,
                     rowheight=22, borderwidth=0, font=("Consolas", 9))
        st.configure("Treeview.Heading", background=EDGE, foreground=CYAN,
                     font=("Segoe UI", 9, "bold"))
        st.map("Treeview", background=[("selected", EDGE)])

    def _panel(self, parent, title):
        f = tk.Frame(parent, bg=PANEL, highlightbackground=EDGE, highlightthickness=1)
        tk.Label(f, text=title, bg=PANEL, fg=CYAN,
                 font=("Segoe UI", 10, "bold")).pack(anchor="w", padx=10, pady=(8, 2))
        return f

    def _btn(self, parent, text, cmd, color=CYAN):
        return tk.Button(parent, text=text, command=cmd, bg=EDGE, fg=color, relief="flat",
                         activebackground="#2b3a5c", activeforeground=color,
                         disabledforeground=GREY, font=("Segoe UI", 9, "bold"),
                         padx=10, pady=5, cursor="hand2")

    def _build(self):
        r = self.root
        for c, w in enumerate((3, 4, 3)):
            r.grid_columnconfigure(c, weight=w, uniform="col")
        r.grid_rowconfigure(1, weight=4)
        r.grid_rowconfigure(2, weight=3)
        r.grid_rowconfigure(3, weight=2)

        # ---- Panel A: header
        hd = tk.Frame(r, bg=BG)
        hd.grid(row=0, column=0, columnspan=3, sticky="ew", padx=12, pady=(10, 4))
        hd.grid_columnconfigure(1, weight=1)
        tk.Label(hd, text="ECU GUARDIAN", bg=BG, fg=CYAN,
                 font=("Segoe UI", 22, "bold")).grid(row=0, column=0, sticky="w")
        tk.Label(hd, text="Automotive ECU Watchdog & Recovery", bg=BG, fg=TEXT,
                 font=("Segoe UI", 11)).grid(row=1, column=0, sticky="w")
        tk.Label(hd, text="Target: QNX OS 8.0 on Raspberry Pi 4  |  %s" % HOST, bg=BG, fg=DIM,
                 font=("Segoe UI", 10)).grid(row=0, column=1, sticky="w", padx=24)
        self.lbl_mode = tk.Label(hd, text="", bg=BG, fg=DIM, font=("Segoe UI", 9))
        self.lbl_mode.grid(row=1, column=1, sticky="w", padx=24)
        self.lbl_link = tk.Label(hd, text="DISCONNECTED", bg=BG, fg=RED,
                                 font=("Segoe UI", 12, "bold"))
        self.lbl_link.grid(row=0, column=2, rowspan=2, padx=12)
        bf = tk.Frame(hd, bg=BG)
        bf.grid(row=0, column=3, rowspan=2)
        self.btn_connect = self._btn(bf, "Connect & start monitor", self.on_connect)
        self.btn_connect.pack(side="left", padx=4)
        self.btn_stop = self._btn(bf, "Stop monitor", self.on_stop, RED)
        self.btn_stop.pack(side="left", padx=4)

        # ---- Panel B: service cards
        pb = self._panel(r, "ECU SERVICES")
        pb.grid(row=1, column=0, sticky="nsew", padx=(12, 6), pady=6)
        self.cards = {}
        for n in SERVICES:
            card = tk.Frame(pb, bg=CARD)
            card.pack(fill="x", padx=10, pady=4)
            stripe = tk.Frame(card, width=7, bg=GREY)
            stripe.pack(side="left", fill="y")
            body = tk.Frame(card, bg=CARD)
            body.pack(side="left", fill="both", expand=True, padx=8, pady=6)
            body.grid_columnconfigure(1, weight=1)
            tk.Label(body, text=n, bg=CARD, fg=TEXT,
                     font=("Consolas", 12, "bold")).grid(row=0, column=0, sticky="w")
            state = tk.Label(body, text="UNKNOWN", bg=CARD, fg=GREY, font=("Segoe UI", 12, "bold"))
            state.grid(row=0, column=1, sticky="e")
            info = tk.Label(body, text="", bg=CARD, fg=DIM, font=("Consolas", 9), anchor="w")
            info.grid(row=1, column=0, columnspan=2, sticky="w")
            rec = tk.Label(body, text="", bg=CARD, fg=DIM, font=("Consolas", 9), anchor="w")
            rec.grid(row=2, column=0, columnspan=2, sticky="w")
            self.cards[n] = (stripe, state, info, rec)

        # ---- Panel C: dependency graph
        pc = self._panel(r, "ECU DEPENDENCY GRAPH  (arrow = supplies data to)")
        pc.grid(row=1, column=1, sticky="nsew", padx=6, pady=6)
        self.cv = tk.Canvas(pc, bg=PANEL, highlightthickness=0, height=320)
        self.cv.pack(fill="both", expand=True, padx=6, pady=6)

        # ---- Panels F + G: injection and LEDs
        right = tk.Frame(r, bg=BG)
        right.grid(row=1, column=2, sticky="nsew", padx=(6, 12), pady=6)
        right.grid_rowconfigure(0, weight=3)
        right.grid_rowconfigure(1, weight=2)
        right.grid_columnconfigure(0, weight=1)

        pf = self._panel(right, "FAULT INJECTION  (real monitor commands)")
        pf.grid(row=0, column=0, sticky="nsew", pady=(0, 6))
        row = tk.Frame(pf, bg=PANEL)
        row.pack(fill="x", padx=10, pady=4)
        tk.Label(row, text="Service", bg=PANEL, fg=TEXT).pack(side="left")
        self.var_svc = tk.StringVar(value=SERVICES[1])
        ttk.Combobox(row, textvariable=self.var_svc, values=SERVICES, state="readonly",
                     width=16).pack(side="left", padx=6)
        tk.Label(row, text="Overrun ms", bg=PANEL, fg=TEXT).pack(side="left", padx=(8, 2))
        self.ent_ms = tk.Entry(row, width=7, bg=BG, fg=TEXT, insertbackground=TEXT, relief="flat")
        self.ent_ms.insert(0, "6000")
        self.ent_ms.pack(side="left")
        self.var_persist = tk.BooleanVar(value=False)
        tk.Checkbutton(pf, text="persistent (re-fires after restart, leads to escalation)",
                       variable=self.var_persist, bg=PANEL, fg=TEXT, selectcolor=BG,
                       activebackground=PANEL, activeforeground=TEXT).pack(anchor="w", padx=10)
        br = tk.Frame(pf, bg=PANEL)
        br.pack(fill="x", padx=10, pady=6)
        self.fault_buttons = []
        for f in ("crash", "stuck", "overrun", "deadlock"):
            b = self._btn(br, f.upper(), lambda x=f: self.on_inject(x), AMBER)
            b.pack(side="left", padx=3)
            self.fault_buttons.append(b)
        self.btn_reset = self._btn(br, "RESET degraded", self.on_reset, CYAN)
        self.btn_reset.pack(side="left", padx=3)
        self.lbl_inject = tk.Label(pf, text="No injection sent.", bg=PANEL, fg=DIM,
                                   wraplength=330, justify="left", font=("Segoe UI", 9))
        self.lbl_inject.pack(anchor="w", padx=10, pady=2)
        tk.Label(pf, text="Faults activate when the service's loop next runs (up to ~3 s). "
                 "The monitor classifies overrun and deadlock as STUCK (no progress).",
                 bg=PANEL, fg=GREY, wraplength=330, justify="left",
                 font=("Segoe UI", 8)).pack(anchor="w", padx=10, pady=(0, 6))

        pg = self._panel(right, "GPIO LED STATUS")
        pg.grid(row=1, column=0, sticky="nsew")
        lr = tk.Frame(pg, bg=PANEL)
        lr.pack(pady=4)
        self.leds = {}
        for name, col, cap in (("green", GREEN, "ALL HEALTHY"), ("amber", AMBER, "RECOVERING"),
                               ("red", RED, "FAULT")):
            f = tk.Frame(lr, bg=PANEL)
            f.pack(side="left", padx=14)
            c = tk.Canvas(f, width=46, height=46, bg=PANEL, highlightthickness=0)
            c.pack()
            item = c.create_oval(4, 4, 42, 42, fill=TINT[col], outline=col, width=2)
            tk.Label(f, text=cap, bg=PANEL, fg=DIM, font=("Segoe UI", 8)).pack()
            self.leds[name] = (c, item, col)
        txt = ("SIMULATED on screen - the monitor is not driving GPIO yet"
               if LED_MODE == "SIMULATED" else "PHYSICAL - monitor drives /dev/gpio")
        tk.Label(pg, text=txt, bg=PANEL, fg=AMBER if LED_MODE == "SIMULATED" else GREEN,
                 font=("Segoe UI", 8, "bold")).pack(pady=(0, 6))

        # ---- Panel D: timeline
        pd = self._panel(r, "FAILURE & RECOVERY TIMELINE  (events from the monitor)")
        pd.grid(row=2, column=0, columnspan=2, sticky="nsew", padx=(12, 6), pady=6)
        tf = tk.Frame(pd, bg=PANEL)
        tf.pack(fill="both", expand=True, padx=6, pady=(0, 6))
        self.tree = ttk.Treeview(tf, columns=("t", "event", "detail"), show="headings", height=8)
        for c, w in (("t", 90), ("event", 140), ("detail", 700)):
            self.tree.heading(c, text={"t": "TIME", "event": "EVENT", "detail": "DETAIL"}[c])
            self.tree.column(c, width=w, anchor="w", stretch=(c == "detail"))
        for ev, col in EVENT_COLORS.items():
            self.tree.tag_configure(ev, foreground=col)
        sb = ttk.Scrollbar(tf, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

        # ---- Panel E: metrics
        pe = self._panel(r, "RECOVERY METRICS  (from monitor events)")
        pe.grid(row=2, column=2, sticky="nsew", padx=(6, 12), pady=6)
        self.metric = {}
        mg = tk.Frame(pe, bg=PANEL)
        mg.pack(fill="x", padx=10)
        mg.grid_columnconfigure(1, weight=1)
        for i, k in enumerate(("Injections accepted", "Recoveries", "Within budget (MET)",
                               "Budget missed", "Degraded incidents", "Recovery min / avg / max",
                               "Restarts (all services)", "Unnecessary restarts")):
            tk.Label(mg, text=k, bg=PANEL, fg=DIM, font=("Segoe UI", 9)).grid(row=i, column=0, sticky="w")
            v = tk.Label(mg, text="-", bg=PANEL, fg=TEXT, font=("Consolas", 10, "bold"))
            v.grid(row=i, column=1, sticky="e")
            self.metric[k] = v
        self.btn_metrics = self._btn(pe, "Show monitor metrics in console", self.on_metrics, CYAN)
        self.btn_metrics.pack(pady=6)

        # ---- raw console (CLI fallback and honesty check)
        pr = self._panel(r, "MONITOR CONSOLE (raw output)")
        pr.grid(row=3, column=0, columnspan=3, sticky="nsew", padx=12, pady=(6, 12))
        self.txt = tk.Text(pr, height=6, bg=BG, fg=DIM, font=("Consolas", 9), relief="flat",
                           state="disabled", wrap="none")
        self.txt.pack(fill="both", expand=True, padx=6, pady=(0, 6))

    # ------------------------------------------------------------ connection
    def on_connect(self):
        if self.busy:
            return
        pw = simpledialog.askstring("Connect", "Password for %s@%s:" % (USER, HOST),
                                    show="*", parent=self.root)
        if not pw:
            return
        self.busy = True
        self.lbl_mode.config(text="Connecting...")
        threading.Thread(target=self._connect_worker, args=(pw,), daemon=True).start()

    def _connect_worker(self, pw):
        link = EcuLink(HOST, USER, BIN_DIR)
        try:
            link.connect(pw)
            if link.monitor_pids():
                self.root.after(0, self._ask_kill, link)
                return
            self._launch(link)
        except Exception as e:
            self.root.after(0, self._fail, "Connect failed: %s" % e, link)

    def _launch(self, link):
        link.start_monitor()
        link.start_polling()
        self.root.after(0, self._attach, link)

    def _ask_kill(self, link):
        if messagebox.askyesno("Monitor already running",
                               "A monitor is already running on the Pi (started elsewhere).\n"
                               "Stop it (this also stops its services) and start a fresh one "
                               "from this GUI?", parent=self.root):
            threading.Thread(target=self._kill_and_launch, args=(link,), daemon=True).start()
        else:
            link.close()
            self.busy = False

    def _kill_and_launch(self, link):
        try:
            link.kill_monitor()
            time.sleep(2.0)
            if link.monitor_pids():
                raise RuntimeError("old monitor did not stop")
            self._launch(link)
        except Exception as e:
            self.root.after(0, self._fail, "Could not restart monitor: %s" % e, link)

    def _attach(self, link):
        self.link = link
        self.t0 = None
        self.tree.delete(*self.tree.get_children())
        self.busy = False

    def _fail(self, msg, link):
        self.busy = False
        try:
            link.close()
        except Exception:
            pass
        messagebox.showerror("ECU GUARDIAN", msg, parent=self.root)

    def on_stop(self):
        if not (self.link and self.link.running) or self.busy:
            return
        if not messagebox.askyesno("Stop monitor", "Stop the monitor and all ECU services on the Pi?",
                                   parent=self.root):
            return
        self.busy = True
        link = self.link

        def work():
            link.stop(wait=4.0)
            self.root.after(0, lambda: setattr(self, "busy", False))
        threading.Thread(target=work, daemon=True).start()

    def on_close(self):
        try:
            if self.link and self.link.running:
                if not messagebox.askyesno("Quit", "Stop the monitor and all ECU services, then quit?",
                                           parent=self.root):
                    return
                self.link.stop(wait=3.0)
            elif self.link:
                self.link.close()
        except Exception:
            pass
        self.root.destroy()

    # ------------------------------------------------------------ commands
    def on_inject(self, fault):
        if not (self.link and self.link.snapshot()["live"]):
            return
        svc, ms = self.var_svc.get(), None
        if fault == "overrun":
            try:
                ms = int(self.ent_ms.get())
            except ValueError:
                messagebox.showwarning("Overrun", "Overrun duration must be a number (ms).")
                return
        if not messagebox.askokcancel("Confirm fault injection",
                                      "Inject '%s' into %s?\n\nThe service will be disrupted and the "
                                      "monitor will try to recover it." % (fault, svc),
                                      parent=self.root):
            return
        try:
            cmd = self.link.inject(svc, fault, self.var_persist.get(), ms)
        except Exception as e:
            self.lbl_inject.config(text="Not sent: %s" % e, fg=AMBER)
            return
        self.lbl_inject.config(text="Sent: %s - waiting for the monitor to accept it..." % cmd, fg=DIM)

    def on_reset(self):
        if self.link and self.link.running:
            self.link.reset("all")

    def on_metrics(self):
        if self.link and self.link.running:
            self.link.request("metrics")

    # ------------------------------------------------------------ updates
    def rel(self, ts):
        if not ts:
            return "-"
        try:
            dt = datetime.strptime(ts, "%Y-%m-%d %H:%M:%S.%f")
        except ValueError:
            return "-"
        if self.t0 is None:
            self.t0 = dt
        return "T+%.3f s" % (dt - self.t0).total_seconds()

    def console_add(self, line):
        if not line.strip() or NOISE.match(line):
            return
        self.txt.config(state="normal")
        self.txt.insert("end", line + "\n")
        n = int(self.txt.index("end-1c").split(".")[0])
        if n > 800:
            self.txt.delete("1.0", "%d.0" % (n - 800))
        self.txt.see("end")
        self.txt.config(state="disabled")

    def on_event(self, p):
        ev, text, ts = p["event"], p["text"], p["ts"]
        if ev == "STARTED":
            self.t0 = None
        if ev == "INJECT":
            self.lbl_inject.config(text="ACCEPTED by monitor: %s" % text, fg=GREEN)
        if ev in EVENT_COLORS:
            item = self.tree.insert("", "end", values=(self.rel(ts), ev, text), tags=(ev,))
            self.tree.see(item)
            kids = self.tree.get_children()
            if len(kids) > 500:
                self.tree.delete(kids[0])

    def tick(self):
        snap = None
        if self.link:
            for _ in range(300):
                try:
                    kind, p = self.link.events.get_nowait()
                except queue.Empty:
                    break
                if kind == "raw":
                    self.console_add(p)
                else:
                    self.on_event(p)
            snap = self.link.snapshot()
        self.refresh(snap)
        self.root.after(250, self.tick)

    def refresh(self, snap):
        running = bool(self.link and self.link.running)
        live = bool(snap and snap["live"])
        svcs = snap["services"] if snap else {n: dict(UNKNOWN_SVC) for n in SERVICES}

        if live:
            self.lbl_link.config(text="LIVE", fg=GREEN)
            self.lbl_mode.config(text="LIVE: every value comes from the real monitor on the Pi")
        elif running:
            self.lbl_link.config(text="NO DATA", fg=AMBER)
            self.lbl_mode.config(text="Waiting for the monitor...")
        else:
            self.lbl_link.config(text="DISCONNECTED", fg=RED)
            if not self.busy:
                self.lbl_mode.config(text="Not connected: status is UNKNOWN, nothing is simulated")
        self.btn_connect.config(state="normal" if (not running and not self.busy) else "disabled")
        self.btn_stop.config(state="normal" if (running and not self.busy) else "disabled")
        st = "normal" if live else "disabled"
        for b in self.fault_buttons + [self.btn_reset, self.btn_metrics]:
            b.config(state=st)

        eff = {n: effective(svcs[n], live) for n in SERVICES}
        for n in SERVICES:
            s, (col, head, detail) = svcs[n], eff[n]
            stripe, state, info, rec = self.cards[n]
            stripe.config(bg=col)
            state.config(text=head + ("  (%s)" % detail if detail and head != "UNKNOWN" else ""), fg=col)
            if live and s["state"] != "UNKNOWN":
                prog = "-" if s["last_progress_ms"] is None else "%d ms ago" % s["last_progress_ms"]
                info.config(text="PID %s   progress %s   restarts %d" %
                            (s["pid"] if s["pid"] else "-", prog, s["restarts"]))
                if s["last_recovery_ms"] is not None:
                    rec.config(text="last recovery %d ms  (budget %s ms: %s)" %
                               (s["last_recovery_ms"], s["budget_ms"], s["budget_result"]))
                else:
                    rec.config(text="last recovery: none yet")
            else:
                info.config(text="no live data")
                rec.config(text="")

        self.draw_graph(svcs, eff)
        self.update_leds(svcs, live)
        self.update_metrics(snap, svcs)

    def draw_graph(self, svcs, eff):
        cv = self.cv
        cv.delete("all")
        w, h = max(cv.winfo_width(), 320), max(cv.winfo_height(), 240)
        pos = {"wheel_speed": (0.14, 0.55), "abs": (0.50, 0.33),
               "traction_control": (0.50, 0.74), "dashboard": (0.86, 0.55)}
        P = {k: (v[0] * w, v[1] * h) for k, v in pos.items()}
        R = 28
        for consumer, suppliers in DEPENDS_ON.items():
            for sup in suppliers:
                (x1, y1), (x2, y2) = P[sup], P[consumer]
                col = eff[sup][0] if eff[sup][0] in (RED, AMBER) else "#334155"
                if sup == "wheel_speed" and consumer == "dashboard":      # drawn over the top
                    cv.create_line(x1, y1 - R, x1 + 30, h * 0.08, x2 - 30, h * 0.08, x2, y2 - R,
                                   smooth=True, arrow=tk.LAST, fill=col, width=2, dash=(5, 3))
                else:
                    dx, dy = x2 - x1, y2 - y1
                    d = (dx * dx + dy * dy) ** 0.5 or 1
                    ux, uy = dx / d, dy / d
                    cv.create_line(x1 + ux * R, y1 + uy * R, x2 - ux * R, y2 - uy * R,
                                   arrow=tk.LAST, fill=col, width=2)
        for n in SERVICES:
            x, y = P[n]
            col, head, detail = eff[n]
            cv.create_oval(x - R, y - R, x + R, y + R, fill=TINT.get(col, "#1e293b"),
                           outline=col, width=3)
            tier = svcs[n]["tier"]
            cv.create_text(x, y, text="T%s" % tier if tier else "?", fill=TEXT,
                           font=("Consolas", 11, "bold"))
            cv.create_text(x, y + R + 12, text=n, fill=TEXT, font=("Consolas", 9, "bold"))
            cv.create_text(x, y + R + 26, text=head + (" - " + detail if detail and head != "UNKNOWN" else ""),
                           fill=col, font=("Segoe UI", 8, "bold"))
        cv.create_text(8, h - 6, anchor="sw", fill=GREY, font=("Segoe UI", 8),
                       text="All links confirmed from dependency_graph[] in common.h (none illustrative). "
                            "T1 = safety-critical tier.")

    def update_leds(self, svcs, live):
        states = [s["state"] for s in svcs.values()]
        affected = any(s["role"] == "affected" for s in svcs.values())
        red = live and any(x in ("FAULTED", "DEGRADED") for x in states)
        amber = live and (any(x in ("INIT", "VERIFYING", "RESTART_PENDING") for x in states) or affected)
        green = live and all(x == "HEALTHY" for x in states) and not affected
        for key, on in (("green", green), ("amber", amber), ("red", red)):
            c, item, col = self.leds[key]
            c.itemconfig(item, fill=col if on else TINT[col])

    def update_metrics(self, snap, svcs):
        m = self.metric
        if not snap:
            for v in m.values():
                v.config(text="-")
            return
        st, rec = snap["stats"], snap["recovery_ms"]
        m["Injections accepted"].config(text=str(st["injections"]))
        m["Recoveries"].config(text=str(st["recoveries"]))
        m["Within budget (MET)"].config(text=str(st["met"]))
        m["Budget missed"].config(text=str(st["missed"]))
        m["Degraded incidents"].config(text=str(st["degraded"]))
        m["Recovery min / avg / max"].config(
            text="%d / %d / %d ms" % (min(rec), sum(rec) / len(rec), max(rec)) if rec else "n/a")
        m["Restarts (all services)"].config(text=str(sum(s["restarts"] for s in svcs.values())))
        m["Unnecessary restarts"].config(text="n/a (see per-service restarts)")


if __name__ == "__main__":
    root = tk.Tk()
    App(root)
    root.mainloop()