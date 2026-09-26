@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64 uwp
echo === LIB ===
echo %LIB%
echo === INCLUDE ===
echo %INCLUDE%
echo === PATH (last 5 lines) ===
echo %PATH%
