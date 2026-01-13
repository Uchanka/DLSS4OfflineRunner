@echo off
setlocal enabledelayedexpansion

cd _bin
echo This script completes all batch-runs of a scene.
set EXE_PATH=StreamlineSample.exe

rem Check for xxhash Python module
call python -c "import xxhash" 2>nul
if %errorlevel% neq 0 (
    echo ERROR: xxhash Python module not found!
    echo Please install it with: pip install xxhash, then rerun this script.
    pause
    exit /b 1
)

rem Set default values first
rem IMPORTANT NOTE: Batch script executes commands in Windows command prompt which can't recognize "/" as path separator.
set DisplayResolution=4
rem set AlignFilename=
set AlignFilename=-AlignFilename
set INPUT_ROOT=..\media\EmptySanityCheck60\Vertical
set OUTPUT_ROOT=%INPUT_ROOT%\screenshots

rem set "Has_Ref=" if you DON'T want to copy references in NPP_GT to OUTPUT_ROOT (for comparison)
set "Has_Ref=y"

rem Override with command-line arguments if provided
if not "%~1"=="" set DisplayResolution=%~1
if not "%~2"=="" set INPUT_ROOT=%~2
if not "%~3"=="" set OUTPUT_ROOT=%~3

set INPUT_COUNT=0
for %%x in (%INPUT_ROOT%\NPP_JI\*.exr) do (
    set /a INPUT_COUNT+=1
)
rem 15 is the batch size, batch count is ceiling division
set /a BATCH_COUNT=(%INPUT_COUNT% + 14) / 15
set /a END_INDEX=BATCH_COUNT-1

rem Let user press y/n to confirm the command and total runs.
echo Using parameters:
echo DisplayResolution: %DisplayResolution%
echo Input path: %INPUT_ROOT%
echo Output path: %OUTPUT_ROOT%
echo Please confirm that "BATCH_COUNT * 15 >= INPUT_COUNT"
echo INPUT_COUNT: %INPUT_COUNT%, BATCH_COUNT: %BATCH_COUNT%

set "FIXED_ARGS=-EnableHack -DisplayResolution %DisplayResolution% -ParseJitter -HackPaths "%INPUT_ROOT%" -StoreOutput %AlignFilename% -OutputPath "%OUTPUT_ROOT%""
echo Command to run: %EXE_PATH% %FIXED_ARGS% -BatchIndex [0 to %END_INDEX%] 

choice /c YN /m "Run with these parameters? Double check OUTPUT_ROOT=%OUTPUT_ROOT% is what you normally pass to last arg -OutputPath."
if %errorlevel% equ 2 (
    echo Aborted by user
    exit /b 0
)

for /L %%i in (0, 1, %END_INDEX%) do (
    echo ===== Batch Index %%i =====
    
    rem Execute the program
    %EXE_PATH% %FIXED_ARGS% -BatchIndex %%i
)

rem Confirm total numbers first
set /a EXPECTED_COUNT=INPUT_COUNT*2
if defined Has_Ref (
    set /a EXPECTED_COUNT=EXPECTED_COUNT+INPUT_COUNT
)
set OUTPUT_COUNT=0
for %%x in (%OUTPUT_ROOT%\*.exr) do (
    set /a OUTPUT_COUNT+=1
)
if %OUTPUT_COUNT% neq %EXPECTED_COUNT% (
    echo ERROR: OUTPUT_COUNT %OUTPUT_COUNT% less than expected %EXPECTED_COUNT%!
)

rem Then go ahead to call python duplicate check helper.
cd ../media
set PY_SCRIPT=check_duplicate.py
set RUN_FOLDER=%OUTPUT_ROOT%

echo.
echo ===== Checking for duplicate frames in !RUN_FOLDER! =====

call python %PY_SCRIPT% "!RUN_FOLDER!"
if %errorlevel% neq 0 (
    echo ERROR: Duplicates found!
    exit /b 1
)

echo.
echo ===== One scene completed =====
endlocal