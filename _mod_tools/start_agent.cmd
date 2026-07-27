@echo off
REM Keep this running while you use the mod catalogue in the hangar.
REM
REM The buttons cannot install anything by themselves - the client's UI
REM scripting has no file or network access - so they leave a request in the
REM client log and this window carries it out. With no agent running the
REM buttons still light up and still write the request, and nothing happens.
title BlitzForge agent
cd /d "%~dp0"
python agent.py
pause
