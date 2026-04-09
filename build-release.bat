@echo off
setlocal

set "ROOT_DIR=%~dp0"
if "%ROOT_DIR:~-1%"=="\" set "ROOT_DIR=%ROOT_DIR:~0,-1%"
set "BUILD_DIR=%ROOT_DIR%\build"

if not exist "%ROOT_DIR%\luau\CMakeLists.txt" (
    echo [0/3] Submodulos nao encontrados, inicializando...
    git -C "%ROOT_DIR%" submodule update --init --recursive
    if errorlevel 1 goto :error
)

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

echo [1/3] Configurando CMake em Release...
cmake -S "%ROOT_DIR%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :error

echo [2/3] Buildando Luau.LanguageServer.CLI em Release...
cmake --build "%BUILD_DIR%" --config Release --target Luau.LanguageServer.CLI
if errorlevel 1 goto :error

echo [3/3] Finalizado.
echo.
echo Build concluido com sucesso.
echo Binario: "%BUILD_DIR%\Release\luau-lsp.exe"
exit /b 0

:error
echo.
echo Falha no build.
echo Se aparecer LNK1104 em luau-lsp.exe, feche o VSCode e processos luau-lsp.exe e rode novamente.
exit /b 1
