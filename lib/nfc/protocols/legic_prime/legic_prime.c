#include "legic_prime.h"

#include <furi.h>

#include <nfc/nfc_common.h>

#define LEGIC_PRIME_PROTOCOL_NAME "LegicPrime"
#define LEGIC_PRIME_DEVICE_NAME   "LegicPrime"

#define LEGIC_PRIME_DATA_FORMAT_VERSION   "Data format version"
//#define LEGIC_PRIME_MANUFACTURE_ID        "Manufacture id"
//#define LEGIC_PRIME_MANUFACTURE_PARAMETER "Manufacture parameter"

static const uint32_t legic_prime_data_format_version = 1;

const NfcDeviceBase nfc_device_legic_prime = {
    .protocol_name = LEGIC_PRIME_PROTOCOL_NAME,
    .alloc = (NfcDeviceAlloc)legic_prime_alloc,
    .free = (NfcDeviceFree)legic_prime_free,
    .reset = (NfcDeviceReset)legic_prime_reset,
    .copy = (NfcDeviceCopy)legic_prime_copy,
    .verify = (NfcDeviceVerify)legic_prime_verify,
    .load = (NfcDeviceLoad)legic_prime_load,
    .save = (NfcDeviceSave)legic_prime_save,
    .is_equal = (NfcDeviceEqual)legic_prime_is_equal,
    .get_name = (NfcDeviceGetName)legic_prime_get_device_name,
    .get_uid = (NfcDeviceGetUid)legic_prime_get_uid,
    .set_uid = (NfcDeviceSetUid)legic_prime_set_uid,
    .get_base_data = (NfcDeviceGetBaseData)legic_prime_get_base_data,
};

LegicPrimeData* legic_prime_alloc(void) {
    LegicPrimeData* data = malloc(sizeof(LegicPrimeData));
    return data;
}

void legic_prime_free(LegicPrimeData* data) {
    furi_check(data);
    free(data);
}

void legic_prime_reset(LegicPrimeData* data) {
    furi_check(data);
    memset(data, 0, sizeof(LegicPrimeData));
}

void legic_prime_copy(LegicPrimeData* data, const LegicPrimeData* other) {
    furi_check(data);
    furi_check(other);

    *data = *other;
}

bool legic_prime_verify(LegicPrimeData* data, const FuriString* device_type) {
    UNUSED(data);
    UNUSED(device_type);

    return false;
}

bool legic_prime_load(LegicPrimeData* data, FlipperFormat* ff, uint32_t version) {
    furi_check(data);

    bool parsed = false;

    do {
        if(version < NFC_UNIFIED_FORMAT_VERSION) break;

        parsed = true;
        uint32_t blocks_total = 0;
        uint32_t blocks_read = 0;
        if(!flipper_format_read_uint32(ff, "Bytes total", &blocks_total, 1)) break;
        if(!flipper_format_read_uint32(ff, "Bytes read", &blocks_read, 1)) break;
        data->blocks_total = (uint16_t)blocks_total;
        data->blocks_read = (uint16_t)blocks_read;

        FuriString* temp_str = furi_string_alloc();
        furi_string_printf(temp_str, "Data");
        if(!flipper_format_read_hex(
               ff,
               furi_string_get_cstr(temp_str),
               (&data->data[0]),
               blocks_total * sizeof(uint8_t))) {
            parsed = false;
            break;
        }
    } while(false);

    return parsed;
}

bool legic_prime_save(const LegicPrimeData* data, FlipperFormat* ff) {
    furi_check(data);

    bool saved = false;

    do {
        if(!flipper_format_write_comment_cstr(ff, LEGIC_PRIME_PROTOCOL_NAME " specific data")) break;
        if(!flipper_format_write_uint32(
               ff, LEGIC_PRIME_DATA_FORMAT_VERSION, &legic_prime_data_format_version, 1))
            break;

        uint32_t blocks_total = data->blocks_total;
        uint32_t blocks_read = data->blocks_read;
        if(!flipper_format_write_uint32(ff, "Bytes total", &blocks_total, 1)) break;
        if(!flipper_format_write_uint32(ff, "Bytes read", &blocks_read, 1)) break;

        saved = true;
        FuriString* temp_str = furi_string_alloc();
        furi_string_printf(temp_str, "Data");
        if(!flipper_format_write_hex(
               ff,
               furi_string_get_cstr(temp_str),
               (&data->data[0]),
               blocks_total * sizeof(uint8_t))) {
            saved = false;
            break;
        }
        furi_string_free(temp_str);
    } while(false);

    return saved;
}

bool legic_prime_is_equal(const LegicPrimeData* data, const LegicPrimeData* other) {
    furi_check(data);
    furi_check(other);

    return memcmp(data, other, sizeof(LegicPrimeData)) == 0;
}

const char* legic_prime_get_device_name(const LegicPrimeData* data, NfcDeviceNameType name_type) {
    UNUSED(data);
    UNUSED(name_type);

    return LEGIC_PRIME_DEVICE_NAME;
}

const uint8_t* legic_prime_get_uid(const LegicPrimeData* data, size_t* uid_len) {
    furi_check(data);

    *uid_len = 4;
    return data->data;
}

bool legic_prime_set_uid(LegicPrimeData* data, const uint8_t* uid, size_t uid_len) {
    furi_check(data);
    UNUSED(uid);
    UNUSED(uid_len);

    const bool uid_valid = 1;

    return uid_valid;
}

LegicPrimeData* legic_prime_get_base_data(const LegicPrimeData* data) {
    UNUSED(data);
    furi_crash("No base data");
}

