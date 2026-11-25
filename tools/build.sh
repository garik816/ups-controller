#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
target=all
nano_processor=atmega328old
esp_index='https://github.com/esp8266/Arduino/releases/download/3.1.2/package_esp8266com_index.json'
install_dependencies=false
run_tests=false
while (($#)); do
  case "$1" in
    --target) target="${2:?Expected all, nano or esp8266}"; shift 2 ;;
    --nano-processor) nano_processor="${2:?Expected atmega328old or atmega328}"; shift 2 ;;
    --esp-index-url) esp_index="${2:?Expected an ESP8266 package index URL}"; shift 2 ;;
    --install) install_dependencies=true; shift ;;
    --test) run_tests=true; shift ;;
    --help|-h)
      printf '%s\n' 'Usage: bash tools/build.sh [--target all|nano|esp8266] [--nano-processor atmega328old|atmega328] [--esp-index-url URL] [--install] [--test]'
      exit 0 ;;
    *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
  esac
done
case "$target" in all|nano|esp8266) ;; *) printf 'Invalid target: %s\n' "$target" >&2; exit 2 ;; esac
case "$nano_processor" in atmega328old|atmega328) ;; *) printf 'Invalid Nano processor: %s\n' "$nano_processor" >&2; exit 2 ;; esac

cd "$repo_dir"
command -v arduino-cli >/dev/null || { printf '%s\n' 'Install Arduino CLI and add it to PATH; see README.md.' >&2; exit 1; }
command -v python3 >/dev/null || { printf '%s\n' 'Python 3 is required.' >&2; exit 1; }
export ARDUINO_NETWORK_CONNECTION_TIMEOUT="${ARDUINO_NETWORK_CONNECTION_TIMEOUT:-180s}"
cli_arguments=()
if [[ "$target" == all || "$target" == esp8266 ]]; then
  # Apply to every command so a stale URL in user configuration is not consulted.
  cli_arguments=(--additional-urls "$esp_index")
fi
run_arduino() {
  arduino-cli "${cli_arguments[@]}" "$@"
}

if "$install_dependencies"; then
  run_arduino core update-index
  if [[ "$target" == all || "$target" == nano ]]; then
    run_arduino core install arduino:avr@1.8.8
  fi
  if [[ "$target" == all || "$target" == esp8266 ]]; then
    run_arduino core install esp8266:esp8266@3.1.2
    run_arduino lib install 'ArduinoJson@6.21.6'
  fi
fi

mkdir -p build
if "$run_tests"; then
  python3 tools/run_native_tests.py --target "$target" --cxx "${CXX:-g++}"
fi

if [[ "$target" == all || "$target" == nano ]]; then
  run_arduino compile --fqbn "arduino:avr:nano:cpu=$nano_processor" --warnings all \
    --libraries "$repo_dir/libraries" --output-dir "$repo_dir/build/arduino_nano" \
    "$repo_dir/firmware/arduino_nano"
fi
if [[ "$target" == all || "$target" == esp8266 ]]; then
  python3 tools/embed_web.py
  run_arduino compile --fqbn 'esp8266:esp8266:nodemcuv2:eesz=4M2M,baud=115200' --warnings all \
    --libraries "$repo_dir/libraries" --output-dir "$repo_dir/build/esp8266" \
    "$repo_dir/firmware/esp8266"
fi
python3 tools/package_firmware.py --target "$target" --nano-processor "$nano_processor"
