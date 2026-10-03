@echo off
rem Per-user install of Kraken Explorer: copies this folder to %LOCALAPPDATA%\Programs and adds a
rem Start menu shortcut. No administrator rights needed. Run again to upgrade.
setlocal
set "DEST=%LOCALAPPDATA%\Programs\Kraken Explorer"
if /i "%~dp0"=="%DEST%\" goto shortcut
robocopy "%~dp0." "%DEST%" /MIR /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
    echo Copy to "%DEST%" failed. Close Kraken Explorer and try again.
    pause
    exit /b 1
)
:shortcut
powershell -NoProfile -Command "$s = (New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Programs') + '\Kraken Explorer.lnk'); $s.TargetPath = $env:DEST + '\kraken-explorer.exe'; $s.WorkingDirectory = $env:DEST; $s.Save()"
echo Installed in "%DEST%". Start it from the Start menu: Kraken Explorer.
if not defined KRAKEN_INSTALL_QUIET pause
