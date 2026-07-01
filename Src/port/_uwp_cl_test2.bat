@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" amd64 uwp
if errorlevel 1 exit /b 1
echo === CL TEST ===
echo int main(){return 0;} > %TEMP%\uwp_test2.cpp
cl.exe /nologo /DWINAPI_FAMILY=WINAPI_FAMILY_APP /EHsc %TEMP%\uwp_test2.cpp /link /APPCONTAINER WindowsApp.lib /OUT:%TEMP%\uwp_test2.exe
if errorlevel 1 (echo COMPILE FAILED) else (echo COMPILE SUCCESS)
