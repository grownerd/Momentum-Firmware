#include "legic_prime_poller_i.h"


#define TAG "LegicPrimePoller"

static LegicPrimeError legic_prime_poller_process_error(NfcError error) {
    switch(error) {
    case NfcErrorNone:
        return LegicPrimeErrorNone;
    case NfcErrorTimeout:
        return LegicPrimeErrorTimeout;
    default:
        return LegicPrimeErrorNotPresent;
    }
}

static LegicPrimeError legic_prime_init_tag(LegicPrimeTagType cardtype, LegicPrimeTag *p_card) {
    p_card->tagtype = cardtype;

    switch (p_card->tagtype) {
        case 0x0d:
            p_card->cmdsize = 6;
            p_card->addrsize = 5;
            p_card->cardsize = 22;
            break;
        case 0x1d:
            p_card->cmdsize = 9;
            p_card->addrsize = 8;
            p_card->cardsize = 256;
            break;
        case 0x3d:
            p_card->cmdsize = 11;
            p_card->addrsize = 10;
            p_card->cardsize = 1024;
            break;
        default:
            p_card->cmdsize = 0;
            p_card->addrsize = 0;
            p_card->cardsize = 0;
            return LegicPrimeErrorNotPresent;
    }
    return LegicPrimeErrorNone;
}

LegicPrimeError legic_prime_poller_frame_exchange(
    const LegicPrimePoller* instance,
    const BitBuffer* tx_buffer,
    BitBuffer* rx_buffer,
    uint32_t fwt) {
    furi_assert(instance);

    const size_t tx_bytes = bit_buffer_get_size_bytes(tx_buffer);
    furi_assert(tx_bytes <= bit_buffer_get_capacity_bytes(instance->tx_buffer));

    LegicPrimeError ret = LegicPrimeErrorNone;

    do {
        NfcError error =
            nfc_legic_prime_poller_trx(instance->nfc, instance->tx_buffer, instance->rx_buffer, fwt);
            //nfc_poller_trx(instance->nfc, instance->tx_buffer, instance->rx_buffer, fwt);
        if(error != NfcErrorNone) {
            ret = legic_prime_poller_process_error(error);
            break;
        }

        bit_buffer_copy(rx_buffer, instance->rx_buffer);
#if 0
        if(!legic_prime_crc_check(instance->rx_buffer)) {
            ret = LegicPrimeErrorWrongCrc;
            break;
        }

        legic_prime_crc_trim(rx_buffer);
#endif
    } while(false);

    return ret;
}

LegicPrimeError legic_prime_poller_polling(
    LegicPrimePoller* instance,
    const LegicPrimePollerPollingCommand* cmd,
    LegicPrimePollerPollingResponse* resp) {
    furi_assert(instance);
    furi_assert(cmd);
    furi_assert(resp);

    LegicPrimeError error = LegicPrimeErrorNone;

    do {
        bit_buffer_reset(instance->tx_buffer);
        bit_buffer_reset(instance->rx_buffer);

        for (int i=0; i<cmd->num_bits; i++)
        {
            uint8_t bit = (cmd->data >> i) & 0x01;
            bit_buffer_append_bit(instance->tx_buffer, bit);
        }

        for (int i=0; i<resp->num_bits; i++)
        {
            uint8_t bit = (resp->data >> i) & 0x01;
            bit_buffer_append_bit(instance->rx_buffer, bit);
        }

        error = legic_prime_poller_frame_exchange(
            instance, instance->tx_buffer, instance->rx_buffer, LEGIC_PRIME_POLLER_POLLING_FWT);

        if(error != LegicPrimeErrorNone) break;
        bit_buffer_write_bytes_mid(instance->rx_buffer, &(resp->data), 0, 1);

    } while(false);

    return error;
}


LegicPrimeError legic_prime_poller_read_byte(
    LegicPrimePoller* instance,
    uint16_t addr,
    LegicPrimePollerReadCommandResponse** const response_ptr) {

    furi_assert(instance);
    furi_assert(addr);
    furi_assert(response_ptr);

    // Prepare bit buffer
    bit_buffer_reset(instance->rx_buffer);
    bit_buffer_reset(instance->tx_buffer);

    uint16_t tx_frame = addr << 1 | 1;

    uint16_t cmdsize = instance->data->tag.cmdsize;

    for (int i=0; i<cmdsize; i++)
    {
        uint8_t bit = (tx_frame >> i) & 0x01;
        bit_buffer_append_bit(instance->tx_buffer, bit);
    }

    LegicPrimeError error = legic_prime_poller_frame_exchange(
        instance, instance->tx_buffer, instance->rx_buffer, LEGIC_PRIME_POLLER_POLLING_FWT);
    if(error == LegicPrimeErrorNone) {
        *response_ptr = (LegicPrimePollerReadCommandResponse*)bit_buffer_get_data(instance->rx_buffer);
    }
    return error;
}

LegicPrimeError legic_prime_poller_write_blocks(
    const LegicPrimePoller* instance,
    const uint8_t block_count,
    const uint8_t* const block_numbers,
    const uint8_t* data,
    LegicPrimePollerWriteCommandResponse** const response_ptr) {
    furi_assert(instance);
    furi_assert(block_count <= 2);
    furi_assert(block_numbers);
    furi_assert(data);
    furi_assert(response_ptr);

    return LegicPrimeErrorNone;
#if 0
    legic_prime_poller_prepare_tx_buffer(
        instance,
        LEGIC_PRIME_CMD_WRITE_WITHOUT_ENCRYPTION,
        LEGIC_PRIME_SERVICE_RW_ACCESS,
        block_count,
        block_numbers,
        block_count,
        data);
    bit_buffer_reset(instance->rx_buffer);

    LegicPrimeError error = legic_prime_poller_frame_exchange(
        instance, instance->tx_buffer, instance->rx_buffer, LEGIC_PRIME_POLLER_POLLING_FWT);
    if(error == LegicPrimeErrorNone) {
        *response_ptr =
            (LegicPrimePollerWriteCommandResponse*)bit_buffer_get_data(instance->rx_buffer);
    }
    return error;
#endif
}

LegicPrimeError legic_prime_poller_activate(LegicPrimePoller* instance, LegicPrimeData* data) {
    furi_assert(instance);

    LegicPrimeError ret;
    uint32_t timeout = 100;

    do {
        bit_buffer_reset(instance->tx_buffer);
        bit_buffer_reset(instance->rx_buffer);

        // Send Polling command
        LegicPrimePollerPollingCommand polling_cmd = {
            .num_bits = 7,
            .data = timeout,
        };

        LegicPrimePollerPollingResponse polling_resp = {
            .num_bits = 6,
            .data = 0,
        };

        ret = legic_prime_poller_polling(instance, &polling_cmd, &polling_resp);
        if(ret != LegicPrimeErrorNone) {
            FURI_LOG_T(TAG, "Activation failed error: %d", ret);
            break;
        }

        ret = legic_prime_init_tag(polling_resp.data, &data->tag);
        if(ret != LegicPrimeErrorNone) {
            FURI_LOG_T(TAG, "Activation failed error: %d", ret);
            break;
        }
        data->blocks_total = data->tag.cardsize;

        instance->state = LegicPrimePollerStateActivated;
    } while(false);

    return ret;
}
