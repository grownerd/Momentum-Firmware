#pragma once

#include <toolbox/bit_buffer.h>
#include <nfc/protocols/nfc_device_base_i.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEGIC_PRIME_DATA_BLOCK_SIZE (1U)

#define LEGIC_PRIME_GUARD_TIME_US     (0)
#define LEGIC_PRIME_FDT_POLL_FC       (0)
#define LEGIC_PRIME_POLL_POLL_MIN_US  (0)


/** @brief Type of possible LegicPrime errors */
typedef enum {
    LegicPrimeErrorNone,
    LegicPrimeErrorNotPresent,
    LegicPrimeErrorBufferOverflow,
    LegicPrimeErrorCommunication,
    LegicPrimeErrorFieldOff,
    LegicPrimeErrorWrongCrc,
    LegicPrimeErrorProtocol,
    LegicPrimeErrorTimeout,
} LegicPrimeError;

typedef enum {
    LegicPrimeCmdWrite = 0,
    LegicPrimeCmdRead = 1,
} LegicPrimeCmd;

typedef enum {
    LegicPrimeTagTypeMim22 = 0x0d,
    LegicPrimeTagTypeMim256 = 0x1d,
    LegicPrimeTagTypeMim1024 = 0x3d,
} LegicPrimeTagType;

typedef struct {
    LegicPrimeTagType tagtype;
    uint8_t cmdsize;
    uint8_t addrsize;
    uint16_t cardsize;
} LegicPrimeTag;

/** @brief Structure used to store LegicPrime data and additional values about reading */
typedef struct {
    uint16_t blocks_total;
    uint16_t blocks_read;
    LegicPrimeTag tag;
    uint8_t data[1024];
} LegicPrimeData;

typedef struct {
    uint8_t foo[1];
} LegicPrimePollerReadCommandResponse;

typedef LegicPrimePollerReadCommandResponse LegicPrimePollerWriteCommandResponse;

extern const NfcDeviceBase nfc_device_legic_prime;

LegicPrimeData* legic_prime_alloc(void);

void legic_prime_free(LegicPrimeData* data);

void legic_prime_reset(LegicPrimeData* data);

void legic_prime_copy(LegicPrimeData* data, const LegicPrimeData* other);

bool legic_prime_verify(LegicPrimeData* data, const FuriString* device_type);

bool legic_prime_load(LegicPrimeData* data, FlipperFormat* ff, uint32_t version);

bool legic_prime_save(const LegicPrimeData* data, FlipperFormat* ff);

bool legic_prime_is_equal(const LegicPrimeData* data, const LegicPrimeData* other);

const char* legic_prime_get_device_name(const LegicPrimeData* data, NfcDeviceNameType name_type);

const uint8_t* legic_prime_get_uid(const LegicPrimeData* data, size_t* uid_len);

bool legic_prime_set_uid(LegicPrimeData* data, const uint8_t* uid, size_t uid_len);

LegicPrimeData* legic_prime_get_base_data(const LegicPrimeData* data);

#ifdef __cplusplus
}
#endif
