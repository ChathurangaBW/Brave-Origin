[CmdletBinding()] param()
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$gxx = 'C:\msys64\mingw64\bin\g++.exe'
$windres = 'C:\msys64\mingw64\bin\windres.exe'
if (!(Test-Path -LiteralPath $gxx)) { throw "Compiler not found: $gxx" }
New-Item -ItemType Directory -Force -Path "$root\dist", "$root\build" | Out-Null
$log = "$root\build.log"
Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$root\build\app.o","$root\dist\BraveOriginFix.exe","$root\dist\BraveOriginFix-cli.exe" -Force -ErrorAction SilentlyContinue
$ErrorActionPreference = 'Continue' # MinGW diagnostics arrive on stderr; exit codes remain authoritative.
"Compiler provenance:" | Set-Content -LiteralPath $log -Encoding UTF8
& $gxx --version 2>&1 | Tee-Object -FilePath $log -Append
& 'C:\msys64\mingw64\bin\objdump.exe' --version 2>&1 | Tee-Object -FilePath $log -Append
"COMMAND: windres -i resources/app.rc -o build/app.o -O coff" | Tee-Object -FilePath $log -Append
& $windres -i "$root\resources\app.rc" -o "$root\build\app.o" -O coff 2>&1 | Tee-Object -FilePath $log -Append
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$common = @('-std=c++17','-O2','-Wall','-Wextra','-static','-static-libgcc','-static-libstdc++','-Wl,--no-insert-timestamp','-Isrc','src/engine.cpp','build/app.o','-lole32','-loleaut32','-lshell32')
"COMMAND: g++ $($common -join ' ') src/gui.cpp -municode -mwindows -Wl,--subsystem,windows -o dist/BraveOriginFix.exe -lcomctl32 -lcomdlg32" | Tee-Object -FilePath $log -Append
& $gxx @common 'src/gui.cpp' '-municode' '-mwindows' '-Wl,--subsystem,windows' '-o' 'dist/BraveOriginFix.exe' '-lcomctl32' '-lcomdlg32' '-lshell32' 2>&1 | Tee-Object -FilePath $log -Append
if ($LASTEXITCODE) { exit $LASTEXITCODE }
"COMMAND: g++ $($common -join ' ') src/cli_main.cpp -o dist/BraveOriginFix-cli.exe" | Tee-Object -FilePath $log -Append
& $gxx @common 'src/cli_main.cpp' '-o' 'dist/BraveOriginFix-cli.exe' 2>&1 | Tee-Object -FilePath $log -Append
exit $LASTEXITCODE
