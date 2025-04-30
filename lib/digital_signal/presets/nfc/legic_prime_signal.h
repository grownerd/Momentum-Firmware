/**
 * @file legic_prime_signal.h
 * @brief DigitalSequence preset for generating Legic Prime compliant signals.
 */
#pragma once

#include <furi_hal_resources.h>

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LegicPrime_Signal LegicPrime_Signal;

/**
 * @brief Allocate an LegicPrime_Signal instance with a set GPIO pin.
 *
 * @param[in] pin GPIO pin to use during transmission.
 * @returns pointer to the allocated instance.
 */
LegicPrime_Signal* legic_prime_signal_alloc(const GpioPin* pin);

/**
 * @brief Delete an LegicPrime_Signal instance.
 *
 * @param[in,out] instance pointer to the instance to be deleted.
 */
void legic_prime_signal_free(LegicPrime_Signal* instance);

/**
 * @brief Transmit arbitrary bytes using an LegicPrime_Signal instance.
 *
 * This function will block until the transmisson has been completed.
 *
 * @param[in] instance pointer to the instance used in transmission.
 * @param[in] tx_data pointer to the data to be transmitted.
 * @param[in] tx_bits size of the data to be transmitted in bits.
 */
void legic_prime_signal_tx(
    LegicPrime_Signal* instance,
    const uint8_t* tx_data,
    size_t tx_bits);

#ifdef __cplusplus
}
#endif
