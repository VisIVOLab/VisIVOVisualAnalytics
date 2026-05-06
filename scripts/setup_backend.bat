@echo off
rem Setup script for the VisIVO Visual Analytics backend (Windows).
rem
rem Creates a Python venv at:
rem   %APPDATA%\VisIVO\.venv
rem and installs the backend dependencies into it. The app's BackendLauncher
rem discovers this venv automatically at runtime.
rem
rem Run this once after extracting the .zip. Re-run it after upgrading the
rem app to refresh dependencies.

setlocal EnableExtensions EnableDelayedExpansion

set "VENV_DIR=%APPDATA%\VisIVO\.venv"
set "SCRIPT_DIR=%~dp0"
set "REQ_FILE=%SCRIPT_DIR%requirements.txt"

if not exist "%REQ_FILE%" (
    echo Error: requirements.txt not found at:
    echo   %REQ_FILE%
    echo Make sure this script is in the same folder as the unzipped release.
    pause
    exit /b 1
)

rem Locate Python: try `python` first, then `py -3`.
set "PY_CMD="
where python >nul 2>nul && set "PY_CMD=python"
if not defined PY_CMD (
    where py >nul 2>nul && set "PY_CMD=py -3"
)
if not defined PY_CMD (
    echo Error: Python was not found on PATH.
    echo Install Python 3.11 or newer from https://www.python.org/downloads/
    echo and tick "Add python.exe to PATH" during installation.
    pause
    exit /b 1
)

for /f "delims=" %%v in ('%PY_CMD% -c "import sys; print('%%d.%%d' %% sys.version_info[:2])"') do set "PY_VERSION=%%v"
echo Using Python !PY_VERSION! ^(via "!PY_CMD!"^)
echo Venv target: %VENV_DIR%
echo Requirements: %REQ_FILE%
echo.

if exist "%VENV_DIR%" (
    set /p YN="An existing venv was found. Recreate it from scratch? [y/N] "
    if /i "!YN!"=="Y" (
        echo Removing existing venv...
        rmdir /S /Q "%VENV_DIR%"
    ) else (
        echo Updating existing venv...
    )
)

if not exist "%APPDATA%\VisIVO" mkdir "%APPDATA%\VisIVO"
if not exist "%VENV_DIR%" (
    echo Creating virtual environment...
    %PY_CMD% -m venv "%VENV_DIR%"
    if errorlevel 1 (
        echo Error: failed to create virtual environment.
        pause
        exit /b 1
    )
)

echo.
echo Installing backend dependencies (this can take several minutes)...
"%VENV_DIR%\Scripts\python.exe" -m pip install --upgrade pip
if errorlevel 1 (
    echo Error: pip upgrade failed.
    pause
    exit /b 1
)
"%VENV_DIR%\Scripts\pip.exe" install -r "%REQ_FILE%"
if errorlevel 1 (
    echo Error: dependency installation failed. See messages above.
    pause
    exit /b 1
)

echo.
echo Done. The backend is ready.
echo Launch bin\VisIVOVisualAnalytics.exe normally; it will use this venv.
pause
endlocal
