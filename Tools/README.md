# Firmware package tool

`package_firmware.py` validates a Slot-A-linked STM32 BIN and creates:

- `<prefix>.manifest.bin`: the bootloader's 152-byte signed manifest;
- `<prefix>.package.zip`: `firmware.bin`, `manifest.bin`, and descriptive
  `package.json`, ready for a programmer/update client to consume.
- `<prefix>.update.bin`: the exact CANopen `0x1F50:01` container stream,
  composed of the 152-byte signed manifest followed immediately by the raw
  firmware image.

The application must be built for Slot A (`0x08009000`). Both updated CubeIDE
projects automatically generate a raw BIN after a successful Debug/Release
build. The `objcopy` command below is only needed for manual export from an ELF;
otherwise use the generated BIN and run the packaging command from this repository:

```powershell
& "C:\ST\STM32CubeIDE_1.16.1\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.12.3.rel1.win32_1.0.200.202406191623\tools\bin\arm-none-eabi-objcopy.exe" `
  -O binary `
  "C:\Projects\VEGA_CANOPEN_TRANSLATOR_STM32\Debug\VEGA_CANOPEN_TRANSLATOR_STM32G0.elf" `
  "C:\Projects\VEGA_CANOPEN_TRANSLATOR_STM32\Debug\VEGA_CANOPEN_TRANSLATOR_STM32G0.bin"

& "C:\Projects\CANOPEN_Bootloader_STM32G0\.venv\Scripts\python.exe" `
  ".\Tools\package_firmware.py" `
  --image "C:\Projects\VEGA_CANOPEN_TRANSLATOR_STM32\Debug\VEGA_CANOPEN_TRANSLATOR_STM32G0.bin" `
  --output-prefix ".\out\vega-1" `
  --version 0x101 `
  --vendor-id 0x12345678 `
  --product-code 0x00000001 `
  --hardware-variant 0x00000001 `
  --hw-min 0x00010000 `
  --hw-max 0x0001FFFF
```

Replace the example compatibility IDs and hardware-revision limits with the
values provisioned in the target's factory identity. Hardware revisions are
encoded as `(major << 16) | minor`, inclusive. The tool refuses images with an
invalid vector table or larger than the 104 KiB Slot-A payload.
`--version` must match the application's numeric version. The current VEGA
`0.1.3` version is `0x103`: `(major << 16) | (minor << 8) | patch`.
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
The firmware-update inactivity timeout and PC request deadline are 15 seconds,
allowing the synchronous signature check and inactive-slot erase before the
begin reply. An individual SDO session still times out after one second without
traffic; clients should keep the transfer moving rather than pause between
segments.

Alternatively, a CANopen client that supports segmented or block domain
downloads may send the complete `.update.bin` to `0x1F50:01`. Its declared
transfer length must include the manifest and image. The bootloader validates
the signed manifest, hardware compatibility, and exact combined length before
erasing; after payload CRC/read-back/image checks pass, the update remains
VERIFIED. This path does not activate or reset the device. It is a transport
contract for the bootloader, not a claim of complete LiftAppTool wizard
interoperability. Do not send the ZIP or a bare application BIN to `0x1F50:01`.
Bootloader 1.0.7 accepts the standard data-only block CRC16 or, only for this
container object, the observed LiftAppTool CRC including actual unused final
segment bytes. Clients under our control should continue using standard CRC.
Padding is not written to the image; all signed image checks remain unchanged.

## First programming / SWD recovery

Rebuild BOTH projects for **STM32G0B1RCT6, 256 KiB Flash / 144 KiB RAM**.
The earlier 512 KiB layout (factory at `0x0807E800`) is NOT valid on this MCU.
The bootloader remains 36 KiB, with two 106 KiB slots and **104 KiB payload**
per slot. Slot A stays at `0x08009000`, but metadata, Slot B, the discovery
record contents and swap-state bounds have changed. Do not mix old binaries,
manifests, discovery records or an existing swap journal with this layout.
Use `STM32G0B1RCTX_FLASH.ld`, not the removed VETX linker scripts.

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
The contract's last word accepts signed flags 1 (legacy), 3 (bounded discovery),
or 7 (also non-Flash reset marker, bootloader 1.0.8). Application 0.1.3 requires
the marker capability; it does not fall back to journal writes for entry.
For the current failing-handoff device, selectively program both bootloader
1.0.8 and the signed Slot A recovery image by SWD without mass erase.
Preserve factory/journal/Slot B/scratch; see the current deployment procedure
in `../Bootloader/README.md`, not the historical 1.0.6 staged-upgrade order.
It refuses to overwrite an output directory.

Outputs:

- `initial.hex`: sparse Intel HEX containing bootloader at `0x08000000`,
  image at `0x08009000`, manifest at `0x08023000`, identity at `0x0803E800`;
- `slot-a.bin`: application plus erased padding and its manifest; base
  `0x08009000`, for explicit selective SWD recovery;
- `factory-identity.bin`: 128-byte record; base `0x0803E800`.

For bootloader-only first programming through J-Link/J-Flash, select the actual
STM32G0B1RC device and write the rebuilt bootloader BIN at `0x08000000`. Then
write ONLY `factory-identity.bin` at **`0x0803E800`**, verify, and reset before
using the CAN update client. The factory erase page is
`0x0803E800..0x0803EFFF`; do not erase the adjacent journal.
Do not load `initial.hex` or `slot-a.bin` if the first application is to be
transferred through CAN. Those files already contain the application.
The old `0x0807E800` address is outside this MCU's physical Flash.

For a NEW or deliberately re-commissioned board, use STM32CubeProgrammer:

1. Connect over SWD and confirm STM32G0B1RC, 256 KiB Flash and the expected bank
   mapping. This software does not change option bytes.
2. On a used device, first back up its unique factory record and journal.
   Factory identity cannot be reconstructed from a generic firmware BIN.
3. For this initial layout installation only, selectively erase
   `0x08000000..0x0803FFFF`, or use full-chip erase ONLY after explicitly
   accepting loss of existing identity and journal and having their replacement.
   The HEX is sparse: merely writing it does NOT clear a stale transaction.
4. Program `initial.hex` with verify. Check that NVM
   `0x0803F000..0x0803FFFF` remains erased. It is intentionally absent from HEX;
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
  --interface pcan --channel PCAN_USBBUS1 --node 1 --boot-node 126 --confirm-update
```

Install the adapter vendor's driver and choose a backend/channel supported by
python-can. The current target uses Classic CAN, 250 kbit/s, application Node ID 1
and bootloader Node ID 126. The client first reads the application address; on a
read timeout it probes the bootloader address without retrying a write. With
only the bootloader installed, this initial probe can take 15 seconds.
The tool verifies the package before opening CAN, discovers execution mode and
capability, requests application handoff only when necessary, sends manifest,
begin, segmented image, finish and activate, then checks the responding
application's numeric version. It never automatically retries an uncertain write,
cancels an interrupted update or changes a non-idle receiver state.
After handoff it changes the SDO destination to `--boot-node`; after activation
it waits for the application at `--node`.

Per the current prototype requirement, `0x2000=1` explicitly authorizes the
outage. Application tasks quiesce cooperatively, the actual final SDO TX buffer
must report successful transmission, and only then does the application reset.
There is NO physical service interlock or authenticated CAN command. Use only
on a controlled bench/trusted bus. Firmware signatures authenticate the image,
not the sender of the service command. Normal power-up boots a valid application
without waiting for CAN or a service input.
The client checks OD `0x200F` for the 104 KiB payload limit before sending the
manifest. An old 512 KiB bootloader is rejected; reflash it through SWD.

On timeout/error inspect `0x200A`, `0x2001`, `0x2007` before manually recovering.
For an interrupted RECEIVING/VERIFIED transfer in recovery mode, an informed
operator may cancel with expedited `0x2000:00=0` before starting again. Never
cancel/reprogram during an active swap. Reporting the expected version is not
proof of durable trial confirmation, especially when reinstalling the same
version: verify the journal/self-test and reset/rollback on hardware.
A new handoff is refused while the running application is still an unconfirmed
trial; allow its startup self-test to complete before requesting another update.

## Moving the signing key to another Windows computer

Copying a CurrentUser DPAPI file to a different computer/account is not a
portable key backup. Use `migrate_signing_key.py` to move the SAME private key.
Do not generate another key unless you intend to replace the bootloader's
compiled public key and reprogram all affected devices.

On the original computer, log in as the Windows user that created the key:

```powershell
& ".\.venv\Scripts\python.exe" ".\Tools\migrate_signing_key.py" export `
  --output "$env:USERPROFILE\Desktop\canopen-signing-key.encrypted.pem"
```

Enter a strong unique password of at least 16 characters twice. Password input
is hidden, not a command-line argument. If hidden input is unavailable the tool
fails rather than echoing the password. The export verifies the existing private
key against `firmware_auth_key.h` and writes only encrypted PKCS8 PEM.
Existing output files are never overwritten.

Move that encrypted PEM through a trusted channel/removable drive; transfer
the password separately. Never attach private-key files to chats or commit
them. Both computers require the same trusted project/public-key header.
The header match is a configuration check, not remote attestation of a device.

On the second computer, install Python 3.10+ with the Python launcher, copy the
project (NOT the first computer's `.venv`), and create its environment:

```powershell
Set-Location "C:\Projects\CANOPEN_Bootloader_STM32G0"
py -3 -m venv .venv
& ".\.venv\Scripts\python.exe" -m pip install -r ".\requirements.txt"

& ".\.venv\Scripts\python.exe" ".\Tools\migrate_signing_key.py" import `
  --source "E:\canopen-signing-key.encrypted.pem"

& ".\.venv\Scripts\python.exe" ".\Tools\migrate_signing_key.py" verify
```

Replace `E:\...` with the transferred PEM path. Import prompts for its password,
checks the public key, protects the private key using the destination account's
DPAPI, and checks a local decrypt round-trip before writing. Output defaults to
`%LOCALAPPDATA%\CANOPEN_Bootloader\PrototypeSigningKey\ed25519-signing-key.dpapi`.
The original computer's key is not changed or deleted.
If a destination key already exists, import fails; do not delete it without
first determining whether it is the correct key or has a required backup.

`verify` checks local decryption, public-key equality and a sign/verify operation.
Compare the printed PUBLIC-key SHA-256 fingerprints on both computers; neither
command prints private-key material. Once imported, `package_firmware.py` uses
the destination DPAPI key automatically and the installed bootloader is unchanged.

Keep a controlled offline copy of the encrypted PEM and its separately protected
password if disaster recovery is required; remove unnecessary transfer copies
according to your storage policy. Ordinary deletion is not guaranteed secure
erasure on SSDs/backups. Losing all usable DPAPI accounts and encrypted backups
means losing signing capability. Anyone with this key can sign firmware, so
limit access to both accounts and backups.

The tool never writes plaintext private-key files. Python/cryptography do not
guarantee erasure of all in-memory copies, nor protection against a compromised
host, memory dumps or OS paging.

## Running automated validation

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
