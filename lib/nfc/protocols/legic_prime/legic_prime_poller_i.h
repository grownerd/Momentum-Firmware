#pragma once

#include "legic_prime_poller.h"
#include <toolbox/bit_buffer.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEGIC_PRIME_POLLER_MAX_BUFFER_SIZE (4096U)

#define LEGIC_PRIME_POLLER_POLLING_FWT (0U)

typedef enum {
    LegicPrimePollerStateIdle,
    LegicPrimePollerStateActivated,
    LegicPrimePollerStateReadBlocks,
    LegicPrimePollerStateReadSuccess,
    LegicPrimePollerStateReadFailed,

    LegicPrimePollerStateNum
} LegicPrimePollerState;

struct LegicPrimePoller {
    Nfc* nfc;
    LegicPrimePollerState state;

    LegicPrimeData* data;
    BitBuffer* tx_buffer;
    BitBuffer* rx_buffer;

    NfcGenericEvent general_event;
    LegicPrimePollerEvent legic_prime_event;
    LegicPrimePollerEventData legic_prime_event_data;
    NfcGenericCallback callback;
    uint8_t block_index;
    void* context;
};

typedef struct {
    uint8_t num_bits;
    uint8_t data;
} LegicPrimePollerPollingCommand;

typedef struct {
    uint8_t data[2];
} LegicPrimePollerPollingResponse;

const LegicPrimeData* legic_prime_poller_get_data(LegicPrimePoller* instance);

/**
 * @brief Performs legic_prime polling operation as part of the activation process
 * 
 * @param[in, out] instance pointer to the instance to be used in the transaction.
 * @param[in] cmd Pointer to polling command structure
 * @param[out] resp Pointer to the response structure
 * @return LegicPrimeErrorNone on success, an error code on failure
*/
LegicPrimeError legic_prime_poller_polling(
    LegicPrimePoller* instance,
    const LegicPrimePollerPollingCommand* cmd,
    LegicPrimePollerPollingResponse* resp);

/**
 * @brief Performs legic_prime write operation with data provided as parameters
 * 
 * @param[in, out] instance pointer to the instance to be used in the transaction.
 * @param[in] block_count Amount of blocks involved in writing procedure
 * @param[in] block_numbers Array with block indexes according to legic_prime docs
 * @param[in] data Data of blocks provided in block_numbers
 * @param[out] response_ptr Pointer to the response structure
 * @return LegicPrimeErrorNone on success, an error code on failure.
*/
LegicPrimeError legic_prime_poller_write_blocks(
    const LegicPrimePoller* instance,
    const uint8_t block_count,
    const uint8_t* const block_numbers,
    const uint8_t* data,
    LegicPrimePollerWriteCommandResponse** const response_ptr);

/**
 * @brief Perform frame exchange procedure.
 *
 * Prepares data for sending by adding crc, after that performs
 * low level calls to send package data to the card
 *
 * @param[in, out] instance pointer to the instance to be used in the transaction.
 * @param[in] tx_buffer pointer to the buffer with data to be transmitted
 * @param[out] rx_buffer pointer to the buffer with received data from card
 * @param[in] fwt timeout window
 * @return LegicPrimeErrorNone on success, an error code on failure.
 */
LegicPrimeError legic_prime_poller_frame_exchange(
    const LegicPrimePoller* instance,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer,
    uint32_t fwt);

#ifdef __cplusplus
}
#endif
