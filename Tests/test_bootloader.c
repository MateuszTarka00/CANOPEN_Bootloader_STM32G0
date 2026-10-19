/** @file test_bootloader.c @brief Unit tests for the portable bootloader transaction core. */
#include "firmware_update.h"
#include "firmware_auth.h"
#include "monocypher-ed25519.h"
#include "bootloader_transaction.h"
#include "persistent_storage.h"
#include "sha256.h"
#include "object_dictionary.h"
#include "canopen.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_FLASH_START UINT32_C(0x08000000)
#define TEST_FLASH_SIZE DEVICE_FLASH_SIZE
#define TEST_PROGRAM_SIZE 8U

static uint8_t hexNibble(char value)
{
    if ((value >= '0') && (value <= '9')) {
        return (uint8_t)(value - '0');
    }
    if ((value >= 'a') && (value <= 'f')) {
        return (uint8_t)(value - 'a' + 10);
    }
    return (uint8_t)(value - 'A' + 10);
}

static void decodeHex(const char *text, uint8_t *output, uint32_t size)
{
    uint32_t i;
    for (i = 0U; i < size; ++i) {
        output[i] = (uint8_t)((hexNibble(text[i * 2U]) << 4U) |
                              hexNibble(text[(i * 2U) + 1U]));
    }
}

static void testEd25519(void)
{
    static const char public_key_hex[] =
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    static const char signature_hex[] =
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
    uint8_t public_key[32];
    uint8_t signature[64];
    uint8_t empty = 0U;
    decodeHex(public_key_hex, public_key, sizeof(public_key));
    decodeHex(signature_hex, signature, sizeof(signature));
    assert(crypto_ed25519_check(signature, public_key, &empty, 0U) == 0);
    signature[0] ^= 1U;
    assert(crypto_ed25519_check(signature, public_key, &empty, 0U) != 0);
    assert(BlAuth_IsReady());
    puts("Ed25519 RFC 8032 verification and provisioned prototype key OK");
}

static void testPersistentUpdateStateValidation(void)
{
    PsData data;
    memset(&data, 0, sizeof(data));
    data.pending_slot = UINT32_MAX;
    assert(Ps_Validate(&data));
    data.update_state = UINT32_C(0x19E);
    assert(Ps_Validate(&data));
    data.update_state = UINT32_C(0x39E);
    assert(Ps_Validate(&data));
    data.update_state = UINT32_C(0x19F);
    assert(!Ps_Validate(&data));
    data.update_state = UINT32_C(0x39F);
    assert(!Ps_Validate(&data));
    puts("persistent swap state validation boundaries OK");
}

typedef struct {
    BlDiagnostics diagnostics;
    uint8_t bytes[TEST_FLASH_SIZE];
    uint32_t erase_count;
    uint32_t program_count;
    uint32_t pending_count;
    uint32_t activate_count;
    uint32_t rollback_count;
    uint32_t jump_count;
    uint32_t now_ms;
    uint32_t erase_advance_ms;
    PsData *persistent;
    bool time_enabled;
    bool fail_read;
    bool fail_erase;
    bool fail_program;
    bool fail_manifest;
    bool fail_image;
    bool fail_pending;
} FakeFlash;

typedef struct {
    uint8_t page[2][0x800U];
} FakeNvm;

static bool fakeNvmRead(void *user, uint8_t page, uint32_t offset,
                        uint8_t *data, uint32_t size)
{
    FakeNvm *nvm = (FakeNvm *)user;
    if ((nvm == NULL) || (data == NULL) || (page >= 2U) ||
        (offset > sizeof(nvm->page[page])) ||
        (size > (sizeof(nvm->page[page]) - offset))) {
        return false;
    }
    memcpy(data, &nvm->page[page][offset], size);
    return true;
}

static bool fakeNvmErase(void *user, uint8_t page)
{
    FakeNvm *nvm = (FakeNvm *)user;
    if ((nvm == NULL) || (page >= 2U)) {
        return false;
    }
    memset(nvm->page[page], 0xFF, sizeof(nvm->page[page]));
    return true;
}

static bool fakeNvmWrite(void *user, uint8_t page, uint32_t offset,
                         const uint8_t *data, uint32_t size)
{
    FakeNvm *nvm = (FakeNvm *)user;
    uint32_t i;
    if ((nvm == NULL) || (data == NULL) || (page >= 2U) ||
        (offset > sizeof(nvm->page[page])) ||
        (size > (sizeof(nvm->page[page]) - offset))) {
        return false;
    }
    for (i = 0U; i < size; ++i) {
        if ((nvm->page[page][offset + i] & data[i]) != data[i]) {
            return false;
        }
    }
    for (i = 0U; i < size; ++i) {
        nvm->page[page][offset + i] &= data[i];
    }
    return true;
}

static BlLayout makeLayout(void)
{
    BlLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.flash_start = TEST_FLASH_START;
    layout.flash_size = TEST_FLASH_SIZE;
    layout.bootloader.address = TEST_FLASH_START;
    layout.bootloader.size = 0x1000U;
    layout.slot[0].address = TEST_FLASH_START + 0x1000U;
    layout.slot[0].size = 0x2000U;
    layout.slot[1].address = TEST_FLASH_START + 0x3000U;
    layout.slot[1].size = 0x2000U;
    layout.scratch.address = TEST_FLASH_START + 0x5000U;
    layout.scratch.size = 0x800U;
    layout.factory.address = TEST_FLASH_START + 0x5800U;
    layout.factory.size = 0x800U;
    layout.journal.address = TEST_FLASH_START + 0x6000U;
    layout.journal.size = 0x1000U;
    layout.erase_unit = 0x800U;
    layout.program_unit = TEST_PROGRAM_SIZE;
    return layout;
}

static bool flashOffset(uint32_t address, uint32_t size, uint32_t *offset)
{
    if ((address < TEST_FLASH_START) ||
        (address - TEST_FLASH_START > TEST_FLASH_SIZE) ||
        (size > TEST_FLASH_SIZE - (address - TEST_FLASH_START))) {
        return false;
    }
    *offset = address - TEST_FLASH_START;
    return true;
}

static bool fakeRead(void *user, uint32_t address, uint8_t *data, uint32_t size)
{
    FakeFlash *flash = user;
    uint32_t offset;
    if (flash->fail_read || (data == NULL) || !flashOffset(address, size, &offset)) {
        return false;
    }
    memcpy(data, &flash->bytes[offset], size);
    return true;
}

static bool fakeErase(void *user, uint32_t address, uint32_t size)
{
    FakeFlash *flash = user;
    uint32_t offset;
    if (flash->fail_erase || !flashOffset(address, size, &offset)) {
        return false;
    }
    memset(&flash->bytes[offset], 0xFF, size);
    flash->erase_count++;
    flash->now_ms += flash->erase_advance_ms;
    return true;
}

static bool fakeProgram(void *user, uint32_t address, const uint8_t *data, uint32_t size)
{
    FakeFlash *flash = user;
    uint32_t offset;
    uint32_t i;
    if (flash->fail_program || (data == NULL) ||
        ((address % TEST_PROGRAM_SIZE) != 0U) ||
        ((size % TEST_PROGRAM_SIZE) != 0U) ||
        !flashOffset(address, size, &offset)) {
        return false;
    }
    for (i = 0U; i < size; ++i) {
        if ((flash->bytes[offset + i] & data[i]) != data[i]) {
            return false;
        }
    }
    for (i = 0U; i < size; ++i) {
        flash->bytes[offset + i] &= data[i];
    }
    flash->program_count++;
    return true;
}

static bool verifyManifest(void *user, const BlImageManifest *manifest)
{
    FakeFlash *flash = user;
    return !flash->fail_manifest && (manifest != NULL) &&
        (manifest->signature_size == 64U) && (manifest->signature[0] == 0xA5U);
}

static bool validateProgrammed(void *user, uint8_t slot,
                               const BlImageManifest *manifest)
{
    FakeFlash *flash = user;
    (void)slot;
    return !flash->fail_image && (manifest != NULL);
}

static bool validateApplication(void *user, uint8_t slot)
{
    FakeFlash *flash = user;
    (void)slot;
    return !flash->fail_image;
}

static bool markPending(void *user, uint8_t slot, const BlImageManifest *manifest)
{
    FakeFlash *flash = user;
    (void)slot;
    if (flash->fail_pending || (manifest == NULL)) {
        return false;
    }
    flash->pending_count++;
    if (flash->persistent != NULL) {
        flash->persistent->update_state = BL_TX_VERIFIED;
        flash->persistent->pending_slot = 1U;
        flash->persistent->pending_version = manifest->version;
        flash->persistent->firmware_size = manifest->image_size;
        flash->persistent->firmware_crc = manifest->image_crc;
        flash->persistent->update_result = 0U;
    }
    return true;
}

static bool activate(void *user, uint8_t slot)
{
    FakeFlash *flash = user;
    (void)slot;
    flash->activate_count++;
    return true;
}

static bool rollback(void *user, uint8_t slot)
{
    FakeFlash *flash = user;
    (void)slot;
    flash->rollback_count++;
    return true;
}

static bool jump(void *user, uint32_t address)
{
    FakeFlash *flash = user;
    if (address < TEST_FLASH_START) {
        return false;
    }
    flash->jump_count++;
    return true;
}

static uint32_t fakeTime(void *user)
{
    return ((FakeFlash *)user)->now_ms;
}

static BlPlatform makePlatform(FakeFlash *flash)
{
    BlPlatform platform;
    memset(&platform, 0, sizeof(platform));
    platform.user = flash;
    platform.diagnostics = &flash->diagnostics;
    platform.read = fakeRead;
    platform.erase = fakeErase;
    platform.program = fakeProgram;
    platform.verify_manifest = verifyManifest;
    platform.validate_programmed_image = validateProgrammed;
    platform.validate_application = validateApplication;
    platform.mark_pending = markPending;
    platform.activate = activate;
    platform.rollback = rollback;
    platform.jump = jump;
    if (flash->time_enabled) {
        platform.get_time_ms = fakeTime;
    }
    return platform;
}

static FiData makeFactory(void)
{
    FiData factory = {1U, 42U, 7U, 1234U, 2U, 1U, 3U};
    return factory;
}

static BlImageManifest makeManifest(const uint8_t *image, uint32_t size)
{
    BlImageManifest manifest;
    BlSha256 sha;
    memset(&manifest, 0, sizeof(manifest));
    manifest.image_size = size;
    manifest.image_crc = Ps_Crc(image, size);
    manifest.version = UINT32_C(0x00010203);
    manifest.compatibility.vendor_id = 1U;
    manifest.compatibility.product_code = 42U;
    manifest.compatibility.hardware_variant = 2U;
    manifest.compatibility.hardware_min = UINT32_C(0x00010003);
    manifest.compatibility.hardware_max = UINT32_C(0x00010003);
    BlSha256_Init(&sha);
    BlSha256_Update(&sha, image, size);
    BlSha256_Final(&sha, manifest.image_digest);
    manifest.signature_size = 64U;
    manifest.signature[0] = 0xA5U;
    return manifest;
}

static void testSha256(void)
{
    static const uint8_t expected[32] = {
        0xBAU, 0x78U, 0x16U, 0xBFU, 0x8FU, 0x01U, 0xCFU, 0xEAU,
        0x41U, 0x41U, 0x40U, 0xDEU, 0x5DU, 0xAEU, 0x22U, 0x23U,
        0xB0U, 0x03U, 0x61U, 0xA3U, 0x96U, 0x17U, 0x7AU, 0x9CU,
        0xB4U, 0x10U, 0xFFU, 0x61U, 0xF2U, 0x00U, 0x15U, 0xADU
    };
    uint8_t digest[32];
    BlSha256 sha;
    BlSha256_Init(&sha);
    BlSha256_Update(&sha, (const uint8_t *)"abc", 3U);
    BlSha256_Final(&sha, digest);
    assert(memcmp(digest, expected, sizeof(expected)) == 0);
}

static bool reopen(FakeFlash *memory, BootloaderInterface *bl)
{
    BlLayout layout = makeLayout();
    BlPlatform platform = makePlatform(memory);
    FiData factory = makeFactory();
    return BootloaderInterface_Init(bl, &layout, &platform, &factory,
                                    0U, BL_NO_SLOT, BL_NO_SLOT);
}

static bool initialize(FakeFlash *memory, BootloaderInterface *bl)
{
    memset(memory, 0, sizeof(*memory));
    memset(memory->bytes, 0xFF, sizeof(memory->bytes));
    return reopen(memory, bl);
}

static void testLayoutAndGuards(void)
{
    FakeFlash memory;
    BootloaderInterface bl;
    BlLayout layout = makeLayout();
    BlPlatform platform = makePlatform(&memory);
    FiData factory = makeFactory();
    uint8_t data[TEST_PROGRAM_SIZE] = {0U};
    memset(&memory, 0, sizeof(memory));
    memset(memory.bytes, 0xFF, sizeof(memory.bytes));
    layout.slot[1].address = layout.slot[0].address;
    assert(!BootloaderInterface_Init(&bl, &layout, &platform, &factory,
                                     0U, BL_NO_SLOT, BL_NO_SLOT));
    layout = makeLayout();
    layout.flash_start++;
    assert(!BootloaderInterface_Init(&bl, &layout, &platform, &factory,
                                     0U, BL_NO_SLOT, BL_NO_SLOT));
    layout = makeLayout();
    assert(BootloaderInterface_Init(&bl, &layout, &platform, &factory,
                                    0U, BL_NO_SLOT, BL_NO_SLOT));
    assert(!BootloaderInterface_EraseArea(&bl, 0U));
    assert(!BootloaderInterface_WriteChunk(&bl, 0U, 0U, data, sizeof(data)));
    assert(!BootloaderInterface_WriteChunk(&bl, 1U, 1U, data, sizeof(data)));
    assert(!BootloaderInterface_WriteChunk(&bl, 1U, 0U, data, 7U));
    assert(bl.write_slot == BL_NO_SLOT);
    puts("bootloader layout: overlap rejection and active-slot/alignment guards OK");
}

static void testSuccessfulUpdateAndRollback(void)
{
    static const uint8_t image[] = {
        0x10U, 0x21U, 0x32U, 0x43U, 0x54U, 0x65U, 0x76U, 0x87U,
        0x98U, 0xA9U, 0xBAU, 0xCBU, 0xDCU, 0xEDU, 0xFEU, 0x0FU,
        0x11U, 0x22U, 0x33U
    };
    FakeFlash memory;
    BootloaderInterface bl;
    FirmwareUpdate update;
    BlImageManifest manifest = makeManifest(image, sizeof(image));
    assert(initialize(&memory, &bl));
    assert(FirmwareUpdate_Init(&update, &bl, 100U));
    assert(FirmwareUpdate_Begin(&update, &manifest, 10U) == FWU_RESULT_NONE);
    assert(update.target_slot == 1U && memory.erase_count == 1U);
    assert(FirmwareUpdate_Write(&update, image, 3U, 11U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Write(&update, &image[3], 8U, 12U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Write(&update, &image[11], 8U, 13U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Finish(&update, 14U) == FWU_RESULT_NONE);
    assert(update.state == FWU_PENDING && memory.pending_count == 1U);
    assert(bl.active_slot == 0U && bl.pending_slot == 1U);
    assert(!BootloaderInterface_WriteChunk(&bl, 1U, 0U, image, TEST_PROGRAM_SIZE));
    assert(!BootloaderInterface_PrepareFirmware(&bl, 1U, &manifest));
    assert(BootloaderInterface_ActivateFirmware(&bl, 1U));
    assert(bl.active_slot == 1U && bl.pending_slot == BL_NO_SLOT && bl.rollback_slot == 0U);
    assert(BootloaderInterface_JumpToApplication(&bl));
    assert(memory.jump_count == 1U);
    assert(BootloaderInterface_RollbackFirmware(&bl));
    assert(bl.active_slot == 0U && memory.rollback_count == 1U);
    puts("bootloader update: segmented writes, CRC/readback, pending, activation, jump and rollback OK");
}

static void testRejectedAndInterruptedUpdates(void)
{
    static const uint8_t image[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U};
    FakeFlash memory;
    BootloaderInterface bl;
    FirmwareUpdate update;
    BlImageManifest manifest = makeManifest(image, sizeof(image));
    uint32_t erases;
    assert(initialize(&memory, &bl));
    assert(FirmwareUpdate_Init(&update, &bl, 10U));
    assert(!FirmwareUpdate_Init(&update, &bl, UINT32_MAX));
    manifest.compatibility.product_code++;
    erases = memory.erase_count;
    assert(FirmwareUpdate_Begin(&update, &manifest, 0U) == FWU_RESULT_INCOMPATIBLE_IMAGE);
    assert(memory.erase_count == erases);
    manifest = makeManifest(image, sizeof(image));
    manifest.signature_size = UINT8_MAX;
    assert(FirmwareUpdate_Begin(&update, &manifest, 0U) == FWU_RESULT_INCOMPATIBLE_IMAGE);
    assert(memory.erase_count == erases);
    manifest = makeManifest(image, sizeof(image));
    memory.fail_manifest = true;
    assert(FirmwareUpdate_Begin(&update, &manifest, 0U) == FWU_RESULT_INCOMPATIBLE_IMAGE);
    assert(memory.erase_count == erases);
    memory.fail_manifest = false;
    assert(FirmwareUpdate_Begin(&update, &manifest, UINT32_MAX - 4U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Write(&update, image, sizeof(image), UINT32_MAX - 2U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Poll(&update, 7U) == FWU_RESULT_TIMEOUT);
    assert(update.state == FWU_FAILED && memory.pending_count == 0U);

    assert(FirmwareUpdate_Begin(&update, &manifest, 20U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Write(&update, image, sizeof(image), 21U) == FWU_RESULT_NONE);
    memory.fail_read = true;
    assert(FirmwareUpdate_Finish(&update, 22U) == FWU_RESULT_READBACK_FAILED);
    assert(memory.pending_count == 0U && bl.active_slot == 0U);
    memory.fail_read = false;

    assert(FirmwareUpdate_Begin(&update, &manifest, 23U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Write(&update, image, 8U, 24U) == FWU_RESULT_NONE);
    assert(!BootloaderInterface_WriteChunk(&bl, update.target_slot, 16U, image, 8U));
    memory.fail_program = true;
    assert(FirmwareUpdate_Write(&update, &image[8], 1U, 25U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Finish(&update, 26U) == FWU_RESULT_WRITE_FAILED);
    assert(bl.write_slot == BL_NO_SLOT && memory.pending_count == 0U);
    memory.fail_program = false;

    assert(FirmwareUpdate_Begin(&update, &manifest, 25U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Write(&update, image, sizeof(image), 26U) == FWU_RESULT_NONE);
    memory.bytes[0x3000U] = 0U;
    assert(FirmwareUpdate_Finish(&update, 27U) == FWU_RESULT_CRC_MISMATCH);
    assert(memory.pending_count == 0U && bl.active_slot == 0U);

    assert(FirmwareUpdate_Begin(&update, &manifest, 28U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Write(&update, image, sizeof(image) + 1U, 29U) == FWU_RESULT_SIZE);
    assert(bl.write_slot == BL_NO_SLOT && memory.pending_count == 0U);

    assert(FirmwareUpdate_Begin(&update, &manifest, 30U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Write(&update, image, sizeof(image), 31U) == FWU_RESULT_NONE);
    memory.fail_pending = true;
    assert(FirmwareUpdate_Finish(&update, 32U) == FWU_RESULT_PENDING_FAILED);
    assert(memory.pending_count == 0U && bl.pending_slot == BL_NO_SLOT);
    assert(bl.recovery_required);
    memory.fail_pending = false;

    assert(reopen(&memory, &bl));
    assert(FirmwareUpdate_Init(&update, &bl, 10U));
    assert(FirmwareUpdate_Begin(&update, &manifest, 40U) == FWU_RESULT_NONE);
    assert(FirmwareUpdate_Cancel(&update) == FWU_RESULT_CANCELLED);
    assert(memory.pending_count == 0U && bl.active_slot == 0U);
    puts("bootloader failure paths: compatibility, authentication, timeout, readback, pending and cancel OK");
}

static void testRestartableSwapAndRollback(bool device_layout)
{
    FakeFlash flash;
    FakeNvm nvm;
    BlLayout layout = makeLayout();
    BlPlatform platform;
    FiData factory = makeFactory();
    BootloaderInterface bootloader;
    BlTransaction transaction;
    PsIo io;
    PsStore store;
    PsData data;
    uint32_t offset_a;
    uint32_t offset_b;
    uint32_t guard = 0U;
    uint32_t i;
    if (device_layout) {
        layout.bootloader.size = DEVICE_BOOT_SIZE;
        layout.slot[0].address = DEVICE_SLOT_A_START;
        layout.slot[0].size = DEVICE_SLOT_SIZE;
        layout.slot[1].address = DEVICE_SLOT_B_START;
        layout.slot[1].size = DEVICE_SLOT_SIZE;
        layout.scratch.address = DEVICE_SCRATCH_START;
        layout.factory.address = DEVICE_FACTORY_START;
        layout.journal.address = DEVICE_NVM_START;
    }
    memset(&flash, 0, sizeof(flash));
    memset(flash.bytes, 0xFF, sizeof(flash.bytes));
    memset(&nvm, 0xFF, sizeof(nvm));
    assert(flashOffset(layout.slot[0].address, layout.slot[0].size, &offset_a));
    assert(flashOffset(layout.slot[1].address, layout.slot[1].size, &offset_b));
    memset(&flash.bytes[offset_a], 0xAA, layout.slot[0].size);
    memset(&flash.bytes[offset_b], 0x55, layout.slot[1].size);
    platform = makePlatform(&flash);
    io.user = &nvm;
    io.read = fakeNvmRead;
    io.erase = fakeNvmErase;
    io.write = fakeNvmWrite;
    Ps_Init(&store, &io);
    assert(Ps_Load(&store, &data) == PS_EMPTY);
    memset(&data, 0, sizeof(data));
    data.pending_slot = 1U;
    data.pending_version = 2U;
    data.update_state = BL_TX_VERIFIED;
    assert(Ps_Save(&store, &data) == PS_OK);
    assert(BootloaderInterface_Init(&bootloader, &layout, &platform, &factory,
                                    0U, BL_NO_SLOT, BL_NO_SLOT));
    BlTransaction_Init(&transaction, &bootloader, &store, &data);
    assert(BlTransaction_ScheduleActivation(&transaction));
    while (BlTransaction_IsBusy(&transaction)) {
        Ps_Init(&store, &io);
        assert(Ps_Load(&store, &data) == PS_OK);
        BlTransaction_Init(&transaction, &bootloader, &store, &data);
        (void)BlTransaction_Poll(&transaction);
        assert(!transaction.failed);
        assert(++guard <= (layout.slot[0].size / layout.erase_unit) * 3U);
    }
    assert(data.update_state == BL_TX_TRIAL);
    assert(flash.bytes[offset_a] == 0x55U);
    assert(flash.bytes[offset_b] == 0xAAU);
    BlTransaction_Init(&transaction, &bootloader, &store, &data);
    assert(BlTransaction_OnStartup(&transaction));
    BlTransaction_Init(&transaction, &bootloader, &store, &data);
    assert(!BlTransaction_OnStartup(&transaction));
    assert(data.update_state == BL_TX_SWAP_ROLLBACK_BASE);
    while (BlTransaction_IsBusy(&transaction)) {
        Ps_Init(&store, &io);
        assert(Ps_Load(&store, &data) == PS_OK);
        BlTransaction_Init(&transaction, &bootloader, &store, &data);
        (void)BlTransaction_Poll(&transaction);
        assert(!transaction.failed);
        assert(++guard <= (layout.slot[0].size / layout.erase_unit) * 6U);
    }
    assert(data.update_state == BL_TX_IDLE);
    assert(data.pending_slot == UINT32_MAX);
    assert(flash.bytes[offset_a] == 0xAAU);
    assert(flash.bytes[offset_b] == 0x55U);
    for (i = 0U; i < layout.slot[0].size; ++i) {
        assert(flash.bytes[offset_a + i] == 0xAAU);
        assert(flash.bytes[offset_b + i] == 0x55U);
    }
    for (i = 0U; i < layout.bootloader.size; ++i) {
        assert(flash.bytes[i] == 0xFFU);
    }
    for (i = layout.factory.address - TEST_FLASH_START; i < TEST_FLASH_SIZE; ++i) {
        assert(flash.bytes[i] == 0xFFU);
    }
    data.update_state = BL_TX_CONFIRMED;
    data.boot_request = 1U;
    data.boot_node_id = 1U;
    data.boot_bitrate = 250000U;
    data.boot_protocol = 2U;
    assert(BlTransaction_OnStartup(&transaction));
    assert(data.update_state == BL_TX_IDLE && data.boot_request == 1U);
    data.update_state = BL_TX_CONFIRMED;
    data.boot_request = 2U;
    assert(Ps_Save(&store, &data) == PS_OK);
    assert(BlTransaction_OnStartup(&transaction));
    assert(data.update_state == BL_TX_IDLE && data.boot_request == 2U);
    puts("A/B restart, trial rollback, and new confirmed-image request preserved OK");
}

static void testDeviceMemoryGeometry(void)
{
    DeviceEraseSpan span;
    uint32_t banks;
    uint32_t offset;
    assert(DEVICE_SLOT_A_START == DEVICE_FLASH_START + DEVICE_BOOT_SIZE);
    assert(DEVICE_SLOT_A_START + DEVICE_SLOT_SIZE == DEVICE_SLOT_B_START);
    assert(DEVICE_SLOT_B_START + DEVICE_SLOT_SIZE == DEVICE_SCRATCH_START);
    assert(DEVICE_SCRATCH_START + DEVICE_FLASH_PAGE_SIZE == DEVICE_FACTORY_START);
    assert(DEVICE_FACTORY_START + DEVICE_FLASH_PAGE_SIZE == DEVICE_NVM_START);
    assert(DEVICE_NVM_START + DEVICE_NVM_SIZE == DEVICE_FLASH_START + DEVICE_FLASH_SIZE);
    assert(DEVICE_IMAGE_MAX == 104U * 1024U && DEVICE_SWAP_STATE_COUNT == 159U);
    for (banks = 1U; banks <= 2U; ++banks) {
        uint32_t bank_size = DEVICE_FLASH_SIZE / banks;
        for (offset = 0U; offset < DEVICE_FLASH_SIZE; offset += DEVICE_FLASH_PAGE_SIZE) {
            uint32_t remaining = DEVICE_FLASH_SIZE - offset;
            assert(DeviceMemory_EraseSpan(offset, remaining, banks, &span));
            assert(span.bank == offset / bank_size + 1U);
            assert(span.page == (offset % bank_size) / DEVICE_FLASH_PAGE_SIZE);
            assert(span.pages > 0U);
            assert((span.page + span.pages) * DEVICE_FLASH_PAGE_SIZE == bank_size);
            assert(span.pages * DEVICE_FLASH_PAGE_SIZE <= remaining);
        }
    }
    assert(DeviceMemory_EraseSpan(0x9000U, DEVICE_SLOT_SIZE, 2U, &span));
    assert(span.bank == 1U && span.page == 18U && span.pages == 46U);
    assert(DeviceMemory_EraseSpan(0x20000U, 0x3800U, 2U, &span));
    assert(span.bank == 2U && span.page == 0U && span.pages == 7U);
    assert(DeviceMemory_EraseSpan(0x3E800U, 0x800U, 1U, &span));
    assert(span.bank == 1U && span.page == 125U && span.pages == 1U);
    assert(DeviceMemory_EraseSpan(0x3E800U, 0x800U, 2U, &span));
    assert(span.bank == 2U && span.page == 61U && span.pages == 1U);
    assert(!DeviceMemory_EraseSpan(0x40000U, 0x800U, 1U, &span));
    assert(!DeviceMemory_EraseSpan(0x3F800U, 0x1000U, 2U, &span));
    assert(!DeviceMemory_EraseSpan(1U, 0x800U, 1U, &span));
    assert(!DeviceMemory_EraseSpan(0U, 1U, 1U, &span));
    assert(!DeviceMemory_EraseSpan(0U, 0U, 1U, &span));
    assert(!DeviceMemory_EraseSpan(0U, 0x800U, 3U, &span));
    assert(!DeviceMemory_EraseSpan(0U, 0x800U, 1U, NULL));
    puts("256 KiB layout and exhaustive single/dual-bank erase planning OK");
}

static void sdoDownload(CoSdo *sdo, uint16_t index, const uint8_t *data, uint32_t size)
{
    CoFrame request = {0};
    CoFrame reply;
    uint32_t offset;
    uint8_t toggle = 0U;
    request.id = 0x601U;
    request.length = 8U;
    request.data[0] = 0x21U;
    Co_Put16(&request.data[1], index);
    Co_Put32(&request.data[4], size);
    assert(CoSdo_Receive(sdo, &request, 10U, &reply));
    assert(reply.id == 0x581U && reply.data[0] == 0x60U);
    for (offset = 0U; offset < size; offset += 7U) {
        uint32_t count = ((size - offset) > 7U) ? 7U : size - offset;
        bool last = (offset + count) == size;
        memset(request.data, 0, sizeof(request.data));
        request.data[0] = (uint8_t)(toggle | ((7U - count) << 1U) | (last ? 1U : 0U));
        memcpy(&request.data[1], &data[offset], count);
        assert(CoSdo_Receive(sdo, &request, 11U + offset, &reply));
        assert(reply.data[0] == (uint8_t)(0x20U | toggle));
        toggle ^= 0x10U;
    }
    assert(sdo->state == CO_SDO_IDLE);
}

static bool discardCanFrame(void *user, const CoFrame *frame)
{
    (void)user;
    (void)frame;
    return true;
}

static void sdoBlockImage(CoSdo *sdo, const uint8_t *image, uint32_t size)
{
    CoFrame request = {0}, reply;
    uint32_t offset;
    request.id = (uint16_t)(0x600U + sdo->node_id); request.length = 8U;
    request.data[0] = 0xC2U;
    Co_Put16(&request.data[1], 0x2002U); request.data[3] = 0U;
    Co_Put32(&request.data[4], size);
    assert(CoSdo_Receive(sdo, &request, 20U, &reply));
    assert(reply.data[0] == 0xA0U && reply.data[4] == 1U);
    for (offset = 0U; offset < size; offset += 7U) {
        uint32_t count = size - offset;
        request.data[0] = (count <= 7U) ? 0x81U : 1U;
        if (count > 7U) { count = 7U; }
        memcpy(&request.data[1], &image[offset], count);
        assert(CoSdo_Receive(sdo, &request, 21U, &reply));
        assert(reply.data[0] == 0xA2U);
    }
    request.data[0] = (uint8_t)(0xC1U | (((7U - size % 7U) % 7U) << 2U));
    assert(CoSdo_Receive(sdo, &request, 22U, &reply));
    assert(reply.data[0] == 0xA1U);
}

static void testObjectDictionaryAndSdo(uint32_t image_size)
{
    FakeFlash flash;
    FakeNvm nvm;
    PsStore store;
    PsData data = {0};
    PsIo io = {&nvm, fakeNvmRead, fakeNvmErase, fakeNvmWrite};
    BootloaderInterface bootloader;
    FirmwareUpdate update;
    FiData factory = makeFactory();
    BootOd od;
    CoSdo sdo;
    CoSdoOps ops;
    CoFrame request = {0};
    CoFrame reply;
    uint8_t image[256] = {0};
    uint8_t record[BL_MANIFEST_RECORD_SIZE];
    uint8_t name[32] = {0};
    uint32_t count = 0U;
    uint8_t toggle = 0U;
    BlImageManifest manifest = makeManifest(image, image_size);
    manifest.signature_size = 64U;
    assert(BlManifest_Encode(&manifest, record));
    assert(initialize(&flash, &bootloader));
    assert(FirmwareUpdate_Init(&update, &bootloader, 5000U));
    memset(&nvm, 0xFF, sizeof(nvm));
    Ps_Init(&store, &io);
    assert(Ps_Load(&store, &data) == PS_EMPTY);
    memset(&data, 0, sizeof(data));
    data.pending_slot = UINT32_MAX;
    data.heartbeat_ms = 1234U;
    assert(Ps_Save(&store, &data) == PS_OK);
    BootOd_Init(&od, &bootloader, &update, NULL, &store, &data, &factory,
                NULL, 1U, true);
    ops = BootOd_SdoOps(&od);
    CoSdo_Init(&sdo, 1U, &ops);
    {
        uint8_t value[4];
        uint32_t width = 0U;
        data.update_state = BL_TX_SWAP_ROLLBACK_BASE;
        assert(BootOd_Read(&od, 0x2001U, 0U, value, &width) == 0U);
        assert(width == 1U && value[0] == 5U);
        data.update_state = BL_TX_IDLE;
    }
    request.id = 0x601U;
    request.length = 8U;
    request.data[0] = 0x40U;
    Co_Put16(&request.data[1], 0x1018U);
    request.data[3] = 4U;
    assert(CoSdo_Receive(&sdo, &request, 0U, &reply));
    assert(reply.data[0] == 0x43U && Co_Get32(&reply.data[4]) == factory.serial);
    Co_Put16(&request.data[1], 0x1017U);
    request.data[3] = 0U;
    assert(CoSdo_Receive(&sdo, &request, 0U, &reply));
    assert(reply.data[0] == 0x4BU && Co_Get16(&reply.data[4]) == 1234U);
    Co_Put16(&request.data[1], 0x1008U);
    assert(CoSdo_Receive(&sdo, &request, 0U, &reply));
    assert(reply.data[0] == 0x41U);
    while (sdo.state == CO_SDO_UPLOAD) {
        uint32_t width;
        request.data[0] = (uint8_t)(0x60U | toggle);
        assert(CoSdo_Receive(&sdo, &request, 1U, &reply));
        width = 7U - ((reply.data[0] >> 1U) & 7U);
        assert(count + width < sizeof(name));
        memcpy(&name[count], &reply.data[1], width);
        count += width;
        toggle ^= 0x10U;
    }
    assert(strcmp((const char *)name, "CANopen Bootloader") == 0);
    request.data[0] = 0x40U;
    assert(CoSdo_Receive(&sdo, &request, 0U, &reply));
    request.data[0] = 0x70U;
    assert(CoSdo_Receive(&sdo, &request, 1U, &reply));
    assert(reply.data[0] == 0x80U && Co_Get32(&reply.data[4]) == CO_ABORT_TOGGLE);
    request.data[0] = 0x40U;
    assert(CoSdo_Receive(&sdo, &request, 0U, &reply));
    assert(CoSdo_Poll(&sdo, 1000U, &reply));
    assert(Co_Get32(&reply.data[4]) == CO_ABORT_TIMEOUT);
    sdoDownload(&sdo, 0x2008U, record, sizeof(record));
    assert(od.manifest_valid && od.manifest.image_size == image_size);
    request.data[0] = 0x2FU;
    Co_Put16(&request.data[1], 0x2000U);
    request.data[4] = 1U;
    assert(CoSdo_Receive(&sdo, &request, 1U, &reply));
    assert(reply.data[0] == 0x60U && update.state == FWU_RECEIVING);
    assert(BootOd_CheckDownload(&od, 0x1F50U, 0U, image_size, true) == CO_ABORT_READ_ONLY);
    assert(BootOd_CheckDownload(&od, 0x1F50U, 2U, image_size, true) == CO_ABORT_SUBINDEX);
    if (image_size <= CO_SDO_BUFFER_SIZE) {
        sdoDownload(&sdo, 0x2002U, image, image_size);
    } else {
        sdoBlockImage(&sdo, image, image_size);
    }
    assert(update.received == image_size);
    assert(FirmwareUpdate_Finish(&update, 1000U) == FWU_RESULT_NONE);
    {
        Canopen node;
        BlTransaction transaction;
        assert(reopen(&flash, &bootloader));
        assert(FirmwareUpdate_Init(&update, &bootloader, 5000U));
        assert(FirmwareUpdate_Begin(&update, &manifest, 0U) == FWU_RESULT_NONE);
        data.update_state = BL_TX_RECEIVING;
        BlTransaction_Init(&transaction, &bootloader, &store, &data);
        assert(Co_Init(&node, 1U, 0U, discardCanFrame, NULL, &bootloader, &update,
                       &transaction, &store, &data, &factory, true, 0U));
        Co_Poll(&node, 4999U);
        assert(update.state == FWU_RECEIVING);
        Co_Poll(&node, 5000U);
        assert(update.state == FWU_FAILED);
        assert(data.update_result == FWU_RESULT_TIMEOUT);
        assert(node.emcy.error_register != 0U);
    }
    puts("OD identity, heartbeat, segmented upload/download, toggle, timeout and image domain OK");
}

static void initContainerFixture(FakeFlash *flash, FakeNvm *nvm, PsStore *store,
                                 PsData *persistent,
                                 BootloaderInterface *bootloader,
                                 FirmwareUpdate *update, FiData *factory,
                                 BootOd *od, CoSdo *sdo)
{
    PsIo io = {nvm, fakeNvmRead, fakeNvmErase, fakeNvmWrite};
    CoSdoOps ops;
    assert(initialize(flash, bootloader));
    flash->time_enabled = true;
    assert(reopen(flash, bootloader));
    memset(nvm, 0xFF, sizeof(*nvm));
    memset(persistent, 0, sizeof(*persistent));
    persistent->pending_slot = UINT32_MAX;
    persistent->heartbeat_ms = 1000U;
    Ps_Init(store, &io);
    assert(Ps_Load(store, persistent) == PS_EMPTY);
    memset(persistent, 0, sizeof(*persistent));
    persistent->pending_slot = UINT32_MAX;
    persistent->heartbeat_ms = 1000U;
    assert(Ps_Save(store, persistent) == PS_OK);
    flash->persistent = persistent;
    *factory = makeFactory();
    assert(FirmwareUpdate_Init(update, bootloader, 5000U));
    BootOd_Init(od, bootloader, update, NULL, store, persistent, factory,
                NULL, 126U, true);
    ops = BootOd_SdoOps(od);
    CoSdo_Init(sdo, 126U, &ops);
}

static void makeContainer(const uint8_t *image, uint32_t image_size,
                          BlImageManifest *manifest,
                          uint8_t record[BL_MANIFEST_RECORD_SIZE],
                          uint8_t *container)
{
    *manifest = makeManifest(image, image_size);
    assert(BlManifest_Encode(manifest, record));
    memcpy(container, record, BL_MANIFEST_RECORD_SIZE);
    memcpy(&container[BL_MANIFEST_RECORD_SIZE], image, image_size);
}

static bool startContainer(CoSdo *sdo, uint32_t total_size, uint32_t now,
                           CoFrame *reply)
{
    CoFrame request = {0};
    request.id = 0x67EU;
    request.length = 8U;
    request.data[0] = 0x21U;
    Co_Put16(&request.data[1], 0x1F50U);
    request.data[3] = 1U;
    Co_Put32(&request.data[4], total_size);
    return CoSdo_Receive(sdo, &request, now, reply);
}

static uint32_t sendContainerSegments(CoSdo *sdo, const uint8_t *container,
                                      uint32_t total_size, uint32_t limit,
                                      uint32_t now)
{
    CoFrame request = {0}, reply;
    uint32_t offset;
    uint8_t toggle = 0U;
    if (!startContainer(sdo, total_size, now, &reply)) {
        return UINT32_MAX;
    }
    if (reply.data[0] == 0x80U) {
        return Co_Get32(&reply.data[4]);
    }
    for (offset = 0U; offset < limit; offset += 7U) {
        uint32_t count = ((total_size - offset) > 7U) ?
            7U : total_size - offset;
        bool last = (offset + count) == total_size;
        request.id = 0x67EU;
        request.length = 8U;
        memset(request.data, 0, sizeof(request.data));
        request.data[0] = (uint8_t)(toggle |
            ((7U - count) << 1U) | (last ? 1U : 0U));
        memcpy(&request.data[1], &container[offset], count);
        if (!CoSdo_Receive(sdo, &request, now + offset + 1U, &reply)) {
            return UINT32_MAX;
        }
        if (reply.data[0] == 0x80U) {
            return Co_Get32(&reply.data[4]);
        }
        toggle ^= 0x10U;
    }
    return 0U;
}

static uint16_t testBlockCrc(uint16_t crc, const uint8_t *data, uint32_t size)
{
    uint32_t i;
    for (i = 0U; i < size; ++i) {
        uint8_t bit;
        crc ^= (uint16_t)((uint16_t)data[i] << 8U);
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (uint16_t)(((crc & 0x8000U) != 0U) ?
                ((uint32_t)crc << 1U) ^ 0x1021U : (uint32_t)crc << 1U);
        }
    }
    return crc;
}

static uint32_t sendContainerBlock(CoSdo *sdo, const uint8_t *container,
                                   uint32_t size, bool corrupt_crc)
{
    CoFrame request = {0}, reply;
    uint32_t offset;
    uint16_t crc = 0U;
    request.id = 0x67EU;
    request.length = 8U;
    request.data[0] = 0xC6U;
    Co_Put16(&request.data[1], 0x1F50U);
    request.data[3] = 1U;
    Co_Put32(&request.data[4], size);
    assert(CoSdo_Receive(sdo, &request, 0U, &reply));
    if (reply.data[0] == 0x80U) {
        return Co_Get32(&reply.data[4]);
    }
    for (offset = 0U; offset < size; offset += 7U) {
        uint32_t count = size - offset;
        bool last;
        if (count > 7U) { count = 7U; }
        last = offset + count == size;
        crc = testBlockCrc(crc, &container[offset], count);
        request.data[0] = last ? 0x81U : 1U;
        memcpy(&request.data[1], &container[offset], count);
        if (count < 7U) {
            memset(&request.data[count + 1U], 0, 7U - count);
        }
        assert(CoSdo_Receive(sdo, &request, offset + 1U, &reply));
        if (reply.data[0] == 0x80U) {
            return Co_Get32(&reply.data[4]);
        }
    }
    request.data[0] = (uint8_t)(0xC1U |
        (((7U - size % 7U) % 7U) << 2U));
    Co_Put16(&request.data[1], (uint16_t)(crc ^ (corrupt_crc ? 1U : 0U)));
    assert(CoSdo_Receive(sdo, &request, size + 1U, &reply));
    return (reply.data[0] == 0x80U) ? Co_Get32(&reply.data[4]) : 0U;
}

static void testSignedContainerDownloads(void)
{
    FakeFlash flash;
    FakeNvm nvm;
    PsStore store;
    PsData persistent;
    BootloaderInterface bootloader;
    FirmwareUpdate update;
    FiData factory;
    BootOd od;
    CoSdo sdo;
    CoFrame reply;
    uint8_t image[64];
    uint8_t container[BL_MANIFEST_RECORD_SIZE + sizeof(image)];
    uint8_t record[BL_MANIFEST_RECORD_SIZE];
    BlImageManifest manifest;
    uint32_t abort_code;
    uint32_t i;

    memset(image, 0x5AU, sizeof(image));
    makeContainer(image, sizeof(image), &manifest, record, container);
    initContainerFixture(&flash, &nvm, &store, &persistent, &bootloader,
                         &update, &factory, &od, &sdo);
    flash.now_ms = 100U;
    flash.erase_advance_ms = 1500U;
    abort_code = sendContainerSegments(&sdo, container, sizeof(container),
                                       sizeof(container), 100U);
    assert(abort_code == 0U);
    assert(update.state == FWU_PENDING && persistent.update_state == BL_TX_VERIFIED);
    assert(persistent.pending_slot == 1U && flash.pending_count == 1U);
    assert(flash.activate_count == 0U && flash.erase_count == 1U);
    assert(sdo.state == CO_SDO_IDLE);

    {
        uint8_t small_image[8];
        uint8_t small_container[BL_MANIFEST_RECORD_SIZE + sizeof(small_image)];
        memset(small_image, 0xA6, sizeof(small_image));
        makeContainer(small_image, sizeof(small_image), &manifest, record,
                      small_container);
        initContainerFixture(&flash, &nvm, &store, &persistent, &bootloader,
                             &update, &factory, &od, &sdo);
        assert(sendContainerBlock(&sdo, small_container,
                                  sizeof(small_container), false) == 0U);
        assert(update.state == FWU_PENDING &&
               persistent.update_state == BL_TX_VERIFIED);
        assert(flash.pending_count == 1U && flash.activate_count == 0U);

        initContainerFixture(&flash, &nvm, &store, &persistent, &bootloader,
                             &update, &factory, &od, &sdo);
        assert(sendContainerBlock(&sdo, small_container,
                                  sizeof(small_container), true) == CO_ABORT_CRC);
        assert(update.state == FWU_IDLE && persistent.update_state == BL_TX_IDLE);
        assert(flash.pending_count == 0U && flash.activate_count == 0U);
    }

    for (i = 0U; i < 5U; ++i) {
        makeContainer(image, sizeof(image), &manifest, record, container);
        if (i == 0U) {
            record[140] ^= 1U;
            memcpy(container, record, sizeof(record));
        } else if (i == 1U) {
            flash.fail_manifest = true;
        } else if (i == 2U) {
            manifest.image_size--;
            assert(BlManifest_Encode(&manifest, record));
            memcpy(container, record, sizeof(record));
        } else if (i == 3U) {
            manifest.compatibility.hardware_min = UINT32_C(0x00010004);
            manifest.compatibility.hardware_max = UINT32_C(0x00010004);
            assert(BlManifest_Encode(&manifest, record));
            memcpy(container, record, sizeof(record));
        } else {
            record[0] ^= 1U;
            Co_Put32(&record[140], Ps_Crc(record, 140U));
            memcpy(container, record, sizeof(record));
        }
        initContainerFixture(&flash, &nvm, &store, &persistent, &bootloader,
                             &update, &factory, &od, &sdo);
        if (i == 1U) {
            flash.fail_manifest = true;
        }
        abort_code = sendContainerSegments(&sdo, container, sizeof(container),
                                           sizeof(container), 0U);
        assert(abort_code == ((i == 2U) ? CO_ABORT_LENGTH : CO_ABORT_VALUE));
        assert(flash.erase_count == 0U && persistent.update_state == BL_TX_IDLE);
        assert(!od.container_active && sdo.state == CO_SDO_IDLE);
    }

    makeContainer(image, sizeof(image), &manifest, record, container);
    container[BL_MANIFEST_RECORD_SIZE + 5U] ^= 0x01U;
    initContainerFixture(&flash, &nvm, &store, &persistent, &bootloader,
                         &update, &factory, &od, &sdo);
    assert(sendContainerSegments(&sdo, container, sizeof(container),
                                 sizeof(container), 0U) == CO_ABORT_HARDWARE);
    assert(flash.erase_count == 1U && flash.pending_count == 0U);
    assert(persistent.update_state == BL_TX_IDLE && update.state == FWU_FAILED);
    assert(flash.activate_count == 0U);

    makeContainer(image, sizeof(image), &manifest, record, container);
    initContainerFixture(&flash, &nvm, &store, &persistent, &bootloader,
                         &update, &factory, &od, &sdo);
    flash.now_ms = 100U;
    flash.erase_advance_ms = 1500U;
    assert(sendContainerSegments(&sdo, container, sizeof(container), 154U,
                                 100U) == 0U);
    assert(sdo.last_activity == 1600U);
    assert(!CoSdo_Poll(&sdo, sdo.last_activity + 999U, &reply));
    assert(CoSdo_Poll(&sdo, sdo.last_activity + sdo.timeout_ms, &reply));
    assert(Co_Get32(&reply.data[4]) == CO_ABORT_TIMEOUT);
    assert(update.state == FWU_IDLE && persistent.update_state == BL_TX_IDLE);
    assert(flash.pending_count == 0U && flash.activate_count == 0U);

    makeContainer(image, sizeof(image), &manifest, record, container);
    initContainerFixture(&flash, &nvm, &store, &persistent, &bootloader,
                         &update, &factory, &od, &sdo);
    assert(startContainer(&sdo, sizeof(container), 0U, &reply));
    {
        CoFrame request = {0};
        request.id = 0x67EU;
        request.length = 8U;
        request.data[0] = 0x80U;
        Co_Put16(&request.data[1], 0x1F50U);
        request.data[3] = 1U;
        assert(!CoSdo_Receive(&sdo, &request, 1U, &reply));
    }
    assert(!od.container_active && flash.erase_count == 0U);
    assert(persistent.update_state == BL_TX_IDLE);
    puts("Signed 1F50 container: segmented/block, small images, validation-before-erase, CRC, cancel and timeout recovery OK");
}

static bool failingNvmWrite(void *user, uint8_t page, uint32_t offset,
                            const uint8_t *data, uint32_t size)
{
    (void)user; (void)page; (void)offset; (void)data; (void)size;
    return false;
}

static uint32_t readDiagnostic(CoSdo *sdo, uint8_t subindex)
{
    CoFrame request = {0}, reply;
    request.id = 0x67EU;
    request.length = 8U;
    request.data[0] = 0x40U;
    Co_Put16(&request.data[1], 0x2010U);
    request.data[3] = subindex;
    assert(CoSdo_Receive(sdo, &request, 0U, &reply));
    assert(reply.data[0] == ((subindex == 0U) ? 0x4FU : 0x43U));
    return (subindex == 0U) ? reply.data[4] : Co_Get32(&reply.data[4]);
}

static void testReceivePollAfterErase(void)
{
    uint32_t mode, wrap, outcome;
    for (mode = 0U; mode < 2U; ++mode) {
        for (wrap = 0U; wrap < 2U; ++wrap) {
            for (outcome = 0U; outcome < 3U; ++outcome) {
                FakeFlash flash;
                FakeNvm nvm;
                PsStore store;
                PsData persistent;
                BootloaderInterface bootloader;
                FirmwareUpdate update;
                FiData factory;
                BootOd od;
                CoSdo sdo;
                Canopen node;
                BlTransaction transaction;
                CoFrame request = {0};
                uint8_t image[256] = {0};
                uint8_t record[BL_MANIFEST_RECORD_SIZE];
                uint8_t container[BL_MANIFEST_RECORD_SIZE + sizeof(image)];
                BlImageManifest manifest;
                uint16_t crc = 0U;
                uint32_t offset;
                uint32_t activity;
                uint8_t toggle = 0U;
                initContainerFixture(&flash, &nvm, &store, &persistent,
                                     &bootloader, &update, &factory, &od, &sdo);
                assert(FirmwareUpdate_Init(&update, &bootloader, 15000U));
                flash.now_ms = wrap ? UINT32_MAX - 200U : 100U;
                flash.erase_advance_ms = 2455U;
                BlTransaction_Init(&transaction, &bootloader, &store, &persistent);
                assert(Co_Init(&node, 126U, 0U, discardCanFrame, NULL,
                               &bootloader, &update, &transaction, &store,
                               &persistent, &factory, true, flash.now_ms));
                Co_Poll(&node, flash.now_ms); /* Send boot-up before SDO. */
                makeContainer(image, sizeof(image), &manifest, record, container);
                request.id = 0x67EU;
                request.length = 8U;
                request.data[0] = mode ? 0xC6U : 0x21U;
                Co_Put16(&request.data[1], 0x1F50U);
                request.data[3] = 1U;
                Co_Put32(&request.data[4], sizeof(container));
                assert(Co_Receive(&node, &request, flash.now_ms));
                Co_Poll(&node, flash.now_ms);
                for (offset = 0U; offset < sizeof(container); offset += 7U) {
                    uint32_t before;
                    uint32_t count = sizeof(container) - offset;
                    bool last;
                    if (count > 7U) { count = 7U; }
                    last = offset + count == sizeof(container);
                    flash.now_ms++;
                    before = flash.now_ms;
                    memset(request.data, 0, sizeof(request.data));
                    request.data[0] = mode ? (last ? 0x81U : 1U) :
                        (uint8_t)(toggle | ((7U - count) << 1U) | (last ? 1U : 0U));
                    memcpy(&request.data[1], &container[offset], count);
                    crc = testBlockCrc(crc, &container[offset], count);
                    assert(Co_Receive(&node, &request, before));
                    assert(node.pending_tx.data[0] != 0x80U);
                    /* Reproduce the old main loop's pre-erase timestamp. */
                    Co_Poll(&node, before);
                    assert(update.result == FWU_RESULT_NONE);
                    assert(node.sdo.state != CO_SDO_IDLE ||
                           (!mode && last));
                    if (offset == 147U) {
                        assert(update.received == 2U && flash.program_count == 0U);
                        assert(update.state == FWU_RECEIVING);
                        activity = flash.now_ms;
                        assert(update.last_activity == activity);
                        assert(node.sdo.last_activity == activity);
                        if (outcome == 1U) {
                            flash.now_ms = activity + 999U;
                            Co_Poll(&node, before);
                            assert(node.sdo.state != CO_SDO_IDLE);
                            flash.now_ms++;
                            Co_Poll(&node, before);
                            assert(node.sdo.state == CO_SDO_IDLE);
                            assert(Co_Get32(&node.pending_tx.data[4]) == CO_ABORT_TIMEOUT);
                            assert(update.result == FWU_RESULT_CANCELLED);
                            break;
                        }
                        if (outcome == 2U) {
                            CoSdo_Reset(&node.sdo);
                            flash.now_ms = activity + 14999U;
                            Co_Poll(&node, before);
                            assert(update.state == FWU_RECEIVING);
                            flash.now_ms++;
                            Co_Poll(&node, before);
                            assert(update.state == FWU_FAILED);
                            assert(update.result == FWU_RESULT_TIMEOUT);
                            assert(persistent.update_result == FWU_RESULT_TIMEOUT);
                            break;
                        }
                    }
                    toggle ^= 0x10U;
                }
                if (outcome == 0U) {
                    if (mode != 0U) {
                        request.data[0] = (uint8_t)(0xC1U |
                            (((7U - sizeof(container) % 7U) % 7U) << 2U));
                        Co_Put16(&request.data[1], crc);
                        flash.now_ms++;
                        assert(Co_Receive(&node, &request, flash.now_ms));
                        assert(node.pending_tx.data[0] == 0xA1U);
                        Co_Poll(&node, flash.now_ms);
                    }
                    assert(update.state == FWU_PENDING);
                    assert(update.received == sizeof(image));
                    assert(flash.program_count != 0U && flash.pending_count == 1U);
                    assert(flash.activate_count == 0U);
                }
            }
        }
    }
    puts("Receive -> 2455 ms erase -> stale poll: segmented/block, wrap, full transfer and real timeout boundaries OK");
}

static void testProgramStartCommand(void)
{
    uint32_t path, mode;
    for (path = 0U; path < 2U; ++path) {
        for (mode = 0U; mode < 13U; ++mode) {
            FakeFlash flash;
            FakeNvm nvm;
            PsStore store;
            PsData persistent;
            BootloaderInterface bootloader;
            FirmwareUpdate update;
            FiData factory;
            BootOd od;
            CoSdo sdo;
            BlTransaction transaction;
            CoFrame request = {0}, reply;
            uint8_t image[256];
            uint8_t record[BL_MANIFEST_RECORD_SIZE];
            uint8_t container[BL_MANIFEST_RECORD_SIZE + sizeof(image)];
            BlImageManifest manifest;
            uint32_t sequence;
            uint32_t bank_programs;
            memset(image, 0x5AU, sizeof(image));
            initContainerFixture(&flash, &nvm, &store, &persistent,
                                 &bootloader, &update, &factory, &od, &sdo);
            BlTransaction_Init(&transaction, &bootloader, &store, &persistent);
            od.transaction = &transaction;
            assert(BootOd_CheckDownload(&od, 0x1F51U, 0U, 1U, true) == CO_ABORT_READ_ONLY);
            assert(BootOd_CheckDownload(&od, 0x1F51U, 2U, 1U, true) == CO_ABORT_SUBINDEX);
            assert(BootOd_CheckDownload(&od, 0x1F51U, 1U, 4U, true) == CO_ABORT_LENGTH);
            {
                uint8_t bytes[4];
                uint32_t size;
                assert(BootOd_Read(&od, 0x1F51U, 0U, bytes, &size) == 0U);
                assert(size == 1U && bytes[0] == 1U);
                assert(BootOd_Read(&od, 0x1F51U, 1U, bytes, &size) == CO_ABORT_WRITE_ONLY);
                assert(BootOd_Read(&od, 0x1F51U, 2U, bytes, &size) == CO_ABORT_SUBINDEX);
            }
            makeContainer(image, sizeof(image), &manifest, record, container);
            if (mode != 1U) {
                assert(sendContainerBlock(&sdo, container, sizeof(container), false) == 0U);
                assert(persistent.update_state == BL_TX_VERIFIED);
            }
            if (mode == 1U) { update.state = FWU_RECEIVING; }
            if (mode == 2U) { persistent.update_state = BL_TX_IDLE;
                persistent.update_result = FWU_RESULT_IMAGE_REJECTED; }
            if (mode == 3U) { flash.diagnostics.value[BL_DIAG_OPERATION] = 7U; }
            if (mode == 4U) { od.update_auth_available = false; }
            if (mode == 5U) { store.io.write = failingNvmWrite; }
            if (mode == 6U) { od.container_active = true; }
            if (mode == 8U) { update.state = FWU_IDLE; od.manifest_valid = false; }
            if (mode == 9U) { persistent.pending_slot = UINT32_MAX; }
            if (mode == 10U) { bootloader.pending_slot = BL_NO_SLOT; }
            if (mode == 11U) { update.state = FWU_FAILED; }
            if (mode == 12U) { transaction.failed = true; }
            sequence = store.sequence;
            bank_programs = flash.program_count;
            request.id = 0x67EU;
            request.length = 8U;
            request.data[0] = 0x2FU;
            Co_Put16(&request.data[1], path ? 0x2000U : 0x1F51U);
            request.data[3] = path ? 0U : 1U;
            request.data[4] = path ? 3U : ((mode == 7U) ? 2U : 1U);
            /* Vendor path has its own invalid-command handling; exercise value 2 on 1F51 only. */
            if (mode == 7U && path) { request.data[4] = 0xFFU; }
            assert(CoSdo_Receive(&sdo, &request, 0U, &reply));
            if (mode == 0U || mode == 8U) {
                assert(reply.data[0] == 0x60U);
                assert(Co_Get16(&reply.data[1]) == (path ? 0x2000U : 0x1F51U));
                assert(store.sequence == sequence + 1U);
                assert(persistent.update_state == BL_TX_SWAP_FORWARD_BASE);
                assert(BlTransaction_IsBusy(&transaction));
                assert(CoSdo_Receive(&sdo, &request, 0U, &reply));
                assert(reply.data[0] == 0x80U);
                assert(store.sequence == sequence + 1U);
                /* Resume the persisted swap with reset-time runtime objects. */
                assert(Ps_Load(&store, &persistent) == PS_OK);
                BlTransaction_Init(&transaction, &bootloader, &store, &persistent);
                while (BlTransaction_IsBusy(&transaction)) {
                    assert(BlTransaction_Poll(&transaction));
                }
                assert(persistent.update_state == BL_TX_TRIAL);
                assert(memcmp(&flash.bytes[bootloader.layout.slot[0].address -
                                          TEST_FLASH_START], image, sizeof(image)) == 0);
            } else {
                assert(reply.data[0] == 0x80U);
                assert(store.sequence == sequence);
                assert(flash.program_count == bank_programs);
                if (mode == 3U || mode == 5U || mode == 12U) {
                    assert(Co_Get32(&reply.data[4]) == CO_ABORT_HARDWARE);
                }
            }
            assert(flash.activate_count == 0U);
        }
    }
    puts("1F51:01=1 / vendor activation: verified-only gate, invalid/fault rejection and restartable A/B swap OK");
}

static void testContainerFailureDiagnostics(void)
{
    FakeFlash flash;
    FakeNvm nvm;
    PsStore store;
    PsData persistent;
    BootloaderInterface bootloader;
    FirmwareUpdate update;
    FiData factory;
    BootOd od;
    CoSdo sdo;
    uint8_t image[28556];
    uint8_t container[BL_MANIFEST_RECORD_SIZE + sizeof(image)];
    uint8_t record[BL_MANIFEST_RECORD_SIZE];
    BlImageManifest manifest;
    uint32_t mode;
    memset(image, 0x5AU, sizeof(image));
    makeContainer(image, sizeof(image), &manifest, record, container);
    for (mode = 0U; mode < 3U; ++mode) {
        initContainerFixture(&flash, &nvm, &store, &persistent, &bootloader,
                             &update, &factory, &od, &sdo);
        {
            BlLayout layout = makeLayout();
            BlPlatform platform = makePlatform(&flash);
            layout.bootloader.size = DEVICE_BOOT_SIZE;
            layout.slot[0].address = DEVICE_SLOT_A_START;
            layout.slot[1].address = DEVICE_SLOT_B_START;
            layout.slot[0].size = DEVICE_SLOT_SIZE;
            layout.slot[1].size = DEVICE_SLOT_SIZE;
            layout.scratch.address = DEVICE_SCRATCH_START;
            layout.factory.address = DEVICE_FACTORY_START;
            layout.journal.address = DEVICE_NVM_START;
            assert(BootloaderInterface_Init(&bootloader, &layout, &platform,
                                            &factory, 0U, BL_NO_SLOT, BL_NO_SLOT));
        }
        if (mode == 0U) {
            store.io.write = failingNvmWrite;
        } else if (mode == 1U) {
            flash.fail_erase = true;
        } else {
            flash.now_ms = UINT32_MAX - 200U;
            flash.erase_advance_ms = 1500U;
        }
        assert(sendContainerBlock(&sdo, container, sizeof(container), false) ==
               ((mode == 2U) ? 0U : CO_ABORT_HARDWARE));
        assert(readDiagnostic(&sdo, 0U) == 14U);
        assert(readDiagnostic(&sdo, 1U) == ((mode == 0U) ? 3U :
                                         ((mode == 1U) ? 6U : 10U)));
        assert(readDiagnostic(&sdo, 2U) == ((mode == 0U) ? PS_IO_ERROR : PS_OK));
        assert(readDiagnostic(&sdo, 13U) ==
               ((mode == 1U) ? FWU_RESULT_ERASE_FAILED : FWU_RESULT_NONE));
        assert(readDiagnostic(&sdo, 14U) == ((mode == 2U) ? sizeof(image) : 0U));
        assert(flash.pending_count == ((mode == 2U) ? 1U : 0U));
        assert(flash.activate_count == 0U);
        assert(BootOd_CheckDownload(&od, 0x2010U, 1U, 4U, true) ==
               CO_ABORT_READ_ONLY);
        {
            uint8_t bytes[4];
            uint32_t size;
            assert(BootOd_Read(&od, 0x2010U, 15U, bytes, &size) ==
                   CO_ABORT_SUBINDEX);
        }
        if (mode == 1U) {
            flash.fail_erase = false;
            assert(sendContainerBlock(&sdo, container, sizeof(container), false) == 0U);
            assert(readDiagnostic(&sdo, 1U) == 10U);
            assert(readDiagnostic(&sdo, 13U) == FWU_RESULT_NONE);
        }
    }
    initContainerFixture(&flash, &nvm, &store, &persistent, &bootloader,
                         &update, &factory, &od, &sdo);
    {
        uint8_t value[16];
        uint32_t size;
        assert(BootOd_Read(&od, 0x100AU, 0U, value, &size) == 0U);
        assert(size == 5U && memcmp(value, "1.0.6", 5U) == 0);
        assert(BootOd_Read(&od, 0x200DU, 0U, value, &size) == 0U);
        assert(size == 4U && Co_Get32(value) == UINT32_C(0x00010006));
    }
    flash.diagnostics.value[BL_DIAG_STAGE] = 3U;
    flash.diagnostics.value[BL_DIAG_OPERATION] = 0x00030003U;
    flash.diagnostics.value[BL_DIAG_STATUS] = 3U;
    flash.diagnostics.value[BL_DIAG_SR] = 0x40000U;
    {
        BlDiagnostics first = flash.diagnostics;
        FakeNvm before = nvm;
        uint8_t command = 3U;
        persistent.update_state = BL_TX_VERIFIED;
        assert(BootOd_Write(&od, 0x2000U, 0U, &command, 1U, 0U) ==
               CO_ABORT_HARDWARE);
        persistent.update_state = BL_TX_IDLE;
        assert(sendContainerBlock(&sdo, container, sizeof(container), false) ==
               CO_ABORT_HARDWARE);
        BlDiagnosticStage(&bootloader, 99U);
        assert(memcmp(&first, &flash.diagnostics, sizeof(first)) == 0);
        assert(memcmp(&before, &nvm, sizeof(nvm)) == 0);
        assert(flash.erase_count == 0U && flash.program_count == 0U &&
               flash.activate_count == 0U);
        assert(readDiagnostic(&sdo, 3U) == 0x00030003U);
        assert(readDiagnostic(&sdo, 6U) == 0x40000U);
    }
    puts("28708-byte container: diagnostics, sticky hardware fault/activation gate, retry and tick wrap OK");
}

typedef struct {
    uint8_t data[DEVICE_IMAGE_MAX];
    uint32_t written;
    uint32_t completed;
    uint32_t aborted;
    bool fail_write;
} BlockSink;

static uint32_t blockCheck(void *user, uint16_t index, uint8_t sub,
                           uint32_t size, bool known)
{
    (void)user;
    return (index == 0x1F50U && sub == 1U && known &&
            size > 0U && size <= DEVICE_IMAGE_MAX) ? 0U : CO_ABORT_LENGTH;
}

static uint32_t blockBegin(void *user, uint16_t index, uint8_t sub,
                           uint32_t size, uint32_t now)
{
    (void)now;
    return blockCheck(user, index, sub, size, true);
}

static uint32_t blockWrite(void *user, uint16_t index, uint8_t sub,
                           const uint8_t *data, uint32_t size, uint32_t now)
{
    BlockSink *sink = (BlockSink *)user;
    (void)index; (void)sub; (void)now;
    if (sink->fail_write) { return CO_ABORT_HARDWARE; }
    assert(size <= sizeof(sink->data) - sink->written);
    memcpy(&sink->data[sink->written], data, size);
    sink->written += size;
    return 0U;
}

static uint32_t blockEnd(void *user, uint16_t index, uint8_t sub,
                         uint32_t size, uint32_t now)
{
    BlockSink *sink = (BlockSink *)user;
    (void)index; (void)sub; (void)now;
    assert(sink->written == size);
    sink->completed++;
    return 0U;
}

static void blockAbort(void *user, uint16_t index, uint8_t sub)
{
    BlockSink *sink = (BlockSink *)user;
    (void)index; (void)sub;
    sink->aborted++;
}

static void initBlockTest(CoSdo *sdo, BlockSink *sink)
{
    CoSdoOps ops = {0};
    memset(sink, 0, sizeof(*sink));
    ops.user = sink;
    ops.check_download = blockCheck;
    ops.write = blockWrite;
    ops.stream_begin = blockBegin;
    ops.stream_write = blockWrite;
    ops.stream_end = blockEnd;
    ops.stream_abort = blockAbort;
    CoSdo_Init(sdo, 126U, &ops);
}

static void blockStart(CoSdo *sdo, uint32_t size, bool crc)
{
    CoFrame request = {0}, reply;
    request.id = 0x67EU; request.length = 8U;
    request.data[0] = crc ? 0xC6U : 0xC2U;
    Co_Put16(&request.data[1], 0x1F50U); request.data[3] = 1U;
    Co_Put32(&request.data[4], size);
    assert(CoSdo_Receive(sdo, &request, 0U, &reply));
    assert(reply.id == 0x5FEU && reply.data[0] == (crc ? 0xA4U : 0xA0U));
    assert(Co_Get16(&reply.data[1]) == 0x1F50U && reply.data[3] == 1U);
    assert(reply.data[4] == 1U);
}

static void testBlockDownload(void)
{
    CoSdo sdo;
    BlockSink sink;
    CoFrame request = {0}, reply;
    uint32_t size;
    request.id = 0x67EU; request.length = 8U;
    for (size = 1U; size <= 257U; ++size) {
        uint32_t transfer_size = (size == 257U) ? DEVICE_IMAGE_MAX : size;
        uint32_t offset;
        initBlockTest(&sdo, &sink);
        blockStart(&sdo, transfer_size, false);
        for (offset = 0U; offset < transfer_size; offset += 7U) {
            uint32_t i, count = transfer_size - offset;
            bool last = count <= 7U;
            if (count > 7U) { count = 7U; }
            request.data[0] = last ? 0x81U : 1U;
            memset(&request.data[1], 0xEE, 7U);
            for (i = 0U; i < count; ++i) {
                request.data[i + 1U] = (uint8_t)(offset + i);
            }
            assert(CoSdo_Receive(&sdo, &request, 1U, &reply));
            assert(reply.data[0] == 0xA2U && reply.data[1] == 1U &&
                   reply.data[2] == 1U);
        }
        assert(sdo.state == CO_SDO_BLOCK_END && sink.completed == 0U);
        request.data[0] = (uint8_t)(0xC1U | (((7U - transfer_size % 7U) % 7U) << 2U));
        assert(CoSdo_Receive(&sdo, &request, 2U, &reply));
        assert(reply.data[0] == 0xA1U && sdo.state == CO_SDO_IDLE);
        assert(sink.written == transfer_size && sink.aborted == 0U);
        for (offset = 0U; offset < transfer_size; ++offset) {
            assert(sink.data[offset] == (uint8_t)offset);
        }
        assert(sink.completed == 1U);
    }
    /* CRC-16/XMODEM known vector, not a copy of the production CRC routine. */
    for (size = 0U; size < 2U; ++size) {
        initBlockTest(&sdo, &sink);
        blockStart(&sdo, 9U, true);
        request.data[0] = 1U; memcpy(&request.data[1], "1234567", 7U);
        assert(CoSdo_Receive(&sdo, &request, 1U, &reply));
        request.data[0] = 0x81U; memcpy(&request.data[1], "89xxxxx", 7U);
        assert(CoSdo_Receive(&sdo, &request, 2U, &reply));
        request.data[0] = 0xD5U;
        Co_Put16(&request.data[1], (uint16_t)(0x31C3U ^ size));
        assert(CoSdo_Receive(&sdo, &request, 3U, &reply));
        assert(reply.data[0] == ((size == 0U) ? 0xA1U : 0x80U));
        assert(sink.written == 9U);
        if (size != 0U) { assert(Co_Get32(&reply.data[4]) == CO_ABORT_CRC); }
    }
    for (size = 0U; size < 5U; ++size) {
        uint32_t offset;
        initBlockTest(&sdo, &sink);
        blockStart(&sdo, 256U, true);
        memset(&request.data[1], 0, 7U);
        for (offset = 0U; offset < 256U; offset += 7U) {
            request.data[0] = (offset + 7U >= 256U) ? 0x81U : 1U;
            assert(CoSdo_Receive(&sdo, &request, 1U, &reply));
            assert(reply.data[0] == 0xA2U);
        }
        assert(sink.written == 256U && sink.completed == 0U);
        request.data[0] = (size == 2U) ? 0xC1U : 0xCDU;
        Co_Put16(&request.data[1], (size == 1U) ? 1U : 0U);
        if (size == 3U) {
            assert(CoSdo_Poll(&sdo, 1001U, &reply));
            assert(Co_Get32(&reply.data[4]) == CO_ABORT_TIMEOUT);
        } else if (size == 4U) {
            request.data[0] = 0x80U;
            Co_Put16(&request.data[1], 0x1F50U); request.data[3] = 1U;
            assert(!CoSdo_Receive(&sdo, &request, 2U, &reply));
        } else {
            assert(CoSdo_Receive(&sdo, &request, 2U, &reply));
            assert(reply.data[0] == ((size == 0U) ? 0xA1U : 0x80U));
            if (size == 1U) { assert(Co_Get32(&reply.data[4]) == CO_ABORT_CRC); }
            if (size == 2U) { assert(Co_Get32(&reply.data[4]) == CO_ABORT_LENGTH); }
        }
        assert(sdo.state == CO_SDO_IDLE);
        assert(sink.completed == ((size == 0U) ? 1U : 0U));
        assert(sink.aborted == ((size == 0U) ? 0U : 1U));
    }
    for (size = 0U; size < 5U; ++size) {
        initBlockTest(&sdo, &sink);
        blockStart(&sdo, 256U, true);
        if (size == 0U) {
            request.data[0] = 2U;
            assert(CoSdo_Receive(&sdo, &request, 1U, &reply));
            assert(Co_Get32(&reply.data[4]) == CO_ABORT_SEQUENCE);
        } else if (size == 1U) {
            assert(CoSdo_Poll(&sdo, 1000U, &reply));
            assert(Co_Get32(&reply.data[4]) == CO_ABORT_TIMEOUT);
        } else if (size == 2U) {
            request.data[0] = 0x80U;
            Co_Put16(&request.data[1], 0x1F50U); request.data[3] = 1U;
            assert(!CoSdo_Receive(&sdo, &request, 1U, &reply));
        } else if (size == 3U) {
            request.data[0] = 0x81U;
            assert(CoSdo_Receive(&sdo, &request, 1U, &reply));
            assert(Co_Get32(&reply.data[4]) == CO_ABORT_LENGTH);
        } else {
            sink.fail_write = true; request.data[0] = 1U;
            assert(CoSdo_Receive(&sdo, &request, 1U, &reply));
            assert(Co_Get32(&reply.data[4]) == CO_ABORT_HARDWARE);
        }
        assert(sink.aborted == 1U && sink.completed == 0U && sdo.state == CO_SDO_IDLE);
    }
    puts("SDO block download: sizes 1..256 and 104 KiB, XMODEM CRC, Node 126, sequence/timeout/abort/write errors OK");
}

int main(void)
{
    testSha256();
    testEd25519();
    testPersistentUpdateStateValidation();
    testDeviceMemoryGeometry();
    testRestartableSwapAndRollback(false);
    testRestartableSwapAndRollback(true);
    testLayoutAndGuards();
    testSuccessfulUpdateAndRollback();
    testRejectedAndInterruptedUpdates();
    testObjectDictionaryAndSdo(64U);
    testObjectDictionaryAndSdo(256U);
    testSignedContainerDownloads();
    testReceivePollAfterErase();
    testProgramStartCommand();
    testContainerFailureDiagnostics();
    testBlockDownload();
    puts("All portable bootloader tests passed.");
    return 0;
}
