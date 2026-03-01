#include "events.h"


// Do not use externally: does not handle negentropy trees

bool deleteEventBasic(lmdb::txn &txn, uint64_t levId) {
    bool deleted = env.dbi_EventPayload.del(txn, lmdb::to_sv<uint64_t>(levId));
    env.delete_Event(txn, levId);
    return deleted;
}



void writeEvents(lmdb::txn &txn, NegentropyFilterCache &neFilterCache, std::vector<EventToWrite> &evs, uint64_t logLevel) {
    std::sort(evs.begin(), evs.end(), [](auto &a, auto &b) {
        auto aC = a.createdAt();
        auto bC = b.createdAt();
        if (aC == bC) return a.id() < b.id();
        return aC < bC;
    });

    std::vector<uint64_t> levIdsToDelete;
    std::string tmpBuf;

    neFilterCache.ctx(txn, [&](const std::function<void(const PackedEventView &, bool)> &updateNegentropy){
        for (size_t i = 0; i < evs.size(); i++) {
            auto &ev = evs[i];

            PackedEventView packed(ev.packedStr);

            if (lookupEventById(txn, packed.id()) || (i != 0 && ev.id() == evs[i-1].id())) {
                ev.status = EventWriteStatus::Duplicate;
                continue;
            }

            if (env.lookup_Event__deletion(txn, std::string(packed.id()) + std::string(packed.pubkey()))) {
                ev.status = EventWriteStatus::Deleted;
                continue;
            }

            EventKind kind(packed.kind());

            {
                std::optional<std::string> replace;

                if (kind.isReplaceable() || kind.isParamReplaceable()) {
                    packed.foreachTag([&](char tagName, std::string_view tagVal){
                        if (tagName != 'd') return true;
                        replace = std::string(tagVal);
                        return false;
                    });
                }

                if (replace) {
                    auto searchStr = std::string(packed.pubkey()) + *replace;
                    auto searchKey = makeKey_StringUint64(searchStr, packed.kind());

                    env.generic_foreachFull(txn, env.dbi_Event__replace, searchKey, lmdb::to_sv<uint64_t>(MAX_U64), [&](auto k, auto v) {
                        ParsedKey_StringUint64 parsedKey(k);
                        if (parsedKey.s == searchStr && parsedKey.n == packed.kind()) {
                            auto otherEv = lookupEventByLevId(txn, lmdb::from_sv<uint64_t>(v));

                            auto thisTimestamp = packed.created_at();
                            auto otherPacked = PackedEventView(otherEv.buf);
                            auto otherTimestamp = otherPacked.created_at();

                            if (otherTimestamp < thisTimestamp ||
                                (otherTimestamp == thisTimestamp && packed.id() < otherPacked.id())) {
                                if (logLevel >= 1) LI << "Deleting event (d-tag). id=" << to_hex(otherPacked.id());
                                levIdsToDelete.push_back(otherEv.primaryKeyId);
                            } else {
                                ev.status = EventWriteStatus::Replaced;
                            }
                        }

                        return false;
                    }, true);
                }
            }

            if (kind.isDeletion()) {
                packed.foreachTag([&](char tagName, std::string_view tagVal){
                    if (tagName == 'e') {
                        auto otherEv = lookupEventById(txn, tagVal);
                        if (otherEv && PackedEventView(otherEv->buf).pubkey() == packed.pubkey()) {
                            if (logLevel >= 1) LI << "Deleting event (kind 5). id=" << to_hex(tagVal);
                            levIdsToDelete.push_back(otherEv->primaryKeyId);
                        }
                    }
                    return true;
                });
            }

            if (ev.status == EventWriteStatus::Pending) {
                ev.levId = env.insert_Event(txn, ev.packedStr);

                tmpBuf.clear();
                tmpBuf += '\x00';
                tmpBuf += ev.jsonStr;
                env.dbi_EventPayload.put(txn, lmdb::to_sv<uint64_t>(ev.levId), tmpBuf);

                updateNegentropy(PackedEventView(ev.packedStr), true);

                ev.status = EventWriteStatus::Written;

                // Deletions happen after event was written to ensure levIds are not reused

                for (auto levId : levIdsToDelete) {
                    auto evToDel = env.lookup_Event(txn, levId);
                    if (!evToDel) continue; // already deleted
                    updateNegentropy(PackedEventView(evToDel->buf), false);
                    deleteEventBasic(txn, levId);
                }

                levIdsToDelete.clear();
            }

            if (levIdsToDelete.size()) throw herr("unprocessed deletion");
        }
    });
}
