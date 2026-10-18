param(
    [string]$Compiler = "gcc"
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path ([System.IO.Path]::GetTempPath()) ("bootloader-tests-" + [guid]::NewGuid() + ".exe")
$sources = @(
    (Join-Path $PSScriptRoot "test_bootloader.c"),
    (Join-Path $root "Bootloader\Src\bootloader_interface.c"),
    (Join-Path $root "Bootloader\Src\firmware_auth.c"),
    (Join-Path $root "Bootloader\Src\firmware_update.c"),
    (Join-Path $root "Bootloader\Src\bootloader_transaction.c"),
    (Join-Path $root "Bootloader\Src\firmware_manifest.c"),
    (Join-Path $root "Bootloader\Src\sha256.c"),
    (Join-Path $root "Bootloader\Src\factory_identity.c"),
    (Join-Path $root "Bootloader\Src\persistent_storage.c"),
    (Join-Path $root "Bootloader\Src\canopen.c"),
    (Join-Path $root "Bootloader\Src\canopen_sdo.c"),
    (Join-Path $root "Bootloader\Src\canopen_nmt.c"),
    (Join-Path $root "Bootloader\Src\canopen_heartbeat.c"),
    (Join-Path $root "Bootloader\Src\canopen_emcy.c"),
    (Join-Path $root "Bootloader\Src\object_dictionary.c"),
    (Join-Path $root "ThirdParty\Monocypher\src\monocypher.c"),
    (Join-Path $root "ThirdParty\Monocypher\src\optional\monocypher-ed25519.c")
)
try {
    & $Compiler "-std=c99" "-Wall" "-Wextra" "-Werror" `
        ("-I" + (Join-Path $root "Bootloader\Inc")) `
        ("-I" + (Join-Path $root "Core\Inc")) `
        ("-I" + (Join-Path $root "ThirdParty\Monocypher\src")) `
        ("-I" + (Join-Path $root "ThirdParty\Monocypher\src\optional")) `
        @sources "-o" $output
    if ($LASTEXITCODE -ne 0) { throw "Bootloader native compilation failed: $LASTEXITCODE" }
    & $output
    if ($LASTEXITCODE -ne 0) { throw "Bootloader tests failed: $LASTEXITCODE" }
} finally {
    if (Test-Path -LiteralPath $output) { Remove-Item -LiteralPath $output }
}
