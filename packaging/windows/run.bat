@echo off
rem ===========================================================================
rem  Digimon World 4 - Recomp @VERSION@  (Windows x86-64, EXPERIMENTAL)
rem
rem  Edite as opcoes abaixo e de dois cliques neste arquivo.
rem  Edit the options below and double-click this file.
rem ===========================================================================

rem API grafica / graphics API: vulkan (padrao / default) ou opengl
set DW4_RENDERER=vulkan

rem Altura da imagem / image height: 480 (original), 720, 1080, 1440, 2160
set DW4_RES=1080

rem Proporcao / aspect ratio: 4:3 (original), 16:9, 21:9
set ASPECT_W=16
set ASPECT_H=9

rem Quadros por segundo / frames per second: 30 (como no Xbox / as on the Xbox)
rem ou 60 (o jogo anda duas vezes mais rapido / the game runs twice as fast)
set DW4_FPS_LIMIT=30

rem Tela cheia / fullscreen: 0 ou 1.  Vsync: 0 ou 1.
set FULLSCREEN=0
set VSYNC=1

rem Teclado como controle da porta 1 / keyboard as the port 1 pad: 0 ou 1
set RECOMP_KEYBOARD=1

rem ===========================================================================
rem  Daqui para baixo nao precisa mexer. / Nothing to edit below.
rem ===========================================================================
setlocal
cd /d "%~dp0"

if not exist "game_data\" mkdir game_data
set FOUND=
for %%F in (game_data\default.xbe game_data\Default.xbe) do if exist "%%F" set FOUND=1
if not defined FOUND (
    echo.
    echo  Faltam os arquivos do jogo. Copie o conteudo do SEU disco de
    echo  Digimon World 4 ^(Xbox, NTSC-U^) para a pasta game_data ao lado deste arquivo:
    echo  default.xbe, area.afs, ndmw.afs, ndmw.igb
    echo.
    echo  The game files are missing. Copy the contents of YOUR OWN Digimon World 4
    echo  disc ^(Xbox, NTSC-U^) into the game_data folder next to this file.
    echo.
    pause
    exit /b 1
)

set DW4_ASPECT=%ASPECT_W%:%ASPECT_H%
set /a WIN_H=DW4_RES
if %DW4_RES% LSS 1080 set /a WIN_H=DW4_RES*2
set /a WIN_W=WIN_H*ASPECT_W/ASPECT_H
set RECOMP_WINDOW_SIZE=%WIN_W%x%WIN_H%
if "%FULLSCREEN%"=="1" set RECOMP_FULLSCREEN=1
if "%VSYNC%"=="0" set RECOMP_PRESENT_VSYNC=0
set DW4_VK_STATS=1
set DW4_GL_STATS=1

if not exist logs\ mkdir logs
set TS=
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss" 2^>nul') do set TS=%%i
if not defined TS set TS=%RANDOM%
set LOG=logs\run-%TS%.log

echo Config: @VERSION@ windows ^| renderer=%DW4_RENDERER% res=%DW4_RES% aspect=%DW4_ASPECT% fps=%DW4_FPS_LIMIT% fullscreen=%FULLSCREEN% vsync=%VSYNC% keyboard=%RECOMP_KEYBOARD% > "%LOG%"
echo Digimon World 4 - Recomp @VERSION@ -- log: %LOG%
echo Feche a janela do jogo para sair. / Close the game window to quit.
dw4_recomp.exe game_data >> "%LOG%" 2>&1
set RC=%ERRORLEVEL%
echo [RUN] codigo de saida / exit code: %RC% >> "%LOG%"
if not "%RC%"=="0" (
    echo.
    echo  O jogo fechou com erro ^(codigo %RC%^). O log esta em %LOG%
    echo  The game exited with an error ^(code %RC%^). The log is at %LOG%
    echo.
    pause
)
exit /b %RC%
