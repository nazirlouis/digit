"""PlatformIO: bundle local vision models; wake and Stop are embedded."""
Import("env")
from pathlib import Path
import subprocess

project = Path(env.subst("$PROJECT_DIR"))
build = Path(env.subst("$BUILD_DIR"))
managed = project / "managed_components"
vision_bin = build / "vision_models.bin"
partitions = {}
for line in (project / "partitions.csv").read_text().splitlines():
    if not line.strip() or line.lstrip().startswith("#"):
        continue
    fields = [field.strip() for field in line.split(",")]
    partitions[fields[0]] = (int(fields[3], 0), int(fields[4], 0))
vision_offset, vision_size = partitions["pedestrian_det"]

def pack(source, target, env):
    subprocess.run([
        env.subst("$PYTHONEXE"),
        str(managed / "espressif__esp-dl" / "fbs_loader" / "pack_espdl_models.py"),
        "-m", str(managed / "espressif__pedestrian_detect" / "models" / "s3" / "pedestrian_detect_pico_s8_v1.espdl"),
        str(project / "models" / "objects" / "objects_224.espdl"),
        "-o", str(vision_bin), "-f", "PDL2",
    ], check=True)
    assert vision_bin.stat().st_size <= vision_size, "Combined vision models exceed flash partition"
    firmware = build / "firmware.bin"
    if firmware.exists():
        assert firmware.stat().st_size <= partitions["factory"][1], "Application exceeds flash partition"
    print("Digit bundle: vision=%d bytes; local wake and Stop embedded; ER 2 has no spoken output" % vision_bin.stat().st_size)

env.AddPostAction("buildprog", pack)
env.AddPreAction("upload", pack)
env.Append(FLASH_EXTRA_IMAGES=[(hex(vision_offset), str(vision_bin))])
# Post scripts run after PlatformIO constructs the original uploader flags.
if env.get("UPLOAD_PROTOCOL", "esptool") == "esptool":
    env.Append(UPLOADERFLAGS=[hex(vision_offset), '"%s"' % vision_bin])

def describe(source, target, env):
    print("Complete upload command (not executed):")
    print(env.subst("$UPLOADCMD", target=target, source=[str(build / "firmware.bin")]))

env.AddCustomTarget("check_bundle", ["buildprog"], describe, "Check upload bundle",
                    "Build models and inspect upload offsets without touching hardware")
