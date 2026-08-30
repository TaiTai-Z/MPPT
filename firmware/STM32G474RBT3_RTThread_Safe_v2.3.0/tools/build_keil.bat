@echo off
setlocal
chcp 65001 >nul
set "PROJECT=%~dp0..\STM32G474_RTThread.uvprojx"
set "LOG=%~dp0..\Out\keil_build.log"
set "HISTORY=%~dp0..\remade_master.md"
if defined KEIL_UV4 set "UV4=%KEIL_UV4%"
if not defined UV4 if exist "D:\Keil\Keil\UV4\UV4.exe" set "UV4=D:\Keil\Keil\UV4\UV4.exe"
if not defined UV4 if exist "C:\Keil_v5\UV4\UV4.exe" set "UV4=C:\Keil_v5\UV4\UV4.exe"
if not defined UV4 (
  echo ERROR: UV4.exe not found. Set KEIL_UV4.
  exit /b 2
)
if not exist "%~dp0..\Out" mkdir "%~dp0..\Out"
"%UV4%" -r "%PROJECT%" -o "%LOG%"
set "RC=%ERRORLEVEL%"
if exist "%LOG%" type "%LOG%"
>>"%HISTORY%" echo ============================================================
>>"%HISTORY%" echo [ARMCLANG] BUILD date=%DATE:~3,10% time=%TIME% version=G474-RBT3-RTT-SAFE-2.3.0 rc=%RC%
>>"%HISTORY%" echo [ARMCLANG] RAW_LOG=Out/keil_build.log
exit /b %RC%
