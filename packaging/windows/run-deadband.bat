@echo off
rem Start Deadband from a source checkout (see docs\windows.md).
rem Expects the virtual environment at .venv in the checkout root:
rem   py -m venv .venv
rem   .venv\Scripts\pip install -r packaging\windows\requirements.txt
setlocal
set "ROOT=%~dp0..\.."
if not exist "%ROOT%\.venv\Scripts\pythonw.exe" (
    echo No .venv found in %ROOT%. Set it up first - see docs\windows.md.
    pause
    exit /b 1
)
rem pythonw: no console window; output goes to %LOCALAPPDATA%\deadband\deadband.log
start "" "%ROOT%\.venv\Scripts\pythonw.exe" "%ROOT%\deadband.py" %*
