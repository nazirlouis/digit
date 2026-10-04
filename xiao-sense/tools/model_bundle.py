"""PlatformIO: pack both vision detectors and speech with the application.

Offsets here must agree with partitions.csv. No device I/O happens on build.
"""
Import("env")
from pathlib import Path
import subprocess
import sys
import shutil

project = Path(env.subst("$PROJECT_DIR"))
build = Path(env.subst("$BUILD_DIR"))
managed = project / "managed_components"
speech = build / "digit_models"
speech_bin = speech / "srmodels.bin"
vision_bin = build / "vision_models.bin"
partitions = {}
for line in (project / "partitions.csv").read_text().splitlines():
    if not line.strip() or line.lstrip().startswith("#"):
        continue
    fields = [field.strip() for field in line.split(",")]
    partitions[fields[0]] = (int(fields[3], 0), int(fields[4], 0))
vision_offset, vision_size = partitions["pedestrian_det"]
speech_offset, speech_size = partitions["model"]

def pack(source, target, env):
    speech.mkdir(parents=True, exist_ok=True)
    sr = managed / "espressif__esp-sr" / "model"
    for model in ("mn7_en", "fst"):
        shutil.copytree(sr / "multinet_model" / model, speech / model, dirs_exist_ok=True)
    # Use the upstream packer without its directory-deleting movemodel wrapper.
    import importlib.util
    spec = importlib.util.spec_from_file_location("sr_pack", sr / "pack_model.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.pack_models(str(speech), "srmodels.bin")
    subprocess.run([
        env.subst("$PYTHONEXE"),
        str(managed / "espressif__esp-dl" / "fbs_loader" / "pack_espdl_models.py"),
        "-m", str(managed / "espressif__pedestrian_detect" / "models" / "s3" / "pedestrian_detect_pico_s8_v1.espdl"),
        str(project / "models" / "objects" / "objects_224.espdl"),
        "-o", str(vision_bin), "-f", "PDL2",
    ], check=True)
    assert speech_bin.stat().st_size <= speech_size, "Speech model exceeds flash partition"
    assert vision_bin.stat().st_size <= vision_size, "Combined vision models exceed flash partition"
    print("Digit bundle: speech=%d bytes, vision=%d bytes" % (speech_bin.stat().st_size, vision_bin.stat().st_size))

# Run after the firmware build, including an up-to-date build invoked for upload.
env.AddPostAction("buildprog", pack)
env.AddPreAction("upload", pack)
env.Append(FLASH_EXTRA_IMAGES=[(hex(vision_offset), str(vision_bin)), (hex(speech_offset), str(speech_bin))])
# A post script runs after PlatformIO has constructed UPLOADERFLAGS from its
# original extra-image list, so extend the actual esptool command as well.
if env.get("UPLOAD_PROTOCOL", "esptool") == "esptool":
    env.Append(UPLOADERFLAGS=[hex(vision_offset), '"%s"' % vision_bin, hex(speech_offset), '"%s"' % speech_bin])

def describe(source, target, env):
    print("Complete upload command (not executed):")
    print(env.subst("$UPLOADCMD", target=target, source=[str(build / "firmware.bin")]))

env.AddCustomTarget("check_bundle", ["buildprog"], describe, "Check upload bundle", "Build models and inspect upload offsets without touching hardware")
