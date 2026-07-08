# CodeScope — download pre-built binary for Windows
# Usage (run in PowerShell):
#   irm https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/install.ps1 | iex
#
# Or save and run:
#   powershell -ExecutionPolicy Bypass -File install.ps1

$ErrorActionPreference = "Stop"

$Repo = "Timwood0x10/CodeScope"
$Arch = "x86_64"
$Artifact = "codescope-x86_64-windows"

Write-Host "=== CodeScope Download ===" -ForegroundColor Cyan
Write-Host "Platform: Windows ${Arch}" -ForegroundColor Cyan

# ── Resolve latest tag ──
Write-Host ""
Write-Host "[1/2] Resolving latest release..." -ForegroundColor Cyan
$apiUrl = "https://api.github.com/repos/$Repo/releases/latest"
try {
    $release = Invoke-RestMethod -Uri $apiUrl -ErrorAction Stop
    $LatestTag = $release.tag_name
    Write-Host "  Latest tag: $LatestTag" -ForegroundColor Green
} catch {
    Write-Host "  ⚠  Failed to get latest tag, falling back to 'latest'" -ForegroundColor Yellow
    $LatestTag = "latest"
}

# ── Download ──
$DownloadUrl = "https://github.com/$Repo/releases/download/$LatestTag/${Artifact}.tar.gz"
$OutFile = "$env:TEMP\codescope.tar.gz"
Write-Host ""
Write-Host "[2/2] Downloading $DownloadUrl ..." -ForegroundColor Cyan

try {
    Invoke-WebRequest -Uri $DownloadUrl -OutFile $OutFile -ErrorAction Stop
} catch {
    Write-Host "  ❌ Download failed: $_" -ForegroundColor Red
    exit 1
}

# ── Extract ──
$InstallDir = if ($env:INSTALL_DIR) { $env:INSTALL_DIR } else { "$env:USERPROFILE\.codescope\bin" }
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

Write-Host "  Extracting to $InstallDir ..." -ForegroundColor Cyan

# tar is available on Windows 10 1803+ (build 17063) via 'tar.exe'
# Fall back to 7z if tar fails
try {
    tar -xzf $OutFile -C $InstallDir --strip-components 1 2>$null
    if (-not (Test-Path "$InstallDir\codescope.exe")) {
        # tar might have extracted without stripping; check subdir
        $subdir = Get-ChildItem "$InstallDir\codescope-*" -Directory | Select-Object -First 1
        if ($subdir) {
            Move-Item "$subdir\*" $InstallDir -Force
            Remove-Item $subdir -Recurse -Force
        }
        # If still no exe, try direct extraction (no subdir)
        if (-not (Test-Path "$InstallDir\codescope.exe")) {
            tar -xzf $OutFile -C $InstallDir 2>$null
        }
    }
} catch {
    Write-Host "  ⚠  tar extraction failed, trying 7z..." -ForegroundColor Yellow
    # Fallback: 7z can extract .tar.gz
    $7zPaths = @(
        "${env:ProgramFiles}\7-Zip\7z.exe",
        "${env:ProgramFiles(x86)}\7-Zip\7z.exe",
        "C:\Program Files\7-Zip\7z.exe"
    )
    $7z = $null
    foreach ($p in $7zPaths) {
        if (Test-Path $p) { $7z = $p; break }
    }
    if (-not $7z) {
        Write-Host "  ❌ Cannot extract: install 7-Zip or use Windows 10 1803+ with built-in tar" -ForegroundColor Red
        Remove-Item $OutFile -Force -ErrorAction SilentlyContinue
        exit 1
    }
    # 7z x archive.tar.gz -> archive.tar, then 7z x archive.tar
    & $7z x "$OutFile" -o"$env:TEMP\codescope_extract" -y -bd | Out-Null
    $tarFile = Get-ChildItem "$env:TEMP\codescope_extract\*.tar" | Select-Object -First 1
    if ($tarFile) {
        & $7z x $tarFile.FullName -o"$InstallDir" -y -bd | Out-Null
    }
    Remove-Item "$env:TEMP\codescope_extract" -Recurse -Force -ErrorAction SilentlyContinue
}

Remove-Item $OutFile -Force -ErrorAction SilentlyContinue

# Verify binary exists
if (-not (Test-Path "$InstallDir\codescope.exe")) {
    Write-Host "  ❌ Binary not found after extraction" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Green
Write-Host ""
Write-Host "Binary installed to: $InstallDir\codescope.exe" -ForegroundColor Cyan
Write-Host ""
Write-Host "Add to PATH (run in PowerShell):" -ForegroundColor Yellow
Write-Host "  `$env:Path = `"$InstallDir;`$env:Path`"" -ForegroundColor White
Write-Host ""
Write-Host "To make permanent, add to your PowerShell profile:" -ForegroundColor Yellow
Write-Host "  [Environment]::SetEnvironmentVariable('Path'," -ForegroundColor White
Write-Host "    [Environment]::GetEnvironmentVariable('Path','User') + ';$InstallDir'," -ForegroundColor White
Write-Host "    'User')" -ForegroundColor White
Write-Host ""
Write-Host "Then index a project:" -ForegroundColor Yellow
Write-Host "  codescope cli index_project '{"""project_path""":"""C:\path\to\project"""}'" -ForegroundColor White
Write-Host ""
Write-Host "Or start MCP server:" -ForegroundColor Yellow
Write-Host "  codescope" -ForegroundColor White
