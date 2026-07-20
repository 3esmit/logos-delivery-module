// Stub header for the advanced liblogosdelivery kernel API used by unit tests.
#pragma once

#include <liblogosdelivery.h>

#ifdef __cplusplus
extern "C" {
#endif

int waku_store_query(void* ctx, FFICallBack callback, void* userData,
                     const char* jsonQuery, const char* peerAddr, int timeoutMs);

#ifdef __cplusplus
}
#endif
