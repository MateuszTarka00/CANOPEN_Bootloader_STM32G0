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
#define TEST_FLASH_SIZE UINT32_C(0x7000)
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
    data.update_state = UINT32_C(0x261);
    assert(Ps_Validate(&data));
    data.update_state = UINT32_C(0x461);
    assert(Ps_Validate(&data));
    data.update_state = UINT32_C(0x262);
    assert(!Ps_Validate(&data));
    data.update_state = UINT32_C(0x462);
    assert(!Ps_Validate(&data));
    puts("persistent swap state validation boundaries OK");
}

typedef struct {
    uint8_t bytes[TEST_FLASH_SIZE];
    uint32_t erase_count;
    uint32_t program_count;
    uint32_t pending_count;
    uint32_t activate_count;
    uint32_t rollback_count;
    uint32_t jump_count;
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

static BlPlatform makePlatform(FakeFlash *flash)
{
    BlPlatform platform;
    memset(&platform, 0, sizeof(platform));
    platform.user = flash;
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

static void testRestartableSwapAndRollback(void)
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
        assert(++guard < 20U);
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
        assert(++guard < 40U);
    }
    assert(data.update_state == BL_TX_IDLE);
    assert(data.pending_slot == UINT32_MAX);
    assert(flash.bytes[offset_a] == 0xAAU);
    assert(flash.bytes[offset_b] == 0x55U);
    data.update_state = BL_TX_CONFIRMED;
    data.boot_request = 1U;
    data.boot_node_id = 1U;
    data.boot_bitrate = 250000U;
    data.boot_protocol = 2U;
    assert(BlTransaction_OnStartup(&transaction));
    assert(data.update_state == BL_TX_IDLE && data.boot_request == 1U);
    puts("A/B restart, trial rollback, and new confirmed-image request preserved OK");
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
    sdoDownload(&sdo, 0x2002U, image, image_size);
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

int main(void)
{
    testSha256();
    testEd25519();
    testPersistentUpdateStateValidation();
    testRestartableSwapAndRollback();
    testLayoutAndGuards();
    testSuccessfulUpdateAndRollback();
    testRejectedAndInterruptedUpdates();
    testObjectDictionaryAndSdo(64U);
    testObjectDictionaryAndSdo(256U);
    puts("All portable bootloader tests passed.");
    return 0;
}
