# NFCF Dumper build script - zero-dependency manual toolchain (no Gradle)
# javac -> d8 -> aapt2 link -> zip(dex) -> zipalign -> apksigner
# NOTE: do not set $ErrorActionPreference='Stop' here - PS5.1 turns native
# tool stderr warnings (javac deprecation etc.) into NativeCommandError.
# Each step below checks $LASTEXITCODE instead.


$sdk      = "$env:LOCALAPPDATA\Android\Sdk"
$bt       = "$sdk\build-tools\36.0.0"
$platJar  = "$sdk\platforms\android-34\android.jar"
$adb      = "$sdk\platform-tools\adb.exe"
$root     = "$PSScriptRoot"
$src      = "$root\src"
$build    = "$root\build"
$apkOut   = "$root\NFCF-Dumper.apk"

# JAVA_HOME on this machine ends with \bin - strip it for the .bat tools
$jdkBin = $env:JAVA_HOME
if ($jdkBin.EndsWith("\bin")) { $jdkBin = $jdkBin.Substring(0, $jdkBin.Length - 4) }
$javac = "$jdkBin\bin\javac.exe"
$keytool = "$jdkBin\bin\keytool.exe"
$env:JAVA_HOME = $jdkBin

Write-Host "== [1/7] clean =="
Remove-Item -Recurse -Force $build -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path "$build\classes" -Force | Out-Null

Write-Host "== [2/7] javac =="
& $javac -source 8 -target 8 -encoding UTF-8 -bootclasspath $platJar -d "$build\classes" "$src\com\tyh\nfcdump\MainActivity.java" 2>$null
if ($LASTEXITCODE -ne 0) { throw "javac failed" }

Write-Host "== [3/7] d8 (dex) =="
$classFiles = Get-ChildItem "$build\classes\com\tyh\nfcdump" -Filter *.class | ForEach-Object { $_.FullName }
& "$bt\d8.bat" --release --lib $platJar --min-api 24 --output $build @classFiles 2>$build\d8_warn.txt
if (-not (Test-Path "$build\classes.dex")) { throw "d8 failed - see $build\d8_warn.txt" }

Write-Host "== [4/7] aapt2 link =="
& "$bt\aapt2.exe" link -I $platJar --manifest "$src\AndroidManifest.xml" --min-sdk-version 24 --target-sdk-version 34 -o "$build\base.apk" --auto-add-overlay
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed" }

# inject classes.dex into the apk (it is just a zip)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open("$build\base.apk", 'Update')
[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$build\classes.dex", "classes.dex", 'Optimal') | Out-Null
$zip.Dispose()

Write-Host "== [5/7] zipalign =="
& "$bt\zipalign.exe" -f 4 "$build\base.apk" "$build\aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }

Write-Host "== [6/7] sign =="
$ks = "$root\debug.keystore"
if (-not (Test-Path $ks)) {
    & $keytool -genkeypair -keystore $ks -storepass android -keypass android -alias nfcdump -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=NFCF Dumper,O=tyh,C=CN" | Out-Null
}
& "$bt\apksigner.bat" sign --ks $ks --ks-pass pass:android --key-pass pass:android --out $apkOut "$build\aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "apksigner failed" }

Write-Host "== [7/7] done =="
Write-Host "APK: $apkOut"
Write-Host ("size: {0:N0} bytes" -f (Get-Item $apkOut).Length)

# optional: install if a device is connected
& $adb start-server 2>$null | Out-Null
$devices = (& $adb devices) | Select-String "\tdevice$"
if ($devices) {
    Write-Host "device detected, installing..."
    & $adb install -r $apkOut
    if ($LASTEXITCODE -eq 0) { Write-Host "installed OK" }
} else {
    Write-Host "no adb device - manual install: adb install -r $apkOut"
}
