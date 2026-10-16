# Builds kirby_ui.dll (+ import library) and runs the config and launcher-UI tests headless.
# Needs the SDL2 MinGW devel package (default C:\libs\SDL2-2.32.2). Output in build\hw\.
param([string]$Sdl = "C:\libs\SDL2-2.32.2\x86_64-w64-mingw32", [switch]$Shot)
$root = Resolve-Path "$PSScriptRoot\..\.."
$gcc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter gcc.exe -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $gcc) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
$out = Join-Path $root "build\hw"
New-Item -ItemType Directory -Force $out | Out-Null
$flags = @("-std=c11", "-O1", "-Wall", "-Wextra", "-Wno-missing-field-initializers", "-I$Sdl\include\SDL2")
$rc = 0

# 1) kirby.ini v2 (no SDL)
& $gcc @flags -o "$out\test_config_ini.exe" "$root\hw\config_ini.c" "$root\hw\tests\test_config_ini.c"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "== test_config_ini"
& "$out\test_config_ini.exe" $out
if ($LASTEXITCODE -ne 0) { $rc = $LASTEXITCODE }

# 2) kirby_ui.dll + import lib
& $gcc @flags -DKL_BUILD_DLL -shared -o "$out\kirby_ui.dll" "-Wl,--out-implib,$out\libkirby_ui.dll.a" `
    "$root\hw\launcher_ui.c" "$root\hw\config_ini.c" "$root\hw\window_sdl.c" "-L$Sdl\lib" -lSDL2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item "$Sdl\bin\SDL2.dll" $out -Force

# 3) UI test linked against the DLL (checks the exported symbols)
& $gcc @flags -o "$out\test_kirby_ui.exe" "$root\hw\tests\test_kirby_ui.c" "$root\hw\config_ini.c" `
    "-L$out" -lkirby_ui "-L$Sdl\lib" -lSDL2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "== test_kirby_ui (kirby_ui.dll)"
& "$out\test_kirby_ui.exe" $out
if ($LASTEXITCODE -ne 0) { $rc = $LASTEXITCODE }
# 4) editable keyboard / controller pages (events carry the window id; controller simulated)
& $gcc @flags -o "$out\test_kirby_ui_edit.exe" "$root\hw\tests\test_kirby_ui_edit.c" "$root\hw\config_ini.c" `
    "-L$out" -lkirby_ui "-L$Sdl\lib" -lSDL2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "== test_kirby_ui_edit (kirby_ui.dll)"
& "$out\test_kirby_ui_edit.exe" $out
if ($LASTEXITCODE -ne 0) { $rc = $LASTEXITCODE }
# 5) SAVE section: return codes, import confirmation, layout checks
& $gcc @flags -o "$out\test_kirby_ui_save.exe" "$root\hw\tests\test_kirby_ui_save.c" "$root\hw\config_ini.c" `
    "-L$out" -lkirby_ui "-L$Sdl\lib" -lSDL2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "== test_kirby_ui_save (kirby_ui.dll)"
& "$out\test_kirby_ui_save.exe" $out
if ($LASTEXITCODE -ne 0) { $rc = $LASTEXITCODE }
exit $rc
