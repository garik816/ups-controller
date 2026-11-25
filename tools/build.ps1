[CmdletBinding()]
param(
    [ValidateSet('all', 'nano', 'esp8266')]
    [string]$Target = 'all',
    [ValidateSet('atmega328old', 'atmega328')]
    [string]$NanoProcessor = 'atmega328old',
    [string]$EspIndexUrl = 'https://github.com/esp8266/Arduino/releases/download/3.1.2/package_esp8266com_index.json',
    [switch]$Install,
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$CliArguments = @()
if ($Target -in @('all', 'esp8266')) {
    # A command-line URL replaces stale additional_urls from a user's CLI config.
    $CliArguments = @('--additional-urls', $EspIndexUrl)
}

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE."
    }
}

function Invoke-Arduino {
    param([string[]]$Arguments)
    Invoke-Checked 'arduino-cli' ($CliArguments + $Arguments)
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
$PreviousNetworkTimeout = $env:ARDUINO_NETWORK_CONNECTION_TIMEOUT
try {
    if ([string]::IsNullOrEmpty($PreviousNetworkTimeout)) {
        $env:ARDUINO_NETWORK_CONNECTION_TIMEOUT = '180s'
    }
    if ($Install) {
        Invoke-Arduino @('core', 'update-index')
        if ($Target -in @('all', 'nano')) {
            Invoke-Arduino @('core', 'install', 'arduino:avr@1.8.8')
        }
        if ($Target -in @('all', 'esp8266')) {
            Invoke-Arduino @('core', 'install', 'esp8266:esp8266@3.1.2')
            Invoke-Arduino @('lib', 'install', 'ArduinoJson@6.21.6')
        }
    }

    New-Item -ItemType Directory -Force 'build' | Out-Null
    if ($Test) {
        Invoke-Checked $PythonProgram ($PythonPrefix + @('tools/run_native_tests.py', '--target', $Target))
    }
    if ($Target -in @('all', 'nano')) {
        Invoke-Arduino @('compile', '--fqbn', "arduino:avr:nano:cpu=$NanoProcessor", '--warnings', 'all', '--libraries', (Join-Path $RepoRoot 'libraries'), '--output-dir', (Join-Path $RepoRoot 'build/arduino_nano'), (Join-Path $RepoRoot 'firmware/arduino_nano'))
    }
    if ($Target -in @('all', 'esp8266')) {
        Invoke-Checked $PythonProgram ($PythonPrefix + @('tools/embed_web.py'))
        Invoke-Arduino @('compile', '--fqbn', 'esp8266:esp8266:nodemcuv2:eesz=4M2M,baud=115200', '--warnings', 'all', '--libraries', (Join-Path $RepoRoot 'libraries'), '--output-dir', (Join-Path $RepoRoot 'build/esp8266'), (Join-Path $RepoRoot 'firmware/esp8266'))
    }
    Invoke-Checked $PythonProgram ($PythonPrefix + @('tools/package_firmware.py', '--target', $Target, '--nano-processor', $NanoProcessor))
} finally {
    $env:ARDUINO_NETWORK_CONNECTION_TIMEOUT = $PreviousNetworkTimeout
    Pop-Location
}
