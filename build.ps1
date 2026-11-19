# Regenerate the cartridge code and build GTAAdvanceRecomp.exe.
#
#   .\build.ps1              regenerate + build (open-source BIOS)
#   .\build.ps1 -SkipRegen   build only
#   .\build.ps1 -RealBios    recompile the framework's bios/gba_bios.bin and link it
#
# Ends by packing build\GTAAdvance.zip, the self-contained copy for other
# PCs. After an incremental `cmake --build`, run tools\package.ps1 by hand.
#
# Toolchain: MSYS2 MinGW64 GCC + Ninja. Do NOT use the framework's
# clang-mingw toolchain here: with LLVM 18 + MinGW ld the runtime's
# thread-locals get bogus TLS offsets and the exe segfaults on the first
# RAM-code dispatch.
param([switch]$SkipRegen, [switch]$RealBios)
$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $MyInvocation.MyCommand.Path
$fw     = Join-Path $root '..\gbarecomp'
if (-not (Test-Path $fw)) { $fw = Join-Path $root '..\..\DukeAdvance\gbarecomp' }
$recomp = Join-Path $fw 'build\Release\gba_recompile.exe'
$rom    = Join-Path $root '..\Grand Theft Auto Advance (USA).gba'
$mingw  = 'C:\msys64\mingw64\bin'
$env:PATH = "$mingw;$env:PATH"

if (-not (Test-Path $recomp)) {
    cmake -S $fw -B "$fw\build" -G 'Visual Studio 17 2022' -A x64
    cmake --build "$fw\build" --config Release --target gba_recompile --parallel
}

if (-not $SkipRegen) {
    # Base config first, then the machine-generated seed overlay.
    python "$root\tools\scan_seeds.py"
    if ($LASTEXITCODE) { throw 'seed scan failed' }
    $cfg = @('--config', "$root\recomp.toml", '--config', "$root\recomp_scan.toml")
    & $recomp --rom $rom @cfg --out "$root\generated"
    if ($LASTEXITCODE) { throw 'cartridge recompile failed' }
}

$biosDir = "$root\generated_bios"
if ($RealBios) {
    $biosDir = "$root\generated_bios_real"
    & $recomp --bios "$fw\bios\gba_bios.bin" --config "$fw\bios\gba_bios.toml" --out $biosDir
    if ($LASTEXITCODE) { throw 'BIOS recompile failed' }
}

& "$mingw\cmake.exe" -S $root -B "$root\build" -G Ninja -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_C_COMPILER=$($mingw -replace '\\','/')/gcc.exe" `
    "-DCMAKE_CXX_COMPILER=$($mingw -replace '\\','/')/g++.exe" `
    "-DGBARECOMP_GENERATED_BIOS_DIR=$($biosDir -replace '\\','/')"
if ($LASTEXITCODE) { throw 'configure failed' }
& "$mingw\cmake.exe" --build "$root\build" --target GTAAdvanceRecomp --parallel
if ($LASTEXITCODE) { throw 'build failed' }
Write-Host "Built $root\build\GTAAdvanceRecomp.exe"

# Every build also refreshes build\GTAAdvance.zip, the self-contained copy
# for other PCs (see tools\package.ps1).
& "$root\tools\package.ps1" -BuildDir "$root\build"
