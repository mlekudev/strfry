#pragma once

#include <iostream>
#include <memory>
#include <algorithm>

#include <hoytech/time.h>
#include <hoytech/hex.h>
#include <hoytech/file_change_monitor.h>
#include <uWebSockets/src/uWS.h>
#include <tao/json.hpp>

#include "golpe.h"

#include "Subscription.h"
#include "ThreadPool.h"
#include "events.h"
#include "filters.h"
#include "jsonParseUtils.h"
#include "Decompressor.h"
#include "PrometheusMetrics.h"
#include "RelayMessages.h"
#include "RelaySender.h"


struct RelayServer {
    uS::Async *hubTrigger = nullptr;

    // Thread Pools

    ThreadPool<MsgWebsocket> tpWebsocket;
    ThreadPool<MsgIngester> tpIngester;
    ThreadPool<MsgWriter> tpWriter;
    ThreadPool<MsgReqWorker> tpReqWorker;
    ThreadPool<MsgReqMonitor> tpReqMonitor;
    ThreadPool<MsgNegentropy> tpNegentropy;
    std::thread cronThread;
    std::thread signalHandlerThread;

    // Sender — owns all message dispatch to the websocket thread pool.
    // Initialized after hubTrigger is set in runWebsocket.
    std::unique_ptr<RelaySender> sender;

    void initSender() { sender = std::make_unique<RelaySender>(tpWebsocket, hubTrigger); }

    void run();

    void runWebsocket(ThreadPool<MsgWebsocket>::Thread &thr);

    void runIngester(ThreadPool<MsgIngester>::Thread &thr);
    void ingesterProcessEvent(lmdb::txn &txn, uint64_t connId, std::string ipAddr, secp256k1_context *secpCtx, const tao::json::value &origJson, std::vector<MsgWriter> &output);
    void ingesterProcessReq(lmdb::txn &txn, uint64_t connId, const tao::json::value &origJson, bool counOnly, std::string &outSubIdStr);
    void ingesterProcessClose(lmdb::txn &txn, uint64_t connId, const tao::json::value &origJson);
    void ingesterProcessNegentropy(lmdb::txn &txn, Decompressor &decomp, uint64_t connId, const tao::json::value &origJson);

    void runWriter(ThreadPool<MsgWriter>::Thread &thr);

    void runReqWorker(ThreadPool<MsgReqWorker>::Thread &thr);

    void runReqMonitor(ThreadPool<MsgReqMonitor>::Thread &thr);

    void runNegentropy(ThreadPool<MsgNegentropy>::Thread &thr);

    void runCron();

    void runSignalHandler();

    // Utils — delegate to sender (can be called by any thread)

    void sendToConn(uint64_t connId, std::string &&payload) { sender->sendToConn(connId, std::move(payload)); }
    void sendToConnBinary(uint64_t connId, std::string &&payload) { sender->sendToConnBinary(connId, std::move(payload)); }
    void sendEvent(uint64_t connId, const SubId &subId, std::string_view evJson) { sender->sendEvent(connId, subId, evJson); }
    void sendEventToBatch(RecipientList &&list, std::string &&evJson) { sender->sendEventToBatch(std::move(list), std::move(evJson)); }
    void sendNoticeError(uint64_t connId, std::string &&payload) { sender->sendNoticeError(connId, std::move(payload)); }
    void sendClosedError(uint64_t connId, const std::string &subId, std::string &&payload) { sender->sendClosedError(connId, subId, std::move(payload)); }
    void sendOKResponse(uint64_t connId, std::string_view eventIdHex, bool written, std::string_view message) { sender->sendOKResponse(connId, eventIdHex, written, message); }
};
