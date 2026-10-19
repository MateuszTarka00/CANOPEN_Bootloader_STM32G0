/* Execute the production discovery policy with deterministic storage/jump IO. */
#include "canopen.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static struct {
    struct {
        bool storage_valid;
        PsData persistent;
        PsStore store;
    } platform;
    Canopen canopen;
    FirmwareUpdate update;
    bool discovery_waiting;
    uint32_t discovery_started;
} runtime;
static bool app_valid, save_ok, jump_ok;
static uint32_t saves, jumps;
static PsData saved;
static jmp_buf fault;
static bool applicationValid(void) { return app_valid; }
static bool bootApplication(void) { ++jumps; return jump_ok; }
static void Error_Handler(void) { longjmp(fault, 1); }
PsResult Ps_Save(PsStore *store, const PsData *data)
{
    (void)store;
    ++saves;
    if (!save_ok) { return PS_IO_ERROR; }
    saved = *data;
    return PS_OK;
}
#include "discovery_bodies.inc"

static void setup(uint32_t start)
{
    memset(&runtime, 0, sizeof(runtime));
    runtime.platform.storage_valid = true;
    runtime.platform.persistent.boot_request = 2U;
    runtime.platform.persistent.boot_node_id = 126U;
    runtime.platform.persistent.boot_bitrate = 250000U;
    runtime.platform.persistent.boot_protocol = 2U;
    runtime.canopen.nmt.node_id = 126U;
    runtime.discovery_started = start;
    saves = 0U; jumps = 0U; app_valid = true; save_ok = true; jump_ok = true;
}

int main(void)
{
    uint32_t wrap, scenario;
    CoFrame q = {0};
    for (wrap = 0U; wrap < 2U; ++wrap) {
        uint32_t start = wrap ? UINT32_MAX - 100U : 100U;
        setup(start);
        assert(consumeDiscoveryRequest() && runtime.discovery_waiting);
        assert(saves == 1U && saved.boot_request == 0U &&
               saved.boot_node_id == 0U && saved.boot_bitrate == 0U &&
               saved.boot_protocol == 0U);
        pollDiscovery(start + 4999U); assert(jumps == 0U);
        pollDiscovery(start + 5000U); assert(jumps == 1U && !runtime.discovery_waiting);
        pollDiscovery(start + 6000U); assert(jumps == 1U);
    }
    setup(0U); runtime.platform.persistent.boot_request = 0U;
    assert(consumeDiscoveryRequest() && !runtime.discovery_waiting && saves == 0U);
    setup(0U); runtime.platform.persistent.boot_request = 1U;
    assert(consumeDiscoveryRequest() && !runtime.discovery_waiting && saves == 0U);
    setup(0U); save_ok = false;
    assert(!consumeDiscoveryRequest() && !runtime.discovery_waiting);
    assert(runtime.platform.persistent.boot_request == 2U);
    setup(0U); app_valid = false;
    assert(consumeDiscoveryRequest() && !runtime.discovery_waiting);
    setup(0U); runtime.platform.persistent.update_state = BL_TX_VERIFIED;
    assert(consumeDiscoveryRequest() && !runtime.discovery_waiting);
    for (scenario = 0U; scenario < 7U; ++scenario) {
        setup(0U); assert(consumeDiscoveryRequest());
        q.id = 0x67EU; q.length = 8U;
        runtime.canopen.tx_pending = true;
        runtime.canopen.pending_tx.id = 0x5FEU;
        runtime.canopen.pending_tx.data[0] = 0x43U;
        switch (scenario) {
        case 1U: q.id = 0U; break;
        case 2U: q.id = 0x601U; break;
        case 3U: q.length = 7U; break;
        case 4U: runtime.canopen.pending_tx.data[0] = 0x80U; break;
        case 5U: runtime.canopen.tx_pending = false; break;
        case 6U: runtime.canopen.pending_tx.id = 0x77EU; break;
        default: break;
        }
        observeDiscovery(&q, false);
        assert(runtime.discovery_waiting);
        observeDiscovery(&q, true);
        assert(runtime.discovery_waiting == (scenario != 0U));
        pollDiscovery(5000U);
        assert(jumps == (scenario == 0U ? 0U : 1U));
    }
    setup(0U); assert(consumeDiscoveryRequest()); jump_ok = false;
    if (setjmp(fault) == 0) { pollDiscovery(5000U); assert(false); }
    setup(0U); assert(consumeDiscoveryRequest());
    runtime.canopen.od.container_active = true;
    if (setjmp(fault) == 0) { pollDiscovery(5000U); assert(false); }
    assert(jumps == 0U);
    setup(0U); assert(consumeDiscoveryRequest());
    runtime.update.state = FWU_RECEIVING;
    if (setjmp(fault) == 0) { pollDiscovery(5000U); assert(false); }
    assert(jumps == 0U);
    setup(0U); assert(consumeDiscoveryRequest());
    runtime.platform.persistent.update_state = BL_TX_VERIFIED;
    if (setjmp(fault) == 0) { pollDiscovery(5000U); assert(false); }
    assert(jumps == 0U);
    puts("boot discovery: one-shot persistence, 4999/5000 ms + wrap, valid SDO contact, fail-stop OK");
    return 0;
}
