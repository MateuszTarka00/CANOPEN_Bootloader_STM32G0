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
or application control-layer dependency is used. CANopen uses node ID 1 and
250 kbit/s in the current board adapter. The app enters bootloader mode through
the existing application handoff journal (protocol 2); update transport is a
separate versioned vendor profile and has not been verified against
LiftAppTool.

The local CANopen services are CiA 301 NMT slave, boot-up, heartbeat producer,
SDO server (expedited and segmented), EMCY, Error Register, Predefined Error
Field, and the standard SDO server parameter. No CiA 417 OD object numbers are
invented or claimed compliant: the applicable licensed profile edition and
normative tables have not been supplied.

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

The enabled Debug/Release verifier and LTO image contain 33,592 bytes of text,
84 bytes of initialized data and 5,708 bytes of BSS. The load image leaves
3,184 bytes before the fixed discovery record at `0x08008FE0`. The record contains
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
