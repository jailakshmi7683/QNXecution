import getpass
import queue
import sys
import time

from ecu_link import EcuLink


def main():
    link = EcuLink()
    link.connect(getpass.getpass("qnxuser password: "),
                 trust_new_host="--trust-new-host" in sys.argv)
    pids = link.monitor_pids()
    if pids:
        print("A monitor is already running (pid %s). Stop it in the QNX shell with: slay monitor" % pids)
        link.close()
        return
    link.start_monitor()
    link.start_polling()

    # Optional, explicit only:  py test_link.py --inject abs crash
    inject = None
    if "--inject" in sys.argv:
        i = sys.argv.index("--inject")
        inject = (sys.argv[i + 1], sys.argv[i + 2])

    t0 = time.time()
    injected = False
    while time.time() - t0 < (25 if inject else 10):
        if inject and not injected and time.time() - t0 > 6:
            print(">>> sending:", link.inject(*inject))
            injected = True
        try:
            kind, p = link.events.get(timeout=0.5)
        except queue.Empty:
            continue
        if kind == "event":
            print(p["ts"], p["event"], p["text"])

    snap = link.snapshot()
    print("\nLIVE:", snap["live"], " stats:", snap["stats"])
    for n, s in snap["services"].items():
        print("%-17s %-10s pid=%s restarts=%s role=%s last_recovery=%s %s" % (
            n, s["state"], s["pid"], s["restarts"], s["role"],
            s["last_recovery_ms"], s["budget_result"]))
    link.stop()


main()