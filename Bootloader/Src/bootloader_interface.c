/**
 * @file bootloader_interface.c
 * @brief Range-checked portable bootloader platform adapter.
 * @details Dependency diagram: bootloader_interface -> factory_identity / persistent_storage.
 */
#include "bootloader_interface.h"
#include "persistent_storage.h"
#include "sha256.h"
#include <string.h>

static bool rangeEnd(const BlRange *range, uint32_t *end)
{
    if ((range == NULL) || (end == NULL) || (range->size == 0U) ||
        (range->size > (UINT32_MAX - range->address))) {
        return false;
    }
    *end = range->address + range->size;
    return true;
}

static bool contained(const BlLayout *layout, const BlRange *range)
{
    uint32_t flash_end;
    uint32_t range_end;
    if ((layout->flash_size == 0U) ||
        (layout->flash_size > (UINT32_MAX - layout->flash_start)) ||
        !rangeEnd(range, &range_end)) {
        return false;
    }
    flash_end = layout->flash_start + layout->flash_size;
    return (range->address >= layout->flash_start) && (range_end <= flash_end);
}

static bool overlaps(const BlRange *left, const BlRange *right)
{
    uint32_t left_end;
    uint32_t right_end;
    if (!rangeEnd(left, &left_end) || !rangeEnd(right, &right_end)) {
        return true;
    }
    return (left->address < right_end) && (right->address < left_end);
}

static bool layoutValid(const BlLayout *layout)
{
    uint32_t i;
    uint32_t j;
    const BlRange *ranges[6];
    if ((layout == NULL) || (layout->erase_unit == 0U) ||
        ((layout->flash_start % layout->erase_unit) != 0U) ||
        (layout->program_unit == 0U) ||
        (layout->program_unit > BL_MAX_PROGRAM_UNIT) ||
        ((layout->erase_unit % layout->program_unit) != 0U)) {
        return false;
    }
    ranges[0] = &layout->bootloader;
    ranges[1] = &layout->slot[0];
    ranges[2] = &layout->slot[1];
    ranges[3] = &layout->scratch;
    ranges[4] = &layout->factory;
    ranges[5] = &layout->journal;
    for (i = 0U; i < 6U; ++i) {
        if (!contained(layout, ranges[i]) ||
            ((ranges[i]->address % layout->erase_unit) != 0U) ||
            ((ranges[i]->size % layout->erase_unit) != 0U)) {
            return false;
        }
        for (j = i + 1U; j < 6U; ++j) {
            if (overlaps(ranges[i], ranges[j])) {
                return false;
            }
        }
    }
    return true;
}

static bool slotValid(const BootloaderInterface *bl, uint8_t slot)
{
    return (bl != NULL) && bl->initialized && (slot < 2U);
}

static bool sameManifest(const BlImageManifest *left, const BlImageManifest *right)
{
    return (left != NULL) && (right != NULL) &&
        (left->signature_size <= BL_MAX_SIGNATURE_SIZE) &&
        (right->signature_size <= BL_MAX_SIGNATURE_SIZE) &&
        (left->image_size == right->image_size) &&
        (left->image_crc == right->image_crc) &&
        (left->version == right->version) &&
        (left->compatibility.vendor_id == right->compatibility.vendor_id) &&
        (left->compatibility.product_code == right->compatibility.product_code) &&
        (left->compatibility.hardware_variant == right->compatibility.hardware_variant) &&
        (left->compatibility.hardware_min == right->compatibility.hardware_min) &&
        (left->compatibility.hardware_max == right->compatibility.hardware_max) &&
        (memcmp(left->image_digest, right->image_digest,
                BL_IMAGE_DIGEST_SIZE) == 0) &&
        (left->signature_size == right->signature_size) &&
        (memcmp(left->signature, right->signature, left->signature_size) == 0);
}

static bool addressFor(const BootloaderInterface *bl, uint8_t slot,
                       uint32_t offset, uint32_t size, uint32_t *address)
{
    if (!slotValid(bl, slot) || (address == NULL) || (size == 0U) ||
        (offset > bl->layout.slot[slot].size) ||
        (size > (bl->layout.slot[slot].size - offset)) ||
        (offset > (UINT32_MAX - bl->layout.slot[slot].address))) {
        return false;
    }
    *address = bl->layout.slot[slot].address + offset;
    return true;
}

bool BootloaderInterface_Init(BootloaderInterface *bl, const BlLayout *layout,
                              const BlPlatform *platform, const FiData *factory,
                              uint8_t active_slot, uint8_t pending_slot,
                              uint8_t rollback_slot)
{
    if (bl == NULL) {
        return false;
    }
    memset(bl, 0, sizeof(*bl));
    if ((platform == NULL) || !layoutValid(layout) ||
        !Fi_Validate(factory) || (active_slot > 1U) ||
        ((pending_slot != BL_NO_SLOT) && (pending_slot > 1U)) ||
        ((rollback_slot != BL_NO_SLOT) && (rollback_slot > 1U)) ||
        ((pending_slot != BL_NO_SLOT) && (pending_slot == active_slot)) ||
        ((rollback_slot != BL_NO_SLOT) && (rollback_slot == active_slot)) ||
        ((pending_slot != BL_NO_SLOT) && (pending_slot == rollback_slot)) ||
        (platform->read == NULL) || (platform->erase == NULL) ||
        (platform->program == NULL) || (platform->verify_manifest == NULL) ||
        (platform->validate_programmed_image == NULL) ||
        (platform->validate_application == NULL) ||
        (platform->mark_pending == NULL) || (platform->activate == NULL) ||
        (platform->rollback == NULL) || (platform->jump == NULL)) {
        return false;
    }
    bl->layout = *layout;
    bl->platform = *platform;
    bl->factory = *factory;
    bl->active_slot = active_slot;
    bl->pending_slot = pending_slot;
    bl->rollback_slot = rollback_slot;
    bl->write_slot = BL_NO_SLOT;
    bl->write_limit = 0U;
    bl->written = 0U;
    bl->initialized = true;
    return true;
}

bool BootloaderInterface_ValidateFirmware(BootloaderInterface *bl, uint8_t slot,
                                           const BlImageManifest *manifest)
{
    if (!slotValid(bl, slot) || bl->recovery_required || (manifest == NULL) ||
        (slot == bl->active_slot) ||
        (slot == bl->pending_slot) ||
        (manifest->image_size == 0U) ||
        (bl->layout.slot[slot].size <= bl->layout.erase_unit) ||
        (manifest->image_size >
         (bl->layout.slot[slot].size - bl->layout.erase_unit)) ||
        (manifest->signature_size == 0U) ||
        (manifest->signature_size > BL_MAX_SIGNATURE_SIZE) ||
        !Fi_Compatible(&bl->factory, &manifest->compatibility)) {
        return false;
    }
    return bl->platform.verify_manifest(bl->platform.user, manifest);
}

bool BootloaderInterface_PrepareFirmware(BootloaderInterface *bl, uint8_t slot,
                                          const BlImageManifest *manifest)
{
    uint32_t unit;
    uint32_t padding;
    if ((bl == NULL) || !bl->initialized || (bl->write_slot != BL_NO_SLOT) ||
        !BootloaderInterface_ValidateFirmware(bl, slot, manifest)) {
        return false;
    }
    unit = bl->layout.program_unit;
    padding = (manifest->image_size % unit == 0U) ? 0U :
        (unit - (manifest->image_size % unit));
    if (padding > (bl->layout.slot[slot].size - manifest->image_size)) {
        return false;
    }
    bl->write_slot = slot;
    bl->write_limit = manifest->image_size + padding;
    bl->written = 0U;
    bl->write_manifest = *manifest;
    if (!BootloaderInterface_EraseArea(bl, slot)) {
        BootloaderInterface_AbortFirmware(bl, slot);
        return false;
    }
    return true;
}

bool BootloaderInterface_EraseArea(BootloaderInterface *bl, uint8_t slot)
{
    if (!slotValid(bl, slot) || bl->recovery_required || (slot == bl->active_slot) ||
        (slot == bl->pending_slot) || (slot != bl->write_slot)) {
        return false;
    }
    return bl->platform.erase(bl->platform.user, bl->layout.slot[slot].address,
                              bl->layout.slot[slot].size);
}

void BootloaderInterface_AbortFirmware(BootloaderInterface *bl, uint8_t slot)
{
    if ((bl != NULL) && bl->initialized && (slot == bl->write_slot)) {
        bl->write_slot = BL_NO_SLOT;
        bl->write_limit = 0U;
        bl->written = 0U;
        memset(&bl->write_manifest, 0, sizeof(bl->write_manifest));
    }
}

bool BootloaderInterface_WriteChunk(BootloaderInterface *bl, uint8_t slot,
                                     uint32_t offset, const uint8_t *data,
                                     uint32_t size)
{
    uint32_t address;
    uint32_t unit;
    if (!slotValid(bl, slot) || bl->recovery_required || (slot == bl->active_slot) ||
        (slot == bl->pending_slot) || (slot != bl->write_slot) || (data == NULL) ||
        (size == 0U) || (offset != bl->written) ||
        (offset > bl->write_limit) || (size > (bl->write_limit - offset))) {
        return false;
    }
    unit = bl->layout.program_unit;
    if (((offset % unit) != 0U) || ((size % unit) != 0U) ||
        !addressFor(bl, slot, offset, size, &address)) {
        return false;
    }
    if (!bl->platform.program(bl->platform.user, address, data, size)) {
        return false;
    }
    bl->written += size;
    return true;
}

bool BootloaderInterface_CalculateCRC(BootloaderInterface *bl, uint8_t slot,
                                       uint32_t size, uint32_t *crc)
{
    uint8_t buffer[BL_CRC_READ_SIZE];
    uint32_t offset = 0U;
    uint32_t running = UINT32_MAX;
    if (!slotValid(bl, slot) || (crc == NULL) || (size == 0U) ||
        (size > bl->layout.slot[slot].size)) {
        return false;
    }
    *crc = 0U;
    while (offset < size) {
        uint32_t address;
        uint32_t remaining = size - offset;
        uint32_t count = (remaining > BL_CRC_READ_SIZE) ? BL_CRC_READ_SIZE : remaining;
        if (!addressFor(bl, slot, offset, count, &address) ||
            !bl->platform.read(bl->platform.user, address, buffer, count)) {
            return false;
        }
        running = Ps_CrcUpdate(running, buffer, count);
        offset += count;
    }
    *crc = running ^ UINT32_MAX;
    return true;
}

bool BootloaderInterface_CalculateSHA256(BootloaderInterface *bl, uint8_t slot,
                                          uint32_t size, uint8_t digest[32])
{
    uint8_t buffer[BL_CRC_READ_SIZE];
    BlSha256 context;
    uint32_t offset = 0U;
    if (!slotValid(bl, slot) || (digest == NULL) || (size == 0U) ||
        (bl->layout.slot[slot].size <= bl->layout.erase_unit) ||
        (size > (bl->layout.slot[slot].size - bl->layout.erase_unit))) {
        return false;
    }
    BlSha256_Init(&context);
    while (offset < size) {
        uint32_t address;
        uint32_t remaining = size - offset;
        uint32_t count = (remaining > sizeof(buffer)) ?
            (uint32_t)sizeof(buffer) : remaining;
        if (!addressFor(bl, slot, offset, count, &address) ||
            !bl->platform.read(bl->platform.user, address, buffer, count)) {
            memset(&context, 0, sizeof(context));
            memset(buffer, 0, sizeof(buffer));
            return false;
        }
        BlSha256_Update(&context, buffer, count);
        offset += count;
    }
    BlSha256_Final(&context, digest);
    memset(buffer, 0, sizeof(buffer));
    return true;
}

bool BootloaderInterface_MarkPendingUpdate(BootloaderInterface *bl, uint8_t slot,
                                            const BlImageManifest *manifest)
{
    uint32_t actual_crc;
    if (!slotValid(bl, slot) || bl->recovery_required || (slot != bl->write_slot) ||
        (bl->written != bl->write_limit) ||
        !sameManifest(&bl->write_manifest, manifest) ||
        !BootloaderInterface_ValidateFirmware(bl, slot, manifest) ||
        !BootloaderInterface_CalculateCRC(bl, slot, manifest->image_size, &actual_crc) ||
        (actual_crc != manifest->image_crc) ||
        !bl->platform.validate_programmed_image(bl->platform.user, slot, manifest)) {
        return false;
    }
    if (!bl->platform.mark_pending(bl->platform.user, slot, manifest)) {
        bl->recovery_required = true;
        return false;
    }
    bl->pending_slot = slot;
    bl->write_slot = BL_NO_SLOT;
    bl->write_limit = 0U;
    bl->written = 0U;
    memset(&bl->write_manifest, 0, sizeof(bl->write_manifest));
    return true;
}

bool BootloaderInterface_ActivateFirmware(BootloaderInterface *bl, uint8_t slot)
{
    uint8_t previous;
    if (!slotValid(bl, slot) || bl->recovery_required ||
        (slot != bl->pending_slot) ||
        !bl->platform.validate_application(bl->platform.user, slot)) {
        return false;
    }
    if (!bl->platform.activate(bl->platform.user, slot)) {
        bl->recovery_required = true;
        return false;
    }
    previous = bl->active_slot;
    bl->active_slot = slot;
    bl->rollback_slot = previous;
    bl->pending_slot = BL_NO_SLOT;
    return true;
}

bool BootloaderInterface_RollbackFirmware(BootloaderInterface *bl)
{
    uint8_t previous;
    if ((bl == NULL) || !bl->initialized || bl->recovery_required ||
        (bl->rollback_slot > 1U) ||
        (bl->rollback_slot == bl->active_slot) ||
        !bl->platform.validate_application(bl->platform.user, bl->rollback_slot)) {
        return false;
    }
    if (!bl->platform.rollback(bl->platform.user, bl->rollback_slot)) {
        bl->recovery_required = true;
        return false;
    }
    previous = bl->active_slot;
    bl->active_slot = bl->rollback_slot;
    bl->rollback_slot = previous;
    bl->pending_slot = BL_NO_SLOT;
    return true;
}

bool BootloaderInterface_JumpToApplication(BootloaderInterface *bl)
{
    uint8_t slot;
    if ((bl == NULL) || !bl->initialized || bl->recovery_required) {
        return false;
    }
    slot = bl->active_slot;
    if (!bl->platform.validate_application(bl->platform.user, slot)) {
        return false;
    }
    return bl->platform.jump(bl->platform.user, bl->layout.slot[slot].address);
}
