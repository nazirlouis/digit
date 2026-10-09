"""Run the standalone rover's range decisions without its hardware startup."""
import ast
from pathlib import Path
import unittest
from motion_jobs import FRONT_STOP_MM


class RoamingRangeTests(unittest.TestCase):
    def test_roaming_uses_the_same_strict_five_cm_boundary(self):
        path = Path(__file__).resolve().parents[1] / "autonomous_drive.py"
        source = ast.parse(path.read_text(encoding="utf-8"))
        names = {"front_wall_seen", "forward_speed"}
        constants = {"DRIVE_SPEED", "APPROACH_SPEED", "SLOW_APPROACH_MM"}
        body = [node for node in source.body if
                (isinstance(node, ast.FunctionDef) and node.name in names) or
                (isinstance(node, ast.Assign) and any(
                    isinstance(target, ast.Name) and target.id in constants for target in node.targets))]
        scope = {"FRONT_STOP_MM": FRONT_STOP_MM}
        exec(compile(ast.Module(body=body, type_ignores=[]), str(path), "exec"), scope)
        for mm in (50, 80, 100, 150, 900):
            self.assertFalse(scope["front_wall_seen"](mm, True))
            self.assertGreater(scope["forward_speed"](mm, True), 0)
        for mm in (1, 30, 49):
            self.assertTrue(scope["front_wall_seen"](mm, True))
            self.assertEqual(scope["forward_speed"](mm, True), 0)
