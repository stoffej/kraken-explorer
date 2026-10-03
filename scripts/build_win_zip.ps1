# Build the Windows zip: kraken-explorer.exe, an embeddable Python next to it (scripts work
# without a Python install), the examples and install.bat / uninstall.bat. With Inno Setup 6
# installed also kraken-explorer-<version>-win64-setup.exe: the same files as one installer.
# Usage (cmake, ninja and MinGW-w64 g++ in PATH, python.org Python installed):
#   powershell -ExecutionPolicy Bypass -File scripts\build_win_zip.ps1 [-Build <dir>]
# CMAKE_ARGS: extra configure flags, e.g. -DKRAKEN_DEPS_DIR=C:\deps (WinLibs' cmake cannot https).
# The user unzips anywhere and runs the exe, or double-clicks install.bat (per-user, no admin).
param([string]$Build = "build\win-release")
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue" # Invoke-WebRequest is 10x slower with the progress bar
$Src = Split-Path $PSScriptRoot -Parent
$Name = "kraken-explorer"
if ((Get-Content "$Src\CMakeLists.txt" -Raw) -notmatch 'project\(kraken_explorer VERSION ([0-9.]+)') { throw "no version in CMakeLists.txt" }
$Version = $Matches[1]

cmake -S $Src -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKRAKEN_TESTS=OFF @(-split $env:CMAKE_ARGS)
if ($LASTEXITCODE) { throw "configure failed" }
cmake --build $Build --target $Name
if ($LASTEXITCODE) { throw "build failed" }

$Stage = "$Build\$Name-$Version-win64"
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item $Stage -ItemType Directory | Out-Null
Copy-Item "$Build\src\$Name.exe" $Stage
strip --strip-unneeded "$Stage\$Name.exe"
Copy-Item "$Src\examples" "$Stage\examples" -Recurse
Copy-Item "$Src\LICENSE", "$Src\packaging\windows\install.bat", "$Src\packaging\windows\uninstall.bat" $Stage

# The embeddable package of the Python the exe was linked against: python3xx.dll, the standard
# library as a zip and a ._pth that keeps it apart from any Python installed on the machine.
$PyExe = (Select-String -Path "$Build\CMakeCache.txt" -Pattern '^_Python_EXECUTABLE:INTERNAL=(.*)$').Matches[0].Groups[1].Value
$PyVer = & $PyExe -c "import platform; print(platform.python_version())"
$PyZip = "$Build\python-$PyVer-embed-amd64.zip"
if (-not (Test-Path $PyZip)) { Invoke-WebRequest "https://www.python.org/ftp/python/$PyVer/python-$PyVer-embed-amd64.zip" -OutFile $PyZip }
Expand-Archive $PyZip $Stage
Rename-Item "$Stage\LICENSE.txt" "LICENSE-python.txt"
Remove-Item "$Stage\python.exe", "$Stage\pythonw.exe", "$Stage\python.cat" -ErrorAction SilentlyContinue

$Zip = "$Stage.zip"
if (Test-Path $Zip) { Remove-Item $Zip }
Compress-Archive $Stage $Zip
Write-Host "built $Zip"

$Iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
if (-not $Iscc) { $Iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" }
if (Test-Path $Iscc) {
    & $Iscc /Qp "/DVersion=$Version" "/DStage=$((Resolve-Path $Stage).Path)" "/DOut=$((Resolve-Path $Build).Path)" "$Src\packaging\windows\kraken-explorer.iss"
    if ($LASTEXITCODE) { throw "installer failed" }
    Write-Host "built $Build\$Name-$Version-win64-setup.exe"
} else {
    Write-Host "Inno Setup not found: no setup.exe (winget install JRSoftware.InnoSetup)"
}
