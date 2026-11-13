"""Exercise terrain metadata and medium controls through the real console binary."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--console", default=str(ROOT / "build" / "lmesh_console"))
args, remaining = parser.parse_known_args()


class TerrainConsole(unittest.TestCase):
    def run_console(self, commands, terrain=True):
        options = ["terrain", str(ROOT / "sim" / "terrain" / "plan0.txt"), "1", "0"] if terrain else ["ring", "8", "1", "0"]
        result = subprocess.run([args.console, *options], input="\n".join(commands) + "\n", capture_output=True, text=True, timeout=15, check=True)
        return [json.loads(line) for line in result.stdout.splitlines()]

    def test_synthetic_runs_have_no_invented_margins(self):
        topo = next(r for r in self.run_console(["@topo"], terrain=False) if "alive" in r)
        self.assertNotIn("margins", topo)
        self.assertEqual(topo["nodes"], list(range(1, 9)))

    def test_terrain_margins_follow_time_and_node_deployment(self):
        rows = self.run_console(["@topo", "@t 300000", "@topo"])
        first, later = [r for r in rows if "margins" in r]
        self.assertEqual(first["nodes"], [1, 2, 3, 4])
        self.assertEqual(len(first["margins"]), 6)
        self.assertTrue(first["terrain"])
        self.assertNotIn(2, first["alive"])
        self.assertIn(2, later["alive"])
        before = next(p for p in first["margins"] if p[:2] == [1, 4])
        after = next(p for p in later["margins"] if p[:2] == [1, 4])
        self.assertNotEqual(before[2], after[2])

    def test_cuts_persist_and_join_restores_terrain(self):
        rows = self.run_console(["@cut 1 4", "@t 300000", "@topo", "@join 1 4", "@topo"])
        cut, healed = [r for r in rows if "margins" in r]
        self.assertFalse(any(p[:2] == [1, 4] for p in cut["links"]))
        self.assertTrue(next(p for p in cut["margins"] if p[:2] == [1, 4])[4])
        self.assertTrue(any(p[:2] == [1, 4] for p in healed["links"]))
        self.assertFalse(next(p for p in healed["margins"] if p[:2] == [1, 4])[4])

    def test_extra_loss_preserves_margin_and_applies_after_advancing(self):
        rows = self.run_console(["@topo", "@loss 100", "@t 1000", "@topo", "@loss 0", "@topo"])
        initial, lossy, restored = [r for r in rows if "margins" in r]
        self.assertTrue(all(p[2] == 100 for p in lossy["links"]))
        self.assertTrue(any(p[2] == 0 for p in restored["links"]))
        self.assertEqual(initial["margins"][0][2], lossy["margins"][0][2])

    def test_manual_kill_survives_scheduled_deployment(self):
        rows = self.run_console(["@kill 2", "@t 300000", "@topo", "@revive 2", "@topo"])
        down, up = [r for r in rows if "margins" in r]
        self.assertNotIn(2, down["alive"])
        self.assertIn(2, up["alive"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
