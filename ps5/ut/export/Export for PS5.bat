@echo off
rem TSword PS5 export: your UltraTears game folder made into the TSword folder the PS5 plays from.
rem   "Export for PS5.bat" [UltraTears game folder] [PS5 folder, e.g. E:\TSword]
setlocal
set "GAME=%~1"
set "OUT=%~2"
if "%GAME%"=="" set /p "GAME=Your UltraTears game folder (where UltraTears.exe and data\ are): "
if "%OUT%"=="" set /p "OUT=Where to write the PS5 folder (e.g. E:\TSword on a USB drive): "
if exist "%GAME%\deps\python\python.exe" (
    "%GAME%\deps\python\python.exe" "%~dp0tools\build_ps5.py" --game "%GAME%" --out "%OUT%"
) else (
    py -3 "%~dp0tools\build_ps5.py" --game "%GAME%" --out "%OUT%"
)
if errorlevel 1 (echo. & echo The export stopped: the lines above say why.) else (echo. & echo Done: put %OUT% on the PS5 as TSword\ on a USB drive or extended storage.)
pause
