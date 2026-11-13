"""Serves the node interface on localhost and, with --sim, bridges it to the mesh-network simulator console."""
import argparse
import itertools
import json
import math
import queue
import random
import subprocess
import sys
import threading
import time
from collections import deque
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from placement import PlacementModel, finite

HERE = Path(__file__).resolve().parent
DEFAULT_EXE = HERE.parent / "build" / ("lmesh_console.exe" if sys.platform == "win32" else "lmesh_console")
TICK_S = 0.05
TOPO_EVERY_S = 1.0
STATUS_EVERY_S = 0.5
SPEED_MIN, SPEED_MAX = 0.05, 200.0
STEP_MAX_S = 3600
TOPOS = ("line", "ring", "diamond", "random", "terrain")
MAX_NODES = 16


class Waiter:
    """A reply the server is waiting for, or None to swallow one reply line."""

    def __init__(self):
        self.q = queue.Queue(maxsize=1)

    def get(self, timeout=2.0):
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return None


class Sim:
    """The simulator process, its virtual clock, and the controls the page can change while it runs.

    Every line written to the console that gets a reply is recorded in `owners`, in order. The page's replies
    (one "ok" line, or rows and an "end" line) go to the page; the replies to the server's own control lines
    (kill/revive/join/cut for outages and loss, topo for bookkeeping) are swallowed, so the page's
    one-command-in-flight queue never sees a reply it did not ask for.
    """

    def __init__(self, exe, topo, nodes, seed, loss, speed, terrain=None):
        self.exe = exe
        self.terrain = Path(terrain).resolve() if terrain else None
        self.lock = threading.Lock()  # stdin and owners
        self.ctl = threading.RLock()  # clock, outages, schedule
        self.clients = set()
        self.clients_lock = threading.Lock()
        self.ids = itertools.count(1)
        self.generation = 0
        self.proc = None
        self.restarting = False
        self.placement_model = None
        self.placement = None
        self.placement_revision = 0
        self.closed = False
        self.speed = min(max(speed, SPEED_MIN), SPEED_MAX)
        self.paused = False
        self.step_ms = 0
        self.chaos = {"node_pct": 0.0, "link_pct": 0.0, "mean_s": 60.0, "spare_attached": True}
        self.rng = random.Random(seed)
        self._start("terrain" if self.terrain else topo, nodes, seed, loss)
        threading.Thread(target=self._tick, daemon=True).start()

    # Process

    def _start(self, topo, nodes, seed, loss):
        self.placement = None
        self.placement_revision += 1
        self.args = {"topo": topo, "nodes": nodes, "seed": seed, "loss": loss, "terrain_available": bool(self.terrain)}
        self.owners = deque()
        source = str(self.terrain) if topo == "terrain" else str(nodes)
        self.proc = subprocess.Popen([str(self.exe), topo, source, str(seed), str(loss)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True, bufsize=1)
        self.sim_ms = 0
        self.loss = loss
        self.topo = None
        self.all_ids = set()
        self.reasons = {}  # node -> set of reasons the server wants it down
        self.we_killed = set()  # nodes the server killed and must revive
        self.chaos_nodes = {}  # node -> sim ms its random outage ends
        self.chaos_links = {}  # (a, b) -> [sim ms the outage ends, loss to restore]
        self.schedule = []
        self.last_topo = 0.0
        self.generation += 1
        self.reader_thread = threading.Thread(target=self._read, args=(self.proc,), daemon=True)
        self.reader_thread.start()
        self._refresh_topo()
        if topo == "terrain" and (not self.topo or not self.topo.get("terrain")):
            self.proc.terminate()
            raise ValueError("terrain console did not start; check the table and rebuild with cmake --build build")
        if self.topo and self.topo.get("nodes"):
            self.args["nodes"] = len(self.topo["nodes"])

    def restart(self, topo, nodes, seed, loss):
        with self.ctl:
            self.restarting = True
            old = self.proc
            with self.lock:
                for _, _, w in self.owners:
                    if w:
                        w.q.put_nowait(None)
                self.owners.clear()
            old.terminate()
            try:
                old.wait(timeout=2)
            except subprocess.TimeoutExpired:
                old.kill()
                old.wait()
            self.reader_thread.join(timeout=2)
            old.stdin.close()
            old.stdout.close()
            self.step_ms = 0
            self.rng = random.Random(seed)
            self._start(topo, nodes, seed, loss)
            self.restarting = False
        self.broadcast_status()

    def write(self, line, waiter=None, page=False):
        """Send one line; page lines' replies go to the page, server lines' replies to `waiter` (or nowhere)."""
        line = line.strip()
        if not line:
            return
        with self.lock:
            if line.split()[0] != "@t":  # @t is the only line with no reply
                self.owners.append(("page" if page else "server", line, waiter))
            self.proc.stdin.write(line + "\n")
            self.proc.stdin.flush()

    def ask(self, line):
        w = Waiter()
        self.write(line, w)
        return w.get()

    def _read(self, proc):
        for line in proc.stdout:
            if proc is not self.proc:
                return
            line = line.rstrip("\r\n")
            try:
                o = json.loads(line)
            except ValueError:
                o = None
            if not (isinstance(o, dict) and "ev" in o):
                with self.lock:
                    head = self.owners[0] if self.owners else None
                    done = isinstance(o, dict) and ("ok" in o or "end" in o)
                    if head and head[0] == "server":
                        self.owners.popleft()
                        if head[2]:
                            head[2].q.put_nowait(o)
                        continue
                    if head and done:
                        self.owners.popleft()
                if head and head[1] == "@topo" and isinstance(o, dict) and "alive" in o:
                    self.topo = o
            self._send_clients("line", line)
        if proc is self.proc and not self.restarting:
            print("simulator exited", file=sys.stderr)

    # Clients

    def subscribe(self):
        q = queue.Queue(maxsize=5000)
        with self.clients_lock:
            self.clients.add(q)
        return q

    def unsubscribe(self, q):
        with self.clients_lock:
            self.clients.discard(q)

    def _send_clients(self, kind, data):
        with self.clients_lock:
            for q in self.clients:
                try:
                    q.put_nowait((kind, data))
                except queue.Full:
                    pass

    def status(self):
        with self.ctl:
            down = {str(n): sorted(r) for n, r in self.reasons.items() if r and n in self.we_killed}
            return {
                "sim": True, "generation": self.generation, "args": self.args,
                "speed": self.speed, "paused": self.paused, "sim_ms": self.sim_ms, "loss": self.loss,
                "chaos": self.chaos, "down": down,
                "placement": self.placement, "placement_revision": self.placement_revision,
                "links_down": [[a, b] for (a, b) in self.chaos_links],
                "schedule": [{k: e[k] for k in ("id", "node", "start_ms", "dur_ms", "every_ms")} |
                             {"down": e["down"], "next_ms": self._next_transition(e, self.sim_ms)}
                             for e in self.schedule],
            }

    def broadcast_status(self):
        self._send_clients("sim", json.dumps(self.status()))

    # Topology bookkeeping

    def _refresh_topo(self):
        t = self.ask("@topo")
        if isinstance(t, dict) and "alive" in t:
            self.topo = t
            self.all_ids |= set(t.get("nodes", t["alive"]))
            self.last_topo = time.monotonic()
            alive = set(t["alive"])
            for n in list(self.we_killed):
                if n in alive:  # revived by hand: stop managing it
                    self.we_killed.discard(n)
                    self.chaos_nodes.pop(n, None)

    def _node_ids(self):
        return self.all_ids

    def _alive(self, n):
        return self.topo is None or n in self.topo["alive"]

    def _want_down(self, n, reason):
        self.reasons.setdefault(n, set()).add(reason)
        if n not in self.we_killed and self._alive(n):
            self.write(f"@kill {n}")
            self.we_killed.add(n)
            if self.topo:
                self.topo["alive"] = [x for x in self.topo["alive"] if x != n]

    def _release(self, n, reason):
        r = self.reasons.get(n, set())
        r.discard(reason)
        if not r and n in self.we_killed:
            self.write(f"@revive {n}")
            self.we_killed.discard(n)
            if self.topo and n not in self.topo["alive"]:
                self.topo["alive"].append(n)

    # Clock

    def _tick(self):
        last = time.monotonic()
        carry = 0.0
        last_status = 0.0
        while not self.closed:
            time.sleep(TICK_S)
            now = time.monotonic()
            with self.ctl:
                if self.proc.poll() is not None:
                    last = now
                    continue
                if not self.paused:
                    carry += (now - last) * 1000.0 * self.speed
                last = now
                ms = int(carry) + self.step_ms
                carry -= int(carry)
                stepped = self.step_ms > 0
                self.step_ms = 0
                try:
                    if ms > 0:
                        self._advance(ms)
                    if (self.schedule or self._chaos_on() or self.we_killed) and now - self.last_topo > TOPO_EVERY_S:
                        self._refresh_topo()
                except OSError:
                    continue
            if stepped or now - last_status > STATUS_EVERY_S:
                last_status = now
                self.broadcast_status()

    def _advance(self, ms):
        """Advance the virtual clock by ms, stopping at each scheduled transition and outage end to act on it."""
        while ms > 0:
            bound = [self.sim_ms + ms]
            bound += [t for t in (self._next_transition(e, self.sim_ms) for e in self.schedule) if t is not None]
            bound += list(self.chaos_nodes.values()) + [v[0] for v in self.chaos_links.values()]
            chunk = max(1, min(bound) - self.sim_ms)
            chunk = min(chunk, ms)
            self.write(f"@t {chunk}")
            self.sim_ms += chunk
            ms -= chunk
            self._apply_schedule()
            self._apply_chaos(chunk)

    # Scheduled downtime

    @staticmethod
    def _sched_down(e, t):
        if t < e["start_ms"]:
            return False
        if e["every_ms"] <= 0:
            return t < e["start_ms"] + e["dur_ms"]
        return (t - e["start_ms"]) % e["every_ms"] < e["dur_ms"]

    @staticmethod
    def _next_transition(e, t):
        s, d, p = e["start_ms"], e["dur_ms"], e["every_ms"]
        if t < s:
            return s
        if p <= 0:
            return s + d if t < s + d else None
        k = (t - s) // p
        base = s + k * p
        return base + d if t < base + d else base + p

    def _apply_schedule(self):
        for e in self.schedule:
            want = self._sched_down(e, self.sim_ms)
            if want != e["down"]:
                e["down"] = want
                (self._want_down if want else self._release)(e["node"], f"schedule {e['id']}")

    def add_schedule(self, node, start_in_s, dur_s, every_s):
        with self.ctl:
            dur_ms = max(1000, int(dur_s * 1000))
            every_ms = max(0, int(every_s * 1000))
            if every_ms and every_ms <= dur_ms:
                raise ValueError("repeat interval must be longer than the outage")
            e = {"id": next(self.ids), "node": node, "start_ms": self.sim_ms + max(0, int(start_in_s * 1000)),
                 "dur_ms": dur_ms, "every_ms": every_ms, "down": False}
            self.schedule.append(e)
            self._apply_schedule()

    def remove_schedule(self, sid):
        with self.ctl:
            for e in [e for e in self.schedule if e["id"] == sid]:
                self.schedule.remove(e)
                self._release(e["node"], f"schedule {e['id']}")

    # Random outages

    def _chaos_on(self):
        return self.chaos["node_pct"] > 0 or self.chaos["link_pct"] > 0 or self.chaos_nodes or self.chaos_links

    def _fail_prob(self, pct, chunk_ms):
        """Chance of an outage starting in chunk_ms, so the long-run fraction of time down is pct with the mean outage."""
        if pct <= 0:
            return 0.0
        d = min(pct, 95.0) / 100.0
        mean_up_ms = self.chaos["mean_s"] * 1000.0 * (1 - d) / d
        return 1 - math.exp(-chunk_ms / mean_up_ms)

    def _outage_ms(self):
        return self.sim_ms + max(1000, int(self.rng.expovariate(1.0 / (self.chaos["mean_s"] * 1000.0))))

    def _apply_chaos(self, chunk_ms):
        for n, until in list(self.chaos_nodes.items()):
            if until <= self.sim_ms:
                del self.chaos_nodes[n]
                self._release(n, "random")
        for key, (until, loss) in list(self.chaos_links.items()):
            if until <= self.sim_ms:
                del self.chaos_links[key]
                self.write(f"@join {key[0]} {key[1]} {loss}")
        if self.topo is None:
            return
        p = self._fail_prob(self.chaos["node_pct"], chunk_ms)
        if p:
            spare = self.topo.get("attached") if self.chaos["spare_attached"] else None
            for n in list(self.topo["alive"]):
                if n != spare and n not in self.chaos_nodes and self.rng.random() < p:
                    self.chaos_nodes[n] = self._outage_ms()
                    self._want_down(n, "random")
        p = self._fail_prob(self.chaos["link_pct"], chunk_ms)
        if p:
            for a, b, loss in list(self.topo["links"]):
                if (a, b) not in self.chaos_links and self.rng.random() < p:
                    self.chaos_links[(a, b)] = [self._outage_ms(), loss]
                    self.write(f"@cut {a} {b}")
                    self.topo["links"] = [x for x in self.topo["links"] if (x[0], x[1]) != (a, b)]

    def set_chaos(self, **kw):
        with self.ctl:
            c = self.chaos
            if "node_pct" in kw:
                c["node_pct"] = min(max(float(kw["node_pct"]), 0.0), 95.0)
            if "link_pct" in kw:
                c["link_pct"] = min(max(float(kw["link_pct"]), 0.0), 95.0)
            if "mean_s" in kw:
                c["mean_s"] = min(max(float(kw["mean_s"]), 1.0), 86400.0)
            if "spare_attached" in kw:
                c["spare_attached"] = bool(kw["spare_attached"])
            if kw.get("heal"):
                for n in list(self.chaos_nodes):
                    self.chaos_nodes[n] = self.sim_ms
                for v in self.chaos_links.values():
                    v[0] = self.sim_ms
                self._apply_chaos(0)

    # Loss, speed, stepping

    def set_loss(self, pct):
        with self.ctl:
            self.loss = min(max(int(pct), 0), 100)
            if self.args["topo"] == "terrain" or self.placement:
                self.write(f"@loss {self.loss}")
                self._refresh_topo()
                return
            self._refresh_topo()
            for a, b, _ in (self.topo or {}).get("links", []):
                self.write(f"@join {a} {b} {self.loss}")
            for v in self.chaos_links.values():  # links in a random outage come back at the new loss
                v[1] = self.loss
            self._refresh_topo()

    def set_speed(self, x):
        with self.ctl:
            self.speed = min(max(float(x), SPEED_MIN), SPEED_MAX)

    def set_paused(self, p):
        with self.ctl:
            self.paused = bool(p)

    def step(self, seconds):
        with self.ctl:
            self.step_ms += int(min(max(float(seconds), 0.001), STEP_MAX_S) * 1000)

    def control(self, req):
        if not isinstance(req, dict):
            raise ValueError("control request must be an object")
        op = req.get("op")
        if op == "speed":
            self.set_speed(req["value"])
        elif op == "pause":
            self.set_paused(req["value"])
        elif op == "step":
            self.step(req["seconds"])
        elif op == "loss":
            self.set_loss(req["value"])
        elif op == "placement":
            self.set_placement(req)
        elif op == "chaos":
            self.set_chaos(**{k: v for k, v in req.items() if k != "op"})
        elif op == "sched_add":
            node = int(req["node"])
            if node not in self._node_ids():
                raise ValueError("no such node")
            self.add_schedule(node, float(req.get("start_in_s", 0)), float(req["dur_s"]), float(req.get("every_s", 0)))
        elif op == "sched_remove":
            self.remove_schedule(int(req["id"]))
        elif op == "restart":
            topo = req.get("topo", self.args["topo"])
            nodes = int(req.get("nodes", self.args["nodes"]))
            seed = int(req.get("seed", self.args["seed"]))
            loss = int(req.get("loss", self.loss))
            if topo not in TOPOS or (topo == "terrain" and not self.terrain) or not 2 <= nodes <= MAX_NODES or not 0 <= loss <= 100:
                raise ValueError("bad restart arguments")
            self.restart(topo, nodes, seed, loss)
            return
        else:
            raise ValueError(f"unknown op {op!r}")
        with self.ctl:
            self._refresh_topo()
        self.broadcast_status()

    def model(self):
        with self.ctl:
            if self.placement_model is None:
                self.placement_model = PlacementModel()
            return self.placement_model

    def placement_draft(self):
        with self.ctl:
            model = self.model()
            if self.placement:
                draft = self.placement
            else:
                self._refresh_topo()
                t = model.meta["terrain"]
                positions = {r[0]: r for r in (self.topo or {}).get("positions", [])}
                nodes = []
                for i, nid in enumerate(sorted(self.all_ids)):
                    if nid in positions:
                        _, x, y, h = positions[nid]
                    elif i == 0:
                        x, y, h = t["bx"], t["by"], 1.5
                    else:
                        angle = 2 * math.pi * (i - 1) / max(1, len(self.all_ids) - 1)
                        x, y, h = t["bx"] + 1600 * math.cos(angle), t["by"] + 1600 * math.sin(angle), 1.5
                    nodes.append({"id": nid, "x": x, "y": y, "h": h})
                draft = {"nodes": nodes, "settings": {"power": 22, "attenuation": 0}}
            return {"ok": True, "meta": model.meta, "generation": self.generation,
                    "revision": self.placement_revision, "active": bool(self.placement), **draft}

    def set_placement(self, req):
        with self.ctl:
            if type(req.get("generation")) is not int or type(req.get("base_revision")) is not int or req.get("generation") != self.generation or req.get("base_revision") != self.placement_revision:
                raise ValueError("placement changed or network restarted; reload the current placement")
            model = self.model()
            nodes, settings = model.validate(req.get("nodes"), req.get("settings"), self.all_ids)
            pairs = model.ask({"op": "layout", "nodes": nodes, "settings": settings})["pairs"]
            values = []
            for pair in pairs:
                a, b = pair["ab"], pair["ba"]
                values.extend([a["margin"], b["margin"], a["loss"], b["loss"], a["rssi"], b["rssi"], a["snr"], b["snr"]])
            line = "@placement " + str(len(nodes)) + " " + " ".join(format(v, ".9g") for v in values)
            reply = self.ask(line)
            if not reply or not reply.get("ok"):
                raise ValueError("console rejected placement; rebuild with cmake --build build")
            self.placement_revision += 1
            self.placement = {"nodes": nodes, "settings": settings, "pairs": pairs, "revision": self.placement_revision}
            self._refresh_topo()

    def coverage(self, req):
        if not isinstance(req, dict):
            raise ValueError("coverage request must be an object")
        # Snapshot under ctl, then compute outside it so coverage never blocks the virtual clock.
        with self.ctl:
            if not self.placement or req.get("generation") != self.generation or req.get("revision") != self.placement_revision:
                raise ValueError("coverage request is for an old placement")
            node = next((n for n in self.placement["nodes"] if n["id"] == req.get("node")), None)
            if node is None:
                raise ValueError("no such node")
            b = req.get("bounds", {})
            if not isinstance(b, dict):
                raise ValueError("coverage bounds must be an object")
            bounds = {"x": finite(b.get("x"), -100000, 100000), "y": finite(b.get("y"), 0, 250000),
                      "width": finite(b.get("width"), 100, 100000), "height": finite(b.get("height"), 100, 100000)}
            settings = self.placement["settings"]
            generation, revision = self.generation, self.placement_revision
        result = self.model().ask({"op": "coverage", "node": node, "settings": settings, "bounds": bounds})
        return {**result, "generation": generation, "revision": revision, "node": node["id"], "bounds": bounds}

    def close(self):
        self.closed = True
        self.restarting = True
        self.proc.terminate()
        self.proc.wait(timeout=3)
        self.reader_thread.join(timeout=2)
        self.proc.stdin.close()
        self.proc.stdout.close()
        if self.placement_model:
            self.placement_model.close()


def make_handler(sim):
    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=str(HERE), **kw)

        def log_message(self, fmt, *args):
            pass

        def _json(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _body(self):
            length = int(self.headers.get("Content-Length", 0))
            if not 0 <= length <= 131072:
                raise ValueError("request is too large")
            return self.rfile.read(length).decode("utf-8", "replace")

        def do_GET(self):
            if self.path in ("/sim/placement", "/sim/placement/map") and sim is not None:
                try:
                    if self.path == "/sim/placement":
                        self._json(200, sim.placement_draft())
                    else:
                        import base64
                        body = base64.b64decode(sim.model().ask({"op": "map"})["png"])
                        self.send_response(200)
                        self.send_header("Content-Type", "image/png")
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)
                except (ValueError, KeyError, TypeError) as error:
                    self._json(400, {"ok": False, "reason": str(error)})
                return
            if self.path == "/sim/info":
                if sim is None:
                    self._json(404, {"sim": False})
                else:
                    self._json(200, sim.status())
                return
            if self.path == "/sim/events" and sim is not None:
                self._events()
                return
            super().do_GET()

        def do_POST(self):
            if sim is None or self.path not in ("/sim/send", "/sim/control", "/sim/coverage"):
                self._json(404, {"ok": False})
                return
            try:
                text = self._body()
            except ValueError as error:
                self._json(400, {"ok": False, "reason": str(error)})
                return
            if self.path == "/sim/send":
                for line in text.splitlines():
                    if line.strip():
                        sim.write(line, page=True)
                self._json(200, {"ok": True})
                return
            try:
                if self.path == "/sim/coverage":
                    self._json(200, sim.coverage(json.loads(text)))
                    return
                sim.control(json.loads(text))
            except (ValueError, KeyError, TypeError) as e:
                self._json(400, {"ok": False, "reason": str(e)})
                return
            self._json(200, {"ok": True, **sim.status()})

        def _events(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            q = sim.subscribe()
            try:
                self.wfile.write(f"event: sim\ndata: {json.dumps(sim.status())}\n\n".encode())
                while True:
                    try:
                        kind, data = q.get(timeout=10)
                        prefix = "event: sim\n" if kind == "sim" else ""
                        self.wfile.write(f"{prefix}data: {data}\n\n".encode())
                    except queue.Empty:
                        self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass
            finally:
                sim.unsubscribe(q)

    return Handler


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--sim", action="store_true", help="run the simulator console and offer it to the page")
    p.add_argument("--exe", type=Path, default=DEFAULT_EXE, help="path to lmesh_console")
    p.add_argument("--topo", default="random", choices=TOPOS)
    p.add_argument("--terrain", type=Path, help="terrain link table; uses its node count and track instead of --topo/--nodes")
    p.add_argument("--nodes", type=int, default=8)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--loss", type=int, default=0, help="loss percent on every link")
    p.add_argument("--speed", type=float, default=1.0, help="simulated seconds per real second")
    a = p.parse_args()

    sim = None
    if a.sim:
        if not a.exe.exists():
            sys.exit(f"{a.exe} not found; build it from the repository root with: cmake --build build")
        if a.topo == "terrain" and not a.terrain:
            p.error("--topo terrain requires --terrain <table>")
        if a.terrain and not a.terrain.is_file():
            p.error(f"terrain table not found: {a.terrain}")
        sim = Sim(a.exe, a.topo, a.nodes, a.seed, a.loss, a.speed, a.terrain)

    server = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(sim))
    server.daemon_threads = True
    mode = f"simulator: {sim.args['topo']} of {sim.args['nodes']}, seed {a.seed}, {a.loss} % loss, x{a.speed:g}" if sim else "USB only"
    print(f"interface at http://localhost:{a.port}/  ({mode}); Ctrl+C to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if sim:
            sim.close()


if __name__ == "__main__":
    main()
