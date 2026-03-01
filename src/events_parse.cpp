#include <openssl/sha.h>

#include "events.h"
#include "jsonParseUtils.h"


std::string nostrJsonToPackedEvent(const tao::json::value &v) {
    PackedEventTagBuilder tagBuilder;

    // Extract values from JSON, add strings to builder

    auto id = from_hex(jsonGetString(v.at("id"), "event id field was not a string"), false);
    auto pubkey = from_hex(jsonGetString(v.at("pubkey"), "event pubkey field was not a string"), false);
    uint64_t created_at = jsonGetUnsigned(v.at("created_at"), "event created_at field was not an integer");
    uint64_t kind = jsonGetUnsigned(v.at("kind"), "event kind field was not an integer");

    if (id.size() != 32) throw herr("unexpected id size");
    if (pubkey.size() != 32) throw herr("unexpected pubkey size");

    jsonGetString(v.at("content"), "event content field was not a string");

    uint64_t expiration = 0;

    if (isReplaceableKind(kind)) {
        // Prepend virtual d-tag
        tagBuilder.add('d', "");
    }

    if (jsonGetArray(v.at("tags"), "tags field not an array").size() > cfg().events__maxNumTags) throw herr("too many tags: ", v.at("tags").get_array().size());
    for (auto &tagArr : v.at("tags").get_array()) {
        auto &tag = jsonGetArray(tagArr, "tag in tags field was not an array");
        if (tag.size() < 1) throw herr("too few fields in tag");

        auto tagName = jsonGetString(tag.at(0), "tag name was not a string");
        auto tagVal = tag.size() >= 2 ? jsonGetString(tag.at(1), "tag val was not a string") : "";

        if (tagName == "e" || tagName == "p") {
            if (tagVal.size() != 64) throw herr("unexpected size for fixed-size tag: ", tagName);
            tagVal = from_hex(tagVal, false);

            tagBuilder.add(tagName[0], tagVal);
        } else if (tagName == "expiration") {
            if (expiration == 0) {
                expiration = parseUint64(tagVal);
                if (expiration < 100) throw herr("invalid expiration");
            }
        } else if (tagName.size() == 1) {
            if (tagVal.size() > cfg().events__maxTagValSize) throw herr("tag val too large: ", tagVal.size());

            if (tagVal.size() <= MAX_INDEXED_TAG_VAL_SIZE) {
                tagBuilder.add(tagName[0], tagVal);
            }
        }
    }

    if (isParamReplaceableKind(kind)) {
        // Append virtual d-tag
        tagBuilder.add('d', "");
    }

    if (isEphemeralKind(kind)) {
        expiration = 1;
    }

    PackedEventBuilder builder(id, pubkey, created_at, kind, expiration, tagBuilder);

    return std::move(builder.buf);
}

Bytes32 nostrHash(const tao::json::value &origJson) {
    tao::json::value arr = tao::json::empty_array;

    arr.emplace_back(0);

    arr.emplace_back(origJson.at("pubkey"));
    arr.emplace_back(origJson.at("created_at"));
    arr.emplace_back(origJson.at("kind"));
    arr.emplace_back(origJson.at("tags"));
    arr.emplace_back(origJson.at("content"));

    std::string encoded = tao::json::to_string(arr);

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(), hash);

    return Bytes32(std::string_view(reinterpret_cast<char*>(hash), SHA256_DIGEST_LENGTH));
}

bool verifySig(secp256k1_context* ctx, std::string_view sig, std::string_view hash, std::string_view pubkey) {
    if (sig.size() != 64 || hash.size() != 32 || pubkey.size() != 32) throw herr("verify sig: bad input size");

    secp256k1_xonly_pubkey pubkeyParsed;
    if (!secp256k1_xonly_pubkey_parse(ctx, &pubkeyParsed, (const uint8_t*)pubkey.data())) throw herr("verify sig: bad pubkey");

    return secp256k1_schnorrsig_verify(
                ctx,
                (const uint8_t*)sig.data(),
                (const uint8_t*)hash.data(),
#ifdef SECP256K1_SCHNORRSIG_EXTRAPARAMS_INIT // old versions of libsecp256k1 didn't take a msg size param, this define added just after
                hash.size(),
#endif
                &pubkeyParsed
    );
}

void verifyNostrEvent(secp256k1_context *secpCtx, PackedEventView packed, const tao::json::value &origJson) {
    auto hash = nostrHash(origJson);
    if (hash != Bytes32(packed.id())) throw herr("bad event id");

    bool valid = verifySig(secpCtx, from_hex(jsonGetString(origJson.at("sig"), "event sig was not a string"), false), packed.id(), packed.pubkey());
    if (!valid) throw herr("bad signature");
}

void verifyNostrEventJsonSize(std::string_view jsonStr) {
    if (jsonStr.size() > cfg().events__maxEventSize) throw herr("event too large: ", jsonStr.size());
}

void verifyEventTimestamp(PackedEventView packed) {
    auto now = hoytech::curr_time_s();
    auto ts = packed.created_at();

    bool isEphemeral = packed.expiration() == 1;

    uint64_t earliest = now - (isEphemeral ? cfg().events__rejectEphemeralEventsOlderThanSeconds : cfg().events__rejectEventsOlderThanSeconds);
    uint64_t latest = now + cfg().events__rejectEventsNewerThanSeconds;

    // overflows
    if (earliest > now) earliest = 0;
    if (latest < now) latest = MAX_U64 - 1;

    if (ts < earliest) throw herr(isEphemeral ? "ephemeral event expired" : "created_at too early");
    if (ts > latest) throw herr("created_at too late");

    if (packed.expiration() > 1 && packed.expiration() <= now) throw herr("event expired");
}


void parseAndVerifyEvent(const tao::json::value &origJson, secp256k1_context *secpCtx, bool verifyMsg, bool verifyTime, std::string &packedStr, std::string &jsonStr) {
    if (!origJson.is_object()) throw herr("event is not an object");

    packedStr = nostrJsonToPackedEvent(origJson);
    PackedEventView packed(packedStr);
    if (verifyTime) verifyEventTimestamp(packed);
    if (verifyMsg) verifyNostrEvent(secpCtx, packed, origJson);

    // Build new object to remove unknown top-level fields from json
    jsonStr = tao::json::to_string(tao::json::value({
        { "content", &origJson.at("content") },
        { "created_at", &origJson.at("created_at") },
        { "id", &origJson.at("id") },
        { "kind", &origJson.at("kind") },
        { "pubkey", &origJson.at("pubkey") },
        { "sig", &origJson.at("sig") },
        { "tags", &origJson.at("tags") },
    }));

    if (verifyMsg) verifyNostrEventJsonSize(jsonStr);
}
