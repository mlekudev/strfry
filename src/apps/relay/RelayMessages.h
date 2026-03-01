#pragma once

#include <string>
#include <variant>

#include "golpe.h"

#include "Subscription.h"


struct MsgWebsocket : NonCopyable {
    struct Send {
        uint64_t connId;
        std::string payload;
    };

    struct SendBinary {
        uint64_t connId;
        std::string payload;
    };

    struct SendEventToBatch {
        RecipientList list;
        std::string evJson;
    };

    struct GracefulShutdown {
    };

    using Var = std::variant<Send, SendBinary, SendEventToBatch, GracefulShutdown>;
    Var msg;
    MsgWebsocket(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgIngester : NonCopyable {
    struct ClientMessage {
        uint64_t connId;
        std::string ipAddr;
        std::string payload;
    };

    struct CloseConn {
        uint64_t connId;
    };

    using Var = std::variant<ClientMessage, CloseConn>;
    Var msg;
    MsgIngester(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgWriter : NonCopyable {
    struct AddEvent {
        uint64_t connId;
        std::string ipAddr;
        std::string packedStr;
        std::string jsonStr;
    };

    struct CloseConn {
        uint64_t connId;
    };

    using Var = std::variant<AddEvent, CloseConn>;
    Var msg;
    MsgWriter(Var &&msg_) : msg(std::move(msg_)) {}
};


// Shared sub-message types used by both MsgReqWorker and MsgReqMonitor
struct SubMsgCommon {
    struct NewSub {
        Subscription sub;
    };

    struct RemoveSub {
        uint64_t connId;
        SubId subId;
    };

    struct CloseConn {
        uint64_t connId;
    };
};

struct MsgReqWorker : NonCopyable {
    using NewSub = SubMsgCommon::NewSub;
    using RemoveSub = SubMsgCommon::RemoveSub;
    using CloseConn = SubMsgCommon::CloseConn;

    using Var = std::variant<NewSub, RemoveSub, CloseConn>;
    Var msg;
    MsgReqWorker(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgReqMonitor : NonCopyable {
    using NewSub = SubMsgCommon::NewSub;
    using RemoveSub = SubMsgCommon::RemoveSub;
    using CloseConn = SubMsgCommon::CloseConn;

    struct DBChange {
    };

    using Var = std::variant<NewSub, RemoveSub, CloseConn, DBChange>;
    Var msg;
    MsgReqMonitor(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgNegentropy : NonCopyable {
    struct NegOpen {
        Subscription sub;
        std::string filterStr;
        std::string negPayload;
    };

    struct NegMsg {
        uint64_t connId;
        SubId subId;
        std::string negPayload;
    };

    struct NegClose {
        uint64_t connId;
        SubId subId;
    };

    struct CloseConn {
        uint64_t connId;
    };

    using Var = std::variant<NegOpen, NegMsg, NegClose, CloseConn>;
    Var msg;
    MsgNegentropy(Var &&msg_) : msg(std::move(msg_)) {}
};
