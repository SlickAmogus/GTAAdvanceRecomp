# GTA Advance room server - deploy or update from Windows in one go.
#
# Reads gta_server.txt next to this script, sends gta_install.sh and the
# sh_master sources (from the Silent Hill Online checkout named by source=)
# to the Linux machine over one ssh connection (its password is asked once),
# and runs gta_install.sh there. With players online it lists them and asks:
# continue (warn them, restart in 30 s), wait (restart as soon as nobody is
# on), or stop (change nothing).
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$cfgPath = Join-Path $here 'gta_server.txt'
$cfg = @{}
if (Test-Path $cfgPath) {
    foreach ($line in Get-Content $cfgPath) {
        if ($line -match '^\s*([A-Za-z_]+)\s*=\s*(.*?)\s*$') { $cfg[$matches[1].ToLower()] = $matches[2] }
    }
}

$target = $cfg['host']
if (-not $target) {
    $target = Read-Host "Server to log in to (for example root@1.2.3.4)"
    if (-not $target) { exit 1 }
    Add-Content -Path $cfgPath -Value "host=$target" -Encoding ascii
    Write-Host "Saved to gta_server.txt."
}
if ($target -notmatch '@') { $target = "root@$target" }

$source = $cfg['source']
if (-not $source) { $source = '..\..\..\silenthill-online\silent-hill-decomp' }
if (-not [IO.Path]::IsPathRooted($source)) { $source = Join-Path $here $source }
$files = [ordered]@{
    'gta_install.sh'       = (Join-Path $here 'gta_install.sh')
    'sh_master.c'          = (Join-Path $source 'online_server\sh_master.c')
    'sh_net_proto.h'       = (Join-Path $source 'pc_port\include\sh_net_proto.h')
    'sh_net_memo_text.h'   = (Join-Path $source 'pc_port\include\sh_net_memo_text.h')
    'sh_net_chat_filter.h' = (Join-Path $source 'pc_port\include\sh_net_chat_filter.h')
}
foreach ($k in $files.Keys) {
    if (-not (Test-Path $files[$k])) {
        Write-Host "ERROR: $($files[$k]) is missing (source= in gta_server.txt names the Silent Hill Online checkout)" -ForegroundColor Red
        exit 1
    }
}
if (-not (Get-Command ssh -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: 'ssh' not found. Enable the OpenSSH Client in Settings > Optional Features." -ForegroundColor Red
    exit 1
}

function Sq($s) { "'" + ($s -replace "'", "'\''") + "'" }

# Runs the remote script over ssh, showing its output as it arrives. The
# script prints "@@ASK <n>" when players are online and reads the answer from
# standard input, or "@@GO" when nobody is; standard input is closed after
# either. Returns the remote exit code (3 = stopped, nothing changed).
function Invoke-Remote([string]$sshArgs, [string]$script) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'ssh'
    $psi.Arguments = $sshArgs
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $p = [System.Diagnostics.Process]::Start($psi)
    $in = $p.StandardInput.BaseStream
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $send = { param([string]$text) $b = $utf8.GetBytes(($text -replace "`r`n", "`n")); $in.Write($b, 0, $b.Length); $in.Flush() }
    & $send $script

    $out = $p.StandardOutput
    $line = New-Object System.Text.StringBuilder
    $hold = $false; $done = $false
    while (($c = $out.Read()) -ge 0) {
        $ch = [char]$c
        if ($ch -ne "`n") {
            if ($line.Length -eq 0) { $hold = ($ch -eq '@') }
            [void]$line.Append($ch)
            if (-not $hold) { [Console]::Out.Write($ch) }
            continue
        }
        $l = $line.ToString().TrimEnd("`r"); $held = $hold; $hold = $false; $line.Length = 0
        if (-not $done -and $l -match '^@@ASK (\d+)') {
            Write-Host ""
            Write-Host "$($matches[1]) player(s) online (listed above)." -ForegroundColor Yellow
            Write-Host "  C = continue: warn them in game, wait 30 seconds, restart with the new version"
            Write-Host "  W = wait: restart as soon as nobody is online (checked every 15 seconds)"
            Write-Host "  S = stop: change nothing; the server keeps running the version it has"
            do { $ans = (Read-Host "Continue, wait or stop? (C/W/S)").Trim().ToLower() } while ('c', 'w', 's' -notcontains $ans)
            & $send "$ans`n"
            $in.Close(); $done = $true
            continue
        }
        if (-not $done -and $l -eq '@@GO') { $in.Close(); $done = $true; continue }
        if ($held) { [Console]::Out.Write($l) }
        [Console]::Out.Write("`n")
    }
    if (-not $done) { try { $in.Close() } catch {} }
    $p.WaitForExit()
    return $p.ExitCode
}

Write-Host "Packaging the server files ..."
$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("set -e`nU=`$HOME/gta-rooms-upload`nmkdir -p `"`$U`" && cd `"`$U`"`n")
$i = 0
foreach ($k in $files.Keys) {
    $b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($files[$k]), 'InsertLineBreaks')
    [void]$sb.Append("base64 -d > $k <<'B64_$i'`n$b64`nB64_$i`n")
    $i++
}
[void]$sb.Append("if [ `"`$(id -u)`" = 0 ]; then SUDO=; else SUDO=sudo; fi`n")
[void]$sb.Append("`$SUDO bash `"`$U/gta_install.sh`" --args $(Sq $cfg['args'])`n")

Write-Host ""
Write-Host "Connecting to $target - enter its password when asked." -ForegroundColor Cyan
$code = Invoke-Remote "-o StrictHostKeyChecking=accept-new -o ServerAliveInterval=20 $target `"bash -s`"" $sb.ToString()

Write-Host ""
if ($code -eq 0) { Write-Host "Done - the GTA Advance room server on $target is up to date and running." -ForegroundColor Green }
elseif ($code -eq 3) { Write-Host "Stopped - nothing was changed." -ForegroundColor Yellow; $code = 0 }
else { Write-Host "Deploy FAILED (exit $code). The output above says why." -ForegroundColor Red }
exit $code
