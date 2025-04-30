#include "legic_prime_signal.h"

#include <digital_signal/digital_sequence.h>

#define BITS_IN_BYTE (8)

#define LEGIC_PRIME_SIGNAL_BIT_MAX_EDGES (2)
#define LEGIC_PRIME_SIGNAL_MAX_BITS (16)
#define LEGIC_PRIME_SIGNAL_MAX_EDGES     (LEGIC_PRIME_SIGNAL_BIT_MAX_EDGES * LEGIC_PRIME_SIGNAL_MAX_BITS)

#define LEGIC_PRIME_SIGNAL_SEQUENCE_SIZE (LEGIC_PRIME_SIGNAL_MAX_BITS)

#define LEGIC_PRIME_SIGNAL_F_SIG       (13560000.0) // Signal carrier
#define LEGIC_PRIME_SIGNAL_T_SIG       (100000000000UL / LEGIC_PRIME_SIGNAL_F_SIG) // == 73.746ns*100, one period @ 13.56MHz in ticks
#define LEGIC_PRIME_SIGNAL_F_SUB       (212000.0) // Signal subcarrier, used by the tag
#define LEGIC_PRIME_SIGNAL_T_SUB       (100000000000UL / LEGIC_PRIME_SIGNAL_F_SUB) // == 4716.98*100, one period @ 212kHz in ticks

#define LEGIC_PRIME_SIGNAL_T_POLLER_CHARGE      DIGITAL_SIGNAL_MS(5)
#define LEGIC_PRIME_SIGNAL_T_POLLER_BIT_START   DIGITAL_SIGNAL_US(16)
#define LEGIC_PRIME_SIGNAL_T_POLLER_BIT_ONE     DIGITAL_SIGNAL_US(103)
#define LEGIC_PRIME_SIGNAL_T_POLLER_BIT_ZERO    DIGITAL_SIGNAL_US(63)
#define LEGIC_PRIME_SIGNAL_T_POLLER_EOF         DIGITAL_SIGNAL_US(330)
#define LEGIC_PRIME_SIGNAL_T_LISTENER_BIT       DIGITAL_SIGNAL_US(100)


typedef enum {
    LegicPrime_SignalIndexZero,
    LegicPrime_SignalIndexOne,
    LegicPrime_SignalIndexEof,
    LegicPrime_SignalIndexCharge,
    LegicPrime_SignalIndexCount,
} LegicPrime_SignalIndex;

typedef DigitalSignal* LegicPrime_SignalBank[LegicPrime_SignalIndexCount];

struct LegicPrime_Signal {
    DigitalSequence* tx_sequence;
    LegicPrime_SignalBank signals;
};

#if 0
static void legic_prime_signal_add_byte(LegicPrime_Signal* instance, uint8_t byte) {
    for(size_t i = 0; i < BITS_IN_BYTE; i++) {
        digital_sequence_add_signal(
            instance->tx_sequence,
            FURI_BIT(byte, i) ? LegicPrime_SignalIndexOne : LegicPrime_SignalIndexZero);
    }
}
#endif

static void legic_prime_signal_encode(
    LegicPrime_Signal* instance,
    const uint8_t* tx_data,
    size_t tx_bits) {
    furi_assert(instance);
    furi_assert(tx_data);

    //digital_sequence_add_signal(instance->tx_sequence, LegicPrime_SignalIndexCharge);

    for(size_t i = 0; i < tx_bits; i++) {
        digital_sequence_add_signal(
            instance->tx_sequence,
            FURI_BIT(tx_data[0], i) ? LegicPrime_SignalIndexOne : LegicPrime_SignalIndexZero);
    }
    // End of frame
    digital_sequence_add_signal(instance->tx_sequence, LegicPrime_SignalIndexEof);

}

static inline void legic_prime_signal_generate_signal(DigitalSignal* signal, uint8_t bit) {
    digital_signal_set_start_level(signal, 1);

    switch (bit)
    {
    case LegicPrime_SignalIndexZero:
        digital_signal_add_period(signal, LEGIC_PRIME_SIGNAL_T_POLLER_BIT_START);
        digital_signal_add_period(signal, LEGIC_PRIME_SIGNAL_T_POLLER_BIT_ZERO);
        break;

    case LegicPrime_SignalIndexOne:
        digital_signal_add_period(signal, LEGIC_PRIME_SIGNAL_T_POLLER_BIT_START);
        digital_signal_add_period(signal, LEGIC_PRIME_SIGNAL_T_POLLER_BIT_ONE);
        break;

    case LegicPrime_SignalIndexEof:
        digital_signal_add_period(signal, LEGIC_PRIME_SIGNAL_T_POLLER_BIT_START);
        digital_signal_add_period(signal, LEGIC_PRIME_SIGNAL_T_POLLER_EOF);
        break;

    case LegicPrime_SignalIndexCharge:
        digital_signal_add_period_with_level(signal, LEGIC_PRIME_SIGNAL_T_POLLER_CHARGE, 0);
        break;

    default:
        break;

    }
}

static inline void legic_prime_signal_bank_fill(LegicPrime_SignalBank bank) {
    for(uint32_t i = 0; i < LegicPrime_SignalIndexCount; ++i) {
        bank[i] = digital_signal_alloc(LEGIC_PRIME_SIGNAL_BIT_MAX_EDGES);
        legic_prime_signal_generate_signal(bank[i], i);
    }
}

static inline void legic_prime_signal_bank_clear(LegicPrime_SignalBank bank) {
    for(uint32_t i = 0; i < LegicPrime_SignalIndexCount; ++i) {
        digital_signal_free(bank[i]);
    }
}

static inline void
    legic_prime_signal_bank_register(LegicPrime_SignalBank bank, DigitalSequence* sequence) {
    for(uint32_t i = 0; i < LegicPrime_SignalIndexCount; ++i) {
        digital_sequence_register_signal(sequence, i, bank[i]);
    }
}

LegicPrime_Signal* legic_prime_signal_alloc(const GpioPin* pin) {
    furi_assert(pin);

    LegicPrime_Signal* instance = malloc(sizeof(LegicPrime_Signal));
    instance->tx_sequence = digital_sequence_alloc(LEGIC_PRIME_SIGNAL_SEQUENCE_SIZE, pin);

    legic_prime_signal_bank_fill(instance->signals);
    legic_prime_signal_bank_register(instance->signals, instance->tx_sequence);

    return instance;
}

void legic_prime_signal_free(LegicPrime_Signal* instance) {
    furi_assert(instance);
    furi_assert(instance->tx_sequence);

    legic_prime_signal_bank_clear(instance->signals);
    digital_sequence_free(instance->tx_sequence);
    free(instance);
}

void legic_prime_signal_tx(
    LegicPrime_Signal* instance,
    const uint8_t* tx_data,
    size_t tx_bits) {
    furi_assert(instance);
    furi_assert(tx_data);

    FURI_CRITICAL_ENTER();
    digital_sequence_clear(instance->tx_sequence);
    legic_prime_signal_encode(instance, tx_data, tx_bits);
    digital_sequence_transmit(instance->tx_sequence);
    FURI_CRITICAL_EXIT();
}
