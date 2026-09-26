@echo off
C:\ProgramData\chocolatey\bin\ninja.exe -j1 -C C:\Users\media\source\repos\Vibe\Apotheosis\build-x64-gpu WebCore > C:\Users\media\source\repos\Vibe\Apotheosis\build-x64-gpu\ninja-build.log 2>&1
echo Ninja finished with exit code %ERRORLEVEL%
