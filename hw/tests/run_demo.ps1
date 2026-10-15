# Compila e abre a janela SDL2 de demonstracao (hw/tests/demo_window.c).
# Requer o SDL2 devel MinGW extraido em C:\libs\SDL2-2.32.2 (ou passe -Sdl).
# Setas movem o sprite, Z muda o fundo, Esc fecha. Use -Frames N para sair sozinho.
param([string]$Sdl = "C:\libs\SDL2-2.32.2\x86_64-w64-mingw32", [int]$Frames = 0)
$root = Resolve-Path "$PSScriptRoot\..\.."
$gcc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter gcc.exe -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $gcc) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
$out = Join-Path $root "build\hw"
New-Item -ItemType Directory -Force $out | Out-Null
& $gcc -std=c11 -O1 -Wall -Wextra -I"$Sdl\include\SDL2" -o "$out\demo_window.exe" `
    "$root\hw\hw.c" "$root\hw\window_sdl.c" "$root\hw\tests\demo_window.c" -L"$Sdl\lib" -lSDL2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item "$Sdl\bin\SDL2.dll" $out -Force
& "$out\demo_window.exe" $Frames
exit $LASTEXITCODE
