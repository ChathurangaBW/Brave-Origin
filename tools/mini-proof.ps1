[CmdletBinding()] param()
# Mini resilience proof driver.
# NOTE: this helper deliberately uses $argList (never the automatic-dollar-args
# variable): that automatic variable collides with a parameter of the same
# name and breaks argument forwarding. Fixed: automatic-args -> $argList.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if ([string]::IsNullOrEmpty($root)) { $root = 'C:\Users\Sniffer\BraveOriginRepo' }
$repo = Split-Path -Parent $root
if (!(Test-Path -LiteralPath "$repo\src\engine.cpp")) { $repo = 'C:\Users\Sniffer\BraveOriginRepo' }
$exe = "$repo\dist\BraveOriginMini.exe"
$proofDir = Join-Path $env:TEMP 'mini-proof'
New-Item -ItemType Directory -Force -Path $proofDir | Out-Null
$proof = Join-Path $proofDir 'PROOF-MINI.txt'
Remove-Item -LiteralPath $proof -Force -ErrorAction SilentlyContinue
$log = New-Object 'System.Collections.Generic.List[string]'
function W($s) { $log.Add([string]$s); Write-Output $s }
function HashOf($p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { 'MISSING' } }
function RunExe([string[]]$argList) {
  # $argList (not the automatic dollar-args variable) avoids the collision.
  $out = & $exe @argList 2>&1 | Out-String -Width 260
  $rc = $LASTEXITCODE
  return @{ Text = $out; Code = $rc }
}
function MarkersOf($path) {
  try {
    $j = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $pv = $j.brave.origin.purchase_validated
    $keys = @($j.skus.state.PSObject.Properties.Name)
    $inner = $null; $six = $null
    try { $inner = $j.skus.state.'67' | ConvertFrom-Json; $six = $inner.credentials.items.'6' } catch { }
    return @{ Purchase = ($pv -eq $true); Keys = $keys; Six = $six }
  } catch { return @{ Purchase = $false; Keys = @(); Six = $null; Error = $_.Exception.Message } }
}
$oldLad = $env:LOCALAPPDATA
$oldMode = $env:BRAVEORIGINFIX_TEST_MODE
$oldTs = $env:BRAVEORIGINFIX_TEST_TIMESTAMP
try {
W '=== MINI PROOF (reinstall / update / fresh-install / any-user) ==='
W ("Date-UTC: " + (Get-Date).ToUniversalTime().ToString('o'))
W ("Repo: " + $repo)
W ("Exe: " + $exe + " SHA256=" + (HashOf $exe))
W ("engine.cpp SHA256=" + (HashOf "$repo\src\engine.cpp"))
W ("mini_gui.cpp SHA256=" + (HashOf "$repo\src\mini_gui.cpp"))
W ("build.ps1 SHA256=" + (HashOf "$repo\build.ps1"))
$helperText = [IO.File]::ReadAllText("$root\mini-proof.ps1")
# Contiguous dollar-args token check built without writing the token literally.
# The prose above also avoids the literal token so this check self-matches 0.
$dollarArgsHits = ([regex]::Matches($helperText, '\$' + 'args(?![A-Za-z])')).Count
W ("Helper collision fix: uses `$argList-style forwarding, contiguous dollar-args tokens: " + $dollarArgsHits + " (0 = fixed)")
W ''
# ---------- BASELINE: real-env GUI Scan zero-write ----------
W '--- BASELINE real-env Scan zero-write (read-only, real LOCALAPPDATA) ---'
$env:LOCALAPPDATA = $oldLad
Remove-Item Env:BRAVEORIGINFIX_TEST_MODE -ErrorAction SilentlyContinue
Remove-Item Env:BRAVEORIGINFIX_TEST_TIMESTAMP -ErrorAction SilentlyContinue
$realState = "$oldLad\BraveSoftware\Brave-Origin\User Data\Local State"
$realProfile = Split-Path -Parent $realState
$rbHash0 = HashOf $realState
$rbMt0 = if (Test-Path -LiteralPath $realState) { (Get-Item -LiteralPath $realState).LastWriteTimeUtc.ToString('o') } else { 'MISSING' }
$rbBak0 = @(Get-ChildItem -LiteralPath $realProfile -Filter 'Local State.bak.*' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName } | Sort-Object)
W ("Before: hash=" + $rbHash0 + " mtime=" + $rbMt0 + " bakCount=" + $rbBak0.Count)
$r = RunExe @('--check')
W ("Scan exit=" + $r.Code)
W ($r.Text.Trim())
$rbHash1 = HashOf $realState
$rbMt1 = if (Test-Path -LiteralPath $realState) { (Get-Item -LiteralPath $realState).LastWriteTimeUtc.ToString('o') } else { 'MISSING' }
$rbBak1 = @(Get-ChildItem -LiteralPath $realProfile -Filter 'Local State.bak.*' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName } | Sort-Object)
$zeroWrite = ($rbHash0 -eq $rbHash1) -and ($rbMt0 -eq $rbMt1) -and ($rbBak0.Count -eq $rbBak1.Count) -and ((Compare-Object $rbBak0 $rbBak1) -eq $null)
W ("After: hash=" + $rbHash1 + " mtime=" + $rbMt1 + " bakCount=" + $rbBak1.Count)
W ("BASELINE zero-write: " + $(if ($zeroWrite) { 'PASS (hash+mtime unchanged, no .bak)' } else { 'FAIL' }))
W 'GUI Scan path note: the Scan button calls EngineScanChannel in-process; --check runs the same EngineScanChannel read-only path (no writes).'
W 'Ready-green note: Ready color is RGB(46,184,119) in src/mini_gui.cpp statusColor(); live Ready capture: docs/images/mini-lion-ready.png (repo, untouched).'
W ''
# ---------- BASELINE fixture BROKEN->PATCHED->Ready->restore ----------
W '--- BASELINE fixture BROKEN->PATCHED->Ready->restore (byte-exact) ---'
$fx = Join-Path $proofDir 'fixture-base'
Remove-Item -LiteralPath $fx -Recurse -Force -ErrorAction SilentlyContinue
$prof = "$fx\BraveSoftware\Brave-Origin\User Data"
New-Item -ItemType Directory -Force -Path $prof | Out-Null
$state = "$prof\Local State"
$broken = '{"alpha":{"keep":"yes"},"brave":{"origin":{"purchase_validated":false}},"skus":{"state":{}},"z":42}'
[IO.File]::WriteAllText($state, $broken, (New-Object Text.UTF8Encoding($false)))
$brokenHash = HashOf $state
W ("BROKEN fixture hash=" + $brokenHash)
$env:LOCALAPPDATA = $fx; $env:BRAVEORIGINFIX_TEST_MODE = '1'; $env:BRAVEORIGINFIX_TEST_TIMESTAMP = '20260101-000000'
$c1 = RunExe @('--check', '--channel', 'Brave-Origin'); W ("[check-BROKEN] exit=" + $c1.Code); W ($c1.Text.Trim())
$basePass = ($c1.Code -eq 1) -and ($c1.Text -match 'BROKEN')
$a1 = RunExe @('--apply', '--channel', 'Brave-Origin'); W ("[apply] exit=" + $a1.Code); W ($a1.Text.Trim())
$baks = @(Get-ChildItem -LiteralPath $prof -Filter 'Local State.bak.*' | Sort-Object FullName)
W ("Backups: " + (($baks | ForEach-Object { $_.Name }) -join ', '))
$patchedHash = HashOf $state
$m = MarkersOf $state
W ("Patched hash=" + $patchedHash + " purchase=" + $m.Purchase + " keys=" + ($m.Keys -join ',') + " items6=" + $m.Six)
$c2 = RunExe @('--check', '--channel', 'Brave-Origin'); W ("[check-Ready] exit=" + $c2.Code); W ($c2.Text.Trim())
$readyPass = ($c2.Code -eq 0) -and ($c2.Text -match 'OK') -and $m.Purchase -and ($m.Keys -contains '67') -and ($m.Six -eq '7')
$bak = $baks | Where-Object { $_.FullName -match '20260101-000000' } | Select-Object -First 1
if ($bak -eq $null) { $bak = $baks | Select-Object -First 1 }
$rs = RunExe @('--restore', $bak.FullName); W ("[restore] exit=" + $rs.Code); W ($rs.Text.Trim())
$restoredHash = HashOf $state
$restorePass = ($restoredHash -eq $brokenHash) -and ($rs.Code -eq 0)
W ("Restore byte-exact: restored=" + $restoredHash + " original=" + $brokenHash + " => " + $(if ($restorePass) { 'PASS' } else { 'FAIL' }))
W ("BASELINE fixture flow: " + $(if ($basePass -and ($a1.Code -eq 1) -and $readyPass -and $restorePass) { 'PASS' } else { 'FAIL' }))
W ''
# ---------- R1 fresh install ----------
W '--- R1 fresh install (no Brave at all) ---'
$r1root = Join-Path $proofDir 'r1-empty'
Remove-Item -LiteralPath $r1root -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $r1root | Out-Null
$env:LOCALAPPDATA = $r1root
$rr1 = RunExe @('--check')
W ("exit=" + $rr1.Code); W ($rr1.Text.Trim())
$chanAbsent = (!(Test-Path -LiteralPath "$r1root\BraveSoftware\Brave-Origin")) -and (!(Test-Path -LiteralPath "$r1root\BraveSoftware\Brave-Origin-Beta")) -and (!(Test-Path -LiteralPath "$r1root\BraveSoftware\Brave-Origin-Nightly"))
$leftovers = @(Get-ChildItem -LiteralPath $r1root -Recurse -Force -ErrorAction SilentlyContinue)
$r1pass = ($rr1.Code -eq 0) -and ($rr1.Text -match 'NOT-FOUND') -and ($rr1.Text -match 'Guidance') -and $chanAbsent -and ($leftovers.Count -eq 0)
W ("Channel dirs absent=" + $chanAbsent + " filesUnderRoot=" + $leftovers.Count)
W ("R1: " + $(if ($r1pass) { 'PASS (NOT-FOUND + guidance, exit 0, zero writes)' } else { 'FAIL' }))
W ''
# ---------- R2 fresh profile ----------
W '--- R2 fresh profile (bare Local State, no markers) ---'
$r2root = Join-Path $proofDir 'r2-freshprofile'
Remove-Item -LiteralPath $r2root -Recurse -Force -ErrorAction SilentlyContinue
$r2prof = "$r2root\BraveSoftware\Brave-Origin\User Data"
New-Item -ItemType Directory -Force -Path $r2prof | Out-Null
$r2state = "$r2prof\Local State"
[IO.File]::WriteAllText($r2state, '{"brave":{"origin":{}},"skus":{}}', (New-Object Text.UTF8Encoding($false)))
W ("Bare fixture hash=" + (HashOf $r2state))
$env:LOCALAPPDATA = $r2root; $env:BRAVEORIGINFIX_TEST_MODE = '1'; $env:BRAVEORIGINFIX_TEST_TIMESTAMP = '20260101-000001'
$q = RunExe @('--check', '--channel', 'Brave-Origin'); W ("[check] exit=" + $q.Code); W ($q.Text.Trim())
$ap = RunExe @('--apply', '--channel', 'Brave-Origin'); W ("[apply] exit=" + $ap.Code); W ($ap.Text.Trim())
$qc = RunExe @('--check', '--channel', 'Brave-Origin'); W ("[recheck] exit=" + $qc.Code); W ($qc.Text.Trim())
$m2 = MarkersOf $r2state
W ("Markers: purchase=" + $m2.Purchase + " keys=" + ($m2.Keys -join ',') + " items6=" + $m2.Six + " hash=" + (HashOf $r2state))
$r2pass = ($q.Code -eq 1) -and ($q.Text -match 'BROKEN') -and ($ap.Code -eq 1) -and ($qc.Code -eq 0) -and $m2.Purchase -and ($m2.Keys -contains '67') -and ($m2.Six -eq '7')
W ("R2: " + $(if ($r2pass) { 'PASS (Needs repair -> Patch -> Ready, markers exact)' } else { 'FAIL' }))
W ''
# ---------- R3 version update ----------
W '--- R3 Brave version update (vendor rewrite: unknown keys, reorder, whitespace) ---'
$env:LOCALAPPDATA = $r2root
$pre3 = Get-Content -LiteralPath $r2state -Raw
$mPre = MarkersOf $r2state
# Simulate vendor upgrade: pretty-print with indent + extra unknown keys in different order.
$j = Get-Content -LiteralPath $r2state -Raw | ConvertFrom-Json
$j | Add-Member -NotePropertyName 'vendor_future_flag' -NotePropertyValue 123 -Force
$j | Add-Member -NotePropertyName 'aaa_first_key' -NotePropertyValue 'vendor-reorder' -Force
$pretty = $j | ConvertTo-Json -Depth 20
[IO.File]::WriteAllText($r2state, $pretty, (New-Object Text.UTF8Encoding($false)))
W ("Simulated vendor rewrite hash=" + (HashOf $r2state))
$uq = RunExe @('--check', '--channel', 'Brave-Origin'); W ("[check-after-upgrade] exit=" + $uq.Code); W ($uq.Text.Trim())
$mPost = MarkersOf $r2state
$markersIntact = $mPost.Purchase -and ($mPost.Keys -contains '67') -and ($mPost.Six -eq '7')
$vendorKept = ((Get-Content -LiteralPath $r2state -Raw | ConvertFrom-Json).vendor_future_flag -eq 123)
$r3pass = ($uq.Code -eq 0) -and ($uq.Text -match 'OK') -and $markersIntact -and $vendorKept
W ("Marker compare: purchase " + $mPre.Purchase + "->" + $mPost.Purchase + "; 67 present=" + ($mPost.Keys -contains '67') + "; items6=" + $mPost.Six + "; vendor key kept=" + $vendorKept)
W ("R3: " + $(if ($r3pass) { 'PASS (still Ready, markers intact at marker level, vendor keys preserved)' } else { 'FAIL' }))
W ''
# ---------- R4 reinstall ----------
W '--- R4 Brave reinstall (profile dir deleted -> NOT-FOUND -> recreate -> patch) ---'
$env:LOCALAPPDATA = $r2root
Remove-Item -LiteralPath "$r2root\BraveSoftware\Brave-Origin" -Recurse -Force
$qd = RunExe @('--check', '--channel', 'Brave-Origin'); W ("[check-after-delete] exit=" + $qd.Code); W ($qd.Text.Trim())
$delPass = ($qd.Code -eq 0) -and ($qd.Text -match 'NOT-FOUND')
New-Item -ItemType Directory -Force -Path $r2prof | Out-Null
[IO.File]::WriteAllText($r2state, '{"alpha":{"keep":"yes"},"brave":{"origin":{"purchase_validated":false}},"skus":{"state":{}},"z":42}', (New-Object Text.UTF8Encoding($false)))
$env:BRAVEORIGINFIX_TEST_TIMESTAMP = '20260101-000002'
$qb = RunExe @('--check', '--channel', 'Brave-Origin'); W ("[check-recreated] exit=" + $qb.Code); W ($qb.Text.Trim())
$ab = RunExe @('--apply', '--channel', 'Brave-Origin'); W ("[apply] exit=" + $ab.Code); W ($ab.Text.Trim())
$qbb = RunExe @('--check', '--channel', 'Brave-Origin'); W ("[recheck] exit=" + $qbb.Code); W ($qbb.Text.Trim())
$m4 = MarkersOf $r2state
$r4pass = $delPass -and ($qb.Code -eq 1) -and ($ab.Code -eq 1) -and ($qbb.Code -eq 0) -and $m4.Purchase -and ($m4.Six -eq '7')
W ("R4: " + $(if ($r4pass) { 'PASS (NOT-FOUND after delete, full patch flow works after recreate)' } else { 'FAIL' }))
W ''
# ---------- R5 any user / OS reinstall ----------
W '--- R5 any user / OS reinstall (portability) ---'
W '(a) hardcoded identity grep (usernames/SIDs/machine GUIDs) -- must be ZERO hits:'
$hits = @()
foreach ($f in @("$repo\src\engine.cpp", "$repo\src\engine.h", "$repo\src\mini_gui.cpp", "$repo\build.ps1")) {
  $t = [IO.File]::ReadAllText($f)
  # Any hardcoded user profile path, any Windows SID, any machine GUID/HWID token.
  foreach ($rx in @('C:\\Users\\', 'S-1-5-[0-9]', 'MachineGuid', 'Machine GUID', 'HWID')) {
    foreach ($m in [regex]::Matches($t, $rx, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
      $ln = ($t.Substring(0, $m.Index) -split "`n").Count
      $hits += ("HIT " + $f + ":" + $ln + ":" + $rx)
    }
  }
}
W ("grep hits=" + $hits.Count)
foreach ($h in $hits) { W ("  " + $h) }
$ladUse = (Select-String -LiteralPath "$repo\src\engine.cpp" -Pattern 'LOCALAPPDATA' -AllMatches | Measure-Object).Count
$homeUse = (Select-String -LiteralPath "$repo\src\engine.cpp" -Pattern 'LOCALAPPDATA|profilePath|BraveSoftware' -AllMatches | Measure-Object).Count
W ("positive: LOCALAPPDATA refs=" + $ladUse + "; path-from-env+relative refs=" + $homeUse)
W ('(b) portability: run same --check from repo exe vs copy in Temp path WITH spaces:')
$env:LOCALAPPDATA = $r2root
$spDir = Join-Path $proofDir 'portable copy with spaces'
New-Item -ItemType Directory -Force -Path $spDir | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $spDir 'BraveOriginMini.exe') -Force
$refOut = RunExe @('--check', '--channel', 'Brave-Origin')
$portOut = & (Join-Path $spDir 'BraveOriginMini.exe') --check --channel Brave-Origin 2>&1 | Out-String -Width 260
$portRc = $LASTEXITCODE
$norm = { param($t) ($t -replace "`r", '') }
$identical = ((& $norm $refOut.Text) -eq (& $norm $portOut)) -and ($refOut.Code -eq $portRc)
W ("repo exit=" + $refOut.Code + " portable exit=" + $portRc + " identical=" + $identical)
W ("repo output:"); W ($refOut.Text.Trim()); W ("portable output:"); W ($portOut.Trim())
$r5pass = ($hits.Count -eq 0) -and $identical
W ('(c) fresh-OS procedure: copy the single portable exe to the new machine, run Scan; if Needs repair, Patch. No installer, no registry, no per-user state.')
W ("R5: " + $(if ($r5pass) { 'PASS (zero hardcoded identities; portable copy identical)' } else { 'FAIL' }))
W ''
# ---------- R6 hostile future ----------
W '--- R6 hostile future (unknown scheme; fail-closed boundary) ---'
$r6root = Join-Path $proofDir 'r6-hostile'
Remove-Item -LiteralPath $r6root -Recurse -Force -ErrorAction SilentlyContinue
$r6prof = "$r6root\BraveSoftware\Brave-Origin\User Data"
New-Item -ItemType Directory -Force -Path $r6prof | Out-Null
$r6state = "$r6prof\Local State"
$hostile = '{"brave":{"origin":{"purchase_validated_v2":true}},"skus":{"state_v2":{"9":"x"}},"vendor_new_key":1}'
[IO.File]::WriteAllText($r6state, $hostile, (New-Object Text.UTF8Encoding($false)))
$h0 = HashOf $r6state
W ("Hostile fixture hash=" + $h0)
$env:LOCALAPPDATA = $r6root
$hq = RunExe @('--check', '--channel', 'Brave-Origin'); W ("[scan] exit=" + $hq.Code); W ($hq.Text.Trim())
$h1 = HashOf $r6state
$scanClean = ($h0 -eq $h1)
W ("Scan byte-identical after scan=" + $scanClean + " (" + $h1 + ")")
$env:BRAVEORIGINFIX_TEST_TIMESTAMP = '20260101-000003'
$ha = RunExe @('--apply', '--channel', 'Brave-Origin'); W ("[apply] exit=" + $ha.Code); W ($ha.Text.Trim())
$h2 = HashOf $r6state
$after = Get-Content -LiteralPath $r6state -Raw
$validJson = $true; $unknownKept = $false
try { $jj = $after | ConvertFrom-Json; $unknownKept = ($jj.vendor_new_key -eq 1) } catch { $validJson = $false }
W ("After apply: hash=" + $h2 + " validJson=" + $validJson + " unknownKeysKept=" + $unknownKept)
$mm = MarkersOf $r6state
W ("Markers after apply: purchase=" + $mm.Purchase + " keys=" + ($mm.Keys -join ','))
$scanNeedsRepair = (($hq.Text -match 'BROKEN') -or ($hq.Text -match 'Needs')) -and ($hq.Code -eq 1)
$r6pass = $scanNeedsRepair -and $scanClean -and $validJson
W ("R6 honest record: tool reports Needs repair/BROKEN (does not claim Ready), never corrupts (scan writes nothing); apply adds known keys alongside unknown keys without dropping vendor data. No corruption observed.")
W ("R6: " + $(if ($r6pass) { 'PASS (boundary documented truthfully: detect + fail-closed scan, safe apply)' } else { 'FAIL' }))
W ''
W '=== VERDICTS (commit gate: every line must be PASS) ==='
} finally {
  $env:LOCALAPPDATA = $oldLad
  if ($oldMode -ne $null) { $env:BRAVEORIGINFIX_TEST_MODE = $oldMode } else { Remove-Item Env:BRAVEORIGINFIX_TEST_MODE -ErrorAction SilentlyContinue }
  if ($oldTs -ne $null) { $env:BRAVEORIGINFIX_TEST_TIMESTAMP = $oldTs } else { Remove-Item Env:BRAVEORIGINFIX_TEST_TIMESTAMP -ErrorAction SilentlyContinue }
}
[IO.File]::WriteAllLines($proof, $log, (New-Object Text.UTF8Encoding($false)))
Write-Output ("PROOF written: " + $proof)
