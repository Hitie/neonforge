param([string]$BuildDir='build-direct',[switch]$SkipTests,[string]$Python='')
$ErrorActionPreference='Stop'
$pythonArgs=@()
if($Python){$pythonExe=$Python}else{
  $launcher=Get-Command py.exe -ErrorAction SilentlyContinue
  if(-not $launcher){throw 'Python 3 is required for SDK checks. Pass -Python with its executable path.'}
  $pythonExe=$launcher.Source;$pythonArgs=@('-3')
}
$vsRoot='C:\Program Files\Microsoft Visual Studio\18\Community'
$toolRoot=Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if(-not $toolRoot){throw 'An installed MSVC x64 toolchain is required.'}
$sdkRoot='C:\Program Files (x86)\Windows Kits\10'
$sdkVersion='10.0.26100.0'
$bin=Join-Path $toolRoot.FullName 'bin\Hostx64\x64'
$sdkBin=Join-Path $sdkRoot ('bin\'+$sdkVersion+'\x64')
$env:PATH=$bin+';'+$sdkBin+';'+$env:PATH
$env:INCLUDE=(Join-Path $toolRoot.FullName 'include')+';'+(Join-Path $sdkRoot ('Include\'+$sdkVersion+'\ucrt'))+';'+(Join-Path $sdkRoot ('Include\'+$sdkVersion+'\shared'))+';'+(Join-Path $sdkRoot ('Include\'+$sdkVersion+'\um'))
$env:LIB=(Join-Path $toolRoot.FullName 'lib\x64')+';'+(Join-Path $sdkRoot ('Lib\'+$sdkVersion+'\ucrt\x64'))+';'+(Join-Path $sdkRoot ('Lib\'+$sdkVersion+'\um\x64'))
$dest=Join-Path $PSScriptRoot $BuildDir
New-Item -ItemType Directory -Path $dest -Force | Out-Null
Push-Location $dest
try {
  $common=@('/nologo','/std:c++20','/EHsc','/W4','/permissive-','/utf-8','/MD','/O2',('/I'+$PSScriptRoot+'\src'),('/I'+$PSScriptRoot+'\include'))
  $ccommon=@($common | Where-Object { $_ -ne '/std:c++20' })
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\src\json.cpp') ($PSScriptRoot+'\src\manifest.cpp') ($PSScriptRoot+'\tests\package_tests.cpp') '/Fe:nf_package_tests.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common '/LD' ($PSScriptRoot+'\src\exports.cpp') ($PSScriptRoot+'\src\kernel.cpp') ($PSScriptRoot+'\src\json.cpp') ($PSScriptRoot+'\src\manifest.cpp') '/Fe:NeonForge.Core.dll'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\tests\kernel_tests.cpp') 'NeonForge.Core.lib' '/Fe:nf_kernel_tests.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\tests\event_routes_test.cpp') '/Fe:nf_event_routes_tests.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common '/DNF_BENCHMARK_RELEASE' ($PSScriptRoot+'\benchmarks\runtime_benchmark.cpp') ($PSScriptRoot+'\src\json.cpp') '/Fe:nf_benchmark.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\src\host.cpp') ($PSScriptRoot+'\src\json.cpp') ($PSScriptRoot+'\src\manifest.cpp') '/Fe:neonforge.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @ccommon '/std:c11' '/LD' ($PSScriptRoot+'\examples\native\provider.c') '/Fe:nf_example.dll'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common '/LD' ($PSScriptRoot+'\examples\native\consumer.cpp') '/Fe:nf_consumer.dll'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @ccommon '/std:c11' '/LD' ($PSScriptRoot+'\tests\failing_plugin.c') '/Fe:nf_failing.dll'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common '/LD' ($PSScriptRoot+'\tests\noncancelling_runtime.cpp') '/Fe:nf_noncancelling_runtime.dll'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @ccommon '/std:c11' '/c' ($PSScriptRoot+'\tests\c_header_test.c')
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\src\winmm_probe.cpp') ($PSScriptRoot+'\src\json.cpp') '/Fe:nf_winmm_probe.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  $generated=Join-Path $dest ('generated-'+[guid]::NewGuid().ToString('N'))
  & $pythonExe @pythonArgs ($PSScriptRoot+'\tools\forge.py') new $generated --id org.neonforge.generated --name 'Generated ABI Check' | Out-Null
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @ccommon '/std:c11' '/LD' (Join-Path $generated 'src\plugin.c') '/Fe:nf_generated.dll'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\tests\plugin_abi_test.cpp') '/Fe:nf_plugin_abi_test.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  if(-not $SkipTests){
    & .\nf_package_tests.exe;if($LASTEXITCODE){exit $LASTEXITCODE}
    & .\nf_kernel_tests.exe;if($LASTEXITCODE){exit $LASTEXITCODE}
    & .\nf_event_routes_tests.exe;if($LASTEXITCODE){exit $LASTEXITCODE}
    & .\nf_plugin_abi_test.exe .\nf_example.dll .\nf_consumer.dll .\nf_generated.dll;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\host_integration.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\tool_tests.py');if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\probe_test.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\benchmark_test.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\package_sdk_test.py') $dest;exit $LASTEXITCODE
  }
} finally { Pop-Location }
