param([ValidateSet("win-x64", "win-arm64")][string]$Runtime = "win-x64")
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
if (-not $IsWindows) { throw "Build the Windows installer on Windows with PowerShell 7." }
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$BuildRoot = Join-Path $ProjectRoot "build/windows/$Runtime"
$Stage = Join-Path $BuildRoot (".package." + [guid]::NewGuid().ToString("N"))
$Package = Join-Path $Stage "Minion Rush Installer"
$Python = Join-Path $BuildRoot "venv/Scripts/python.exe"

function Run-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
}

New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
New-Item -ItemType Directory -Path $Stage | Out-Null
try {
    if (-not (Test-Path $Python)) { Run-Checked "python" @("-m", "venv", (Join-Path $BuildRoot "venv")) }
    $PythonArchitecture = & $Python -c "import platform; print(platform.machine().lower())"
    if ($LASTEXITCODE -ne 0) { throw "Cannot determine Python architecture." }
    if (($Runtime -eq "win-x64" -and $PythonArchitecture -notin @("amd64", "x86_64")) -or
        ($Runtime -eq "win-arm64" -and $PythonArchitecture -notin @("arm64", "aarch64"))) {
        throw "Use Windows and Python matching the target architecture ($Runtime)."
    }
    Run-Checked $Python @("-m", "pip", "install", "-r", (Join-Path $ProjectRoot "installer/windows/requirements.txt"))
    Run-Checked $Python @("-m", "pip", "check")
    Run-Checked $Python @("-B", (Join-Path $ProjectRoot "tools/validate_source.py"))
    Run-Checked $Python @("-B", "-m", "unittest", "discover", "-s", (Join-Path $ProjectRoot "tests"), "-p", "test_ipa*.py", "-v")
    Run-Checked $Python @("-B", "-m", "unittest", "discover", "-s", (Join-Path $ProjectRoot "tests"), "-p", "test_windows*.py", "-v")
    Run-Checked "dotnet" @("run", "--project", (Join-Path $ProjectRoot "tests/InstallerWindowsTests.csproj"),
        "--artifacts-path", (Join-Path $BuildRoot "tests"), "-c", "Release")
    Run-Checked "dotnet" @("run", "--project", (Join-Path $ProjectRoot "tests/InstallerWindowsUiTests.csproj"),
        "--artifacts-path", (Join-Path $BuildRoot "ui-tests"), "-c", "Release", "--", (Join-Path $BuildRoot "qa"))
    Run-Checked "dotnet" @("publish", (Join-Path $ProjectRoot "installer/windows/MinionRushInstaller.csproj"),
        "--artifacts-path", (Join-Path $BuildRoot "compiler"),
        "-c", "Release", "-r", $Runtime, "--self-contained", "true", "-o", $Package,
        "-p:PublishSingleFile=true", "-p:IncludeNativeLibrariesForSelfExtract=true", "-p:DebugType=none")
    Run-Checked $Python @("-m", "PyInstaller", "--noconfirm", "--clean", "--onedir", "--console",
        "--recursive-copy-metadata", "pymobiledevice3",
        "--name", "MinionRushDeviceBackend", "--paths", (Join-Path $ProjectRoot "tools"),
        "--distpath", (Join-Path $BuildRoot "device"), "--workpath", (Join-Path $BuildRoot "pyinstaller"),
        "--specpath", $BuildRoot, (Join-Path $ProjectRoot "tools/ipa_device_backend.py"))
    $Backend = Join-Path $Package "backend"
    New-Item -ItemType Directory -Path $Backend -Force | Out-Null
    Copy-Item -Path (Join-Path $BuildRoot "device/MinionRushDeviceBackend/*") -Destination $Backend -Recurse -Force
    Run-Checked $Python @("-B", (Join-Path $ProjectRoot "tools/check_device_backend.py"),
        (Join-Path $Backend "MinionRushDeviceBackend.exe"))
    Copy-Item -Path (Join-Path $ProjectRoot "LICENSE") -Destination $Package
    Copy-Item -Path (Join-Path $ProjectRoot "docs/INSTALLER_WINDOWS.md") -Destination (Join-Path $Package "README.md")
    Run-Checked $Python @("-B", (Join-Path $ProjectRoot "tools/package_windows_licenses.py"),
        (Join-Path $Package "THIRD_PARTY_NOTICES.txt"),
        (Join-Path $BuildRoot "compiler/obj/MinionRushInstaller/project.assets.json"))
    Run-Checked $Python @("-B", (Join-Path $ProjectRoot "tools/validate_windows_installer.py"),
        $Package, "--architecture", $Runtime.Substring(4))
    $Archive = Join-Path $BuildRoot "minion-rush-installer-1.0.0-$Runtime.zip"
    Compress-Archive -Path $Package -DestinationPath $Archive -Force
    $Extracted = Join-Path $Stage "extracted"
    Expand-Archive -LiteralPath $Archive -DestinationPath $Extracted
    Run-Checked $Python @("-B", (Join-Path $ProjectRoot "tools/validate_windows_installer.py"),
        (Join-Path $Extracted "Minion Rush Installer"), "--architecture", $Runtime.Substring(4))
    $OriginalFiles = @(Get-ChildItem -LiteralPath $Package -File -Recurse)
    $ExtractedPackage = Join-Path $Extracted "Minion Rush Installer"
    $ExtractedFiles = @(Get-ChildItem -LiteralPath $ExtractedPackage -File -Recurse)
    if ($OriginalFiles.Count -ne $ExtractedFiles.Count) { throw "Archive file count differs." }
    foreach ($Original in $OriginalFiles) {
        $Relative = [System.IO.Path]::GetRelativePath($Package, $Original.FullName)
        $Copy = Join-Path $ExtractedPackage $Relative
        if (-not (Test-Path -LiteralPath $Copy -PathType Leaf) -or
            (Get-FileHash -LiteralPath $Original.FullName).Hash -ne (Get-FileHash -LiteralPath $Copy).Hash) {
            throw "Archive content differs: $Relative"
        }
    }
    $Hash = (Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLowerInvariant()
    "$Hash  $(Split-Path $Archive -Leaf)`n" | Set-Content -NoNewline -Encoding ascii -Path "$Archive.sha256"
    Write-Output "DONE: $Archive (unsigned; package checks passed; physical USB installation not tested)"
} finally {
    Remove-Item -LiteralPath $Stage -Recurse -Force
}
