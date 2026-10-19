/* Compile the target vector validator, not a parallel validation model. */
#include "bootloader_layout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vector_body.inc"

int main(void)
{
    uint32_t slot_a[2] = {DEVICE_RAM_END, BL_SLOT_A_START + 9U};
    uint32_t slot_b[2];
    memcpy(slot_b, slot_a, sizeof(slot_b));
    assert(vectorValid(slot_a, 256U));
    assert(vectorValid(slot_b, 256U));
    slot_b[1] = BL_SLOT_B_START + 9U;
    assert(!vectorValid(slot_b, 256U));
    slot_b[1] = BL_SLOT_A_START + 8U;
    assert(!vectorValid(slot_b, 256U)); /* Thumb bit required. */
    slot_b[1] = BL_SLOT_A_START - 1U;
    assert(!vectorValid(slot_b, 256U));
    slot_b[1] = BL_SLOT_A_START + 257U;
    assert(!vectorValid(slot_b, 256U));
    slot_b[1] = BL_SLOT_A_START + 255U;
    assert(vectorValid(slot_b, 256U));
    slot_b[0] = DEVICE_RAM_END + 8U;
    assert(!vectorValid(slot_b, 256U));
    slot_b[0] = DEVICE_RAM_START - 8U;
    assert(!vectorValid(slot_b, 256U));
    slot_b[0] = DEVICE_RAM_END - 4U;
    assert(!vectorValid(slot_b, 256U));
    slot_b[0] = DEVICE_RAM_START;
    assert(vectorValid(slot_b, 256U));
    assert(!vectorValid(slot_b, 7U));
    assert(!vectorValid(slot_b, BL_SLOT_IMAGE_MAX + 1U));
    slot_b[0] = UINT32_MAX;
    slot_b[1] = UINT32_MAX;
    assert(!vectorValid(slot_b, 256U));
    puts("Target image vectors: A-linked image in either slot, stack/Thumb/range/size rejection OK");
    return 0;
}
