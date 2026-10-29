@echo off
setlocal
if "%~1"=="" (
  python "%~dp0summarize-runtime-log.py" "%LOCALAPPDATA%\MojoRecomp\logs\cot\runtime.log"
) else (
  python "%~dp0summarize-runtime-log.py" %*
)
exit /b %errorlevel%
