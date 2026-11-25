#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
target=all
nano_processor=atmega328old
install_dependencies=false
run_tests=false
while (($#)); do
  case "$1" in
    --target) target="${2:?Expected all, nano or esp8266}"; shift 2 ;;
    --nano-processor) nano_processor="${2:?Expected atmega328old or atmega328}"; shift 2 ;;
    --install) install_dependencies=true; shift ;;
    --test) run_tests=true; shift ;;
    --help|-h)
      printf '%s\n' 'Usage: bash tools/build.sh [--target all|nano|esp8266] [--nano-processor atmega328old|atmega328] [--install] [--test]'
      exit 0 ;;
    *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
  esac
done
case "$target" in all|nano|esp8266) ;; *) printf 'Invalid target: %s\n' "$target" >&2; exit 2 ;; esac
case "$nano_processor" in atmega328old|atmega328) ;; *) printf 'Invalid Nano processor: %s\n' "$nano_processor" >&2; exit 2 ;; esac

cd "$repo_dir"
command -v arduino-cli >/dev/null || { printf '%s\n' 'Install Arduino CLI and add it to PATH; see README.md.' >&2; exit 1; }
command -v python3 >/dev/null || { printf '%s\n' 'Python 3 is required.' >&2; exit 1; }
esp_index='https://arduino.esp8266.com/stable/package_esp8266com_index.json'

if "$install_dependencies"; then
  arduino-cli core update-index --additional-urls "$esp_index"
  if [[ "$target" == all || "$target" == nano ]]; then
    arduino-cli core install arduino:avr@1.8.8
  fi
  if [[ "$target" == all || "$target" == esp8266 ]]; then
    arduino-cli core install esp8266:esp8266@3.1.2 --additional-urls "$esp_index"
    arduino-cli lib install 'ArduinoJson@6.21.6'
  fi
fi

mkdir -p build
if "$run_tests"; then
  python3 tools/run_native_tests.py --target "$target" --cxx "${CXX:-g++}"
fi

if [[ "$target" == all || "$target" == nano ]]; then
  arduino-cli compile --fqbn "arduino:avr:nano:cpu=$nano_processor" --warnings all \
    --libraries "$repo_dir/libraries" --output-dir "$repo_dir/build/arduino_nano" \
    "$repo_dir/firmware/arduino_nano"
fi
if [[ "$target" == all || "$target" == esp8266 ]]; then
  python3 tools/embed_web.py
  arduino-cli compile --fqbn 'esp8266:esp8266:nodemcuv2:eesz=4M2M,baud=115200' --warnings all \
    --libraries "$repo_dir/libraries" --output-dir "$repo_dir/build/esp8266" \
    "$repo_dir/firmware/esp8266"
fi
python3 tools/package_firmware.py --target "$target" --nano-processor "$nano_processor"
