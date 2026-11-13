"""Server-owned terrain model process. The browser submits positions, never link qualities."""
import json
import math
from pathlib import Path
import queue
import shutil
import subprocess
import threading

HERE = Path(__file__).resolve().parent


def finite(value, low, high):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not low <= value <= high:
        raise ValueError("invalid or out-of-range placement value")
    return value


class PlacementModel:
    def __init__(self):
        node = shutil.which("node")
        if not node:
            raise ValueError("Manual terrain placement requires Node.js 18+ on the server")
        self.lock = threading.Lock()
        self.replies = queue.Queue()
        self.proc = subprocess.Popen([node, str(HERE / "terrain" / "service.cjs")], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.meta = self.ask({"op": "meta"})

    def _read(self):
        for line in self.proc.stdout:
            self.replies.put(line)
        self.replies.put(None)

    def ask(self, request):
        with self.lock:
            if self.proc.poll() is not None:
                raise ValueError("terrain model stopped; restart the server")
            try:
                self.proc.stdin.write(json.dumps(request, allow_nan=False) + "\n")
                self.proc.stdin.flush()
                line = self.replies.get(timeout=15)
                if line is None:
                    raise ValueError("terrain model stopped; check the local DEM assets")
                result = json.loads(line)
            except (OSError, queue.Empty, json.JSONDecodeError) as error:
                self.close()
                raise ValueError("terrain model failed or timed out; restart the server") from error
            if not result.get("ok"):
                raise ValueError(result.get("reason", "terrain calculation failed"))
            return result

    def validate(self, nodes, settings, ids):
        if not isinstance(nodes, list) or len(nodes) != len(ids) or not 2 <= len(nodes) <= 16:
            raise ValueError("placement must contain every simulator node exactly once")
        t = self.meta["terrain"]
        result = []
        for n in nodes:
            if not isinstance(n, dict) or type(n.get("id")) is not int:
                raise ValueError("invalid placement node")
            x = finite(n.get("x"), t["x0"], t["x0"] + (t["n"] - 1) * t["cell"])
            y = finite(n.get("y"), t["ytop"] - (t["n"] - 1) * t["cell"], t["ytop"])
            h = finite(n.get("h"), 0.5, 30)
            if math.hypot(x - t["bx"], y - t["by"]) > t["rdisk"] - 40:
                raise ValueError("place nodes inside the Malapert terrain disk")
            result.append({"id": n["id"], "x": x, "y": y, "h": h})
        if {n["id"] for n in result} != ids:
            raise ValueError("placement node IDs do not match this network")
        if not isinstance(settings, dict):
            raise ValueError("missing radio settings")
        radio = {"power": finite(settings.get("power"), 0, 22), "attenuation": finite(settings.get("attenuation"), 0, 30)}
        return sorted(result, key=lambda n: n["id"]), radio

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.reader.join(timeout=2)
        self.proc.stdin.close()
        self.proc.stdout.close()
