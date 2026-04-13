@echo off
setlocal

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"
set "BUILD_DIR_LINUX=build-linux-release"

where wsl >nul 2>nul
if errorlevel 1 (
    echo WSL nao encontrado. Instale o WSL para gerar build Linux a partir do Windows.
    exit /b 1
)

for /f "delims=" %%i in ('wsl wslpath -a "%ROOT_DIR%"') do set "ROOT_DIR_WSL=%%i"
if not defined ROOT_DIR_WSL (
    echo Nao foi possivel converter o caminho do projeto para WSL.
    exit /b 1
)

echo [1/3] Verificando submodulos no ambiente Linux...
wsl bash -lc "set -e; cd \"%ROOT_DIR_WSL%\"; if [ ! -f luau/CMakeLists.txt ]; then git submodule update --init --recursive; fi"
if errorlevel 1 goto :error

echo [2/3] Configurando CMake em Release ^(Linux^)...
wsl bash -lc "set -e; cd \"%ROOT_DIR_WSL%\"; cmake -S . -B %BUILD_DIR_LINUX% -DCMAKE_BUILD_TYPE=Release"
if errorlevel 1 goto :error

echo [3/3] Buildando Luau.LanguageServer.CLI em Release ^(Linux^)...
wsl bash -lc "set -e; cd \"%ROOT_DIR_WSL%\"; cmake --build %BUILD_DIR_LINUX% --target Luau.LanguageServer.CLI -j\$(nproc)"
if errorlevel 1 goto :error

echo.
echo Build Linux concluido com sucesso.
echo Binario: "%ROOT_DIR_WSL%/%BUILD_DIR_LINUX%/luau-lsp"
exit /b 0

:error
echo.
echo Falha no build Linux.
exit /b 1
