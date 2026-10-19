# Builds and runs the cable-link tests (no SDL, no game):
#   test_link      SIO multi-play between 2-4 independent hw instances (hw.c compiled once per instance with
#                  -DHW_PREFIX=iN_ so the instances share no state)
#   test_link_ini  [link] section of kirby.ini: round trip, validation and PARITY with the runtime's
#                  rt_sio_read_ini (extracted from recomp/runtime/rt_sio.c by extract_rt_ini.py)
# Output in build\hw\.
$root = Resolve-Path "$PSScriptRoot\..\.."
$gcc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter gcc.exe -ErrorAction SilentlyContinue |
       Select-Object -First 1 -ExpandProperty FullName
if (-not $gcc) { $gcc = (Get-Command gcc -ErrorAction Stop).Source }
$out = Join-Path $root "build\hw"
New-Item -ItemType Directory -Force $out | Out-Null
$flags = @("-std=c11", "-O1", "-Wall", "-Wextra")
$rc = 0

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
if ($LASTEXITCODE -ne 0) { $rc = $LASTEXITCODE }

Write-Host "== test_link_ini"
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py) { $py = "$root\.venv\Scripts\python.exe" }
& $py "$root\hw\tests\extract_rt_ini.py" "$root\recomp\runtime\rt_sio.c" "$out\rt_sio_ini_extract.c"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $gcc @flags "-I$out" -o "$out\test_link_ini.exe" "$root\hw\tests\test_link_ini.c" "$root\hw\config_ini.c"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$out\test_link_ini.exe" $out
if ($LASTEXITCODE -ne 0) { $rc = $LASTEXITCODE }
exit $rc
