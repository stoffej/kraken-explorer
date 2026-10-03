@echo off
rem Removes what install.bat added. Settings (%APPDATA%\kraken-explorer) and the log cache
rem (%LOCALAPPDATA%\kraken-explorer) are kept.
setlocal
set "DEST=%LOCALAPPDATA%\Programs\Kraken Explorer"
del "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Kraken Explorer.lnk" 2>nul
cd /d "%TEMP%"
rem Last line: this file deletes itself when run from the install folder.
rmdir /s /q "%DEST%" & echo Kraken Explorer removed. & if not defined KRAKEN_INSTALL_QUIET pause
