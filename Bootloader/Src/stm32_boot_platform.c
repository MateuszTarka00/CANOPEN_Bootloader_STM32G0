/**
 * @file stm32_boot_platform.c
 * @brief Range-checked target HAL adapters for the fixed STM32G0B1 layout.
 */
#include "stm32_boot_platform.h"
#include "bootloader_layout.h"
#include "firmware_auth.h"
#include "firmware_auth_key.h"
#include "firmware_manifest.h"
#include "fdcan.h"
#include <string.h>

#define BL_CAN_DEFAULT_NODE_ID DEVICE_BOOT_NODE_ID
#define BL_CAN_DEFAULT_BITRATE UINT32_C(250000)
#define BL_CAN_PROTOCOL_VERSION 2U
#define BL_CAN_SUPPORTED_BITRATE UINT32_C(250000)

/* Fixed discovery record checked by the application before requesting reset. */
__attribute__((section(".boot_contract"), used))
static const uint32_t boot_contract[8] = {
    UINT32_C(0x424C4331), BL_CAN_PROTOCOL_VERSION, BL_SLOT_A_START,
    BL_SLOT_SIZE, BL_FACTORY_START, BL_NVM_START, BL_BOOT_SIZE,
    BL_SIGNING_PUBLIC_KEY_PROVISIONED | 2U
};

static bool flashContains(uint32_t address, uint32_t size)
{
    if ((address < BL_FLASH_START) ||
        (address > (BL_FLASH_START + BL_FLASH_SIZE)) ||
        (size > (BL_FLASH_START + BL_FLASH_SIZE - address))) {
        return false;
    }
    return size != 0U;
}

static bool writableRange(uint32_t address, uint32_t size)
{
    uint32_t slot_a_end = BL_SLOT_A_START + BL_SLOT_SIZE;
    uint32_t slot_b_end = BL_SLOT_B_START + BL_SLOT_SIZE;
    bool in_slot_a = (address >= BL_SLOT_A_START) &&
        (address <= slot_a_end) && (size <= (slot_a_end - address));
    bool in_slot_b = (address >= BL_SLOT_B_START) &&
        (address <= slot_b_end) && (size <= (slot_b_end - address));
    bool in_scratch = (address >= BL_SWAP_SCRATCH_START) &&
        (address <= BL_FACTORY_START) && (size <= (BL_FACTORY_START - address));
    return in_slot_a || in_slot_b || in_scratch;
}

static bool flashRead(void *user, uint32_t address, uint8_t *data, uint32_t size)
{
    (void)user;
    if ((data == NULL) || !flashContains(address, size)) {
        return false;
    }
    memcpy(data, (const void *)(uintptr_t)address, size);
    return true;
}

static void flashFailure(void *user, uint32_t operation, HAL_StatusTypeDef status,
                         uint32_t address, uint32_t bank, uint32_t page,
                         uint32_t page_error)
{
    BlDiagnostics *diag = &((BlStm32Context *)user)->diagnostics;
    (void)FLASH_GuardFailure(status);
    if (diag->value[BL_DIAG_OPERATION] == 0U) {
        diag->value[BL_DIAG_OPERATION] = operation | (FLASH_Guard.stage << 16U);
        diag->value[BL_DIAG_STATUS] = status;
        diag->value[BL_DIAG_HAL_ERROR] = FLASH_Guard.sr & FLASH_SR_ERRORS;
        diag->value[BL_DIAG_SR] = FLASH_Guard.sr;
        diag->value[BL_DIAG_ADDRESS] = address;
        diag->value[BL_DIAG_BANK] = bank;
        diag->value[BL_DIAG_PAGE] = page;
        diag->value[BL_DIAG_PAGE_ERROR] = page_error;
        diag->value[BL_DIAG_OPTR] = FLASH->OPTR;
        diag->value[BL_DIAG_BANK_COUNT] = FLASH_BANK_NB;
    }
}

static bool flashEraseRaw(void *user, uint32_t address, uint32_t size)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t error = UINT32_MAX;
    uint32_t offset;
    uint32_t remaining = size;
    DeviceEraseSpan span;
    HAL_StatusTypeDef status;
    HAL_StatusTypeDef lock_status;
    if (!flashContains(address, size) || ((address % BL_ERASE_PAGE_SIZE) != 0U) ||
        ((size % BL_ERASE_PAGE_SIZE) != 0U) ||
        (FLASH_SIZE != BL_FLASH_SIZE) ||
        (FLASH_PAGE_SIZE != BL_ERASE_PAGE_SIZE) ||
        ((FLASH_BANK_NB == 2U) &&
         ((FLASH->OPTR & FLASH_OPTR_nSWAP_BANK) == 0U))) {
        flashFailure(user, 1U, HAL_ERROR, address, 0U, 0U, error);
        return false;
    }
    status = HAL_FLASH_Unlock();
    if (status != HAL_OK) {
        flashFailure(user, 2U, status, address, 0U, 0U, error);
        return false;
    }
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    offset = address - BL_FLASH_START;
    status = HAL_OK;
    while ((remaining != 0U) && (status == HAL_OK)) {
        if (!DeviceMemory_EraseSpan(offset, remaining, FLASH_BANK_NB, &span)) {
            status = HAL_ERROR;
            break;
        }
        erase.Banks = (span.bank == 1U) ? FLASH_BANK_1 : FLASH_BANK_2;
        erase.Page = span.page;
        erase.NbPages = span.pages;
        status = HAL_FLASHEx_Erase(&erase, &error);
        if (status != HAL_OK) {
            flashFailure(user, 3U, status, BL_FLASH_START + offset,
                         erase.Banks, erase.Page, error);
        }
        offset += span.pages * BL_ERASE_PAGE_SIZE;
        remaining -= span.pages * BL_ERASE_PAGE_SIZE;
    }
    lock_status = (status == HAL_OK) ? HAL_FLASH_Lock() : status;
    if (lock_status != HAL_OK) {
        flashFailure(user, 4U, lock_status, address, 0U, 0U, error);
    }
    return (status == HAL_OK) && (lock_status == HAL_OK);
}

static bool flashErase(void *user, uint32_t address, uint32_t size)
{
    return writableRange(address, size) && flashEraseRaw(user, address, size);
}

static bool flashProgramRaw(void *user, uint32_t address, const uint8_t *data, uint32_t size)
{
    uint32_t offset;
    HAL_StatusTypeDef status = HAL_OK;
    HAL_StatusTypeDef lock_status;
    if ((data == NULL) || ((address % BL_PROGRAM_UNIT_SIZE) != 0U) ||
        ((size % BL_PROGRAM_UNIT_SIZE) != 0U) ||
        !flashContains(address, size) ||
        (FLASH_SIZE != BL_FLASH_SIZE) ||
        (FLASH_PAGE_SIZE != BL_ERASE_PAGE_SIZE) ||
        ((FLASH_BANK_NB == 2U) &&
         ((FLASH->OPTR & FLASH_OPTR_nSWAP_BANK) == 0U))) {
        flashFailure(user, 5U, HAL_ERROR, address, 0U, 0U, UINT32_MAX);
        return false;
    }
    status = HAL_FLASH_Unlock();
    if (status != HAL_OK) {
        flashFailure(user, 6U, status, address, 0U, 0U, UINT32_MAX);
        return false;
    }
    for (offset = 0U; offset < size; offset += BL_PROGRAM_UNIT_SIZE) {
        uint64_t word;
        memcpy(&word, &data[offset], sizeof(word));
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                   address + offset, word);
        if (status != HAL_OK) {
            flashFailure(user, 7U, status, address + offset, 0U, 0U, UINT32_MAX);
            break;
        }
    }
    lock_status = (status == HAL_OK) ? HAL_FLASH_Lock() : status;
    if (lock_status != HAL_OK) {
        flashFailure(user, 8U, lock_status, address, 0U, 0U, UINT32_MAX);
    }
    if ((status != HAL_OK) || (lock_status != HAL_OK)) {
        return false;
    }
    if (memcmp((const void *)(uintptr_t)address, data, size) != 0) {
        flashFailure(user, 9U, HAL_ERROR, address, 0U, 0U, UINT32_MAX);
        return false;
    }
    return true;
}

static bool flashProgram(void *user, uint32_t address,
                         const uint8_t *data, uint32_t size)
{
    return writableRange(address, size) &&
        flashProgramRaw(user, address, data, size);
}

static bool nvmRange(uint8_t page, uint32_t offset, uint32_t size)
{
    return (page < 2U) && (offset <= BL_ERASE_PAGE_SIZE) &&
        (size <= BL_ERASE_PAGE_SIZE - offset) &&
        (FLASH_SIZE == BL_FLASH_SIZE) &&
        (FLASH_PAGE_SIZE == BL_ERASE_PAGE_SIZE) &&
        ((FLASH_BANK_NB != 2U) ||
         ((FLASH->OPTR & FLASH_OPTR_nSWAP_BANK) != 0U));
}

static bool nvmRead(void *user, uint8_t page, uint32_t offset,
                    uint8_t *data, uint32_t size)
{
    BlStm32Context *context = (BlStm32Context *)user;
    uint32_t address = BL_NVM_START + ((uint32_t)page * BL_ERASE_PAGE_SIZE) + offset;
    if ((context == NULL) || (data == NULL) || !nvmRange(page, offset, size)) {
        return false;
    }
    memcpy(data, (const void *)(uintptr_t)address, size);
    return true;
}

static bool nvmErase(void *user, uint8_t page)
{
    uint32_t address = BL_NVM_START + ((uint32_t)page * BL_ERASE_PAGE_SIZE);
    if (!nvmRange(page, 0U, BL_ERASE_PAGE_SIZE)) {
        flashFailure(user, 1U, HAL_ERROR, address, 0U, 0U, UINT32_MAX);
        return false;
    }
    return flashEraseRaw(user, address, BL_ERASE_PAGE_SIZE);
}

static bool nvmWrite(void *user, uint8_t page, uint32_t offset,
                     const uint8_t *data, uint32_t size)
{
    uint32_t address = BL_NVM_START + ((uint32_t)page * BL_ERASE_PAGE_SIZE) + offset;
    if (!nvmRange(page, offset, size) || (data == NULL)) {
        flashFailure(user, 5U, HAL_ERROR, address, 0U, 0U, UINT32_MAX);
        return false;
    }
    return flashProgramRaw(user, address, data, size);
}

static bool persist(BlStm32Context *context)
{
    return Ps_Save(&context->store, &context->persistent) == PS_OK;
}

static bool verifyManifest(void *user, const BlImageManifest *manifest)
{
    (void)user;
    return BlAuth_VerifyManifest(manifest);
}

static bool vectorValid(const volatile uint32_t *vectors, uint32_t image_size)
{
    uint32_t stack_pointer;
    uint32_t reset_handler;
    uint32_t ram_end = UINT32_C(0x20024000);
    if ((image_size < 8U) || (image_size > BL_SLOT_IMAGE_MAX)) {
        return false;
    }
    stack_pointer = vectors[0];
    reset_handler = vectors[1];
    return ((stack_pointer >= UINT32_C(0x20000000)) &&
            (stack_pointer <= ram_end) && ((stack_pointer & 7U) == 0U) &&
            ((reset_handler & 1U) != 0U) &&
            ((reset_handler & ~UINT32_C(1)) >= BL_SLOT_A_START) &&
            ((reset_handler & ~UINT32_C(1)) < (BL_SLOT_A_START + image_size)));
}

static bool readManifest(uint8_t slot, BlImageManifest *manifest)
{
    uint32_t address;
    uint8_t record[BL_MANIFEST_RECORD_SIZE];
    if ((slot >= BL_SLOT_COUNT) || (manifest == NULL)) {
        return false;
    }
    address = ((slot == 0U) ? BL_SLOT_A_START : BL_SLOT_B_START) +
        BL_SLOT_SIZE - BL_ERASE_PAGE_SIZE;
    memcpy(record, (const void *)(uintptr_t)address, sizeof(record));
    return BlManifest_Decode(record, sizeof(record), manifest);
}

static bool validateImage(BlStm32Context *context, uint8_t slot,
                          const BlImageManifest *manifest)
{
    uint32_t address;
    uint32_t crc;
    uint8_t digest[BL_IMAGE_DIGEST_SIZE];
    BlLayout layout = BlStm32_Layout();
    BlPlatform platform = BlStm32_Platform(context);
    BootloaderInterface verifier;
    if ((manifest == NULL) || (slot >= BL_SLOT_COUNT) ||
        !Fi_Compatible(&context->factory, &manifest->compatibility)) {
        return false;
    }
    address = layout.slot[slot].address;
    /* Both stored images are linked for execution from Slot A after swapping. */
    if (!vectorValid((const volatile uint32_t *)(uintptr_t)address,
                     manifest->image_size) ||
        (verifyManifest(context, manifest) == false)) {
        return false;
    }
    if (!BootloaderInterface_Init(&verifier, &layout, &platform,
                                  &context->factory, 0U,
                                  BL_NO_SLOT, BL_NO_SLOT)) {
        return false;
    }
    if (!BootloaderInterface_CalculateCRC(&verifier, slot,
                                          manifest->image_size, &crc) ||
        (crc != manifest->image_crc) ||
        !BootloaderInterface_CalculateSHA256(&verifier, slot,
                                              manifest->image_size, digest)) {
        return false;
    }
    return memcmp(digest, manifest->image_digest, sizeof(digest)) == 0;
}

static bool validateProgrammedImage(void *user, uint8_t slot,
                                    const BlImageManifest *manifest)
{
    return validateImage((BlStm32Context *)user, slot, manifest);
}

static bool validateApplication(void *user, uint8_t slot)
{
    BlStm32Context *context = (BlStm32Context *)user;
    BlImageManifest manifest;
    if (!readManifest(slot, &manifest)) {
        return false;
    }
    return validateImage(context, slot, &manifest);
}

static bool markPending(void *user, uint8_t slot,
                        const BlImageManifest *manifest)
{
    BlStm32Context *context = (BlStm32Context *)user;
    uint8_t record[BL_MANIFEST_RECORD_SIZE];
    uint8_t verify[BL_MANIFEST_RECORD_SIZE];
    uint32_t address;
    if ((context == NULL) || (slot != 1U) ||
        !BlManifest_Encode(manifest, record)) {
        return false;
    }
    address = BL_SLOT_B_START + BL_SLOT_SIZE - BL_ERASE_PAGE_SIZE;
    if (!flashProgram(context, address, record, 144U) ||
        !flashProgram(context, address + 144U, &record[144], 8U)) {
        return false;
    }
    memcpy(verify, (const void *)(uintptr_t)address, sizeof(verify));
    if (memcmp(record, verify, sizeof(record)) != 0) {
        return false;
    }
    context->persistent.update_state = BL_TX_VERIFIED;
    context->persistent.pending_slot = 1U;
    context->persistent.pending_version = manifest->version;
    context->persistent.firmware_size = manifest->image_size;
    context->persistent.firmware_crc = manifest->image_crc;
    context->persistent.update_result = 0U;
    return persist(context);
}

static bool activate(void *user, uint8_t slot)
{
    BlStm32Context *context = (BlStm32Context *)user;
    return (FLASH_Guard.status == HAL_OK) &&
        (context != NULL) && (slot == 1U) &&
        (context->transaction != NULL) &&
        BlTransaction_ScheduleActivation(context->transaction);
}

static bool rollback(void *user, uint8_t slot)
{
    BlStm32Context *context = (BlStm32Context *)user;
    if ((context == NULL) || (slot != 0U) ||
        (context->transaction == NULL)) {
        return false;
    }
    context->persistent.update_state = BL_TX_SWAP_ROLLBACK_BASE;
    return persist(context);
}

static bool jump(void *user, uint32_t vector_address)
{
    uint32_t stack_pointer;
    uint32_t reset_handler;
    (void)user;
    if ((FLASH_Guard.status != HAL_OK) || (vector_address != BL_SLOT_A_START)) {
        return false;
    }
    stack_pointer = *(const volatile uint32_t *)(uintptr_t)vector_address;
    reset_handler = *(const volatile uint32_t *)(uintptr_t)(vector_address + 4U);
    if (((stack_pointer & 7U) != 0U) ||
        (stack_pointer < UINT32_C(0x20000000)) ||
        (stack_pointer > UINT32_C(0x20024000)) ||
        ((reset_handler & 1U) == 0U) ||
        ((reset_handler & ~UINT32_C(1)) < BL_SLOT_A_START) ||
        ((reset_handler & ~UINT32_C(1)) >=
         (BL_SLOT_A_START + BL_SLOT_IMAGE_MAX))) {
        return false;
    }
    __disable_irq();
    (void)HAL_FDCAN_Stop(&hfdcan1);
    (void)HAL_FDCAN_DeInit(&hfdcan1);
    HAL_SuspendTick();
    __HAL_RCC_TIM1_FORCE_RESET();
    __HAL_RCC_TIM1_RELEASE_RESET();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;
    SCB->VTOR = BL_SLOT_A_START;
    __DSB();
    __ISB();
    NVIC->ICER[0] = UINT32_MAX;
    NVIC->ICPR[0] = UINT32_MAX;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    /* No C stack access after replacing MSP; restore reset-like IRQ masking. */
    __asm volatile (
        "msr msp, %0\n"
        "movs r2, #0\n"
        "msr control, r2\n"
        "isb\n"
        "cpsie i\n"
        "bx %1\n" : : "r" (stack_pointer), "r" (reset_handler) : "r2", "memory");
    __builtin_unreachable();
}

static uint32_t getTimeMs(void *user)
{
    (void)user;
    return HAL_GetTick();
}

static void readFactory(FiData *factory, bool *valid)
{
    FiResult result;
    if ((factory == NULL) || (valid == NULL) ||
        (FLASH_SIZE != BL_FLASH_SIZE) ||
        (FLASH_PAGE_SIZE != BL_ERASE_PAGE_SIZE) ||
        ((FLASH_BANK_NB == 2U) &&
         ((FLASH->OPTR & FLASH_OPTR_nSWAP_BANK) == 0U))) {
        if (factory != NULL) {
            memset(factory, 0, sizeof(*factory));
        }
        if (valid != NULL) {
            *valid = false;
        }
        return;
    }
    result = Fi_Decode((const uint8_t *)(uintptr_t)BL_FACTORY_START,
                       FI_RECORD_SIZE, factory);
    *valid = (result == FI_OK);
}

static bool supportedHandoff(const PsData *data)
{
    return ((data->boot_request == 1U) || (data->boot_request == 2U)) &&
        (data->boot_node_id >= 1U) && (data->boot_node_id <= 127U) &&
        (data->boot_bitrate == BL_CAN_SUPPORTED_BITRATE) &&
        (data->boot_protocol == BL_CAN_PROTOCOL_VERSION);
}

bool BlStm32_ContextInit(BlStm32Context *context)
{
    PsIo io;
    PsResult result;
    if (context == NULL) {
        return false;
    }
    memset(context, 0, sizeof(*context));
    io.user = context;
    io.read = nvmRead;
    io.erase = nvmErase;
    io.write = nvmWrite;
    Ps_Init(&context->store, &io);
    result = Ps_Load(&context->store, &context->persistent);
    if (result == PS_EMPTY) {
        memset(&context->persistent, 0, sizeof(context->persistent));
        context->persistent.pending_slot = UINT32_MAX;
        context->persistent.heartbeat_ms = 1000U;
        (void)strncpy(context->persistent.device_name, "VEGA", PS_STRING_SIZE - 1U);
        (void)strncpy(context->persistent.hardware_version, "unknown", PS_STRING_SIZE - 1U);
        (void)strncpy(context->persistent.software_version, "unknown", PS_STRING_SIZE - 1U);
        context->store.loaded = true;
        if (Ps_Save(&context->store, &context->persistent) != PS_OK) {
            return false;
        }
        result = PS_OK;
    }
    context->storage_valid = (result == PS_OK);
    readFactory(&context->factory, &context->factory_valid);
    if (context->storage_valid && context->factory_valid &&
        (context->persistent.update_state == BL_TX_IDLE) &&
        (context->persistent.firmware_size == 0U)) {
        BlImageManifest initial;
        if (readManifest(0U, &initial) && validateImage(context, 0U, &initial)) {
            context->persistent.vendor_id = context->factory.vendor_id;
            context->persistent.product_code = context->factory.product_code;
            context->persistent.revision = context->factory.product_revision;
            context->persistent.serial = context->factory.serial;
            context->persistent.active_version = initial.version;
            context->persistent.firmware_size = initial.image_size;
            context->persistent.firmware_crc = initial.image_crc;
            if (Ps_Save(&context->store, &context->persistent) != PS_OK) {
                return false;
            }
        }
    }
    context->node_id = BL_CAN_DEFAULT_NODE_ID;
    context->bitrate = BL_CAN_DEFAULT_BITRATE;
    if (context->storage_valid && supportedHandoff(&context->persistent)) {
        context->handoff_requested = true;
        context->bitrate = context->persistent.boot_bitrate;
    }
    return true;
}

BlLayout BlStm32_Layout(void)
{
    BlLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.flash_start = BL_FLASH_START;
    layout.flash_size = BL_FLASH_SIZE;
    layout.bootloader.address = BL_BOOT_START;
    layout.bootloader.size = BL_BOOT_SIZE;
    layout.slot[0].address = BL_SLOT_A_START;
    layout.slot[0].size = BL_SLOT_SIZE;
    layout.slot[1].address = BL_SLOT_B_START;
    layout.slot[1].size = BL_SLOT_SIZE;
    layout.scratch.address = BL_SWAP_SCRATCH_START;
    layout.scratch.size = BL_ERASE_PAGE_SIZE;
    layout.factory.address = BL_FACTORY_START;
    layout.factory.size = BL_ERASE_PAGE_SIZE;
    layout.journal.address = BL_NVM_START;
    layout.journal.size = BL_NVM_SIZE;
    layout.erase_unit = BL_ERASE_PAGE_SIZE;
    layout.program_unit = BL_PROGRAM_UNIT_SIZE;
    return layout;
}

BlPlatform BlStm32_Platform(BlStm32Context *context)
{
    BlPlatform platform;
    memset(&platform, 0, sizeof(platform));
    platform.user = context;
    platform.diagnostics = &context->diagnostics;
    platform.read = flashRead;
    platform.erase = flashErase;
    platform.program = flashProgram;
    platform.verify_manifest = verifyManifest;
    platform.validate_programmed_image = validateProgrammedImage;
    platform.validate_application = validateApplication;
    platform.mark_pending = markPending;
    platform.activate = activate;
    platform.rollback = rollback;
    platform.jump = jump;
    platform.get_time_ms = getTimeMs;
    return platform;
}

bool BlStm32_UpdateAuthAvailable(void)
{
    return BlAuth_IsReady();
}

bool BlStm32_FdcanInit(uint8_t node_id, uint32_t bitrate)
{
    FDCAN_FilterTypeDef filter;
    if ((node_id == 0U) || (node_id > 127U) ||
        (bitrate != BL_CAN_SUPPORTED_BITRATE)) {
        return false;
    }
    MX_FDCAN1_Init();
    memset(&filter, 0, sizeof(filter));
    filter.IdType = FDCAN_STANDARD_ID;
    filter.FilterIndex = 0U;
    filter.FilterType = FDCAN_FILTER_MASK;
    filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    filter.FilterID1 = 0U;
    filter.FilterID2 = 0x7FFU;
    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &filter) != HAL_OK) {
        return false;
    }
    filter.FilterIndex = 1U;
    filter.FilterID1 = 0x600U + node_id;
    if (HAL_FDCAN_ConfigFilter(&hfdcan1, &filter) != HAL_OK) {
        return false;
    }
    if ((HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_REJECT,
                                      FDCAN_REJECT,
                                      FDCAN_REJECT_REMOTE,
                                      FDCAN_REJECT_REMOTE) != HAL_OK) ||
        (HAL_FDCAN_Start(&hfdcan1) != HAL_OK)) {
        return false;
    }
    return true;
}

static uint8_t frameLength(uint32_t dlc)
{
    switch (dlc) {
    case FDCAN_DLC_BYTES_0: return 0U;
    case FDCAN_DLC_BYTES_1: return 1U;
    case FDCAN_DLC_BYTES_2: return 2U;
    case FDCAN_DLC_BYTES_3: return 3U;
    case FDCAN_DLC_BYTES_4: return 4U;
    case FDCAN_DLC_BYTES_5: return 5U;
    case FDCAN_DLC_BYTES_6: return 6U;
    case FDCAN_DLC_BYTES_7: return 7U;
    case FDCAN_DLC_BYTES_8: return 8U;
    default: return 0xFFU;
    }
}

bool BlStm32_FdcanReceive(CoFrame *frame)
{
    FDCAN_RxHeaderTypeDef header;
    uint8_t data[8];
    uint8_t length;
    if ((frame == NULL) || (HAL_FDCAN_GetRxFifoFillLevel(
            &hfdcan1, FDCAN_RX_FIFO0) == 0U)) {
        return false;
    }
    if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0,
                               &header, data) != HAL_OK) {
        return false;
    }
    length = frameLength(header.DataLength);
    if ((header.IdType != FDCAN_STANDARD_ID) ||
        (header.RxFrameType != FDCAN_DATA_FRAME) ||
        (length == 0xFFU) || (length > 8U)) {
        return false;
    }
    memset(frame, 0, sizeof(*frame));
    frame->id = (uint16_t)header.Identifier;
    frame->length = length;
    memcpy(frame->data, data, sizeof(data));
    return true;
}

bool BlStm32_FdcanSend(void *user, const CoFrame *frame)
{
    FDCAN_TxHeaderTypeDef header;
    (void)user;
    if ((frame == NULL) || (frame->id > 0x7FFU) ||
        (frame->length > 8U) ||
        (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) == 0U)) {
        return false;
    }
    memset(&header, 0, sizeof(header));
    header.Identifier = frame->id;
    header.IdType = FDCAN_STANDARD_ID;
    header.TxFrameType = FDCAN_DATA_FRAME;
    header.DataLength = (uint32_t)frame->length << 16U;
    header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    header.BitRateSwitch = FDCAN_BRS_OFF;
    header.FDFormat = FDCAN_CLASSIC_CAN;
    header.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    return HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &header,
                                         (uint8_t *)frame->data) == HAL_OK;
}

void BlStm32_SystemReset(void)
{
    NVIC_SystemReset();
}
