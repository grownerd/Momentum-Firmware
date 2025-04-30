#pragma once

#include "legic_prime.h"
#include <lib/nfc/nfc.h>

#include <nfc/nfc_poller.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LegicPrimePoller opaque type definition.
 */
typedef struct LegicPrimePoller LegicPrimePoller;

/**
 * @brief Enumeration of possible LegicPrime poller event types.
 */
typedef enum {
    LegicPrimePollerEventTypeError, /**< An error occured during activation procedure. */
    LegicPrimePollerEventTypeReady, /**< The card was activated and fully read by the poller. */
    LegicPrimePollerEventTypeIncomplete, /**< The card was activated and partly read by the poller. */
} LegicPrimePollerEventType;

/**
 * @brief LegicPrime poller event data.
 */
typedef union {
    LegicPrimeError error; /**< Error code indicating card activation fail reason. */
} LegicPrimePollerEventData;

/**
 * @brief LegicPrimePoller poller event structure.
 *
 * Upon emission of an event, an instance of this struct will be passed to the callback.
 */
typedef struct {
    LegicPrimePollerEventType type; /**< Type of emmitted event. */
    LegicPrimePollerEventData* data; /**< Pointer to event specific data. */
} LegicPrimePollerEvent;

/**
 * @brief Perform collision resolution procedure.
 *
 * Must ONLY be used inside the callback function.
 *
 * Perfoms the collision resolution procedure as defined in LegicPrime standars. The data
 * field will be filled with LegicPrime data on success.
 *
 * @param[in, out] instance pointer to the instance to be used in the transaction.
 * @param[out] data pointer to the LegicPrime data structure to be filled.
 * @return LegicPrimeErrorNone on success, an error code on failure.
 */
LegicPrimeError legic_prime_poller_activate(LegicPrimePoller* instance, LegicPrimeData* data);

/**
 * @brief Performs legic_prime read operation for blocks provided as parameters
 * 
 * @param[in, out] instance pointer to the instance to be used in the transaction.
 * @param[in] block_count Amount of blocks involved in reading procedure
 * @param[in] block_numbers Array with block indexes according to legic_prime docs
 * @param[out] response_ptr Pointer to the response structure
 * @return LegicPrimeErrorNone on success, an error code on failure.
*/
LegicPrimeError legic_prime_poller_read_blocks(
    LegicPrimePoller* instance,
    const uint8_t block_count,
    const uint8_t* const block_numbers,
    LegicPrimePollerReadCommandResponse** const response_ptr);

#ifdef __cplusplus
}
#endif
