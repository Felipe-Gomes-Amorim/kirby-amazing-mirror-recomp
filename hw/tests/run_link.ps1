# Builds and runs the cable-link (SIO multi-play) tests: 2-4 independent hw instances, no game, no SDL.
# hw.c is compiled once per instance with -DHW_PREFIX=iN_ so the four instances share no state.
# Output in build\hw\.
$root = Resolve-Path "$PSScriptRoot\..\.."
$gcc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter gcc.exe -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $gcc) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
$out = Join-Path $root "build\hw"
New-Item -ItemType Directory -Force $out | Out-Null
$flags = @("-std=c11", "-O1", "-Wall", "-Wextra")
$objs = @()
foreach ($i in 0..3) {
    $o = "$out\link_i$i.o"
    & $gcc @flags "-DHW_PREFIX=i${i}_" -c -o $o "$root\hw\hw.c"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $objs += $o
}
& $gcc @flags -o "$out\test_link.exe" "$root\hw\tests\test_link.c" "$root\hw\link_harness.c" @objs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "== test_link"
& "$out\test_link.exe"
exit $LASTEXITCODE
