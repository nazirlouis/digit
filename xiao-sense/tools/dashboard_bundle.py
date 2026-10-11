"""Refresh the dashboard before PlatformIO executes its incremental build.

ESP-IDF's CMake configure step also creates this asset, but PlatformIO can reuse
its CMake model after web_server.cpp changes. Run at post-script load time, not
as a buildprog pre-action: object freshness is evaluated before that action.
The packer's content hash changes the assembly source when its incbin changes.
"""
Import("env")

import importlib.util
from pathlib import Path


def pack_current_dashboard(build_env):
    project = Path(build_env.subst("$PROJECT_DIR"))
    build = Path(build_env.subst("$BUILD_DIR"))
    # SCons intentionally omits __file__ from an extra script's namespace.
    packer_path = project / "tools" / "pack_dashboard.py"
    spec = importlib.util.spec_from_file_location("digit_dashboard_packer", packer_path)
    packer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(packer)
    original, compressed = packer.pack_dashboard(
        project / "src" / "web_server.cpp", build / "esp-idf" / "src")
    print(f"Dashboard: {original} HTML bytes -> {compressed} gzip bytes "
          f"({original - compressed} bytes saved)")


# Script evaluation precedes SCons dependency checks and object execution.
pack_current_dashboard(env)
