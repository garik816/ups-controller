#!/usr/bin/env python3
"""Package compiled firmware with a version check and SHA-256 manifest."""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("all", "nano", "esp8266"), default="all")
    parser.add_argument("--nano-processor", choices=("atmega328old", "atmega328"), default="atmega328old")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise SystemExit("VERSION must contain a semantic version such as 1.0.0")
    header = (root / "libraries/UpsControl/src/FirmwareVersion.h").read_text(encoding="utf-8")
    match = re.search(r'#define\s+UPS_FIRMWARE_VERSION\s+"([^"]+)"', header)
    if match is None or match.group(1) != version:
        raise SystemExit("VERSION and UPS_FIRMWARE_VERSION disagree")
    properties = (root / "libraries/UpsControl/library.properties").read_text(encoding="utf-8")
    library_version = re.search(r"^version=(.+)$", properties, re.MULTILINE)
    if library_version is None or library_version.group(1).strip() != version:
        raise SystemExit("VERSION and UpsControl/library.properties disagree")

    targets = {
        "nano": {
            "source": "build/arduino_nano/arduino_nano.ino.hex",
            "suffix": "arduino-nano.hex",
            "board": "Arduino Nano ATmega328P",
            "fqbn": "arduino:avr:nano:cpu=" + args.nano_processor,
            "core": "arduino:avr@1.8.8",
        },
        "esp8266": {
            "source": "build/esp8266/esp8266.ino.bin",
            "suffix": "esp8266-nodemcu.bin",
            "board": "NodeMCU v3 / ESP8266 / 4 MB flash",
            "fqbn": "esp8266:esp8266:nodemcuv2:eesz=4M2M,baud=115200",
            "core": "esp8266:esp8266@3.1.2",
        },
    }
    selected = targets if args.target == "all" else {args.target: targets[args.target]}
    for spec in selected.values():
        source = root / spec["source"]
        if not source.is_file() or source.stat().st_size == 0:
            raise SystemExit("Missing or empty firmware: " + str(source))

    destination = root / "dist"
    destination.mkdir(exist_ok=True)
    # dist is generated; remove only names produced by this packager.
    for old in destination.glob("ups-controller-*"):
        if old.is_file():
            old.unlink()
    for name in ("manifest.json", "SHA256SUMS"):
        (destination / name).unlink(missing_ok=True)

    artifacts = []
    for target, spec in selected.items():
        filename = "ups-controller-" + version + "-" + spec["suffix"]
        output = destination / filename
        shutil.copyfile(root / spec["source"], output)
        artifacts.append({
            "target": target,
            "board": spec["board"],
            "fqbn": spec["fqbn"],
            "core": spec["core"],
            "filename": filename,
            "bytes": output.stat().st_size,
            "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        })

    try:
        commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, stderr=subprocess.DEVNULL, text=True).strip()
        dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root, stderr=subprocess.DEVNULL, text=True).strip())
    except (FileNotFoundError, subprocess.CalledProcessError):
        commit, dirty = None, None
    manifest = {
        "schema": 1,
        "project": "ups-controller",
        "version": version,
        "source_commit": commit,
        "source_dirty": dirty,
        "arduino_cli": "1.6.0",
        "arduino_json": "6.21.6",
        "artifacts": artifacts,
    }
    (destination / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    checksums = []
    for path in sorted(destination.iterdir()):
        if path.is_file() and (path.name.startswith("ups-controller-") or path.name == "manifest.json"):
            checksums.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + path.name)
    (destination / "SHA256SUMS").write_text("\n".join(checksums) + "\n", encoding="ascii")
    for artifact in artifacts:
        print("dist/{filename}: {bytes} bytes, SHA-256 {sha256}".format(**artifact))


if __name__ == "__main__":
    main()
