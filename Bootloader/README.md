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
state VERIFIED. Transfer completion alone does not schedule activation:
the observed LiftAppTool `0x1F51:01=1` command or vendor `0x2000:00=3`
schedules the restartable A/B swap. Normal trial/rollback rules remain in use.
Small containers use the streaming
path too. Both segmented and block SDO downloads are supported, including a
manifest that crosses segment boundaries.

The vendor path remains available: upload `manifest.bin` to `0x2008:00`, write
`0x2000:00=1`, download only the raw image to `0x2002:00`, then explicitly
finish (`2`) and schedule activation (`3`). Both activation commands share
the same verified-image gate. Only the observed one-byte start value 1 is
accepted at `0x1F51:01`; other control values are rejected. No unlock password,
device-type constant or undocumented LiftAppTool envelope is invented.
This protocol work is not a claim of full
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
2. Power-cycle after programming, then enter service mode as before.
   Read Node 126 `0x100A:00` = `1.0.5`, `0x200D:00` = `0x00010005`,
   and `0x2010:00` = 14 to verify this image.
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

The observed `0x1F51:01=1` start command is supported from 1.0.5; no password
or new activation metadata is defined. End-to-end wizard activation
and full interoperability still require hardware verification. These diagnostics identify
the actual failing path; they are not a claim that the physical fault is fixed.

### 1.0.5 stored-image vectors and observed start command

The supplied transfer ending rejected final download verification, persisted
IMAGE_REJECTED (10), then showed a one-byte `0x1F51:01=1` request followed by
NMT Reset Node. The reset cleared volatile `0x2010` diagnostics; zeros after
reset do not establish that final verification succeeded.

Vector validation now reads MSP/Reset_Handler from the selected storage slot
but checks the executable address against Slot A (`0x08009000`) and the signed
image size. Slot B (`0x08023800`) is storage, not a second link address. Stack
RAM bounds/alignment, Thumb bit, image-size and entry-point bounds remain
enforced, as do Ed25519, compatibility, CRC and SHA-256 checks.

`0x1F51:00` is a read-only U8 count of 1; `0x1F51:01` accepts only the observed
U8 value 1 (start). Reading subindex 1 is rejected as write-only. Other values,
lengths and subindices are rejected. This implements the observed command,
not the complete CiA 302 program-control state machine. It shares activation
logic with vendor command 3: valid factory/update capability, no active
container or failed/receiving transfer, persistent VERIFIED with no error,
pending Slot B in both runtime and journal, and no latched Flash fault.
Scheduling writes the existing durable swap state before the SDO success
response; journal failure returns a hardware abort. The cooperative transaction
then swaps A/B, tolerates a subsequent NMT reset, and enters the existing trial
boot/confirmation/rollback flow. No direct jump to Slot B or automatic
activation on transfer completion is introduced.

Native tests compile the production target vector validator and check valid
A-linked vectors in either storage buffer, invalid B-linked entries, erased
vectors, RAM/Thumb/entry/size boundaries. SDO tests check the exact start
request, OD access/length/value rejection, unverified/failed/faulted transfers,
journal failure, repeat commands, restored verified state, and durable-swap
resumption through Slot A payload comparison and TRIAL state.

After programming 1.0.5, retry the complete signed container: the previous
IMAGE_REJECTED download is not pending/activatable. Capture the final download
response and `0x1F51` response. Do not interrupt power while swapping.
An empty initial Slot A provides no valid old application to roll back to;
a failed first trial can therefore return to recovery rather than an old app.

### 1.0.4 post-Flash timeout clock correction

The 1.0.3 hardware trace passed journal save and slot preparation, then accepted
two image bytes (stage 8, no Flash failure snapshot) before reporting firmware
result 7 (TIMEOUT). The header-completion segment took about 2455 ms.
The main loop reused its pre-receive timestamp for Co_Poll, while the receive
callback refreshed last_activity after synchronous preparation. Unsigned
subtraction therefore interpreted the older poll timestamp as a huge elapsed
interval, immediately failing the update; the next segment received a generic
hardware abort because the update was no longer receiving.

The main loop now samples HAL_GetTick again before polling. Co_Poll also uses
the existing SDO time callback when available, both before firmware timeout
checking and after possible journal recovery, before SDO/heartbeat polling.
Platforms without a time callback retain caller-supplied timestamps. Timeout
thresholds remain 1000 ms for SDO and 15000 ms for firmware inactivity; no
timeout disabling, signed timestamp casts or Flash flag clearing is used.

A native regression reproduces receive -> 2455 ms erase -> polling with the
old timestamp. It covers segmented and CRC block containers, normal/wrapped
tick counters, complete transfers, SDO boundaries at 999/1000 ms and firmware
boundaries at 14999/15000 ms. It failed on 1.0.3 before the correction.
Hardware verification of the next transfer is still required.

### 1.0.3 polling-only peripheral configuration

The bootloader initializes FDCAN1 only and receives frames by polling FIFO0.
Previously its RX notification enabled the shared interrupt handler, which
also called HAL for the uninitialized FDCAN2 handle (`Instance == NULL`).
The HAL then accessed addresses `0x00000050`/`0x00000054` as CAN IR/IE.
With the normal main-Flash boot alias these are vector-table locations,
and nonzero masked values can cause attempted stores to `0x00000050`.
Inspection of the previous ELF confirmed these store branches were reachable.
This is a software defect consistent with the observed Flash errors, but its
relationship to every captured PGAERR/PGSERR/CFGBSY fault still needs hardware
confirmation.

FDCAN MSP initialization no longer enables either shared NVIC line, and the
boot adapter no longer activates RX interrupts. Unused TIM14 and shared
TIM16/TIM17/FDCAN handlers have been removed, retaining the startup fail-stop
default rather than accessing uninitialized handles. TIM1 remains the active
HAL tick interrupt. Voltage-scaling failure now enters Error_Handler before
PLL/clock configuration. Application RTOS/CAN interrupt handling is unchanged.

No bank mapping, Flash guard, flag clearing, option bytes, journal format or
signed-container protocol has changed. With the captured two-bank/no-swap
configuration, journal addresses `0x0803F000` and `0x0803F800` map to bank 2,
local pages 62 and 63. A hardware retry after a power cycle is required;
do not clear error flags to manufacture a passing result.

### 1.0.2 busy-Flash containment (partial mitigation)

The supplied `callStack.PNG`, `rejestry.PNG` and `rejestry flash.PNG` show a
HardFault during manifest-completion journal erase, with `CFGBSY=1`,
`BSY1=BSY2=0`, `SR=0x00040000` and `CR=0x40000088` (LOCK clear).
The optimized/LTO call stack attributes the location to `HAL_FLASH_Lock`
inside `flashEraseRaw`; this does **not** prove the exact faulting instruction
or explain why CFGBSY remained asserted. This release contains subsequent
unsafe control writes; it is not a demonstrated root-cause cure.

Both projects use the same local synchronous HAL guard. Before unlock, operation
start, completion cleanup and lock, it checks all BSY1/BSY2/CFGBSY flags and Flash
errors. Waiting is bounded by 1 s or 1,000,000 polling iterations, whichever
comes first (including a stopped tick; tick arithmetic tolerates wrap).
On the first error/timeout, it snapshots SR **before** any flag clearing or
cleanup and latches a volatile fault until MCU reset. No PER/PG cleanup or LOCK
write follows a failed wait, and subsequent erase/program/unlock/lock requests
are rejected even if the busy flags later clear. No dummy Flash writes or
option-byte changes are used. These paths are synchronous and serialized;
interrupt/option-byte programming is not part of either adapter's API.

The boot adapter also latches HAL/unlock/lock/readback failures. Existing SDO
uploads and the CAN service loop remain enabled if the hardware can continue
executing; further downloads, journal writes and activation are rejected until
reset. Failure diagnostics cannot be cleared by starting another transfer.
`0x2010` retains its existing 14-subindex wire layout:
subindex 03 now packs the original operation in bits 0..15 and guard stage in
bits 16..31. Guard stages are 0 = adapter/HAL failure outside a wait,
1 = pre-unlock, 2 = pre-operation, 3 = operation completion,
4 = before PER/PG cleanup, 5 = pre-lock. Subindex 01 retains the first failing
update stage; 04 is HAL status (1 error, 2 busy, 3 timeout), 05 the error mask,
06 the first SR, and 07..0C retain address/bank/page/page-error/OPTR/bank count.
For example, operation `0x00030003`, status 3, SR `0x00040000` identifies
erase-completion timeout with CFGBSY, **before** cleanup or lock.

For the next bench attempt, follow the selective bootloader reflash above;
the existing signed application/package and factory/journal need not be replaced.
The application adapter is hardened for its next normal signed deployment,
not a reason to overwrite Slot A directly or invalidate its signature metadata.
Capture the complete CAN trace, version reads, all `0x2010:01..0E`,
`0x2001:00` and `0x2007:00` immediately after the first abort, before resetting.
Do not activate or repeatedly retry after a latched fault. No debugger reading
is needed for this evidence; whether the original HardFault is avoided still
requires this controlled hardware test.

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
| `0x1F51:00` | RO, U8 | Program count 1 |
| `0x1F51:01` | WO, U8 | Observed LiftAppTool start value `1`: schedule verified Slot B activation; other values unsupported |
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
The peripheral regression compiles production FDCAN MSP/boot initialization,
clock configuration and TIM1 ISR bodies with observable HAL substitutes.
It checks polling-only CAN without NVIC/RX notification activation, an untouched
FDCAN2 handle, initialization failure propagation, TIM1 dispatch, and fail-stop
behavior before clock setup when voltage scaling fails. The runner also rejects
unused CAN/TIM14/TIM17 handle dependencies in the bootloader ISR source.
The Flash regression compiles the actual synchronous HAL function bodies with
observable register/erase/program substitutes and the production guard header,
covering errors, CFGBSY, BSY1/2, timeout, tick wrap/stall, suppressed cleanup/lock,
sticky rejection and first-SR preservation. Portable SDO tests also verify that
a latched adapter fault preserves diagnostics and rejects activation/retries
without Flash/journal writes. The runner checks that both projects' guarded
HAL sources and headers remain identical.
Per-source `.su`/`.cyclo` reports are disabled: GCC does not emit these during
LTO compilation. A target stack-depth assessment remains separate work.
These tests do not replace target link-size,
FDCAN, flash-bank, power-cut, signature interoperability, LiftAppTool, or
CiA conformance testing.

The 1.0.5 Debug/Release verifier and LTO image contain 35,788 bytes of
text, 84 bytes of initialized data and 5,700 bytes of BSS. The load image ends
at `0x08008C00`, leaving 992 bytes before the fixed discovery record at
`0x08008FE0`. The record contains
the protocol, layout and signing gate and is checked by the application.
The linker asserts that the load image cannot overlap this record or either
application slot. The exported bootloader BIN spans exactly 36 KiB including
the record and address gap, not 36 KiB of executable code.
Both projects automatically generate the raw BIN in their selected Debug/Release
output directory after a successful CubeIDE build.
Both 1.0.5 BIN outputs are byte-identical. The 1.0.3 ELF inspection confirmed that TIM14
and both shared TIM16/TIM17/FDCAN vectors resolve to Default_Handler, TIM1
retains its tick handler, and HAL_FDCAN_IRQHandler/FDCAN2/TIM17 handles are not
linked into the bootloader.
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
