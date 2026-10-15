# Compila e roda as demos SDL2 de hw/ (janela e audio).
# Requer o SDL2 devel MinGW extraido em C:\libs\SDL2-2.32.2 (ou passe -Sdl).
#   .\run_demo.ps1                 janela (setas, Z fundo, X nota PSG, Esc fecha)
#   .\run_demo.ps1 -Demo audio     toca ~2 s de onda PCM + pulso PSG
#   .\run_demo.ps1 -Demo minimize -Seconds 60   minimiza/restaura 5x e confere underruns
# -Frames N (janela) ou -Seconds S (audio) para sair sozinho.
param([string]$Sdl = "C:\libs\SDL2-2.32.2\x86_64-w64-mingw32", [string]$Demo = "window",
      [int]$Frames = 0, [double]$Seconds = 2.0)
$root = Resolve-Path "$PSScriptRoot\..\.."
$gcc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter gcc.exe -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $gcc) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
$out = Join-Path $root "build\hw"
New-Item -ItemType Directory -Force $out | Out-Null
$common = @("-std=c11", "-O1", "-Wall", "-Wextra", "-I$Sdl\include\SDL2")
$srcs = @("$root\hw\hw.c", "$root\hw\audio_sdl.c")
if ($Demo -eq "ui") {
    # teste headless (driver dummy): ini, soltar arquivo, progresso, icone, titulo, tela cheia
    & $gcc @common -o "$out\test_launcher_ui.exe" "$root\hw\config_ini.c" "$root\hw\window_sdl.c" "$root\hw\tests\test_launcher_ui.c" "-L$Sdl\lib" -lSDL2
    $exe = "$out\test_launcher_ui.exe"; $arg = $out
} elseif ($Demo -eq "minimize") {
    & $gcc @common -o "$out\test_minimize.exe" @srcs "$root\hw\window_sdl.c" "$root\hw\tests\test_minimize.c" "-L$Sdl\lib" -lSDL2
    $exe = "$out\test_minimize.exe"; $arg = [Math]::Max(12, $Seconds)
} elseif ($Demo -eq "audio") {
    & $gcc @common -o "$out\demo_audio.exe" @srcs "$root\hw\tests\demo_audio.c" "-L$Sdl\lib" -lSDL2
    $exe = "$out\demo_audio.exe"; $arg = $Seconds
} else {
    & $gcc @common -o "$out\demo_window.exe" @srcs "$root\hw\window_sdl.c" "$root\hw\tests\demo_window.c" "-L$Sdl\lib" -lSDL2
    $exe = "$out\demo_window.exe"; $arg = $Frames
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item "$Sdl\bin\SDL2.dll" $out -Force
& $exe $arg
exit $LASTEXITCODE
