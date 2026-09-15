@echo off
setlocal
if exist .pio rmdir /s /q .pio
if exist sdkconfig del /q sdkconfig
for %%F in (sdkconfig.*) do (
  if /I not "%%~nxF"=="sdkconfig.defaults" del /q "%%F"
)
echo Cleaned cached PlatformIO / ESP-IDF configuration.
echo Now use PlatformIO Build, or run: pio run
endlocal
