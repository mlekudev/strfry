#pragma once

#include <string>
#include <string_view>

#include <tao/json.hpp>
#include <uWebSockets/src/uWS.h>

#include "Subscription.h"
#include "ThreadPool.h"
#include "PrometheusMetrics.h"
#include "RelayMessages.h"


struct RelaySender {
    ThreadPool<MsgWebsocket> &tpWebsocket;
    uS::Async *hubTrigger;

    RelaySender(ThreadPool<MsgWebsocket> &tp, uS::Async *trigger)
        : tpWebsocket(tp), hubTrigger(trigger) {}

    void sendToConn(uint64_t connId, std::string &&payload) {
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::Send{connId, std::move(payload)}});
        hubTrigger->send();
    }

    void sendToConnBinary(uint64_t connId, std::string &&payload) {
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::SendBinary{connId, std::move(payload)}});
        hubTrigger->send();
    }

    void sendEvent(uint64_t connId, const SubId &subId, std::string_view evJson) {
        PROM_INC_RELAY_MSG("EVENT");
        auto subIdSv = subId.sv();

        std::string reply;
        reply.reserve(13 + subIdSv.size() + evJson.size());

        reply += "[\"EVENT\",\"";
        reply += subIdSv;
        reply += "\",";
        reply += evJson;
        reply += "]";

        sendToConn(connId, std::move(reply));
    }

    void sendEventToBatch(RecipientList &&list, std::string &&evJson) {
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::SendEventToBatch{std::move(list), std::move(evJson)}});
        hubTrigger->send();
    }

    void sendNoticeError(uint64_t connId, std::string &&payload) {
        PROM_INC_RELAY_MSG("NOTICE");
        LI << "sending error to [" << connId << "]: " << payload;
        auto reply = tao::json::value::array({ "NOTICE", std::string("ERROR: ") + payload });
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::Send{connId, std::move(tao::json::to_string(reply))}});
        hubTrigger->send();
    }

    void sendClosedError(uint64_t connId, const std::string &subId, std::string &&payload) {
        PROM_INC_RELAY_MSG("CLOSED");
        LI << "sending closed to [" << connId << "]: " << payload;
        auto reply = tao::json::value::array({ "CLOSED", subId, std::string("ERROR: ") + payload });
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::Send{connId, std::move(tao::json::to_string(reply))}});
        hubTrigger->send();
    }

    void sendOKResponse(uint64_t connId, std::string_view eventIdHex, bool written, std::string_view message) {
        PROM_INC_RELAY_MSG("OK");
        auto reply = tao::json::value::array({ "OK", eventIdHex, written, message });
        tpWebsocket.dispatch(0, MsgWebsocket{MsgWebsocket::Send{connId, std::move(tao::json::to_string(reply))}});
        hubTrigger->send();
    }
};
