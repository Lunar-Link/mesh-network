"""Real DEM -> server -> C medium -> routing/message tests. No mocked radio predictions."""
import argparse
import copy
from contextlib import contextmanager
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "interface"))
from placement import PlacementModel
from serve import Sim

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--console", default=str(ROOT / "build" / "lmesh_console"))
args, remaining = parser.parse_known_args()

# Both relays clear the real Malapert ridge; the direct 1 -> 4 path is obstructed.
DIAMOND = json.loads((ROOT / "sim" / "terrain" / "placement-diamond.json").read_text())["nodes"]
RADIO = {"power": 22, "attenuation": 0}


@contextmanager
def network(seed=1, count=4, table=None):
    sim = Sim(Path(args.console), "ring", count, seed, 0, 0.05, table)
    sim.set_paused(True)
    try:
        yield sim
    finally:
        sim.close()


def apply(sim, nodes, radio=None):
    sim.control({"op": "placement", "generation": sim.generation, "base_revision": sim.placement_revision,
                 "nodes": nodes, "settings": radio or RADIO})


def advance(sim, ms):
    with sim.ctl:
        sim._advance(ms)
        sim.ask("@topo")  # wait until C has processed the complete clock advance


def dump(sim, command):
    q = sim.subscribe()
    try:
        with sim.ctl:
            sim.write(command, page=True)
            sim.ask("@topo")
        rows = []
        while not q.empty():
            kind, text = q.get_nowait()
            if kind == "line":
                row = json.loads(text)
                if not any(k in row for k in ("ev", "ok", "end")):
                    rows.append(row)
        return rows
    finally:
        sim.unsubscribe(q)


def route(sim, dest):
    return next((r for r in dump(sim, "routes") if r["dest"] == dest and not r["retracted"]), None)


def move_away(nodes, nid):
    return [dict(n, x=13387.5, y=130632.5) if n["id"] == nid else dict(n) for n in nodes]


class PlacementIntegration(unittest.TestCase):
    def test_moving_the_used_relay_reroutes_and_delivers_across_five_seeds(self):
        for seed in range(1, 6):
            with self.subTest(seed=seed), network(seed) as sim:
                apply(sim, DIAMOND)
                # Demonstrate this is a real alternate-path case, not just a color change.
                physical = {tuple(p[:2]) for p in sim.topo["links"]}
                self.assertNotIn((1, 4), physical)
                self.assertTrue({(1, 2), (1, 3), (2, 4), (3, 4)} <= physical)
                advance(sim, 120000)
                before = route(sim, 4)
                self.assertIsNotNone(before)
                first = before["next"]
                self.assertIn(first, (2, 3))
                submitted = sim.ask("send 4 n 1800 before-moving")
                self.assertTrue(submitted["ok"])
                advance(sim, 60000)
                self.assertEqual(next(m for m in dump(sim, "msgs") if m["id"] == submitted["id"])["state"], "delivered")
                apply(sim, move_away(DIAMOND, first))
                self.assertFalse(any(first in p[:2] for p in sim.topo["links"]))
                advance(sim, 180000)
                after = route(sim, 4)
                self.assertIsNotNone(after)
                self.assertEqual(after["next"], 5 - first)
                second = sim.ask("send 4 n 1800 after-moving")
                advance(sim, 60000)
                messages = dump(sim, "msgs")
                self.assertEqual(next(m for m in messages if m["id"] == second["id"])["state"], "delivered")
                sim.ask("@attach 4")
                # Destination counts exactly two application deliveries, including any retries.
                self.assertEqual(dump(sim, "stats")[0]["delivered"], 2)

    def test_partition_holds_message_then_move_restores_delivery_once(self):
        with network() as sim:
            apply(sim, DIAMOND)
            advance(sim, 120000)
            apply(sim, move_away(DIAMOND, 4))
            advance(sim, 180000)
            self.assertIsNone(route(sim, 4))
            sent = sim.ask("send 4 n 1800 survive-placement-outage")
            advance(sim, 60000)
            self.assertEqual(next(m for m in dump(sim, "msgs") if m["id"] == sent["id"])["state"], "queued")
            apply(sim, DIAMOND)
            advance(sim, 180000)
            self.assertEqual(next(m for m in dump(sim, "msgs") if m["id"] == sent["id"])["state"], "delivered")
            sim.ask("@attach 4")
            self.assertEqual(dump(sim, "stats")[0]["delivered"], 1)

    def test_server_rejects_invalid_and_stale_edits_without_changing_medium(self):
        with network() as sim:
            apply(sim, DIAMOND)
            before, revision = copy.deepcopy(sim.topo), sim.placement_revision
            for change in ("nan", "outside", "duplicate", "height", "missing", "generation", "revision", "quality_only"):
                req = {"op": "placement", "generation": sim.generation, "base_revision": revision,
                       "nodes": copy.deepcopy(DIAMOND), "settings": dict(RADIO)}
                if change == "nan": req["nodes"][0]["x"] = float("nan")
                elif change == "outside": req["nodes"][0]["x"] = 999999
                elif change == "duplicate": req["nodes"][1]["id"] = 1
                elif change == "height": req["nodes"][0]["h"] = 0
                elif change == "missing": req["nodes"].pop()
                elif change == "generation": req["generation"] -= 1
                elif change == "revision": req["base_revision"] -= 1
                else: req.pop("nodes"); req["pairs"] = []
                with self.subTest(change=change), self.assertRaises(ValueError): sim.control(req)
                self.assertEqual(sim.placement_revision, revision)
                self.assertEqual(sim.ask("@topo"), before)

    def test_repeated_edits_cut_heal_loss_and_kill_are_preserved(self):
        with network() as sim:
            apply(sim, DIAMOND)
            sim.ask("@cut 1 2")
            sim.ask("@kill 3")
            for i in range(10):
                moved = copy.deepcopy(DIAMOND); moved[1]["x"] += i * 10
                apply(sim, moved)
            self.assertNotIn(3, sim.topo["alive"])
            self.assertFalse(any(p[:2] == [1, 2] for p in sim.topo["links"]))
            sim.ask("@join 1 2")
            sim.ask("@revive 3")
            self.assertTrue(any(p[:2] == [1, 2] for p in sim.ask("@topo")["links"]))
            sim.set_loss(100)
            self.assertFalse(sim.topo["links"])
            apply(sim, DIAMOND)
            self.assertFalse(sim.topo["links"])
            sim.set_loss(0)
            self.assertTrue(sim.topo["links"])
            # Server schedules heal to the placement model, not the old synthetic medium.
            with sim.ctl:
                sim.chaos_links[(1, 2)] = [sim.sim_ms + 1000, 0]
                sim.write("@cut 1 2")
                apply(sim, DIAMOND)
                self.assertFalse(any(p[:2] == [1, 2] for p in sim.topo["links"]))
                sim.chaos_links[(1, 2)][0] = sim.sim_ms
                sim._apply_chaos(0)
                self.assertTrue(any(p[:2] == [1, 2] for p in sim.ask("@topo")["links"]))

    def test_static_positions_stop_the_recorded_track_and_restart_clears_placement(self):
        with network(table=ROOT / "sim" / "terrain" / "plan0.txt") as sim:
            draft = sim.placement_draft()
            apply(sim, draft["nodes"])
            before = copy.deepcopy(sim.topo["margins"])
            advance(sim, 300000)
            self.assertEqual(sim.ask("@topo")["margins"], before)
            stale = {"op": "placement", "generation": sim.generation, "base_revision": sim.placement_revision,
                     "nodes": draft["nodes"], "settings": RADIO}
            sim.restart("ring", 8, 1, 0)
            self.assertIsNone(sim.placement)
            with self.assertRaises(ValueError): sim.control(stale)

    def test_sixteen_nodes_and_atomic_c_rejection(self):
        with network(count=16) as sim:
            draft = sim.placement_draft()
            apply(sim, draft["nodes"])
            self.assertEqual(len(sim.topo["margins"]), 120)
            before = copy.deepcopy(sim.topo)
            # Reject a partial or malformed C transaction before changing any pair.
            self.assertFalse(sim.ask("@placement 16 20 20 0 0 -80 -80 8 8")["ok"])
            self.assertEqual(sim.ask("@topo"), before)
            values = []
            for p in sim.placement["pairs"]:
                a, b = p["ab"], p["ba"]
                values.extend([a["margin"], b["margin"], a["loss"], b["loss"], a["rssi"], b["rssi"], a["snr"], b["snr"]])
            values[0] += 1
            # The last field fails after all preceding pairs have been parsed.
            values[-1] = float("nan")
            self.assertFalse(sim.ask("@placement 16 " + " ".join(format(v, ".9g") for v in values))["ok"])
            self.assertEqual(sim.ask("@topo"), before)

    def test_radio_parameters_and_mast_height_recalculate_the_model(self):
        with network() as sim:
            apply(sim, DIAMOND)
            initial = copy.deepcopy(sim.placement["pairs"])
            apply(sim, DIAMOND, {"power": 20, "attenuation": 8})
            for old, new in zip(initial, sim.placement["pairs"]):
                self.assertAlmostEqual(old["ab"]["margin"] - new["ab"]["margin"], 10, places=6)
            higher = copy.deepcopy(DIAMOND); higher[0]["h"] = 10
            apply(sim, higher)
            self.assertNotEqual(initial[0]["ab"]["margin"], sim.placement["pairs"][0]["ab"]["margin"])
            apply(sim, DIAMOND, {"power": 0, "attenuation": 30})
            self.assertFalse(any(1 in p[:2] for p in sim.topo["links"]))

    def test_coverage_and_image_are_real_and_revision_bound(self):
        with network() as sim:
            apply(sim, DIAMOND)
            req = {"generation": sim.generation, "revision": sim.placement_revision, "node": 1,
                   "bounds": {"x": 1387.5, "y": 124632.5, "width": 6000, "height": 6000}}
            result = sim.coverage(req)
            self.assertEqual(len(result["values"]), 3600)
            self.assertTrue(any(v is not None and v < 0 for v in result["values"]))
            self.assertTrue(any(v is not None and v > 20 for v in result["values"]))
            self.assertTrue(sim.model().ask({"op": "map"})["png"].startswith("iVBORw0KGgo"))
            apply(sim, DIAMOND)
            with self.assertRaises(ValueError): sim.coverage(req)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
