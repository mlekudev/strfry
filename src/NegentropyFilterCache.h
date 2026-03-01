#pragma once

#include <string>

#include <negentropy/storage/BTreeLMDB.h>

#include "golpe.h"

#include "filters.h"
#include "PackedEvent.h"


struct NegentropyFilterCache {
    struct FilterInfo {
        NostrFilter f;
        uint64_t treeId;
    };

    std::vector<FilterInfo> filters;
    uint64_t modificationCounter = 0;

    // Kind-based dispatch index: maps kind -> filter indices that could match that kind.
    // Filters with no kind restriction go into noKindFilterIdxs.
    flat_hash_map<uint64_t, std::vector<size_t>> kindToFilterIdx;
    std::vector<size_t> noKindFilterIdxs;

    void ctx(lmdb::txn &txn, const std::function<void(const std::function<void(const PackedEventView &, bool)> &)> &cb) {
        freshenCache(txn);

        std::vector<std::unique_ptr<negentropy::storage::BTreeLMDB>> storages(filters.size());

        cb([&](const PackedEventView &ev, bool insert){
            auto tryFilter = [&](size_t i) {
                const auto &filter = filters[i];
                if (!filter.f.doesMatch(ev)) return;
                if (!storages[i]) storages[i] = std::make_unique<negentropy::storage::BTreeLMDB>(txn, negentropyDbi, filter.treeId);
                if (insert) storages[i]->insert(ev.created_at(), ev.id());
                else storages[i]->erase(ev.created_at(), ev.id());
            };

            // Check filters that match this event's kind, plus filters with no kind restriction
            auto it = kindToFilterIdx.find(ev.kind());
            if (it != kindToFilterIdx.end()) {
                for (auto idx : it->second) tryFilter(idx);
            }
            for (auto idx : noKindFilterIdxs) tryFilter(idx);
        });
    }

  private:
    void freshenCache(lmdb::txn &txn) {
        uint64_t curr = env.lookup_Meta(txn, 1)->negentropyModificationCounter();

        if (curr != modificationCounter) {
            filters.clear();
            kindToFilterIdx.clear();
            noKindFilterIdxs.clear();

            env.foreach_NegentropyFilter(txn, [&](auto &f){
                filters.emplace_back(
                    NostrFilter(tao::json::from_string(f.filter()), MAX_U64),
                    f.primaryKeyId
                );
                return true;
            });

            // Build kind-based dispatch index
            for (size_t i = 0; i < filters.size(); i++) {
                if (filters[i].f.kinds) {
                    for (size_t j = 0; j < filters[i].f.kinds->size(); j++) {
                        kindToFilterIdx[filters[i].f.kinds->at(j)].push_back(i);
                    }
                } else {
                    noKindFilterIdxs.push_back(i);
                }
            }

            modificationCounter = curr;
        }
    }
};
