#include "events.h"


std::optional<defaultDb::environment::View_Event> lookupEventById(lmdb::txn &txn, std::string_view id) {
    std::optional<defaultDb::environment::View_Event> output;

    env.generic_foreachFull(txn, env.dbi_Event__id, makeKey_StringUint64(id, 0), lmdb::to_sv<uint64_t>(0), [&](auto k, auto v) {
        if (k.starts_with(id)) output = env.lookup_Event(txn, lmdb::from_sv<uint64_t>(v));
        return false;
    });

    return output;
}

defaultDb::environment::View_Event lookupEventByLevId(lmdb::txn &txn, uint64_t levId) {
    auto view = env.lookup_Event(txn, levId);
    if (!view) throw herr("unable to lookup event by levId");
    return *view;
}

uint64_t getMostRecentLevId(lmdb::txn &txn) {
    uint64_t levId = 0;

    env.foreach_Event(txn, [&](auto &ev){
        levId = ev.primaryKeyId;
        return false;
    }, true);

    return levId;
}


// Return result validity same as getEventJson(), see below

std::string_view decodeEventPayload(lmdb::txn &txn, Decompressor &decomp, std::string_view raw, uint32_t *outDictId, size_t *outCompressedSize) {
    if (raw.size() == 0) throw herr("empty event in EventPayload");

    if (raw[0] == '\x00') {
        if (outDictId) *outDictId = 0;
        return raw.substr(1);
    } else if (raw[0] == '\x01') {
        raw = raw.substr(1);
        if (raw.size() < 4) throw herr("EventPayload record too short to read dictId");
        uint32_t dictId = lmdb::from_sv<uint32_t>(raw.substr(0, 4));
        raw = raw.substr(4);

        decomp.reserve(cfg().events__maxEventSize);
        std::string_view buf = decomp.decompress(txn, dictId, raw);

        if (outDictId) *outDictId = dictId;
        if (outCompressedSize) *outCompressedSize = raw.size();
        return buf;
    } else {
        throw herr("Unexpected first byte in EventPayload");
    }
}

// Return result only valid until one of: next call to getEventJson/decodeEventPayload, write to/closing of txn, or any action on decomp object

std::string_view getEventJson(lmdb::txn &txn, Decompressor &decomp, uint64_t levId) {
    std::string_view eventPayload;

    bool found = env.dbi_EventPayload.get(txn, lmdb::to_sv<uint64_t>(levId), eventPayload);
    if (!found) throw herr("couldn't find event in EventPayload");

    return getEventJson(txn, decomp, levId, eventPayload);
}

std::string_view getEventJson(lmdb::txn &txn, Decompressor &decomp, uint64_t levId, std::string_view eventPayload) {
    return decodeEventPayload(txn, decomp, eventPayload, nullptr, nullptr);
}
