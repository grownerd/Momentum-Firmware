#pragma once

#include <nfc/protocols/legic_prime/legic_prime.h>

#include "../nfc_protocol_support_render_common.h"

void nfc_render_legic_prime_info(
    const LegicPrimeData* data,
    NfcProtocolFormatType format_type,
    FuriString* str);

void nfc_render_legic_prime_tech_type(const LegicPrimeData* data, FuriString* str);

void nfc_render_legic_prime_format_bytes(FuriString* str, const uint8_t* data, size_t size);

void nfc_render_legic_prime_brief(const LegicPrimeData* data, FuriString* str);

void nfc_render_legic_prime_extra(const LegicPrimeData* data, FuriString* str);

void nfc_render_legic_prime_dump(const LegicPrimeData* data, FuriString* str);
