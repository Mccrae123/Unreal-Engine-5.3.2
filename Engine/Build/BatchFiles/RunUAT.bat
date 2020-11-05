@echo off

rem ## Unreal Engine 4 AutomationTool setup script
rem ## Copyright Epic Games, Inc. All Rights Reserved.
rem ##
rem ## This script is expecting to exist in the UE4/Engine/Build/BatchFiles directory.  It will not work correctly
rem ## if you copy it to a different location and run it.

setlocal 
echo Running AutomationTool...

set UATExecutable=AutomationToolLauncher.exe
set UATDirectory=Binaries\DotNET\
set UATCompileArg=-compile

if "%UE_USE_DOTNET%" == "1" (
	REM We do not use the automaton tool launcher for dotnet builds
	set UATExecutable=AutomationTool.exe
	set UATDirectory=Binaries\DotNET\AutomationTool
	REM -compile is not supported with netcore
	set UATCompileArg=
)


rem ## Change the CWD to /Engine. 
pushd "%~dp0..\..\"
if not exist Build\BatchFiles\RunUAT.bat goto Error_BatchFileInWrongLocation

rem ## Use the pre-compiled UAT scripts if -nocompile is specified in the command line
for %%P in (%*) do if /I "%%P" == "-nocompile" goto RunPrecompiled

rem ## If we're running in an installed build, default to precompiled
if exist Build\InstalledBuild.txt goto RunPrecompiled

rem ## check for force precompiled
if not "%ForcePrecompiledUAT%"=="" goto RunPrecompiled

rem ## check if the UAT projects are present. if not, we'll just use the precompiled ones.
if not exist Source\Programs\AutomationTool\AutomationTool.csproj goto RunPrecompiled
if not exist Source\Programs\AutomationToolLauncher\AutomationToolLauncher.csproj goto RunPrecompiled

rem ## Get the path to MSBuild
if "%UE_USE_DOTNET%" == "1" (
	goto BuildNetCore
)

:BuildNetFW
call "%~dp0GetMSBuildPath.bat"
if errorlevel 1 goto RunPrecompiled
%MSBUILD_EXE% /nologo /verbosity:quiet Source\Programs\AutomationToolLauncher\AutomationToolLauncher.csproj /property:Configuration=Development /property:Platform=AnyCPU
if errorlevel 1 goto Error_UATCompileFailed
%MSBUILD_EXE% /nologo /verbosity:quiet Source\Programs\AutomationTool\AutomationTool.csproj /property:Configuration=Development /property:Platform=AnyCPU /property:AutomationToolProjectOnly=true
if errorlevel 1 goto Error_UATCompileFailed
goto DoRunUAT

:BuildNetCore
rem ## Verify that dotnet is present
call "%~dp0GetDotnetPath.bat"
if errorlevel 1 goto Error_NoDotnetSDK

echo Building AutomationTool...
dotnet build Source\Programs\AutomationTool\AutomationToolCore.csproj -c Development -v quiet 
if errorlevel 1 goto Error_UATCompileFailed
goto DoRunUAT

rem ## ok, well it doesn't look like visual studio is installed, let's try running the precompiled one.
:RunPrecompiled

set UATCompileArg=
if "%UE_USE_DOTNET%" == "1" (
	if not exist Binaries\DotNET\AutomationTool\AutomationTool.exe goto Error_NoFallbackExecutable
) else (
	if not exist Binaries\DotNET\AutomationTool.exe goto Error_NoFallbackExecutable
	if not exist Binaries\DotNET\AutomationToolLauncher.exe set UATExecutable=AutomationTool.exe
)
goto DoRunUAT




rem ## Run AutomationTool
:DoRunUAT
pushd %UATDirectory%
%UATExecutable% %* %UATCompileArg%
popd

for %%P in (%*) do if /I "%%P" == "-noturnkeyvariables" goto SkipTurnkey

rem ## Turnkey needs to update env vars in the calling process so that if it is run multiple times the Sdk env var changes are in effect
if EXIST ~dp0..\..\Intermediate\Turnkey\PostTurnkeyVariables.bat (
	rem ## We need to endlocal so that the vars in the batch file work. NOTE: Working directory from pushd will be UNDONE here, but since we are about to quit, it's okay
	endlocal 
	echo Updating environment variables set by a Turnkey sub-process
	call ~dp0..\..\Engine\Intermediate\Turnkey\PostTurnkeyVariables.bat
	del ~dp0..\..\Engine\Intermediate\Turnkey\PostTurnkeyVariables.bat
	rem ## setlocal again so that any popd's etc don't have an effect on calling process
	setlocal
)
:SkipTurnkey

if not %ERRORLEVEL% == 0 goto Error_UATFailed

rem ## Success!
goto Exit


:Error_BatchFileInWrongLocation
echo RunUAT.bat ERROR: The batch file does not appear to be located in the /Engine/Build/BatchFiles directory.  This script must be run from within that directory.
set RUNUAT_EXITCODE=1
goto Exit_Failure

:Error_NoVisualStudioEnvironment
echo RunUAT.bat ERROR: A valid version of Visual Studio 2015 does not appear to be installed.
set RUNUAT_EXITCODE=1
goto Exit_Failure

:Error_NoDotnetSDK
echo RunUAT.bat ERROR: Unable to find a install of Dotnet SDK.  Please make sure you have it installed and that `dotnet` is a globally available command.
set RUNUAT_EXITCODE=1
goto Exit_Failure

:Error_NoFallbackExecutable
echo RunUAT.bat ERROR: Visual studio and/or AutomationTool.csproj was not found, nor was Engine\Binaries\DotNET\AutomationTool.exe. Can't run the automation tool.
set RUNUAT_EXITCODE=1
goto Exit_Failure

:Error_UATCompileFailed
echo RunUAT.bat ERROR: AutomationTool failed to compile.
set RUNUAT_EXITCODE=1
goto Exit_Failure


:Error_UATFailed
set RUNUAT_EXITCODE=%ERRORLEVEL%
goto Exit_Failure

:Exit_Failure
echo BUILD FAILED
popd
exit /B %RUNUAT_EXITCODE%

:Exit
rem ## Restore original CWD in case we change it
popd
exit /B 0


