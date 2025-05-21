#include "legic_prime_listener.h"

#include <nfc/protocols/nfc_generic_event.h>

typedef enum {
    LegicPrime_ListenerStateIdle,
    LegicPrime_ListenerStateActivated,
} LegicPrimeListenerState;

struct LegicPrimeListener {
    Nfc* nfc;
    LegicPrimeData* data;
    LegicPrimeListenerState state;

    NfcGenericEvent generic_event;
    NfcGenericCallback callback;
    void* context;
};

LegicPrimeError legic_prime_listener_frame_exchange(
    const LegicPrimeListener* instance,
    const BitBuffer* tx_buffer);
