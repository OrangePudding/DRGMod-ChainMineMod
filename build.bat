@echo off
setlocal
set VS2019=D:\Program Files (x86)\Microsoft Visual Studio\2019\Community
call "%VS2019%\VC\Auxiliary\Build\vcvars64.bat" >nul
set ROOT=F:\stuff\DRG\ue4ss-research
set UE4SS_DLL=E:\SteamLibrary\steamapps\common\Deep Rock Galactic\FSD\Binaries\Win64\ue4ss\UE4SS.dll

cd /d %ROOT%\ChainMineMod

echo === generating ue4ss.lib ===
"%VS2019%\VC\Tools\MSVC\14.29.30133\bin\Hostx64\x64\dumpbin.exe" /exports "%UE4SS_DLL%" > exports_raw.txt
python make_def.py
lib /machine:x64 /def:ue4ss.def /out:ue4ss.lib

echo === compiling ===
set INC=/I "%ROOT%\RE-UE4SS\UE4SS\include"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\Unreal\include"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\Unreal\generated_include"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\Unreal\include\Unreal"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\Unreal\include\Unreal\Core"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\Function\include"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\DynamicOutput\include"
set INC=%INC% /I "%ROOT%\thirdparty\zydis\include"
set INC=%INC% /I "%ROOT%\thirdparty\zydis\dependencies\zycore\include"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\File\include"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\Constructs\include"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\Common\include"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\Helpers\include"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\ASMHelper\include"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\SinglePassSigScanner\include"
set INC=%INC% /I "%ROOT%\RE-UE4SS\deps\first\IniParser\include"

cl /nologo /LD /EHa /std:c++20 /O2 /MD /D UNICODE /D _UNICODE /D UE_BUILD_SHIPPING=1 /D UBT_COMPILED_PLATFORM=Windows /D PLATFORM_WINDOWS=1 %INC% main.cpp /link /OUT:main.dll ue4ss.lib

echo === done ===