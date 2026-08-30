@echo off
setlocal
where py >nul 2>nul
if errorlevel 1 (
  echo ERROR: Python launcher not found.
  exit /b 2
)
py -3 "%~dp0..\tests\check_project.py"
exit /b %ERRORLEVEL%
