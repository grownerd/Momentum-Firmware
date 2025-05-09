#include "legic_prime_render.h"

void nfc_render_legic_prime_format_bytes(FuriString* str, const uint8_t* data, size_t size) {
    for(size_t i = 0; i < size; i++) {
        furi_string_cat_printf(str, " %02X", data[i]);
    }
}

void nfc_render_legic_prime_tech_type(const LegicPrimeData* data, FuriString* str) {
    furi_string_cat_printf(str, "Tech: Legic Prime MIM%d (NFC-A)\n", data->blocks_read);
}

void nfc_render_legic_prime_info(
    const LegicPrimeData* data,
    NfcProtocolFormatType format_type,
    FuriString* str) {
    if(format_type == NfcProtocolFormatTypeFull) {
        nfc_render_legic_prime_tech_type(data, str);
    }

    nfc_render_legic_prime_brief(data, str);

    if(format_type == NfcProtocolFormatTypeFull) {
        nfc_render_legic_prime_extra(data, str);
    }
}

void nfc_render_legic_prime_brief(const LegicPrimeData* data, FuriString* str) {
    furi_string_cat_printf(str, "\nUID: %02X %02X %02X %02X  ", data->data[0], data->data[1], data->data[2], data->data[3]);
}

void nfc_render_legic_prime_extra(const LegicPrimeData* data, FuriString* str) {
    furi_string_cat_printf(str, "Data:");
    nfc_render_legic_prime_format_bytes(str, (const uint8_t*)&(data->data), (size_t)data->blocks_read);
}
