// Stub header for liblogosdelivery_kernel - mirrors the subset of
// library/liblogosdelivery_kernel.h from logos-delivery (master 8ad99f1) that
// delivery_module_plugin.cpp actually consumes, so unit tests compile without
// the real library. Keep in sync with the real header when bumping the
// logos-delivery flake input.
//
// The real kernel header declares the full unstable waku_* surface; only the
// functions used by the module are stubbed here on purpose, so an accidental
// new kernel dependency fails loudly at compile time.

#pragma once
#ifndef LOGOS_DELIVERY_KERNEL_TEST_STUB_H
#define LOGOS_DELIVERY_KERNEL_TEST_STUB_H

// Shared FFICallBack typedef and RET_* return codes live in the stable header.
#include "liblogosdelivery.h"

#ifdef __cplusplus
extern "C"
{
#endif

  int waku_store_query(void *ctx,
                       FFICallBack callback,
                       void *userData,
                       const char *jsonQuery,
                       const char *peerAddr,
                       int timeoutMs);

  int waku_get_connected_peers_info(void *ctx,
                                    FFICallBack callback,
                                    void *userData);

#ifdef __cplusplus
}
#endif
#endif /* LOGOS_DELIVERY_KERNEL_TEST_STUB_H */
