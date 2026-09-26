@echo off
setlocal
set MSBUILDCMD="C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\msbuild.exe"
%MSBUILDCMD% "C:\Users\media\source\repos\Vibe\Apotheosis\Src\harness\Harness.vcxproj" /p:Configuration=Release /p:Platform=x64 /p:AppxPackage=true /v:minimal /m:1 /nologo
endlocal
