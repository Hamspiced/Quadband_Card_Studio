// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
#include "QuadbandPCSC.h"
#include <stddef.h>
#include <PCSC/winscard.h>
int32_t QBCreateContext(int32_t *context) { return SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, context); }
int32_t QBReleaseContext(int32_t context) { return SCardReleaseContext(context); }
int32_t QBListReaders(int32_t context, char *readers, uint32_t *length) { return SCardListReaders(context, NULL, readers, length); }
int32_t QBConnect(int32_t context, const char *reader, int32_t *card, uint32_t *protocol) {
    return SCardConnect(context, reader, SCARD_SHARE_SHARED, SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, card, protocol);
}
int32_t QBBegin(int32_t card) { return SCardBeginTransaction(card); }
int32_t QBReset(int32_t card, uint32_t *protocol) {
    return SCardReconnect(card, SCARD_SHARE_SHARED, SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, SCARD_RESET_CARD, protocol);
}
int32_t QBEnd(int32_t card) { return SCardEndTransaction(card, SCARD_LEAVE_CARD); }
int32_t QBDisconnect(int32_t card) { return SCardDisconnect(card, SCARD_LEAVE_CARD); }
int32_t QBConnectDirect(int32_t context, const char *reader, int32_t *card) {
    uint32_t protocol = 0;
    return SCardConnect(context, reader, SCARD_SHARE_DIRECT, SCARD_PROTOCOL_UNDEFINED, card, &protocol);
}
int32_t QBControl(int32_t card, const uint8_t *bytes, uint32_t count, uint8_t *response, uint32_t *length) {
    // PC/SC-lite / macOS SCARD_CTL_CODE(3500), not the Windows CTL_CODE value.
    uint32_t actual = 0;
    int32_t result = SCardControl(card, 0x42000000u + 3500u, bytes, count, response, *length, &actual);
    *length = actual;
    return result;
}
int32_t QBStatus(int32_t card, char *names, uint32_t *nameLength, uint32_t *state,
                 uint32_t *protocol, uint8_t *atr, uint32_t *atrLength) {
    return SCardStatus(card, names, nameLength, state, protocol, atr, atrLength);
}
int32_t QBTransmit(int32_t card, uint32_t protocol, const uint8_t *bytes, uint32_t count,
                   uint8_t *response, uint32_t *length) {
    SCARD_IO_REQUEST request = { protocol, sizeof(SCARD_IO_REQUEST) };
    return SCardTransmit(card, &request, bytes, count, NULL, response, length);
}

int32_t QBCardPresent(int32_t context, const char *reader, int *present) {
    SCARD_READERSTATE state = {0};
    state.szReader = reader;
    state.dwCurrentState = SCARD_STATE_UNAWARE;
    int32_t result = SCardGetStatusChange(context, 0, &state, 1);
    if (result != SCARD_S_SUCCESS) return result;
    if (state.dwEventState & (SCARD_STATE_UNKNOWN | SCARD_STATE_UNAVAILABLE)) return SCARD_E_READER_UNAVAILABLE;
    *present = (state.dwEventState & SCARD_STATE_PRESENT) != 0;
    return SCARD_S_SUCCESS;
}
