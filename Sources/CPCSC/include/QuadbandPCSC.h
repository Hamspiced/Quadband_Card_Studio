// Copyright (C) 2026 Midwest Gadgets. SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>
// Keep legacy PC/SC headers behind a C boundary; Apple's module excludes Swift.
int32_t QBCreateContext(int32_t *context);
int32_t QBReleaseContext(int32_t context);
int32_t QBListReaders(int32_t context, char *readers, uint32_t *length);
int32_t QBConnect(int32_t context, const char *reader, int32_t *card, uint32_t *protocol);
int32_t QBReset(int32_t card, uint32_t *protocol);
int32_t QBBegin(int32_t card);
int32_t QBEnd(int32_t card);
int32_t QBDisconnect(int32_t card);
int32_t QBStatus(int32_t card, char *names, uint32_t *nameLength, uint32_t *state,
                 uint32_t *protocol, uint8_t *atr, uint32_t *atrLength);
int32_t QBTransmit(int32_t card, uint32_t protocol, const uint8_t *bytes, uint32_t count,
                   uint8_t *response, uint32_t *length);

int32_t QBCardPresent(int32_t context, const char *reader, int *present);
int32_t QBConnectDirect(int32_t context, const char *reader, int32_t *card);
int32_t QBControl(int32_t card, const uint8_t *bytes, uint32_t count, uint8_t *response, uint32_t *length);
