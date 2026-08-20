// Test stub for the typed C ABI emitted by logos-delivery's nim-ffi backend.
#pragma once
#ifndef LOGOS_DELIVERY_TEST_STUB_H
#define LOGOS_DELIVERY_TEST_STUB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RET_OK 0
#define RET_ERR 1
#define RET_MISSING_CALLBACK 2
#define RET_STALE_WARN 3

typedef void (*FFICallBack)(int callerRet, const char* msg, size_t len, void* userData);
typedef void (*LogosDeliveryScalarRawFn)(int callerRet, char* msg, size_t len, void* userData);
typedef void (*LogosDeliveryCreateRawFn)(int callerRet,
                                         const char* ctxAddress,
                                         const char* errorMessage,
                                         void* userData);

typedef struct {
    void* ptr;
} LogosDeliveryCtx;

typedef void (*LogosDeliveryCreateFn)(int callerRet,
                                      LogosDeliveryCtx* ctx,
                                      const char* errorMessage,
                                      void* userData);

typedef struct { const char* configJson; } LogosdeliveryCreateNodeCtorReq;
typedef struct { const char* messageJson; } LogosdeliverySendReq;
typedef struct { const char* contentTopic; } LogosdeliverySubscribeReq;
typedef struct { const char* contentTopic; } LogosdeliveryUnsubscribeReq;
typedef struct {
    const char* jsonQuery;
    const char* peerAddr;
    int32_t timeoutMs;
} WakuStoreQueryReq;
typedef struct { uint8_t _placeholder; } WakuGetConnectedPeersInfoReq;
typedef struct {
    const char* channelIdStr;
    const char* contentTopicStr;
    const char* senderIdStr;
} LogosdeliveryChannelCreateReq;
typedef struct { const char* channelIdStr; } LogosdeliveryChannelExistsReq;
typedef struct {
    const char* channelIdStr;
    const char* messageJson;
} LogosdeliveryChannelSendReq;
typedef struct { const char* channelIdStr; } LogosdeliveryChannelCloseReq;
typedef struct { uint8_t _placeholder; } LogosdeliveryGetAvailableNodeInfoIdsReq;
typedef struct { const char* nodeInfoId; } LogosdeliveryGetNodeInfoReq;
typedef struct { uint8_t _placeholder; } LogosdeliveryGetAvailableConfigsReq;

typedef void (*LogosdeliverySendReplyFn)(int, const char*, const char*, void*);
typedef void (*LogosdeliverySubscribeReplyFn)(int, const char*, const char*, void*);
typedef void (*LogosdeliveryUnsubscribeReplyFn)(int, const char*, const char*, void*);
typedef void (*WakuStoreQueryReplyFn)(int, const char*, const char*, void*);
typedef void (*WakuGetConnectedPeersInfoReplyFn)(int, const char*, const char*, void*);
typedef void (*LogosdeliveryChannelCreateReplyFn)(int, const char*, const char*, void*);
typedef void (*LogosdeliveryChannelExistsReplyFn)(int, const char*, const char*, void*);
typedef void (*LogosdeliveryChannelSendReplyFn)(int, const char*, const char*, void*);
typedef void (*LogosdeliveryChannelCloseReplyFn)(int, const char*, const char*, void*);
typedef void (*LogosdeliveryGetAvailableNodeInfoIdsReplyFn)(int, const char*, const char*, void*);
typedef void (*LogosdeliveryGetNodeInfoReplyFn)(int, const char*, const char*, void*);
typedef void (*LogosdeliveryGetAvailableConfigsReplyFn)(int, const char*, const char*, void*);

void* logosdelivery_create_node(const LogosdeliveryCreateNodeCtorReq* req,
                                LogosDeliveryCreateRawFn onCreated,
                                void* userData);
int logosdelivery_ctx_create(const char* configJson,
                             LogosDeliveryCreateFn onCreated,
                             void* userData);
int logosdelivery_start_node(void* ctx, LogosDeliveryScalarRawFn callback, void* userData);
int logosdelivery_stop_node(void* ctx, LogosDeliveryScalarRawFn callback, void* userData);
int logosdelivery_destroy(void* ctx);

int logosdelivery_send(void* ctx,
                       LogosdeliverySendReplyFn callback,
                       void* userData,
                       const LogosdeliverySendReq* req);
int logosdelivery_subscribe(void* ctx,
                            LogosdeliverySubscribeReplyFn callback,
                            void* userData,
                            const LogosdeliverySubscribeReq* req);
int logosdelivery_unsubscribe(void* ctx,
                              LogosdeliveryUnsubscribeReplyFn callback,
                              void* userData,
                              const LogosdeliveryUnsubscribeReq* req);
int waku_store_query(void* ctx,
                     WakuStoreQueryReplyFn callback,
                     void* userData,
                     const WakuStoreQueryReq* req);
int waku_get_connected_peers_info(void* ctx,
                                  WakuGetConnectedPeersInfoReplyFn callback,
                                  void* userData,
                                  const WakuGetConnectedPeersInfoReq* req);
int logosdelivery_channel_create(void* ctx,
                                 LogosdeliveryChannelCreateReplyFn callback,
                                 void* userData,
                                 const LogosdeliveryChannelCreateReq* req);
int logosdelivery_channel_exists(void* ctx,
                                 LogosdeliveryChannelExistsReplyFn callback,
                                 void* userData,
                                 const LogosdeliveryChannelExistsReq* req);
int logosdelivery_channel_send(void* ctx,
                               LogosdeliveryChannelSendReplyFn callback,
                               void* userData,
                               const LogosdeliveryChannelSendReq* req);
int logosdelivery_channel_close(void* ctx,
                                LogosdeliveryChannelCloseReplyFn callback,
                                void* userData,
                                const LogosdeliveryChannelCloseReq* req);
int logosdelivery_get_available_node_info_ids(
    void* ctx,
    LogosdeliveryGetAvailableNodeInfoIdsReplyFn callback,
    void* userData,
    const LogosdeliveryGetAvailableNodeInfoIdsReq* req);
int logosdelivery_get_node_info(void* ctx,
                                LogosdeliveryGetNodeInfoReplyFn callback,
                                void* userData,
                                const LogosdeliveryGetNodeInfoReq* req);
int logosdelivery_get_available_configs(void* ctx,
                                        LogosdeliveryGetAvailableConfigsReplyFn callback,
                                        void* userData,
                                        const LogosdeliveryGetAvailableConfigsReq* req);

uint64_t logosdelivery_add_event_listener(void* ctx,
                                          const char* eventName,
                                          FFICallBack callback,
                                          void* userData);
int logosdelivery_remove_event_listener(void* ctx, uint64_t listenerId);

#ifdef __cplusplus
}
#endif

#endif
