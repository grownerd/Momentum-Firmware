#pragma once

#include "legic_prime_poller.h"
#include <toolbox/bit_buffer.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEGIC_PRIME_POLLER_MAX_BUFFER_SIZE (4096U)

typedef enum {
    LegicPrimePollerStateIdle,
    LegicPrimePollerStateRequestMode,
    LegicPrimePollerStateReadBlocks,
    LegicPrimePollerStateReadSuccess,
    LegicPrimePollerStateReadFailed,
    LegicPrimePollerStateWriteBlocks,
    LegicPrimePollerStateWriteSuccess,
    LegicPrimePollerStateWriteFailed,

    LegicPrimePollerStateNum
} LegicPrimePollerState;

struct LegicPrimePoller {
    Nfc* nfc;
    LegicPrimePollerState state;

    LegicPrimeData* data;
    LegicPrimePollerTrxData* trx_data;

    NfcGenericEvent general_event;
    LegicPrimePollerEvent legic_prime_event;
    LegicPrimePollerEventData legic_prime_event_data;
    NfcGenericCallback callback;
    void* context;
};

const LegicPrimeData* legic_prime_poller_get_data(LegicPrimePoller* instance);

/**
 * @brief Performs legic_prime write operation with data provided as parameters
 * 
 * @param[in, out] instance pointer to the instance to be used in the transaction.
 * @param[out] trx_data Pointer to the struct that holds all trx data.
 * @return LegicPrimeErrorNone on success, an error code on failure.
*/
LegicPrimeError
    legic_prime_poller_trx(LegicPrimePoller* instance, LegicPrimePollerTrxData* trx_data);

#ifdef __cplusplus
}

#endif
