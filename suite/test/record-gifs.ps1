# record-gifs.ps1: records the README's GIFs on Windows, in your own Notepad++ look.
#
#   powershell -ExecutionPolicy Bypass -File record-gifs.ps1 -Npp <portable KEX folder> [-Out <folder>] [-Scenes all]
#
# Starts the portable Notepad++ KEX in -Npp (its own settings, next to your installed one) with the self-test's
# demo mode: it plays short scenes of the features and records each one from the screen into <Out>\<scene>.kexrec.
# Your look comes from %APPDATA%\Notepad++\config.xml (dark mode, toolbar, tab bar, hidden menu bar), without its
# file history, search history and folder paths. The window stays in front while it records (about 2 minutes);
# the real mouse isn't used, so moving it does no harm, but a window or notification over it would be recorded.
# At the end the recordings are zipped into <Out>\recordings.zip; suite/test/kexrec2gif.py makes the GIFs.
param(
    [Parameter(Mandatory = $true)][string]$Npp,
    [string]$Out = (Join-Path $PSScriptRoot 'out'),
    [string]$Scenes = 'all'
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Npp 'notepad++.exe'
if (-not (Test-Path $exe)) { throw "no notepad++.exe in $Npp" }
if (-not (Test-Path (Join-Path $Npp 'doLocalConf.xml'))) { throw "$Npp isn't a portable Notepad++ (no doLocalConf.xml)" }

# the user's look, nothing private
$mine = Join-Path $env:APPDATA 'Notepad++\config.xml'
if (Test-Path $mine) {
    $xml = Get-Content -Raw -Encoding UTF8 $mine
    $xml = [regex]::Replace($xml, '\s*<FindHistory[\s\S]*?</FindHistory>', '')
    $xml = [regex]::Replace($xml, '\s*<FileBrowser[\s\S]*?</FileBrowser>', '')
    $xml = [regex]::Replace($xml, '(<History[^>]*>)[\s\S]*?(</History>)', '$1$2')
    $xml = $xml -replace '<GUIConfig name="titleBar" short="no" />', '<GUIConfig name="titleBar" short="yes" />'
    [IO.File]::WriteAllText((Join-Path $Npp 'config.xml'), $xml, (New-Object Text.UTF8Encoding $false))
    Write-Host "config.xml: your look from $mine"
} else { Write-Host "no ${mine}, so Notepad++ keeps its default look" }
foreach ($f in 'kex.ini', 'kex.log', 'session.xml') { Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $Npp $f) }

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out 'selftest-log.txt'
$nppArgs = "-multiInst -nosession `"-pluginMessage=KexSuite_selftest=1;KexSuite_demo=$Scenes;KexSuite_testdir=$Out`""
$p = Start-Process -FilePath $exe -ArgumentList $nppArgs -WorkingDirectory $Npp -PassThru
Write-Host "recording (Notepad++ KEX pid $($p.Id)); leave the window in front until it closes"
$deadline = (Get-Date).AddMinutes(6)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    if ((Test-Path $log) -and (Select-String -Quiet -Path $log -Pattern '^DONE')) { break }
    if ($p.HasExited) { break }
}
Start-Sleep -Seconds 1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
if (Test-Path $log) { Select-String -Path $log -Pattern '^(DEMO|DONE)' | ForEach-Object { Write-Host $_.Line } }
$recs = Get-ChildItem $Out -Filter *.kexrec
if (-not $recs) { throw "no recordings in $Out" }
Compress-Archive -Force -Path (@($recs.FullName) + $log) -DestinationPath (Join-Path $Out 'recordings.zip')
Write-Host ("recordings.zip: {0:N1} MB, {1} scenes" -f ((Get-Item (Join-Path $Out 'recordings.zip')).Length / 1MB), $recs.Count)
