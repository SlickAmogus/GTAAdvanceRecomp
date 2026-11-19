# Packs everything needed to run the game on another PC into build\GTAAdvance.zip.
#
#   .\tools\package.ps1
#
# The zip is self-contained: the executable, its DLLs and assets, the ROM and
# the BIOS, with a game.toml that refers to them by relative path. Extract it
# anywhere and run GTAAdvanceRecomp.exe.
#
# It contains the cartridge ROM and the BIOS dump, so it is for the owner's own
# machines only: never commit it or share it. (build\ is gitignored.)
param([string]$BuildDir)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build' }
$rom  = Join-Path $root '..\Grand Theft Auto Advance (USA).gba'
$bios = Join-Path $root '..\gbarecomp\bios\gba_bios.bin'
if (-not (Test-Path $bios)) { $bios = Join-Path $root '..\..\DukeAdvance\gbarecomp\bios\gba_bios.bin' }
$exe  = Join-Path $BuildDir 'GTAAdvanceRecomp.exe'
foreach ($f in $exe, $rom, $bios) { if (-not (Test-Path -LiteralPath $f)) { throw "missing $f" } }

$stage = Join-Path $BuildDir 'package\GTAAdvance'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item -LiteralPath $exe $stage
Get-ChildItem -LiteralPath $BuildDir -Filter *.dll | Copy-Item -Destination $stage
Copy-Item -LiteralPath (Join-Path $BuildDir 'assets') $stage -Recurse
Copy-Item -LiteralPath $rom  (Join-Path $stage 'GTAAdvance.gba')
Copy-Item -LiteralPath $bios (Join-Path $stage 'gba_bios.bin')

# game.toml with paths relative to itself (the runtime resolves them against
# the file's own folder). Player settings (gta_pc.ini, input.ini, saves) are
# left out so each machine keeps its own.
$toml = Get-Content -LiteralPath (Join-Path $root 'game.toml') -Raw
$toml = [regex]::Replace($toml, '(?m)^path\s*=\s*".*\.gba"\s*$', 'path = "GTAAdvance.gba"')
$toml = [regex]::Replace($toml, '(?m)^path\s*=\s*".*gba_bios\.bin"\s*$', 'path = "gba_bios.bin"')
if ($toml -notmatch 'path = "GTAAdvance\.gba"' -or $toml -notmatch 'path = "gba_bios\.bin"') {
    throw 'could not rewrite the ROM/BIOS paths in game.toml'
}
[IO.File]::WriteAllText((Join-Path $stage 'game.toml'), $toml, (New-Object Text.UTF8Encoding $false))

$zip = Join-Path $BuildDir 'GTAAdvance.zip'
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Remove-Item -Recurse -Force (Join-Path $BuildDir 'package')
Write-Host ("Packed {0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
