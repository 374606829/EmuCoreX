@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul
cd /d "%~dp0"

rem --- JAVA_HOME: must be JDK root, not ...\bin ---
if defined JAVA_HOME (
  set "_JH=!JAVA_HOME!"
  if /i "!_JH:~-5!"=="\bin\" set "JAVA_HOME=!_JH:~0,-5!"
  if /i "!_JH:~-4!"=="\bin" set "JAVA_HOME=!_JH:~0,-4!"
  set "_JH="
)

set "SDK_DIR=%USERPROFILE%\AppData\Local\Android\Sdk"
if not exist "%SDK_DIR%" (
  echo Android SDK not found: "%SDK_DIR%"
  exit /b 1
)
set "ANDROID_HOME=%SDK_DIR%"
set "ANDROID_SDK_ROOT=%SDK_DIR%"
set "SDK_DIR_ESCAPED=%SDK_DIR:\=\\%"
echo sdk.dir=%SDK_DIR_ESCAPED%> local.properties

if not defined JAVA_HOME (
  for /f "delims=" %%J in ('dir /b /ad "C:\Program Files\Eclipse Adoptium\jdk-21*" 2^>nul ^| sort /R') do (
    if not defined JAVA_HOME if exist "C:\Program Files\Eclipse Adoptium\%%J\bin\java.exe" set "JAVA_HOME=C:\Program Files\Eclipse Adoptium\%%J"
  )
)
if not defined JAVA_HOME (
  for /f "delims=" %%J in ('dir /b /ad "C:\Program Files\Java" 2^>nul ^| sort /R') do (
    if not defined JAVA_HOME if exist "C:\Program Files\Java\%%J\bin\java.exe" set "JAVA_HOME=C:\Program Files\Java\%%J"
  )
)
if not defined JAVA_HOME (
  echo WARNING: JAVA_HOME not set. EmuCoreX Gradle daemon expects JDK 21 ^(see gradle\gradle-daemon-jvm.properties^).
)

rem --- Same signing layout as ARMSX2 build_signed_release.bat ---
set "KS_PROP=app\emucorex_keystore.properties"
set "KEYSTORE_PATH=mykey.keystore"
set "ALIAS_NAME=key"
set "STORE_PASS=wyzz666"
set "KEY_PASS=wyzz666"

if exist "%KEYSTORE_PATH%" (
  > "%KS_PROP%" (
    echo storeFile=../%KEYSTORE_PATH%
    echo storePassword=%STORE_PASS%
    echo keyAlias=%ALIAS_NAME%
    echo keyPassword=%KEY_PASS%
  )
  echo Using keystore: "%KEYSTORE_PATH%" alias: "%ALIAS_NAME%"
) else (
  echo WARNING: "%KEYSTORE_PATH%" not found. Release will be unsigned unless you add the keystore at repo root.
)

set "APP_ID=com.sbro.emucorex"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=release"

if /i "%MODE%"=="debug" goto :do_debug
if /i "%MODE%"=="release" goto :do_release
if /i "%MODE%"=="all" goto :do_all

echo Usage: %~nx0 [debug ^| release ^| all]
echo   debug   - assembleDebug
echo   release - signed assembleRelease ^(default^), same flow as ARMSX2 build_signed_release.bat
echo   all     - debug then signed release
exit /b 1

:do_debug
echo === EmuCoreX: assembleDebug ===
call gradlew.bat :app:assembleDebug --no-daemon --stacktrace
set "EC=!ERRORLEVEL!"
if not "!EC!"=="0" (
  echo [ERROR] Debug build failed with code !EC!
  exit /b !EC!
)
echo.
echo Debug APK: %CD%\app\build\outputs\apk\debug\app-debug.apk
goto :eof

:do_release
call :run_sdkmanager_checks
echo Using virtual drive to bypass Unicode path issues...
set "VIRTUAL_DRIVE=V:"
if exist "%VIRTUAL_DRIVE%\" subst "%VIRTUAL_DRIVE%" /d
subst "%VIRTUAL_DRIVE%" "%CD%"
pushd "%VIRTUAL_DRIVE%\"

echo === EmuCoreX: assembleRelease ^(signed when keystore present^) ===
call gradlew.bat :app:assembleRelease --no-daemon --stacktrace
set "GRADLE_EXIT_CODE=!ERRORLEVEL!"

popd
subst "%VIRTUAL_DRIVE%" /d

if not "!GRADLE_EXIT_CODE!"=="0" (
  echo [ERROR] Release build failed with code !GRADLE_EXIT_CODE!
  exit /b !GRADLE_EXIT_CODE!
)

set "APK_DIR=app\build\outputs\apk\release"
echo Release APKs in %APK_DIR%:
dir /b "%APK_DIR%" 2>nul
powershell -NoProfile -ExecutionPolicy Bypass -Command "$p=''; try{$p=(Get-Content '%CD%\%APK_DIR%\\output-metadata.json' -ErrorAction Stop | ConvertFrom-Json).elements[0].outputFile}catch{}; if([string]::IsNullOrWhiteSpace($p)){ $p=$null }; if($p){ Set-Content -Path '%CD%\apk_name.txt' -Value $p }"
if not exist "apk_name.txt" (
  dir /b "%APK_DIR%\*.apk" > "apk_name.txt" 2>nul
)
set "APK_ANY="
if exist "apk_name.txt" (
  for /f "usebackq delims=" %%P in ("apk_name.txt") do set "APK_ANY=%%P" & goto :got_apk_name
)
if exist "apk_name.txt" del /f /q "apk_name.txt" >nul 2>&1
:got_apk_name
if not defined APK_ANY (
  for %%F in ("%APK_DIR%\*.apk") do (
    set "APK_ANY=%%~nxF"
    goto :got_apk_name2
  )
)
:got_apk_name2
set "APK_IN=%CD%\%APK_DIR%\%APK_ANY%"
if defined APK_ANY (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$p=$env:APK_ANY; if(-not [string]::IsNullOrWhiteSpace($p)){ $full=Join-Path (Convert-Path '%CD%\%APK_DIR%') $p; Set-Content -Path '%CD%\apk_in_path.txt' -Value $full }" 2>nul
  if exist "apk_in_path.txt" (
    for /f "usebackq delims=" %%I in ("apk_in_path.txt") do set "APK_IN=%%I"
    del /f /q "apk_in_path.txt" >nul 2>&1
  )
)
if exist "apk_name.txt" del /f /q "apk_name.txt" >nul 2>&1

if defined APK_ANY (
  set "BT_PATH="
  for /f "delims=" %%V in ('dir /b /ad "%ANDROID_HOME%\build-tools" 2^>nul ^| sort /R') do (
    if not defined BT_PATH set "BT_PATH=%ANDROID_HOME%\build-tools\%%V"
  )
  echo Using APK_IN=!APK_IN!
  if not defined BT_PATH (
    echo build-tools not found; assuming Gradle produced a signed APK
    set "APK_SIGNED=!APK_IN!"
  ) else if not exist "!BT_PATH!\apksigner.bat" (
    echo apksigner not available; assuming Gradle produced a signed APK
    set "APK_SIGNED=!APK_IN!"
  ) else (
    echo Verifying current APK signature...
    call "!BT_PATH!\apksigner.bat" verify --verbose "!APK_IN!" >nul 2>&1
    if errorlevel 1 (
      if not exist "%KEYSTORE_PATH%" (
        echo APK unsigned and no "%KEYSTORE_PATH%" - cannot sign with apksigner.
        set "APK_SIGNED=!APK_IN!"
      ) else (
        echo APK is unsigned, proceeding to zipalign and sign...
        set "APK_ALIGNED=!APK_IN:.apk=-aligned.apk!"
        set "APK_SIGNED=%CD%\%APK_DIR%\signed-!APK_ANY!"
        call "!BT_PATH!\zipalign.exe" -p -f 4 "!APK_IN!" "!APK_ALIGNED!"
        if errorlevel 1 (
          echo zipalign failed, signing original APK instead...
          set "APK_ALIGNED=!APK_IN!"
        )
        call "!BT_PATH!\apksigner.bat" sign --ks "%KEYSTORE_PATH%" --ks-key-alias "%ALIAS_NAME%" --ks-pass pass:"%STORE_PASS%" --key-pass pass:"%KEY_PASS%" --out "!APK_SIGNED!" "!APK_ALIGNED!"
        if errorlevel 1 exit /b !errorlevel!
        call "!BT_PATH!\apksigner.bat" verify --verbose "!APK_SIGNED!"
        if errorlevel 1 exit /b !errorlevel!
        echo Signed APK: !APK_SIGNED!
        if exist "!APK_IN!" del /f /q "!APK_IN!"
        if exist "!APK_ALIGNED!" if /i not "!APK_ALIGNED!"=="!APK_IN!" del /f /q "!APK_ALIGNED!"
      )
    ) else (
      echo APK already signed: !APK_IN!
      set "APK_SIGNED=!APK_IN!"
    )
  )
  if not defined APK_SIGNED if defined APK_ANY set "APK_SIGNED=!APK_IN!"
  echo Remaining APKs:
  dir /b "%APK_DIR%"
  set "ADB_EXE=%ANDROID_HOME%\platform-tools\adb.exe"
  if not exist "%ADB_EXE%" if exist "E:\Android\adb.exe" set "ADB_EXE=E:\Android\adb.exe"
  if exist "%ADB_EXE%" (
    echo Installing to device via adb...
    "%ADB_EXE%" uninstall %APP_ID% >nul 2>&1
    "%ADB_EXE%" install -r "!APK_SIGNED!"
    echo Verifying package and launcher entry...
    "%ADB_EXE%" shell pm path %APP_ID%
    "%ADB_EXE%" shell cmd package resolve-activity -a android.intent.action.MAIN -c android.intent.category.LAUNCHER %APP_ID%
    if exist "textures" (
      echo Syncing local textures to device app files...
      "%ADB_EXE%" shell mkdir -p "/sdcard/Android/data/%APP_ID%/files"
      "%ADB_EXE%" shell rm -rf "/sdcard/Android/data/%APP_ID%/files/textures"
      "%ADB_EXE%" push "textures" "/sdcard/Android/data/%APP_ID%/files/textures"
      echo Device textures listing:
      "%ADB_EXE%" shell ls -la "/sdcard/Android/data/%APP_ID%/files/textures"
    )
  )
) else (
  echo [WARN] No APK found under %APK_DIR%
)
exit /b 0

:do_all
call :do_debug
if errorlevel 1 exit /b !ERRORLEVEL!
echo.
call :do_release
exit /b !ERRORLEVEL!

:run_sdkmanager_checks
set "SDKMANAGER="
if exist "%ANDROID_HOME%\cmdline-tools\latest\bin\sdkmanager.bat" set "SDKMANAGER=%ANDROID_HOME%\cmdline-tools\latest\bin\sdkmanager.bat"
if not defined SDKMANAGER if exist "%ANDROID_HOME%\cmdline-tools\bin\sdkmanager.bat" set "SDKMANAGER=%ANDROID_HOME%\cmdline-tools\bin\sdkmanager.bat"
if not defined SDKMANAGER if exist "%ANDROID_HOME%\tools\bin\sdkmanager.bat" set "SDKMANAGER=%ANDROID_HOME%\tools\bin\sdkmanager.bat"
if defined SDKMANAGER (
  echo Ensuring required Android SDK components are installed...
  echo y | "%SDKMANAGER%" --licenses >nul 2>&1
  for /f "usebackq delims=" %%S in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$m=Select-String -Path 'app\\build.gradle.kts' -Pattern 'compileSdk\\s*=\\s*(\\d+)'; if($m){$m.Matches[0].Groups[1].Value}else{'36'}"`) do set "COMPILE_SDK=%%S"
  if not defined COMPILE_SDK set "COMPILE_SDK=36"
  echo Detected compileSdk=!COMPILE_SDK!
  echo y | "%SDKMANAGER%" "platforms;android-!COMPILE_SDK!" "platform-tools" >nul 2>&1
  echo y | "%SDKMANAGER%" "ndk;29.0.14206865" "cmake;3.22.1" >nul 2>&1
  set "BT_REQ=!COMPILE_SDK!.0.0"
  set "BT_PATH="
  for /f "delims=" %%V in ('dir /b /ad "%ANDROID_HOME%\build-tools" 2^>nul ^| sort /R') do (
    if not defined BT_PATH set "BT_PATH=%ANDROID_HOME%\build-tools\%%V"
  )
  if not defined BT_PATH (
    echo y | "%SDKMANAGER%" "build-tools;!BT_REQ!" >nul 2>&1
  )
) else (
  echo sdkmanager not found under %ANDROID_HOME%. Skipping auto-install checks.
)
exit /b 0
