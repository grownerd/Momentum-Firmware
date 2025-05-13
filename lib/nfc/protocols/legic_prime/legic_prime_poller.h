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
    LegicPrimePollerEventTypeRequestMode,
    LegicPrimePollerEventTypeSuccess,
    LegicPrimePollerEventTypeFail,
} LegicPrimePollerEventType;

/**
 * @brief LegicPrime poller mode.
 */
typedef enum {
    LegicPrimePollerModeRead, /**< Poller reading mode. */
    LegicPrimePollerModeWrite, /**< Poller writing mode. */
} LegicPrimePollerMode;

/**
 * @brief LegicPrime poller request mode event data.
 *
 * This instance of this structure must be filled on LegicPrimePollerEventTypeRequestMode event.
 */
typedef struct {
    LegicPrimePollerMode mode; /**< Mode to be used by poller. */
    const LegicPrimeData* data; /**< Data to be used by poller. */
} LegicPrimePollerEventDataRequestMode;

/**
 * @brief LegicPrime poller event data.
 */
typedef union {
    LegicPrimeError error; /**< Error code indicating card activation fail reason. */
    LegicPrimePollerEventDataRequestMode poller_mode; /**< Poller mode context. */
} LegicPrimePollerEventData;

/**
 * @brief LegicPrimePoller poller event structure.
 *
 * Upon emission of an event, an instance of this struct will be passed to the callback.
 */
typedef struct {
    LegicPrimePollerEventType type; /**< Type of emmitted event. */
    LegicPrimePollerEventData* data; /**< Pointer to event specific data. */
    const LegicPrimeData* write_data;
} LegicPrimePollerEvent;

/**
 * @brief Perform collision resolution procedure.
 *
 * Must ONLY be used inside the callback function.
 *
 * @param[in, out] instance pointer to the instance to be used in the transaction.
 * @param[out] data pointer to the LegicPrime data structure to be filled.
 * @return LegicPrimeErrorNone on success, an error code on failure.
 */
LegicPrimeError legic_prime_poller_activate(LegicPrimePoller* instance, LegicPrimeData* data);

#ifdef __cplusplus
}
#endif
