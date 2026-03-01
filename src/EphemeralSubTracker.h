#pragma once

#include <shared_mutex>
#include "golpe.h"

// Thread-safe tracker for ephemeral event subscription state.
// The ReqMonitor thread updates kind refcounts and catch-all count on sub add/remove.
// The Ingester thread queries it lock-free (shared_lock) to skip signature verification
// for ephemeral events that have no subscribers.

struct EphemeralSubTracker : NonCopyable {
    bool hasSubscribersForKind(uint64_t kind) {
        std::shared_lock lock(mtx);
        return catchAllCount > 0 || kindRefcounts.count(kind) > 0;
    }

    void addKind(uint64_t kind) {
        std::unique_lock lock(mtx);
        kindRefcounts[kind]++;
    }

    void removeKind(uint64_t kind) {
        std::unique_lock lock(mtx);
        auto it = kindRefcounts.find(kind);
        if (it != kindRefcounts.end()) {
            if (--it->second == 0) kindRefcounts.erase(it);
        }
    }

    void addCatchAll() {
        std::unique_lock lock(mtx);
        catchAllCount++;
    }

    void removeCatchAll() {
        std::unique_lock lock(mtx);
        if (catchAllCount > 0) catchAllCount--;
    }

  private:
    mutable std::shared_mutex mtx;
    flat_hash_map<uint64_t, uint64_t> kindRefcounts;
    uint64_t catchAllCount = 0;
};
