# STM32G0B1 CANopen bootloader

This is the standalone bootloader project in `CANOPEN_Bootloader_STM32G0`.
The VEGA firmware repository is changed only at the integration boundary:
slot-A linking, vector relocation, trial confirmation, and the shared NVM
state-range validation.

## Architecture

```text
STM32 FDCAN1 / Flash HAL
          |
  STM32 boot platform
   |             |
CANopen node   Flash/NVM callbacks
   |             |
NMT / heartbeat / EMCY / SDO
   |             |
Object Dictionary -- firmware update receiver
                         |
             manifest + SHA-256 + Ed25519
                         |
             power-fail-safe page swap
               A <-> scratch <-> B
                         |
                 trial / rollback
```

The bootloader runs a cooperative 1 ms poll loop; no FreeRTOS, heap allocation,
or application control-layer dependency is used. CANopen uses node ID 126 and
250 kbit/s in the current board adapter. The app enters bootloader mode through
the existing application handoff journal (protocol 2); update transport is a
separate versioned vendor profile and has not been verified against
LiftAppTool.

The local CANopen services are CiA 301 NMT slave, boot-up, heartbeat producer,
SDO server (expedited, segmented and size-indicated block download), EMCY, Error Register, Predefined Error
Field, and the standard SDO server parameter. No CiA 417 OD object numbers are
invented or claimed compliant: the applicable licensed profile edition and
normative tables have not been supplied.

### LiftAppTool compatibility: transport stage only

The supplied manual (pages 16 and 28) and the recorded CAN trace establish
bootloader Node-ID 126. The manual screenshot shows block download to
`0x1F50:01`. This target now uses Node 126 even when an older handoff journal
contains the application's Node-ID. SDO request/reply IDs are `0x67E`/`0x5FE`;
heartbeat is `0x77E`. Keep this address free of other devices during service.

`0x1F50:00` reads as U8 count 1. `0x1F50:01` is a write-only,
self-describing update-container stream: exactly 152 bytes of signed manifest
followed by the raw Slot-A-linked image. The SDO length covers both parts.
The receiver buffers and validates the complete manifest (record CRC/commit,
Ed25519 signature, hardware compatibility, and exact declared image length)
before erasing the inactive slot. It then streams the image, checks transfer
CRC, read-back digest and image validity, and leaves the persistent update
state VERIFIED. It does not schedule activation; normal trial/rollback rules
still require a separate activation policy. Small containers use the streaming
path too. Both segmented and block SDO downloads are supported, including a
manifest that crosses segment boundaries.

The vendor path remains available: upload `manifest.bin` to `0x2008:00`, write
`0x2000:00=1`, download only the raw image to `0x2002:00`, then explicitly
finish (`2`) and schedule activation (`3`). This path is unchanged. No control
values for `0x1F51`, unlock password, device-type constant or undocumented
LiftAppTool envelope are invented. This protocol work is not a claim of full
LiftAppTool wizard interoperability. ZIP files must never be sent as firmware
data; use the generated `.update.bin` only for its documented 1F50 container
transfer, or use the existing ZIP-aware PC client for the vendor path.

Block download requires a declared nonzero size and negotiates one segment
per sub-block (a permitted block size), so Flash writes finish before the next
sub-block is acknowledged without requiring a large RX queue or RAM buffer.
CRC-16/XMODEM is supported when requested. Invalid sequence numbers abort
the session; no uncertain Flash write is automatically replayed. Size, final
padding, CRC, client abort, stream errors and timeout are checked. A failed
stream invokes the existing cancellation path and cannot activate an image.
Block upload and size-unspecified block download are not supported. The
small block size trades throughput for bounded memory and RX servicing.

### Diagnosing the 2026-10-18 21:19 transfer failure (build 1.0.1)

The latest `update.txt` proves a declared 28,708-byte download, 147 bytes
acknowledged, then failure on the 22nd seven-byte segment after 417 ms.
That segment finishes the existing 152-byte `VGB1` manifest and contains the
first two image bytes. ACKs for the prefix do **not** prove authentication.
The boundary runs manifest decoding/length checks, authentication, the
RECEIVING journal commit, `FirmwareUpdate_Begin`, its validation and
`BootloaderInterface_PrepareFirmware` validation/slot erase. Two image bytes
only enter the eight-byte programming buffer: image programming and pending
manifest commit cannot yet be the failing operations.

`0x06060000` alone does not identify which of the above failed. In particular,
a failed journal erase/program before slot preparation also produces it.
Neither bad bank addressing nor a timeout is established by the trace.
The current HAL adapter already splits bank-local page numbers using the
HAL's runtime bank count; Slot B is bank 2 page 7 in dual-bank mode, or
bank 1 page 71 in single-bank mode. Slot A crossing a bank boundary is also
split. Preparation already refreshes the firmware/SDO clocks after its
synchronous work; the container refreshes the timestamp again before writing
its first image bytes. Native tests cover a 1,500-ms erase, including tick wrap.
Do not change option bytes, erase protections, signatures or the image format
based on the 417-ms delay alone.

`LiftAppToolbox_format_BIN_dla_AI.md` explicitly calls `FWUP`/64 bytes a
hypothesis, not a verified format. This build does **not** add it or accept
unsigned raw images. Existing signed containers, the vendor path, A/B recovery
and Ed25519 remain in use.

Build 1.0.1 adds volatile, read-only diagnostic record **`0x2010`** on Node 126.
Read `0x2010:00` as U8 (14 entries); all other subindices below are U32,
little-endian. Diagnostics reset at the next accepted container initiation or
vendor begin, and at MCU reset. SDO abort cleanup and diagnostic reads do not
clear the stage or the first Flash-failure snapshot. Save them **before**
resetting, starting another download or sending another update command.

| Address | Meaning |
|---|---|
| `0x2010:01` | Last entered stage (table below) |
| `0x2010:02` | RECEIVING `Ps_Save` result: 0 OK, 1 EMPTY, 2 CORRUPT, 3 IO_ERROR, 4 INVALID; zero also means not attempted before stage 3 |
| `0x2010:03` | First Flash failure operation: 0 none, 1 erase preflight, 2 erase unlock, 3 HAL erase, 4 erase lock, 5 program preflight, 6 program unlock, 7 HAL program, 8 program lock, 9 program readback |
| `0x2010:04` | HAL status at that failure: 0 OK, 1 ERROR, 2 BUSY, 3 TIMEOUT |
| `0x2010:05` | `HAL_FLASH_GetError()` captured before cleanup/next HAL call |
| `0x2010:06` | `FLASH->SR` after the failing call (HAL may already have cleared error flags) |
| `0x2010:07` | Operation address; for erase, start of the failing bank span |
| `0x2010:08` | HAL erase bank mask (1 or 2); zero when not applicable |
| `0x2010:09` | HAL erase starting page within that bank |
| `0x2010:0A` | HAL `PageError`, the actual failing bank-local page, or `0xFFFFFFFF` if not supplied |
| `0x2010:0B` | `FLASH->OPTR` at first failure |
| `0x2010:0C` | HAL runtime `FLASH_BANK_NB` at first failure |
| `0x2010:0D` | Current firmware result: notably 4 ERASE_FAILED, 6 WRITE_FAILED, 7 TIMEOUT, 11 PENDING_FAILED, 12 CANCELLED |
| `0x2010:0E` | Number of image bytes accepted (excludes the manifest) |

Stages: **0** none; **1** decode/container length; **2** initial authentication;
**3** RECEIVING journal save; **4** Begin validation; **5** Prepare validation;
**6** slot erase; **7** ready to receive; **8** image buffer/program;
**9** finish/readback/pending commit; **10** verified/pending success.
A failed stage remains entered, not completed. For example, stage 3 plus
PS_IO_ERROR isolates the journal, while stage 6 plus operation 3 isolates
slot HAL erase. Stage 5 plus result 4 and no Flash failure means Prepare
rejected before calling erase; result 4 alone is not proof of HAL erase failure.

Bench procedure:

1. Back up Flash. Program only `Debug\CANOPEN_Bootloader_STM32G0.bin` or
   `Release\CANOPEN_Bootloader_STM32G0.bin` (36,864 bytes) at `0x08000000`
   using SWD, **without mass erase**; preserve application, factory and NVM.
2. Enter service mode as before. Read Node 126 `0x100A:00` = `1.0.1`,
   `0x200D:00` = `0x00010001`, and `0x2010:00` = 14 to verify this image.
3. Retry the **same signed 28,708-byte file**, capturing full CAN traffic.
   If it aborts, do not reset/retry yet: read every `0x2010:01..0E` and
   `0x2007:00`, saving the raw values alongside the trace.
4. For manual raw CAN, send eight bytes `40 10 20 ss 00 00 00 00` on
   `0x67E` (`ss` = `01` through `0E`); expect `43 10 20 ss vv vv vv vv`
   on `0x5FE`. Subindex 00 uses response command `4F`, U8 value `0E`.
5. Interpret the failing address as journal (`0x0803F000`/`0x0803F800`) or
   Slot B (`0x08023800` onward). HAL error bit `0x10` is WRP, `0x08`
   PROG, `0x20` PGA, `0x40` SIZ, `0x80` PGS; retain the complete mask.
   OPTR `0x00100000` is nSWAP_BANK and `0x00200000` DUAL_BANK, but use
   the captured HAL bank count, which also depends on the device sales type.
   Do not automatically clear error flags/change option bytes to hide a failure.

No new `0x1F51`, password or activation metadata is defined. Wizard activation
and full interoperability still require evidence. These diagnostics identify
the actual failing path; they are not a claim that the physical fault is fixed.

## Flash map

The target is **STM32G0B1RCT6 (LQFP64)**: 256 KiB Flash, 144 KiB RAM,
2 KiB erase pages and 8-byte programming units. Both CubeIDE configurations
use MCU `STM32G0B1RCTx` and `STM32G0B1RCTX_FLASH.ld`.
The fixed map gives most usable Flash to the expanding application while
reserving 36 KiB for the mostly-static bootloader, with signature verification
enabled. The earlier 32 KiB estimate excluded the disabled verifier and is obsolete.

| Region | Start | Size | Purpose |
|---|---:|---:|---|
| Bootloader | `0x08000000` | 36 KiB | CANopen, verification, swap/recovery |
| Slot A | `0x08009000` | 106 KiB | Executable application, fixed link address |
| Slot A manifest | `0x08023000` | 2 KiB | Signed image metadata; inside Slot A |
| Slot B | `0x08023800` | 106 KiB | Staging/rollback image, same link address |
| Slot B manifest | `0x0803D800` | 2 KiB | Signed image metadata; inside Slot B |
| Swap scratch | `0x0803E000` | 2 KiB | One page during A/B swap |
| Factory identity | `0x0803E800` | 2 KiB | Commissioned immutable product identity |
| NVM journal | `0x0803F000` | 4 KiB | Two-page power-fail-safe metadata journal |

Each slot has 104 KiB (`0x1A000`) of payload before its manifest page.
The application occupies approximately 48 KiB in Debug and 28 KiB in Release,
leaving room for growth within the fixed payload limit. Its linker
targets Slot A at `0x08009000`; its vector-table offset and trial-confirmation
hook must remain paired with this bootloader layout.

The B image is linked for Slot A. After full verification, the bootloader swaps
every page through scratch, so Slot A contains the new image at its linked
address and Slot B retains the previous image. Only Slot A is executed.
Persisted page/phase values make the three-copy operation restartable after
power loss.

`device_memory.h` is the identical portable memory contract in both projects.
Runtime checks require exactly 256 KiB and reject swapped-bank mappings.
The HAL's actual bank count determines page numbering: factory is page 125 in
single-bank mode, or bank 2 page 61 in normal dual-bank mode. Multi-page erases
are split at bank boundaries, including Slot A crossing `0x08020000` in dual-bank
mode. No option bytes are changed. The 53 pages per slot require 159 swap states;
both journal decoders reject out-of-layout page/phase values.

The previous 512 KiB layout is not compatible with RCT6. Back up any identity
and journal before replacing the bootloader; this is initial commissioning,
not automatic journal migration. Rebuild both projects, regenerate packages
and factory files, and follow the [SWD instructions](../Tools/README.md).

## Image authenticity and enablement gate

The manifest format is 152 bytes. Bytes 0..75 are the canonical Ed25519-signed
message; they include the image length, CRC-32, version, compatibility tuple,
SHA-256 digest, and signature algorithm/size. The 64-byte signature occupies
bytes 76..139; record CRC and commit markers occupy bytes 140..151. CRC is
only corruption detection. Ed25519 authenticates metadata and its digest.

The verifier uses Monocypher 4.0.2, included under `ThirdParty/Monocypher` with
its upstream dual BSD-2-Clause/CC0 license. A prototype Ed25519 key pair has been generated locally. Its public key is
compiled into `Bootloader/Inc/firmware_auth_key.h`; the private key is stored
outside the repository at
`%LOCALAPPDATA%\CANOPEN_Bootloader\PrototypeSigningKey\ed25519-signing-key.dpapi`.
The private-key file is protected with Windows DPAPI for the current user.
`BL_SIGNING_PUBLIC_KEY_PROVISIONED` is `1`: signature verification is enabled.
First programming must include a matching signed Slot-A image and factory
identity. See [commissioning commands](../Tools/README.md) for sparse initial
HEX generation, SWD recovery, and the PC CAN update client.
DPAPI protection is tied to this Windows account; arrange a secure recovery
backup before relying on the key. Never commit or share the private key. Do
not use this prototype key as a production signing identity.

## Vendor update profile v1

The SDO server implements:

| Index | Access | Meaning |
|---|---|---|
| `0x2000:00` | RW, U8 | Control: `0` cancel, `1` begin, `2` verify/finish, `3` schedule activation |
| `0x2001:00` | RO, U8 | Public update state |
| `0x2002:00` | WO, DOMAIN | Sequential firmware payload, segmented SDO download |
| `0x1F50:01` | WO, DOMAIN | 152-byte signed manifest followed by image; automatic verify to VERIFIED, no activation |
| `0x2003:00` | RO, U32 | Firmware CRC-32 |
| `0x2004:00` | RO, U32 | Firmware payload size |
| `0x2005:00` | RO, U32 | Pending version |
| `0x2006:00` | RO, U32 | Active version |
| `0x2007:00` | RO, U32 | Last update result |
| `0x2008:00` | RW, DOMAIN | 152-byte signed manifest |
| `0x2009:00` | RO, U32 | Application handoff contract version |
| `0x200A:00` | RO, U8 | Execution mode |
| `0x200B:00` | RO, U8 | Update capability (1 only when key and factory identity are valid) |
| `0x200C:00` | RO, U8 | Update-profile version |
| `0x200D:00` | RO, U32 | Bootloader software version |
| `0x200E:00` | RO, U8 | Active slot |
| `0x200F:00` | RO, U32 | Maximum image payload size |

The server validates the signed manifest and compatibility before erasing the
inactive slot. Segment data is written sequentially with bounded buffering;
timeouts, length, CRC, read-back SHA-256, vector table, and signature are
checked before activation can be scheduled. An interrupted `RECEIVING` state
can restart with a fresh manifest and a new transfer. Cancellation clears the
service handoff request and journal state.

## Trial and rollback

```text
new signed image in B -> verify -> journal VERIFIED
 -> journal forward page/phase -> swap A and B
 -> TRIAL -> application startup self-test for 1 s
    -> app confirms (state 0x501) -> next boot finalizes to IDLE
    -> reset/no confirmation -> swap A and B back -> IDLE -> old app
```

The application confirms only after the scheduler is running, both FDCAN
peripherals are started, and all four critical RTOS tasks have been created,
entered their service loops, and remain Ready/Running/Blocked for the 1-second
stability interval. If the tasks do not reach that state within 5 seconds, the
application resets without confirming and the bootloader rolls back on the next
startup. A failed journal confirmation also resets. This is a software startup
timeout; it is not a replacement for a product-approved runtime watchdog.
Hardware-in-loop power-cut testing is still required.

## Build and validation

STM32CubeIDE Debug/Release source lists include `Bootloader` and
`ThirdParty`, exclude the unused FreeRTOS middleware/application task stub,
and enable `-Os -flto` for compilation and linking. `Tests/run_native.py`
runs the portable C99 tests using the development dependency's Zig compiler.
Per-source `.su`/`.cyclo` reports are disabled: GCC does not emit these during
LTO compilation. A target stack-depth assessment remains separate work.
These tests do not replace target link-size,
FDCAN, flash-bank, power-cut, signature interoperability, LiftAppTool, or
CiA conformance testing.

The diagnostic 1.0.1 Debug/Release verifier and LTO image contain 36,144 bytes of
text, 84 bytes of initialized data and 5,940 bytes of BSS. The load image leaves
632 bytes before the fixed discovery record at `0x08008FE0`. The record contains
the protocol, layout and signing gate and is checked by the application.
The linker asserts that the load image cannot overlap this record or either
application slot. The exported bootloader BIN spans exactly 36 KiB including
the record and address gap, not 36 KiB of executable code.
Both projects automatically generate the raw BIN in their selected Debug/Release
output directory after a successful CubeIDE build.
CubeIDE 1.16.1 still generates `.su`/`.cyclo` peer-output rules when the reports
are disabled; clean LTO builds warn that these diagnostic outputs were not
emitted. These are generator/report warnings, not linker overflow or C errors.

A valid signed application boots normally. Only an explicit update request
keeps a healthy device in service mode; invalid images/unfinished transactions
remain in recovery. Per the prototype requirement there is no physical service
interlock: the application cooperatively quiesces its other tasks after the
request and resets only after the exact SDO response has actually transmitted.
This is not a lift safety authorization mechanism; use a controlled bench.

Before production programming:

1. Replace the prototype Ed25519 public key with the approved production key.
2. Sign and package the exact image/manifest format above; verify against an
   independent Ed25519 implementation.
3. Build both projects and confirm the bootloader fits the 36 KiB region and
   the application fits the 104 KiB payload region.
4. Validate the actual MCU option-byte bank mapping, write/erase behavior, and
   power interruption at every journal/swap phase.
5. Exercise trial confirmation, failed self-test, rollback, CAN loss, SDO
   timeout, and reset recovery on hardware.
6. Verify LiftAppTool protocol and CiA 417 conformance separately; neither is
   implied by this implementation.
