@echo off
REM Spusti serial huby pre lokalne dosky, kazdy vo vlastnom okne.
REM Musi sa spustat z prostredia start_VSCode.bat (kvoli PATH/HOME),
REM alebo staci ze python cesta nizsie je absolutna - to je splnene.
REM
REM Hub drzi COM port trvalo a zdiela ho cez TCP na localhost:<port>.
REM Fedor sa pripaja PuTTY -> Raw -> localhost:<port>. DFU si port
REM vypyta sam (~~HUB:PAUSE~~ / ~~HUB:RESUME~~).
REM
REM POZOR: pyserial je LEN v ZephCore\.venv, nie v Tools\python312.

set "PY=D:\FkDev\FkProj\VSC\ZephCore\.venv\Scripts\python.exe"
set "HUB=D:\FkDev\FkProj\VSC\MeshCore\test_nrf-fota\fota_serial_hub.py"
set "WD=D:\FkDev\FkProj\VSC\MeshCore"

start "HUB xiao-nicerf"    /D "%WD%" "%PY%" "%HUB%" --device xiao-nicerf
start "HUB t1000e-local"   /D "%WD%" "%PY%" "%HUB%" --device t1000e-local
start "HUB promicro-local" /D "%WD%" "%PY%" "%HUB%" --device promicro-local

echo Spustene 3 huby:  xiao-nicerf 7421, t1000e-local 7422, promicro-local 7455
