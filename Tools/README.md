# Firmware package tool

`package_firmware.py` validates a Slot-A-linked STM32 BIN and creates:

- `<prefix>.manifest.bin`: the bootloader's 152-byte signed manifest;
- `<prefix>.package.zip`: `firmware.bin`, `manifest.bin`, and descriptive
  `package.json`, ready for a programmer/update client to consume.

The application must be built for Slot A (`0x08009000`). Export the raw BIN
from the VEGA ELF, then run from this repository:

```powershell
& "C:\ST\STM32CubeIDE_1.16.1\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.12.3.rel1.win32_1.0.200.202406191623\tools\bin\arm-none-eabi-objcopy.exe" `
  -O binary `
  "C:\Projects\VEGA_CANOPEN_TRANSLATOR_STM32\Debug\VEGA_CANOPEN_TRANSLATOR_STM32G0.elf" `
  "C:\Projects\VEGA_CANOPEN_TRANSLATOR_STM32\Debug\VEGA_CANOPEN_TRANSLATOR_STM32G0.bin"

& "C:\Projects\CANOPEN_Bootloader_STM32G0\.venv\Scripts\python.exe" `
  ".\Tools\package_firmware.py" `
  --image "C:\Projects\VEGA_CANOPEN_TRANSLATOR_STM32\Debug\VEGA_CANOPEN_TRANSLATOR_STM32G0.bin" `
  --output-prefix ".\out\vega-1" `
  --version 0x100 `
  --vendor-id 0x12345678 `
  --product-code 0x00000001 `
  --hardware-variant 0x00000001 `
  --hw-min 0x00010000 `
  --hw-max 0x0001FFFF
```

Replace the example compatibility IDs and hardware-revision limits with the
values provisioned in the target's factory identity. Hardware revisions are
encoded as `(major << 16) | minor`, inclusive. The tool refuses images with an
invalid vector table or larger than the 232 KiB Slot-A payload.
`--version` must match the application's numeric version. The current VEGA
`0.1.0` version is `0x100`: `(major << 16) | (minor << 8) | patch`.
It is NOT encoded like the hardware revision.

The default key is the prototype's Windows-DPAPI-protected OpenSSH private key
in `%LOCALAPPDATA%\CANOPEN_Bootloader\PrototypeSigningKey`. It can only be
decrypted by the Windows account that protected it. Never put a private key in
the repository, an update package, or a command-line argument. The tool checks
that this signing key matches the public-key bytes compiled into
`Bootloader/Inc/firmware_auth_key.h`.

Install the package dependency into the selected Python environment:

```powershell
& "C:\Projects\CANOPEN_Bootloader_STM32G0\.venv\Scripts\python.exe" -m pip install -r requirements.txt
```

This utility creates the manifest and package; it does not program Flash, send
SDO, or modify the bootloader's key-provisioning gate.
`BL_SIGNING_PUBLIC_KEY_PROVISIONED` is now `1`. A raw unsigned application BIN
is not sufficient: first programming must include its signed manifest and a
matching factory record.

The transfer client must write the exact 152-byte `manifest.bin` to `0x2008:00`,
start with `0x2000:00 = 1`, download `firmware.bin` to `0x2002:00`, finish with
`0x2000:00 = 2`, and schedule activation with `0x2000:00 = 3` only after
verification succeeds.
The receiver inactivity timeout and PC request deadline are 15 seconds, allowing
the synchronous signature check and inactive-slot erase before the begin reply.

## First programming / SWD recovery

Rebuild BOTH projects: the earlier 32 KiB bootloader / `0x08008000` application
layout is obsolete. The full signature verifier requires a 36 KiB bootloader;
both slots now reserve 232 KiB for application bytes. Do not mix old binaries,
manifests, discovery records or an existing swap journal with this layout.

Export the current bootloader ELF (Debug or Release) to BIN:

```powershell
$objcopy = "C:\ST\STM32CubeIDE_1.16.1\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.12.3.rel1.win32_1.0.200.202406191623\tools\bin\arm-none-eabi-objcopy.exe"
& $objcopy -O binary ".\Release\CANOPEN_Bootloader_STM32G0.elf" ".\out\bootloader.bin"

& ".\.venv\Scripts\python.exe" ".\Tools\prepare_factory.py" `
  --bootloader ".\out\bootloader.bin" `
  --package ".\out\vega-1.package.zip" `
  --serial 1234 `
  --product-revision 1 `
  --hardware-revision 0x00010003 `
  --output ".\out\initial-device-1234"
```

The identity values above are EXAMPLES, not assigned production identifiers.
Use your actual vendor/product/variant in the package and the actual serial,
product revision and hardware revision here. Hardware revision must fall inside
the manifest's signed compatibility range. The tool verifies signature, image
digest, compatibility, the 32-byte bootloader discovery record at `0x08008FE0`,
and that the compiled bootloader contains the expected public key.
It refuses to overwrite an output directory.

Outputs:

- `initial.hex`: sparse Intel HEX containing bootloader at `0x08000000`,
  image at `0x08009000`, manifest at `0x08043000`, identity at `0x0807E800`;
- `slot-a.bin`: application plus erased padding and its manifest; base
  `0x08009000`, for explicit selective SWD recovery;
- `factory-identity.bin`: 128-byte record; base `0x0807E800`.

For a NEW or deliberately re-commissioned board, use STM32CubeProgrammer:

1. Connect over SWD and confirm STM32G0B1, 512 KiB Flash and the expected bank
   mapping. This software does not change option bytes.
2. On a used device, first back up its unique factory record and journal.
   Factory identity cannot be reconstructed from a generic firmware BIN.
3. For this initial layout installation only, selectively erase
   `0x08000000..0x0807FFFF`, or use full-chip erase ONLY after explicitly
   accepting loss of existing identity and journal and having their replacement.
   The HEX is sparse: merely writing it does NOT clear a stale transaction.
4. Program `initial.hex` with verify. Check that NVM
   `0x0807F000..0x0807FFFF` remains erased. It is intentionally absent from HEX;
   scratch and Slot B are also absent.
5. Reset. The bootloader validates the signed image against factory identity,
   initializes the blank journal, imports validated initial image metadata and
   boots the application normally. Invalid/missing application remains in
   CANopen recovery mode.

For later CAN updates do not erase factory/NVM or reinstall this initial HEX.
For later SWD service preserve identity and reconcile the current transaction
before any selective erase. This is NOT an in-place migration of the old map.
No script here connects to or erases physical hardware.

## CAN update from the PC

`update_firmware.py` is an independent python-can client, not a claim of
LiftAppTool compatibility. Example for a PCAN adapter:

```powershell
& ".\.venv\Scripts\python.exe" ".\Tools\update_firmware.py" `
  --package ".\out\vega-1.package.zip" `
  --interface pcan --channel PCAN_USBBUS1 --node 1 --confirm-update
```

Install the adapter vendor's driver and choose a backend/channel supported by
python-can. The current target uses Classic CAN, 250 kbit/s, Node ID 1.
The tool verifies the package before opening CAN, discovers execution mode and
capability, requests application handoff only when necessary, sends manifest,
begin, segmented image, finish and activate, then checks the responding
application's numeric version. It never automatically retries an uncertain write,
cancels an interrupted update or changes a non-idle receiver state.

Per the current prototype requirement, `0x2000=1` explicitly authorizes the
outage. Application tasks quiesce cooperatively, the actual final SDO TX buffer
must report successful transmission, and only then does the application reset.
There is NO physical service interlock or authenticated CAN command. Use only
on a controlled bench/trusted bus. Firmware signatures authenticate the image,
not the sender of the service command. Normal power-up boots a valid application
without waiting for CAN or a service input.

On timeout/error inspect `0x200A`, `0x2001`, `0x2007` before manually recovering.
For an interrupted RECEIVING/VERIFIED transfer in recovery mode, an informed
operator may cancel with expedited `0x2000:00=0` before starting again. Never
cancel/reprogram during an active swap. Reporting the expected version is not
proof of durable trial confirmation, especially when reinstalling the same
version: verify the journal/self-test and reset/rollback on hardware.
A new handoff is refused while the running application is still an unconfirmed
trial; allow its startup self-test to complete before requesting another update.

## Automated validation

```powershell
& ".\.venv\Scripts\python.exe" -m pip install -r ".\requirements-dev.txt"
& ".\.venv\Scripts\python.exe" -m unittest discover -s ".\Tests" -p "test_*.py" -v
& ".\.venv\Scripts\python.exe" ".\Tests\run_native.py" `
  --application-root "C:\Projects\VEGA_CANOPEN_TRANSLATOR_STM32"
```

The native runner uses Zig's C compiler (no PowerShell execution-policy change),
runs portable C tests, checks Python signing/factory encoding against the real
C decoder and Ed25519 verifier, and optionally runs the application's existing
CANopen/handoff tests. Signature interoperability uses the matching local
DPAPI prototype private key; run it under the Windows account holding that key.
Python tests use temporary generated keys and virtual CAN, not physical hardware.
They cover sparse HEX/checksums, identity, tampering, timeout/abort, and a complete
application-to-bootloader-to-application protocol exchange.
