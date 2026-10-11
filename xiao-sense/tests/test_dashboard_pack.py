"""Verify the firmware asset matches the dashboard and refreshes after edits."""

import gzip
import hashlib
import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "pack_dashboard", ROOT / "tools" / "pack_dashboard.py")
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)


class DashboardPackTests(unittest.TestCase):
    def test_firmware_asset_round_trip_and_determinism(self):
        source = ROOT / "src" / "web_server.cpp"
        html = packer.dashboard_html(source)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            original, compressed = packer.pack_dashboard(source, output)
            asset = output / "dashboard.html.gz"
            packed = asset.read_bytes()
            timestamp = asset.stat().st_mtime_ns
            asm_path = output / "dashboard_html.S"
            asm_bytes = asm_path.read_bytes()
            asm_timestamp = asm_path.stat().st_mtime_ns
            packer.pack_dashboard(source, output)
            self.assertEqual(asset.read_bytes(), packed)
            self.assertEqual(asset.stat().st_mtime_ns, timestamp)
            self.assertEqual(asm_path.read_bytes(), asm_bytes)
            self.assertEqual(asm_path.stat().st_mtime_ns, asm_timestamp)
            self.assertEqual(gzip.decompress(packed), html)
            self.assertEqual(original, len(html))
            self.assertLess(compressed, original // 2)
            self.assertEqual(packed[4:8], b"\0\0\0\0")
            assembly = (output / "dashboard_html.S").read_text()
            self.assertIn("dashboard_html_gz_start:", assembly)
            self.assertIn("dashboard_html_gz_end:", assembly)
            self.assertIn(asset.resolve().as_posix(), assembly)
            self.assertIn(hashlib.sha256(packed).hexdigest(), assembly)

    def test_edits_refresh_the_asset_and_checkout_newlines_do_not(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / "web_server.cpp"
            source.write_bytes(b'const char page[] = R"HTML(\r\n<p>one</p>\r\n)HTML";')
            packer.pack_dashboard(source, directory)
            asset = directory / "dashboard.html.gz"
            first = asset.read_bytes()
            asm_path = directory / "dashboard_html.S"
            first_assembly = asm_path.read_bytes()
            source.write_bytes(b'const char page[] = R"HTML(\n<p>one</p>\n)HTML";')
            packer.pack_dashboard(source, directory)
            self.assertEqual(asset.read_bytes(), first)
            self.assertEqual(asm_path.read_bytes(), first_assembly)
            source.write_text('const char page[] = R"HTML(<p>two</p>)HTML";')
            packer.pack_dashboard(source, directory)
            self.assertEqual(gzip.decompress(asset.read_bytes()), b"<p>two</p>")
            self.assertNotEqual(asset.read_bytes(), first)
            self.assertNotEqual(asm_path.read_bytes(), first_assembly)
            self.assertIn(hashlib.sha256(asset.read_bytes()).hexdigest(),
                          asm_path.read_text())

    def test_platformio_script_refreshes_asset_immediately_after_edit(self):
        class BuildEnvironment:
            def __init__(self, project, build):
                self.paths = {"$PROJECT_DIR": str(project), "$BUILD_DIR": str(build)}

            def subst(self, name):
                return self.paths[name]

            def AddPreAction(self, *args):
                raise AssertionError("Dashboard must pack at script load, before object freshness checks")

        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            (project / "tools").mkdir()
            shutil.copyfile(ROOT / "tools" / "pack_dashboard.py",
                            project / "tools" / "pack_dashboard.py")
            source = project / "src" / "web_server.cpp"
            source.parent.mkdir()
            build = project / ".pio" / "build" / "test_env"
            output = build / "esp-idf" / "src"
            source.write_text('const char page[] = R"HTML(<p>configured</p>)HTML";')
            packer.pack_dashboard(source, output)  # Existing CMake configuration.
            asm_path = output / "dashboard_html.S"
            configured_assembly = asm_path.read_bytes()
            source.write_text('const char page[] = R"HTML(<p>incremental edit</p>)HTML";')
            env = BuildEnvironment(project, build)
            script = (ROOT / "tools" / "dashboard_bundle.py").read_text()
            # SCons executes extra scripts without __file__ in their globals.
            exec(compile(script, "dashboard_bundle.py", "exec"),
                 {"env": env, "Import": lambda name: None})
            self.assertEqual(gzip.decompress((output / "dashboard.html.gz").read_bytes()),
                             b"<p>incremental edit</p>")
            self.assertNotEqual(asm_path.read_bytes(), configured_assembly)
            timestamp = asm_path.stat().st_mtime_ns
            exec(compile(script, "dashboard_bundle.py", "exec"),
                 {"env": env, "Import": lambda name: None})
            self.assertEqual(asm_path.stat().st_mtime_ns, timestamp)

    def test_platformio_registers_script_before_model_bundle(self):
        config = (ROOT / "platformio.ini").read_text()
        self.assertIn("post:tools/dashboard_bundle.py", config)
        self.assertLess(config.index("post:tools/dashboard_bundle.py"),
                        config.index("post:tools/model_bundle.py"))


if __name__ == "__main__":
    unittest.main()
