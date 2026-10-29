# Balatro360 converter: copies the game files out of your own copy of Balatro
# into the Balatro360 folder of the release, next to default.xex.
#
# Balatro for PC is a LOVE game: Balatro.exe is the LOVE runtime with the
# game's files appended as a zip archive (on a Mac, Balatro.love is that zip
# alone). This finds the archive, extracts it into Balatro360\game and leaves
# out the PC-only .dll and .exe files, as tools/prepare_game.py does.
# Windows PowerShell 5.1 (Windows 10 and 11); ASCII only, as 5.1 reads a
# script without a byte order mark in the system code page.
#
# -Source path skips the file dialog (dropping Balatro.exe on the .bat).

param([string]$Source = '')

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.IO.Compression

$FatxMaxName = 42   # the 360's file system limits each name to 42 characters

function Fail([string]$message) {
    Write-Host ''
    Write-Host "Error: $message" -ForegroundColor Red
    exit 1
}

$release = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$target = Join-Path $release 'Balatro360'
if (-not (Test-Path (Join-Path $target 'default.xex'))) {
    Fail "the Balatro360 folder with default.xex isn't next to this converter ($target)."
}

Write-Host 'Balatro360 converter'
Write-Host '--------------------'
$source = $Source
if (-not $source) {
    Write-Host 'Choose Balatro.exe from your Balatro installation.'
    Write-Host '(Steam: right-click Balatro, Manage, Browse local files.)'
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Choose your Balatro.exe'
    $dialog.Filter = 'Balatro (Balatro.exe, Balatro.love)|Balatro.exe;*.love|All files (*.*)|*.*'
    foreach ($guess in @("${env:ProgramFiles(x86)}\Steam\steamapps\common\Balatro",
                         "$env:ProgramFiles\Steam\steamapps\common\Balatro")) {
        if ($guess -and (Test-Path $guess)) {
            $dialog.InitialDirectory = $guess
            break
        }
    }
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        Write-Host 'Nothing chosen; nothing changed.'
        exit 0
    }
    $source = $dialog.FileName
}
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { Fail "$source isn't a file." }
Write-Host "Reading $source"

# The zip archive is at the end of the file: its end of central directory
# record says how big the central directory is and where it starts relative
# to the archive, which gives where the archive starts in the file.
$bytes = [System.IO.File]::ReadAllBytes($source)
$eocd = -1
$lowest = [Math]::Max(0, $bytes.Length - 65557)
for ($i = $bytes.Length - 22; $i -ge $lowest; $i--) {
    if ($bytes[$i] -eq 0x50 -and $bytes[$i + 1] -eq 0x4B -and $bytes[$i + 2] -eq 0x05 -and $bytes[$i + 3] -eq 0x06) {
        $eocd = $i
        break
    }
}
if ($eocd -lt 0) { Fail "$source doesn't contain a game archive. Choose Balatro.exe." }
$cdSize = [long][BitConverter]::ToUInt32($bytes, $eocd + 12)
$cdOffset = [long][BitConverter]::ToUInt32($bytes, $eocd + 16)
$start = [long]$eocd - $cdSize - $cdOffset
if ($start -lt 0) { Fail "$source has a damaged game archive." }

$stream = New-Object System.IO.MemoryStream($bytes, [int]$start, [int]($bytes.Length - $start), $false)
$zip = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Read)
$names = @{}
foreach ($entry in $zip.Entries) { $names[$entry.FullName.Replace('\', '/')] = $true }
if (-not $names.ContainsKey('main.lua')) { Fail "$source has no main.lua; is it Balatro.exe?" }

# Extract next to the target first, so a failure leaves the old game folder.
$staging = Join-Path $target 'game.new'
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Path $staging | Out-Null

$count = 0
$total = 0
$longNames = @()
foreach ($entry in $zip.Entries) {
    $path = $entry.FullName.Replace('\', '/')
    if ($path.EndsWith('/')) { continue }
    $lower = $path.ToLowerInvariant()
    if ($lower.EndsWith('.dll') -or $lower.EndsWith('.exe')) { continue }
    $parts = $path.Split('/')
    if ($parts -contains '..' -or $path.StartsWith('/')) { continue }
    foreach ($part in $parts) {
        if ($part.Length -gt $FatxMaxName) { $longNames += $path }
    }
    $dest = Join-Path $staging ($parts -join '\')
    $folder = Split-Path -Parent $dest
    if (-not (Test-Path $folder)) { New-Item -ItemType Directory -Path $folder | Out-Null }
    $in = $entry.Open()
    $out = [System.IO.File]::Create($dest)
    try { $in.CopyTo($out) } finally { $out.Dispose(); $in.Dispose() }
    $count++
    $total += $entry.Length
}
$zip.Dispose()

# Replace the game folder, keeping a b360_config.lua someone wrote there.
$game = Join-Path $target 'game'
$config = Join-Path $game 'b360_config.lua'
$keptConfig = $null
if (Test-Path $config) { $keptConfig = [System.IO.File]::ReadAllBytes($config) }
if (Test-Path $game) { Remove-Item -Recurse -Force $game }
Rename-Item -Path $staging -NewName 'game'
if ($keptConfig) { [System.IO.File]::WriteAllBytes($config, $keptConfig) }

$version = 'unknown'
$versionFile = Join-Path $game 'version.jkr'
if (Test-Path $versionFile) { $version = (Get-Content -TotalCount 1 $versionFile).Trim() }

Write-Host ''
Write-Host ("Balatro version: {0}" -f $version)
Write-Host ("Copied {0} files ({1:N1} MB) to {2}" -f $count, ($total / 1MB), $game)
if ($longNames.Count -gt 0) {
    Write-Host ("Warning: these names are longer than the 360's {0} characters and won't copy:" -f $FatxMaxName) `
        -ForegroundColor Yellow
    $longNames | Sort-Object -Unique | ForEach-Object { Write-Host "  $_" }
}
Write-Host ''
Write-Host 'Done. Copy the whole Balatro360 folder to your console (a USB drive or the'
Write-Host 'hard disk) and launch default.xex from it.' -ForegroundColor Green
