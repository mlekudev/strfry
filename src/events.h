#pragma once

#include <secp256k1_schnorrsig.h>

#include "golpe.h"

#include "Bytes32.h"
#include "PackedEvent.h"
#include "NegentropyFilterCache.h"
#include "Decompressor.h"




struct EventKind {
    uint64_t value;

    explicit EventKind(uint64_t v) : value(v) {}
    operator uint64_t() const { return value; }

    bool isReplaceable() const {
        return value == 0 || value == 3 || value == 41 ||
               (value >= 10'000 && value < 20'000);
    }

    bool isParamReplaceable() const {
        return value >= 30'000 && value < 40'000;
    }

    bool isEphemeral() const {
        return value >= 20'000 && value < 30'000;
    }

    bool isDeletion() const { return value == 5; }
};

// Backward-compatible free functions wrapping EventKind
inline bool isReplaceableKind(uint64_t kind) { return EventKind(kind).isReplaceable(); }
inline bool isParamReplaceableKind(uint64_t kind) { return EventKind(kind).isParamReplaceable(); }
inline bool isEphemeralKind(uint64_t kind) { return EventKind(kind).isEphemeral(); }




std::string nostrJsonToPackedEvent(const tao::json::value &v);
Bytes32 nostrHash(const tao::json::value &origJson);

bool verifySig(secp256k1_context* ctx, std::string_view sig, std::string_view hash, std::string_view pubkey);
void verifyNostrEvent(secp256k1_context *secpCtx, PackedEventView packed, const tao::json::value &origJson);
void verifyNostrEventJsonSize(std::string_view jsonStr);
void verifyEventTimestamp(PackedEventView packed);

void parseAndVerifyEvent(const tao::json::value &origJson, secp256k1_context *secpCtx, bool verifyMsg, bool verifyTime, std::string &packedStr, std::string &jsonStr);



std::optional<defaultDb::environment::View_Event> lookupEventById(lmdb::txn &txn, std::string_view id);
inline bool eventExistsById(lmdb::txn &txn, std::string_view id) { return lookupEventById(txn, id).has_value(); }
defaultDb::environment::View_Event lookupEventByLevId(lmdb::txn &txn, uint64_t levId); // throws if can't find
uint64_t getMostRecentLevId(lmdb::txn &txn);
std::string_view decodeEventPayload(lmdb::txn &txn, Decompressor &decomp, std::string_view raw, uint32_t *outDictId, size_t *outCompressedSize);
std::string_view getEventJson(lmdb::txn &txn, Decompressor &decomp, uint64_t levId);
std::string_view getEventJson(lmdb::txn &txn, Decompressor &decomp, uint64_t levId, std::string_view eventPayload);




enum class EventSourceType {
    None = 0,
    IP4 = 1,
    IP6 = 2,
    Import = 3,
    Stream = 4,
    Sync = 5,
    Stored = 6,
};

inline std::string eventSourceTypeToStr(EventSourceType t) {
    if (t == EventSourceType::IP4) return "IP4";
    else if (t == EventSourceType::IP6) return "IP6";
    else if (t == EventSourceType::Import) return "Import";
    else if (t == EventSourceType::Stream) return "Stream";
    else if (t == EventSourceType::Sync) return "Sync";
    else return "?";
}



enum class EventWriteStatus {
    Pending,
    Written,
    Duplicate,
    Replaced,
    Deleted,
};


struct EventToWrite {
    std::string packedStr;
    std::string jsonStr;
    std::string ipAddr;
    uint64_t connId = 0;
    EventWriteStatus status = EventWriteStatus::Pending;
    uint64_t levId = 0;

    EventToWrite() {}

    EventToWrite(std::string packedStr, std::string jsonStr, uint64_t connId = 0, std::string ipAddr = "") : packedStr(packedStr), jsonStr(jsonStr), ipAddr(ipAddr), connId(connId) {
    }

    std::string_view id() {
        return PackedEventView(packedStr).id();
    }

    uint64_t createdAt() {
        return PackedEventView(packedStr).created_at();
    }
};


void writeEvents(lmdb::txn &txn, NegentropyFilterCache &neFilterCache, std::vector<EventToWrite> &evs, uint64_t logLevel = 1);
bool deleteEventBasic(lmdb::txn &txn, uint64_t levId);

template <typename C>
uint64_t deleteEvents(lmdb::txn &txn, NegentropyFilterCache &neFilterCache, const C &levIds) {
    uint64_t numDeleted = 0;

    neFilterCache.ctx(txn, [&](const std::function<void(const PackedEventView &, bool)> &updateNegentropy){
        for (auto levId : levIds) {
            auto evToDel = env.lookup_Event(txn, levId);
            if (!evToDel) continue; // already deleted
            updateNegentropy(PackedEventView(evToDel->buf), false);
            if (deleteEventBasic(txn, levId)) numDeleted++;
        }
    });

    return numDeleted;
}
