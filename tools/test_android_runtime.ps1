# Native Android regressions, without game files. Builds with the project's NDK and runs through adb.
param([string]$Sdk, [string]$DeviceSerial)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $Sdk) {
    $sdkProperties = Join-Path $repoRoot 'android/botwrecompandroid/local.properties'
    if (Test-Path -LiteralPath $sdkProperties) {
        $sdkLine = Get-Content -LiteralPath $sdkProperties | Where-Object { $_ -match '^sdk\.dir=' } | Select-Object -First 1
        if ($sdkLine) { $Sdk = $sdkLine.Substring(8).Replace('\:', ':').Replace('\\', '\') }
    }
    if (-not $Sdk) { $Sdk = $env:ANDROID_HOME }
    if (-not $Sdk) { $Sdk = Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
}
$compilerPath = Join-Path $Sdk 'ndk/30.0.16248370/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
$adbPath = Join-Path $Sdk 'platform-tools/adb.exe'
$deviceArgs = @()
if ($DeviceSerial) { $deviceArgs = @('-s', $DeviceSerial) }
$deviceDir = '/data/local/tmp/botwrecompandroid-tests'
Push-Location $repoRoot
try {
    New-Item -ItemType Directory -Force 'android/out' | Out-Null
    & $adbPath @deviceArgs shell mkdir -p $deviceDir
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the device test directory.' }
    # (tests including the Vulkan backend need the headers the app build puts in android/deps/out)
    $vulkanTests = @('test_bc_shadow')
    $haveVulkanHeaders = Test-Path -LiteralPath 'android/deps/out/volk/volk.h'
    foreach ($testName in @('test_kernel_affinity', 'test_bc_shadow', 'test_nvn_lifetime')) {
        if (-not $haveVulkanHeaders -and $vulkanTests -contains $testName) { Write-Host "SKIP: $testName (no android/deps/out: build the app once)"; continue }
        & $compilerPath --target=aarch64-linux-android30 -O1 -g -ffunction-sections -fdata-sections '-Wl,--gc-sections' -Wno-format -Iruntime -Iruntime/nvn -Iandroid/deps/out/include -Iandroid/deps/out/volk -DVK_NO_PROTOTYPES "tools/$testName.c" -o "android/out/$testName"
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $testName" }
        & $adbPath @deviceArgs push "android/out/$testName" "$deviceDir/$testName"
        if ($LASTEXITCODE -ne 0) { throw "Upload failed: $testName" }
        & $adbPath @deviceArgs shell chmod 755 "$deviceDir/$testName"
        if ($LASTEXITCODE -ne 0) { throw "Could not make executable: $testName" }
        & $adbPath @deviceArgs shell "$deviceDir/$testName"
        if ($LASTEXITCODE -ne 0) { throw "Regression failed: $testName" }
    }
} finally {
    Pop-Location
}
