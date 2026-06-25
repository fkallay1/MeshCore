@echo off
rem Spusti push_otapkg.py (vyber .otapkg.json -> adb push do telefonu).
rem Double-click; netreba zadavat cesty.
cd /d "%~dp0"

rem 1) Znamy PlatformIO python (ctypes staci, tkinter netreba).
set "PYEXE=D:\FkDev\.platformio\python3\python.exe"
if exist "%PYEXE%" (
    "%PYEXE%" "%~dp0push_otapkg.py"
    goto :done
)

rem 2) py launcher, 3) python z PATH.
where py >nul 2>nul
if %errorlevel%==0 (
    py "%~dp0push_otapkg.py"
) else (
    python "%~dp0push_otapkg.py"
)

:done
if %errorlevel% neq 0 pause
