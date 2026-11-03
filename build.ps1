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
  $common=@('/nologo','/std:c++20','/EHsc','/W4','/permissive-','/utf-8','/MD','/O2',('/I'+$PSScriptRoot+'\src'),('/I'+$PSScriptRoot+'\include'),('/I'+$PSScriptRoot+'\third_party\lua'))
  $ccommon=@($common | Where-Object { $_ -ne '/std:c++20' })
  & $pythonExe @pythonArgs ($PSScriptRoot+'\tools\verify_dependencies.py')
  if($LASTEXITCODE){exit $LASTEXITCODE}
  $luaSources=@(Get-ChildItem -LiteralPath ($PSScriptRoot+'\third_party\lua') -Filter '*.c' | Where-Object { $_.Name -notin @('lua.c','luac.c') })
  $luaCommon=@($common | Where-Object { $_ -ne '/W4' })
  & ($bin+'\cl.exe') @luaCommon '/W0' '/TP' '/c' @($luaSources.FullName)
  if($LASTEXITCODE){exit $LASTEXITCODE}
  $luaObjects=@($luaSources | ForEach-Object { $_.BaseName+'.obj' })
  & ($bin+'\lib.exe') '/nologo' '/OUT:nf_lua.lib' @luaObjects
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common '/c' ($PSScriptRoot+'\src\script_host.cpp')
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\src\json.cpp') ($PSScriptRoot+'\src\manifest.cpp') ($PSScriptRoot+'\tests\package_tests.cpp') '/Fe:nf_package_tests.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common '/c' ($PSScriptRoot+'\src\data_values.cpp') ($PSScriptRoot+'\src\data_model.cpp') ($PSScriptRoot+'\src\data_project.cpp') ($PSScriptRoot+'\src\data_authoring.cpp') ($PSScriptRoot+'\src\data_session.cpp')
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\tests\data_tests.cpp') 'data_values.obj' 'data_model.obj' 'data_project.obj' 'data_authoring.obj' 'data_session.obj' 'json.obj' 'manifest.obj' '/Fe:nf_data_tests.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\src\data_cli.cpp') 'data_values.obj' 'data_model.obj' 'data_project.obj' 'data_authoring.obj' 'data_session.obj' 'json.obj' 'manifest.obj' '/Fe:nf_data.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common '/c' ($PSScriptRoot+'\src\resource_model.cpp') ($PSScriptRoot+'\src\resource_hash.cpp') ($PSScriptRoot+'\src\resource_session.cpp')
  if($LASTEXITCODE){exit $LASTEXITCODE}
  $resourceObjects=@('resource_model.obj','resource_hash.obj','resource_session.obj','data_values.obj','data_model.obj','data_project.obj','data_authoring.obj','data_session.obj','json.obj','manifest.obj','bcrypt.lib')
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\tests\resource_tests.cpp') @resourceObjects '/Fe:nf_resource_tests.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\src\resource_cli.cpp') @resourceObjects '/Fe:nf_resources.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common '/LD' ($PSScriptRoot+'\src\exports.cpp') ($PSScriptRoot+'\src\kernel.cpp') ($PSScriptRoot+'\src\json.cpp') ($PSScriptRoot+'\src\manifest.cpp') '/Fe:NeonForge.Core.dll'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\tests\kernel_tests.cpp') 'NeonForge.Core.lib' '/Fe:nf_kernel_tests.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\tests\typed_tests.cpp') 'NeonForge.Core.lib' '/Fe:nf_typed_tests.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @ccommon '/std:c11' ($PSScriptRoot+'\examples\native\typed_calls.c') 'NeonForge.Core.lib' '/Fe:nf_typed_example.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\tests\event_routes_test.cpp') '/Fe:nf_event_routes_tests.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common '/DNF_BENCHMARK_RELEASE' ($PSScriptRoot+'\benchmarks\runtime_benchmark.cpp') ($PSScriptRoot+'\src\json.cpp') '/Fe:nf_benchmark.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\tests\script_tests.cpp') 'script_host.obj' 'json.obj' 'manifest.obj' 'nf_lua.lib' 'NeonForge.Core.lib' '/Fe:nf_script_tests.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\benchmarks\script_benchmark.cpp') 'script_host.obj' 'json.obj' 'manifest.obj' 'nf_lua.lib' 'NeonForge.Core.lib' '/Fe:nf_script_benchmark.exe'
  if($LASTEXITCODE){exit $LASTEXITCODE}
  & ($bin+'\cl.exe') @common ($PSScriptRoot+'\src\host.cpp') ($PSScriptRoot+'\src\json.cpp') ($PSScriptRoot+'\src\manifest.cpp') 'script_host.obj' 'nf_lua.lib' '/Fe:neonforge.exe'
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
    & .\nf_data_tests.exe;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\data_cli_test.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\data_authoring_test.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\data_store_test.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & .\nf_resource_tests.exe;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\resource_cli_test.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & .\nf_kernel_tests.exe;if($LASTEXITCODE){exit $LASTEXITCODE}
    & .\nf_script_tests.exe;if($LASTEXITCODE){exit $LASTEXITCODE}
    & .\nf_typed_tests.exe;if($LASTEXITCODE){exit $LASTEXITCODE}
    & .\nf_typed_example.exe;if($LASTEXITCODE){exit $LASTEXITCODE}
    & .\nf_event_routes_tests.exe;if($LASTEXITCODE){exit $LASTEXITCODE}
    & .\nf_plugin_abi_test.exe .\nf_example.dll .\nf_consumer.dll .\nf_generated.dll;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\host_integration.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\script_integration.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\script_quotas_test.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\dependency_tests.py');if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\game_audit_test.py');if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\script_benchmark_test.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\tool_tests.py');if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\probe_test.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\benchmark_test.py') $dest;if($LASTEXITCODE){exit $LASTEXITCODE}
    & $pythonExe @pythonArgs ($PSScriptRoot+'\tests\package_sdk_test.py') $dest;exit $LASTEXITCODE
  }
} finally { Pop-Location }
