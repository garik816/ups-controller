#!/usr/bin/env python3
"""Build and execute host tests; locate the real ArduinoJson through Arduino CLI."""

import argparse
import json
import os
import subprocess
from pathlib import Path


def arduino_json_source(cli, config_file):
    command = [cli]
    if config_file:
        command += ["--config-file", config_file]
    command += ["lib", "list", "--json"]
    result = subprocess.run(command, check=True, capture_output=True, text=True, encoding="utf-8")
    listing = json.loads(result.stdout)
    for installed in listing.get("installed_libraries", []):
        library = installed.get("library", {})
        if library.get("name") != "ArduinoJson":
            continue
        if library.get("version") != "6.21.6":
            raise SystemExit("Settings tests require ArduinoJson 6.21.6; run arduino-cli lib install ArduinoJson@6.21.6")
        source = Path(library.get("source_dir") or Path(library["install_dir"]) / "src")
        if (source / "ArduinoJson.h").is_file():
            return source
    raise SystemExit("ArduinoJson 6.21.6 was not found in the selected Arduino CLI configuration; install ESP dependencies with --install / -Install.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("all", "nano", "esp8266"), default="all")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--config-file", help="Optional Arduino CLI configuration used to locate ArduinoJson")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = root / "build"
    build.mkdir(exist_ok=True)
    suffix = ".exe" if os.name == "nt" else ""
    base = [args.cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    includes = ["-I", str(root / "libraries/UpsControl/src")]
    core = str(root / "libraries/UpsControl/src/UpsController.cpp")
    controller_binary = build / ("test_controller" + suffix)
    print("Building and running controller tests", flush=True)
    subprocess.run(base + includes + [str(root / "tests/test_controller.cpp"), core, "-o", str(controller_binary)], check=True)
    subprocess.run([str(controller_binary)], cwd=root, check=True)
    if args.target == "nano":
        return

    source = arduino_json_source(args.arduino_cli, args.config_file)
    settings_binary = build / ("test_settings" + suffix)
    settings_includes = ["-I", str(root / "tests/stubs"), "-I", str(root / "firmware/esp8266"), "-I", str(source)]
    print("Building and running settings tests with ArduinoJson 6.21.6", flush=True)
    subprocess.run(base + settings_includes + includes + [str(root / "tests/test_settings.cpp"), str(root / "firmware/esp8266/Settings.cpp"), core, "-o", str(settings_binary)], check=True)
    subprocess.run([str(settings_binary)], cwd=root, check=True)


if __name__ == "__main__":
    main()
