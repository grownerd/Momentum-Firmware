#include "legic_prime_poller_i.h"

#include <nfc/protocols/nfc_poller_base.h>

#include <furi.h>
#include <furi_hal.h>

#define TAG "LegicPrimePoller"

typedef NfcCommand (*LegicPrimePollerReadHandler)(LegicPrimePoller* instance);

const LegicPrimeData* legic_prime_poller_get_data(LegicPrimePoller* instance) {
    furi_assert(instance);
    furi_assert(instance->data);

    return instance->data;
}

static LegicPrimePoller* legic_prime_poller_alloc(Nfc* nfc) {
    furi_assert(nfc);
    LegicPrimePoller* instance = malloc(sizeof(LegicPrimePoller));
    instance->nfc = nfc;
    instance->tx_buffer = bit_buffer_alloc(LEGIC_PRIME_POLLER_MAX_BUFFER_SIZE);
    instance->rx_buffer = bit_buffer_alloc(LEGIC_PRIME_POLLER_MAX_BUFFER_SIZE);

    nfc_config(instance->nfc, NfcModePoller, NfcTechLegicPrime);
#if 0
    nfc_set_guard_time_us(instance->nfc, LEGIC_PRIME_GUARD_TIME_US);
    nfc_set_fdt_poll_fc(instance->nfc, LEGIC_PRIME_FDT_POLL_FC);
    nfc_set_fdt_poll_poll_us(instance->nfc, LEGIC_PRIME_POLL_POLL_MIN_US);
#else
    nfc_set_guard_time_us(instance->nfc, 0);
    nfc_set_fdt_poll_fc(instance->nfc, 0);
    nfc_set_fdt_poll_poll_us(instance->nfc, 0);
#endif

    instance->data = legic_prime_alloc();

    instance->legic_prime_event.data = &instance->legic_prime_event_data;
    instance->general_event.protocol = NfcProtocolLegicPrime;
    instance->general_event.event_data = &instance->legic_prime_event;
    instance->general_event.instance = instance;

    return instance;
}

static void legic_prime_poller_free(LegicPrimePoller* instance) {
    furi_assert(instance);

    furi_assert(instance->tx_buffer);
    furi_assert(instance->rx_buffer);
    furi_assert(instance->data);

#if 1
    bit_buffer_free(instance->tx_buffer);
    bit_buffer_free(instance->rx_buffer);
    legic_prime_free(instance->data);
    free(instance);
#endif
}

static void
    legic_prime_poller_set_callback(LegicPrimePoller* instance, NfcGenericCallback callback, void* context) {
    furi_assert(instance);
    furi_assert(callback);

    instance->callback = callback;
    instance->context = context;
}

NfcCommand legic_prime_poller_state_handler_idle(LegicPrimePoller* instance) {
    FURI_LOG_D(TAG, "Idle");
    legic_prime_reset(instance->data);
    instance->state = LegicPrimePollerStateActivated;

    return NfcCommandContinue;
}

NfcCommand legic_prime_poller_state_handler_activate(LegicPrimePoller* instance) {
    FURI_LOG_D(TAG, "Activate");

    LegicPrimeError error = legic_prime_poller_activate(instance, instance->data);
    if(error == LegicPrimeErrorNone) {
        // TODO: Maybe fill the buffer with sequential addresses instead?
        //furi_hal_random_fill_buf(instance->data->data, LEGIC_PRIME_DATA_BLOCK_SIZE);

        instance->callback(instance->general_event, instance->context);

        instance->state = LegicPrimePollerStateReadBlocks;

    } else if(error != LegicPrimeErrorTimeout) {
    //} else {
        instance->legic_prime_event.type = LegicPrimePollerEventTypeError;
        instance->legic_prime_event_data.error = error;
        instance->state = LegicPrimePollerStateReadFailed;
    }

    NfcCommand command = NfcCommandContinue;
    return command;
}


NfcCommand legic_prime_poller_state_handler_read_blocks(LegicPrimePoller* instance) {
    FURI_LOG_D(TAG, "Read Blocks");
    //UNUSED(instance);

    uint8_t block_count = 1;
    uint8_t block_list[4] = {0, 0, 0, 0};
    block_list[0] = instance->block_index;

    LegicPrimePollerReadCommandResponse* response;

    for (int i=0; i<256; i++)
    {
        block_list[0] = i;
        LegicPrimeError error = legic_prime_poller_read_blocks(
            instance, block_count, block_list, &response);
        if(error == LegicPrimeErrorNone) {
#if 0
            block_count = (response->SF1 == 0) ? response->block_count : block_count;
            uint8_t* data_ptr =
                instance->data->data.dump + instance->data->blocks_total * sizeof(LegicPrimeBlock);

            *data_ptr++ = response->SF1;
            *data_ptr++ = response->SF2;

            if(response->SF1 == 0) {
                uint8_t* response_data_ptr = response->data;
                instance->data->blocks_read++;
                memcpy(data_ptr, response_data_ptr, LEGIC_PRIME_DATA_BLOCK_SIZE);
            } else {
                memset(data_ptr, 0, LEGIC_PRIME_DATA_BLOCK_SIZE);
            }
            instance->data->blocks_total++;

            if(instance->data->blocks_total == LEGIC_PRIME_BLOCKS_TOTAL_COUNT) {
                instance->state = LegicPrimePollerStateReadSuccess;
            }
#else
            uint8_t* data_ptr =
                instance->data->data;

            uint8_t* response_data_ptr = response->foo;
            instance->data->blocks_read++;
            memcpy(data_ptr, response_data_ptr, LEGIC_PRIME_DATA_BLOCK_SIZE);
            instance->state = LegicPrimePollerStateReadSuccess;

            FURI_LOG_I(TAG, "Read byte %3d: 0x%02X", i, response_data_ptr[0]);
#endif
        } else {
            instance->legic_prime_event.type = LegicPrimePollerEventTypeError;
            instance->legic_prime_event_data.error = error;
            instance->state = LegicPrimePollerStateReadFailed;
        }
    }
    return NfcCommandContinue;
}

NfcCommand legic_prime_poller_state_handler_read_success(LegicPrimePoller* instance) {
    FURI_LOG_D(TAG, "Read Success");
    return instance->callback(instance->general_event, instance->context);
}

NfcCommand legic_prime_poller_state_handler_read_failed(LegicPrimePoller* instance) {
    FURI_LOG_D(TAG, "Read Fail");
    instance->callback(instance->general_event, instance->context);
    return NfcCommandStop;
}

static const LegicPrimePollerReadHandler legic_prime_poller_handler[LegicPrimePollerStateNum] = {
    [LegicPrimePollerStateIdle] = legic_prime_poller_state_handler_idle,
    [LegicPrimePollerStateActivated] = legic_prime_poller_state_handler_activate,
    [LegicPrimePollerStateReadBlocks] = legic_prime_poller_state_handler_read_blocks,
    [LegicPrimePollerStateReadSuccess] = legic_prime_poller_state_handler_read_success,
    [LegicPrimePollerStateReadFailed] = legic_prime_poller_state_handler_read_failed,
};

static NfcCommand legic_prime_poller_run(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.protocol == NfcProtocolInvalid);
    furi_assert(event.event_data);

    LegicPrimePoller* instance = context;
    NfcEvent* nfc_event = event.event_data;
    NfcCommand command = NfcCommandContinue;

    if(nfc_event->type == NfcEventTypePollerReady) {
        command = legic_prime_poller_handler[instance->state](instance);
    }

    return command;
}

static bool legic_prime_poller_detect(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.event_data);
    furi_assert(event.instance);
    furi_assert(event.protocol == NfcProtocolInvalid);

    bool protocol_detected = false;
    LegicPrimePoller* instance = context;
    NfcEvent* nfc_event = event.event_data;
    furi_assert(instance->state == LegicPrimePollerStateIdle);

    if(nfc_event->type == NfcEventTypePollerReady) {
        LegicPrimeError error = legic_prime_poller_activate(instance, instance->data);
        protocol_detected = (error == LegicPrimeErrorNone);
    }

    return protocol_detected;
}

const NfcPollerBase nfc_poller_legic_prime = {
    .alloc = (NfcPollerAlloc)legic_prime_poller_alloc,
    .free = (NfcPollerFree)legic_prime_poller_free,
    .set_callback = (NfcPollerSetCallback)legic_prime_poller_set_callback,
    .run = (NfcPollerRun)legic_prime_poller_run,
    .detect = (NfcPollerDetect)legic_prime_poller_detect,
    .get_data = (NfcPollerGetData)legic_prime_poller_get_data,
};
