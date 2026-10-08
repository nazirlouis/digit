"""Contract tests for the authored cross-controller reaction catalog."""
import copy
import json
import unittest

import generate_reactions as catalog


class ReactionContractTests(unittest.TestCase):
    def setUp(self):
        self.data = json.loads(catalog.SOURCE.read_text(encoding="utf-8"))

    def test_current_catalog_is_valid(self):
        entries = catalog.validate(self.data)
        self.assertEqual(len(entries), 13)
        self.assertEqual(entries[1]["profiles"]["wake"]["voice_ms"], 780)

    def test_rejects_reordered_faces(self):
        self.data["reactions"][1], self.data["reactions"][2] = self.data["reactions"][2], self.data["reactions"][1]
        with self.assertRaisesRegex(ValueError, "ordered reactions"):
            catalog.validate(self.data)

    def test_rejects_sound_that_outlives_face(self):
        self.data["reactions"][1]["profiles"]["expression"]["notes"][0]["ms"] = 1600
        with self.assertRaisesRegex(ValueError, "settling tail"):
            catalog.validate(self.data)

    def test_rejects_silent_timing_drift(self):
        self.data["reactions"][1]["profiles"]["expression"]["hold_ms"] += 1
        with self.assertRaisesRegex(ValueError, "timelines must end together"):
            catalog.validate(self.data)

    def test_rejects_unbounded_motifs_and_unexpected_persistence(self):
        for field, value, message in (("notes", [], "note count"), ("hold_ms", None, "persistent")):
            data = copy.deepcopy(self.data)
            data["reactions"][1]["profiles"]["expression"][field] = value
            with self.assertRaisesRegex(ValueError, message):
                catalog.validate(data)

    def test_rejects_unsafe_frequencies_and_amplitudes(self):
        for field, value in (("hz", [500, 8000]), ("level", 1.2), ("warble", .9), ("ms", 0)):
            data = copy.deepcopy(self.data)
            data["reactions"][1]["profiles"]["expression"]["notes"][0][field] = value
            with self.assertRaises(ValueError):
                catalog.validate(data)

    def test_profile_gains_cannot_boost_or_mute(self):
        for gain in (0, -1, 1.01, float("nan"), "0.5"):
            data = copy.deepcopy(self.data)
            data["reactions"][1]["profiles"]["expression"]["gain"] = gain
            with self.assertRaisesRegex(ValueError, "never boost"):
                catalog.validate(data)

    def test_generation_is_deterministic_and_contains_shared_fingerprint(self):
        entries = catalog.validate(self.data)
        cpp = catalog.render_cpp(entries, "1234abcd")
        py = catalog.render_python(entries, "1234abcd")
        self.assertEqual(cpp, catalog.render_cpp(entries, "1234abcd"))
        self.assertEqual(py, catalog.render_python(entries, "1234abcd"))
        self.assertIn("version = 0x1234abcdU", cpp)
        self.assertIn("CATALOG_VERSION = 0x1234abcd", py)
        namespace = {}
        exec(py, namespace)
        for entry in entries:
            profile = entry["profiles"]["expression"]
            self.assertEqual(namespace["TIMINGS"][entry["name"]],
                             (profile["enter_ms"], profile["hold_ms"], profile["exit_ms"]))


if __name__ == "__main__":
    unittest.main()
