[CmdletBinding()]
param(
    [ValidateSet('all', 'nano', 'esp8266')]
    [string]$Target = 'all',
    [ValidateSet('atmega328old', 'atmega328')]
    [string]$NanoProcessor = 'atmega328old',
    [switch]$Install,
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE."
    }
}

if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
    throw 'Install Arduino CLI and add it to PATH; see README.md.'
}
if (Get-Command py -ErrorAction SilentlyContinue) {
    $PythonProgram = 'py'
    $PythonPrefix = @('-3')
} elseif (Get-Command python3 -ErrorAction SilentlyContinue) {
    $PythonProgram = 'python3'
    $PythonPrefix = @()
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    $PythonProgram = 'python'
    $PythonPrefix = @()
} else {
    throw 'Python 3 is required.'
}

Push-Location $RepoRoot
try {
    $EspIndex = 'https://arduino.esp8266.com/stable/package_esp8266com_index.json'
    if ($Install) {
        Invoke-Checked 'arduino-cli' @('core', 'update-index', '--additional-urls', $EspIndex)
        if ($Target -in @('all', 'nano')) {
            Invoke-Checked 'arduino-cli' @('core', 'install', 'arduino:avr@1.8.8')
        }
        if ($Target -in @('all', 'esp8266')) {
            Invoke-Checked 'arduino-cli' @('core', 'install', 'esp8266:esp8266@3.1.2', '--additional-urls', $EspIndex)
            Invoke-Checked 'arduino-cli' @('lib', 'install', 'ArduinoJson@6.21.6')
        }
    }

    New-Item -ItemType Directory -Force 'build' | Out-Null
    if ($Test) {
        Invoke-Checked $PythonProgram ($PythonPrefix + @('tools/run_native_tests.py', '--target', $Target))
    }
    if ($Target -in @('all', 'nano')) {
        Invoke-Checked 'arduino-cli' @('compile', '--fqbn', "arduino:avr:nano:cpu=$NanoProcessor", '--warnings', 'all', '--libraries', (Join-Path $RepoRoot 'libraries'), '--output-dir', (Join-Path $RepoRoot 'build/arduino_nano'), (Join-Path $RepoRoot 'firmware/arduino_nano'))
    }
    if ($Target -in @('all', 'esp8266')) {
        Invoke-Checked $PythonProgram ($PythonPrefix + @('tools/embed_web.py'))
        Invoke-Checked 'arduino-cli' @('compile', '--fqbn', 'esp8266:esp8266:nodemcuv2:eesz=4M2M,baud=115200', '--warnings', 'all', '--libraries', (Join-Path $RepoRoot 'libraries'), '--output-dir', (Join-Path $RepoRoot 'build/esp8266'), (Join-Path $RepoRoot 'firmware/esp8266'))
    }
    Invoke-Checked $PythonProgram ($PythonPrefix + @('tools/package_firmware.py', '--target', $Target, '--nano-processor', $NanoProcessor))
} finally {
    Pop-Location
}
