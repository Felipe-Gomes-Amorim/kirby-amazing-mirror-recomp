# Compila e roda os testes de hw/ de forma independente de recomp/.
# Saidas em build/hw/ (*.ppm, *.exe).
$root = Resolve-Path "$PSScriptRoot\..\.."
$gcc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter gcc.exe -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $gcc) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
$out = Join-Path $root "build\hw"
New-Item -ItemType Directory -Force $out | Out-Null
$rc = 0
foreach ($t in "test_frame", "test_ppu", "test_audio", "test_psg", "test_m4a_sync") {
    & $gcc -std=c11 -O1 -Wall -Wextra -o "$out\$t.exe" "$root\hw\hw.c" "$root\hw\tests\$t.c"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "== $t"
    & "$out\$t.exe" $out
    if ($LASTEXITCODE -ne 0) { $rc = $LASTEXITCODE }
}
exit $rc
