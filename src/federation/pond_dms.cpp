#include "federation/pond_dms.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "base/json.h"
#include "base/log.h"
#include "base/seal.h"
#include "core/identity.h"
#include "core/link.h"
#include "core/pond_dms.hpp"
#include "core/ponds.h"
#include "core/streams.h"
#include "events/message.h"
#include "federation/ponds.h"
#include "storage/files.h"
#include "storage/store.h"

#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace puddle;
using pdm::Fetch;
using pdm::Letter;

namespace {

constexpr const char *kDomain = "puddle-letter-v1\n"; /* what a letter's signature and sealing are bound to (spec/10) */
constexpr int64_t kChainFresh = 10 * 60 * 1000;      /* a member's chain, asked of the Pond, is taken as current this long */
constexpr int64_t kReceiptWait = 60 * 1000;          /* relayed, but no receipt: sent again after this, doubling (a Puddle that just
                                                      * went may still look connected to the Pond, which then hands it to no one) */
constexpr int64_t kAwayFirst = 15 * 60 * 1000;       /* the other Puddle away: tried again after this, doubling */
constexpr int64_t kAwayMax = 4 * 3600 * 1000;
constexpr int64_t kFetchAgain = 5 * 60 * 1000;
constexpr uint64_t kHashesPiece = 1ull << 40;        /* the piece index of a file's "hashes"; a chunk's is its number */
constexpr size_t kSealedMax = 128 * 1024;            /* what a Pond relays (its DM_SEALED_MAX) */

struct Job {
    std::string pond, kind, data;
};

struct Worker {
    puddle_ctx *ctx = nullptr;
    std::thread sender, fetcher;
    std::mutex mu;
    std::condition_variable cv, fcv;
    bool stopping = false, poked = false, fpoked = false;
    std::deque<Job> jobs;
    struct Known {
        int64_t at = 0;
        puddle_chain_head head{};
    };
    std::map<std::string, Known> chains; /* by member id */
};

Worker *worker(puddle_ctx *ctx)
{
    return static_cast<Worker *>(ctx->pond_dms_worker);
}

std::string me(puddle_ctx *ctx)
{
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(ctx->identity, id);
    return id;
}

std::string randomHex(size_t bytes)
{
    std::vector<unsigned char> raw(bytes);
    RAND_bytes(raw.data(), (int)bytes);
    std::string out(bytes * 2 + 1, '\0');
    puddle_hex_encode(raw.data(), (unsigned)bytes, out.data());
    out.resize(bytes * 2);
    return out;
}

std::string b64(const void *data, size_t len)
{
    std::string out((len + 2) / 3 * 4 + 4, '\0');
    out.resize(puddle_base64_encode((const unsigned char *)data, len, out.data(), out.size()));
    return out;
}

bool unb64(const std::string &in, std::string &out, size_t max)
{
    if (in.size() > (max + 2) / 3 * 4)
        return false;
    out.assign(in.size() / 4 * 3 + 3, '\0');
    size_t n = 0;
    if (puddle_base64_decode(in.c_str(), (unsigned char *)out.data(), out.size(), &n) != 0 || n > max)
        return false;
    out.resize(n);
    return true;
}

std::string jsonStr(puddle_span all, const char *key)
{
    puddle_span v;
    if (puddle_json_member(all.p, all.len, key, &v) != 0)
        return "";
    std::string out(v.len + 1, '\0');
    if (puddle_json_string(v, out.data(), out.size()) != 0)
        return "";
    out.resize(strlen(out.c_str()));
    return out;
}

int64_t jsonNum(puddle_span all, const char *key)
{
    puddle_span v;
    int64_t n = -1;
    if (puddle_json_member(all.p, all.len, key, &v) != 0 || puddle_json_int64(v, &n) != 0)
        return -1;
    return n;
}

template <class F> std::string build(size_t guess, F fill)
{
    for (size_t cap = guess; cap < (16u << 20); cap *= 2) {
        std::string buf(cap, '\0');
        puddle_jw w;
        puddle_jw_init(&w, buf.data(), buf.size());
        fill(&w);
        size_t n = puddle_jw_finish(&w);
        if (n) {
            buf.resize(n);
            return buf;
        }
    }
    return "";
}

std::string info(const std::string &from, const std::string &to, const std::string &pond, const std::string &id)
{
    return std::string(kDomain) + from + "\n" + to + "\n" + pond + "\n" + id;
}

/* ---- a member's chain, from the Pond ---- */

bool chainOf(puddle_ctx *ctx, const std::string &pond, const std::string &member, bool fresh, puddle_chain_head &out, const char **code)
{
    Worker *w = worker(ctx);
    int64_t now = puddle_ctx_now_ms(ctx);
    {
        std::lock_guard<std::mutex> g(w->mu);
        auto it = w->chains.find(member);
        if (it != w->chains.end() && !fresh && now - it->second.at < kChainFresh) {
            out = it->second.head;
            return true;
        }
    }
    int status = 0;
    std::string reply;
    if (!puddle_pond_call(ctx, pond.c_str(), "GET", "/pond/v1/members/" + member + "/chain", "", 1u << 20, 20, &status, &reply, code))
        return false;
    if (status != 200) {
        *code = status == 404 ? "left" : "failed";
        return false;
    }
    std::string text = jsonStr({reply.data(), reply.size()}, "chain");
    char err[200];
    if (text.empty() || puddle_chain_verify(text.data(), text.size(), member.c_str(), &out, err, sizeof err) != 0) {
        puddle_log(PUDDLE_LOG_WARN, "pond %.12s… gave a chain for %.12s… that is not theirs: %s", pond.c_str(), member.c_str(), err);
        *code = "failed";
        return false;
    }
    std::lock_guard<std::mutex> g(w->mu);
    w->chains[member] = {now, out};
    return true;
}

/* ---- sending ---- */

/* The letter's own text: what is signed and sealed. */
std::string letterText(puddle_ctx *ctx, const Letter &l)
{
    return build(l.event.size() * 2 + 2048, [&](puddle_jw *w) {
        puddle_jw_object(w);
        puddle_jw_num(w, "v", 1);
        puddle_jw_str(w, "kind", l.kind.c_str());
        puddle_jw_str(w, "from", me(ctx).c_str());
        puddle_jw_str(w, "to", l.to.c_str());
        puddle_jw_str(w, "pond", l.pond.c_str());
        puddle_jw_str(w, "id", l.id.c_str());
        puddle_jw_num(w, "ts", puddle_ctx_now_ms(ctx));
        if (l.kind == "message") {
            puddle_jw_key(w, "event");
            puddle_jw_string_n(w, l.event.data(), l.event.size());
            puddle_jw_str(w, "event_sig", l.eventSig.c_str());
            if (!l.files.empty()) {
                puddle_jw_key(w, "keys");
                puddle_jw_array(w);
                for (auto &f : l.files)
                    puddle_jw_string(w, f.key.c_str());
                puddle_jw_end_array(w);
            }
        } else if (l.kind == "offer") {
            puddle_jw_str(w, "link", l.link.c_str());
        } else if (l.kind == "receipt") {
            puddle_jw_key(w, "ids");
            puddle_jw_array(w);
            for (auto &i : l.ids)
                puddle_jw_string(w, i.c_str());
            puddle_jw_end_array(w);
        }
        puddle_jw_end_object(w);
    });
}

enum class Sent { Relayed, Away, Refused, Unreachable };

/* Seal `l` to its recipient and give it to the Pond. */
Sent sendOne(puddle_ctx *ctx, const Letter &l, std::string &reason)
{
    const char *code = "failed";
    puddle_chain_head head;
    if (!chainOf(ctx, l.pond, l.to, l.attempts > 0, head, &code)) {
        reason = code;
        return !strcmp(code, "left") || !strcmp(code, "removed") ? Sent::Refused : Sent::Unreachable;
    }
    std::string text = letterText(ctx, l);
    char sig[PUDDLE_SIG_TEXT_MAX];
    if (text.empty() || puddle_text_sign(ctx->identity, kDomain, text.data(), text.size(), sig) != 0) {
        reason = "failed";
        return Sent::Refused;
    }
    std::string plain = build(text.size() * 2 + 512, [&](puddle_jw *w) {
        puddle_jw_object(w);
        puddle_jw_key(w, "letter");
        puddle_jw_string_n(w, text.data(), text.size());
        puddle_jw_str(w, "sig", sig);
        puddle_jw_end_object(w);
    });
    std::string inf = info(me(ctx), l.to, l.pond, l.id);
    unsigned char enc[32];
    puddle_hpke hc;
    std::string sealed(plain.size() + PUDDLE_SEAL_TAG_LEN, '\0');
    if (plain.empty() || puddle_hpke_sender(head.enc_pub, inf.data(), inf.size(), enc, &hc) != 0 ||
        puddle_hpke_seal(&hc, NULL, 0, plain.data(), plain.size(), (unsigned char *)sealed.data()) != 0) {
        reason = "failed";
        return Sent::Refused;
    }
    std::string sealed64 = b64(sealed.data(), sealed.size());
    if (sealed64.size() > kSealedMax) {
        reason = "too_large";
        return Sent::Refused;
    }
    char kid[65];
    puddle_key_id(head.enc_pub, kid);
    std::string body = build(sealed64.size() + 512, [&](puddle_jw *w) {
        puddle_jw_object(w);
        puddle_jw_str(w, "to", l.to.c_str());
        puddle_jw_str(w, "id", l.id.c_str());
        puddle_jw_str(w, "kid", kid);
        puddle_jw_str(w, "enc", b64(enc, 32).c_str());
        puddle_jw_str(w, "sealed", sealed64.c_str());
        puddle_jw_end_object(w);
    });
    int status = 0;
    std::string reply;
    if (!puddle_pond_call(ctx, l.pond.c_str(), "POST", "/pond/v1/dms/send", body, 4096, 30, &status, &reply, &code)) {
        reason = code;
        return !strcmp(code, "removed") ? Sent::Refused : Sent::Unreachable;
    }
    puddle_span r = {reply.data(), reply.size()};
    if (status == 200) {
        puddle_span v;
        bool relayed = puddle_json_member(r.p, r.len, "relayed", &v) == 0 && v.len == 4 && !memcmp(v.p, "true", 4);
        reason = relayed ? "" : "away";
        return relayed ? Sent::Relayed : Sent::Away;
    }
    std::string e = jsonStr(r, "error");
    reason = e.empty() ? "failed" : e;
    if (status == 429)
        return Sent::Unreachable; /* slow down: later */
    if (status == 404)
        reason = "left";
    return Sent::Refused;
}

/* What waits: send what is due, give up on what is too old, forget what is done. When the next is due, in `next`. */
void sendDue(puddle_ctx *ctx, int64_t &next)
{
    pdm::Store *st = pdm::of(ctx);
    int64_t now = puddle_ctx_now_ms(ctx);
    for (Letter &l : st->letters()) {
        if (l.state != Letter::Waiting) {
            if (now - l.created > pdm::kKeepMs) /* its files can no longer be fetched */
                st->removeLetter(l.id);
            continue;
        }
        if (now - l.created > pdm::kKeepMs) {
            l.state = Letter::Failed, l.reason = "not_delivered";
            st->put(l);
            continue;
        }
        if (l.next > now) {
            next = std::min(next, l.next);
            continue;
        }
        {
            std::lock_guard<std::mutex> g(worker(ctx)->mu);
            if (worker(ctx)->stopping)
                return;
        }
        std::string reason;
        Sent s = sendOne(ctx, l, reason);
        if (l.kind == "receipt") { /* tried once: if it didn't arrive, the letter it answers comes again */
            st->removeLetter(l.id);
            continue;
        }
        l.attempts++;
        l.reason = reason;
        int64_t away = std::min(kAwayMax, kAwayFirst << std::min(l.attempts - 1, 5));
        switch (s) {
        case Sent::Relayed: l.next = now + std::min(kAwayMax, kReceiptWait << std::min(l.attempts - 1, 8)); break;
        case Sent::Away: l.next = now + away; break;
        case Sent::Unreachable: l.next = now + std::min<int64_t>(away, 5 * 60 * 1000); break;
        case Sent::Refused: l.state = Letter::Failed; break;
        }
        st->put(l);
        if (l.state == Letter::Waiting)
            next = std::min(next, l.next);
    }
}

/* A piece of a file sealed under the file's key, as records of up to PUDDLE_SEAL_RECORD bytes (spec/10, "Files"): record `k` of
 * piece `index` is sealed as record index*64 + k, with the file's place in the letter as its status, the last one marked. */
std::string sealPiece(const std::string &key, uint64_t index, int n, const std::string &plain)
{
    std::string out;
    size_t at = 0, k = 0;
    do {
        size_t len = std::min<size_t>(PUDDLE_SEAL_RECORD, plain.size() - at);
        bool last = at + len == plain.size();
        std::string rec(len + PUDDLE_SEAL_TAG_LEN, '\0');
        if (k >= 64 || puddle_seal_record((const unsigned char *)key.data(), index * 64 + k, n, last, plain.data() + at, len, (unsigned char *)rec.data()) != 0)
            return "";
        out += rec;
        at += len, k++;
    } while (at < plain.size());
    return out;
}

bool openPiece(const std::string &key, uint64_t index, int n, const std::string &sealed, std::string &out)
{
    out.clear();
    size_t at = 0, k = 0;
    while (at < sealed.size()) {
        size_t len = std::min<size_t>(PUDDLE_SEAL_RECORD + PUDDLE_SEAL_TAG_LEN, sealed.size() - at);
        bool last = at + len == sealed.size();
        if (len < PUDDLE_SEAL_TAG_LEN || k >= 64)
            return false;
        std::string rec(len - PUDDLE_SEAL_TAG_LEN, '\0');
        if (puddle_open_record((const unsigned char *)key.data(), index * 64 + k, n, last, (const unsigned char *)sealed.data() + at, len,
                               (unsigned char *)rec.data()) != 0)
            return false;
        out += rec;
        at += len, k++;
    }
    return at > 0;
}

/* The Pond asks for a piece of a file of ours: seal it and send it. */
void servePiece(puddle_ctx *ctx, const std::string &pond, puddle_span want)
{
    std::string to = jsonStr(want, "to"), id = jsonStr(want, "letter"), part = jsonStr(want, "part");
    int64_t n = jsonNum(want, "file");
    auto l = pdm::of(ctx)->letter(id);
    if (!l || l->to != to || l->pond != pond || n < 0 || n >= (int64_t)l->files.size() || !ctx->files)
        return;
    const Letter::File &f = l->files[n];
    std::string key;
    if (!unb64(f.key, key, 32) || key.size() != 32)
        return;
    std::string plain;
    uint64_t index;
    if (part == "hashes") {
        int64_t size;
        unsigned char *hashes;
        uint32_t count;
        if (puddle_files_chunk_list(ctx->files, f.id.c_str(), &size, &hashes, &count) != 0)
            return;
        unsigned char be[8];
        for (int i = 0; i < 8; i++)
            be[i] = (unsigned char)((uint64_t)size >> (56 - 8 * i));
        plain.assign((const char *)be, 8);
        plain.append((const char *)hashes, (size_t)count * 32);
        free(hashes);
        index = kHashesPiece;
    } else {
        char *end;
        unsigned long long c = strtoull(part.c_str(), &end, 10);
        unsigned char *data;
        size_t len;
        if (*end || part.empty() || puddle_files_read_chunk(ctx->files, f.id.c_str(), (uint32_t)c, &data, &len) != 0)
            return;
        plain.assign((const char *)data, len);
        free(data);
        index = c;
    }
    std::string out = sealPiece(key, index, (int)n, plain);
    if (out.empty())
        return;
    int status;
    std::string reply;
    const char *code;
    puddle_pond_call(ctx, pond.c_str(), "POST", "/pond/v1/dms/files/" + id + "/" + std::to_string(n) + "/" + part + "?to=" + to, out, 4096, 60,
                     &status, &reply, &code);
}

/* ---- receiving ---- */

void receipt(puddle_ctx *ctx, const std::string &pond, const std::string &to, const std::vector<std::string> &ids)
{
    Letter r;
    r.id = randomHex(32), r.pond = pond, r.to = to, r.kind = "receipt", r.ids = ids;
    r.created = r.next = puddle_ctx_now_ms(ctx);
    pdm::of(ctx)->put(r);
}

pdm::Conv convFor(puddle_ctx *ctx, const std::string &pond, const std::string &member)
{
    pdm::Store *st = pdm::of(ctx);
    std::string chat = pdm::chatId(pond, me(ctx), member);
    if (auto c = st->conv(chat))
        return *c;
    pdm::Conv c;
    c.chat = chat, c.pond = pond, c.member = member, c.created = puddle_ctx_now_ms(ctx);
    char name[512] = "";
    if (ctx->ponds)
        puddle_ponds_name(ctx->ponds, pond.c_str(), name, sizeof name);
    c.pondName = name;
    st->put(c);
    return c;
}

/* A `dm` the Pond relayed to us: open it, check it, keep what it says, and say it came. */
void dropped(const std::string &pond, const std::string &from, const char *why)
{
    puddle_log(PUDDLE_LOG_INFO, "a letter from %.12s… through pond %.12s… was dropped: %s", from.c_str(), pond.c_str(), why);
}

void receive(puddle_ctx *ctx, const std::string &pond, puddle_span d)
{
    std::string id = jsonStr(d, "id"), from = jsonStr(d, "from"), enc64 = jsonStr(d, "enc"), sealed64 = jsonStr(d, "sealed");
    std::string mine = me(ctx), enc, sealed;
    if (from.size() != PUDDLE_USER_ID_LEN || id.size() != 64 || !unb64(enc64, enc, 32) || enc.size() != 32 || !unb64(sealed64, sealed, kSealedMax) ||
        sealed.size() <= PUDDLE_SEAL_TAG_LEN)
        return dropped(pond, from, "malformed");
    std::string inf = info(from, mine, pond, id), plain(sealed.size() - PUDDLE_SEAL_TAG_LEN, '\0');
    puddle_hpke hc;
    if (puddle_identity_open_seal(ctx->identity, (const unsigned char *)enc.data(), inf.data(), inf.size(), &hc) != 0 ||
        puddle_hpke_open(&hc, NULL, 0, (const unsigned char *)sealed.data(), sealed.size(), (unsigned char *)plain.data()) != 0) {
        puddle_log(PUDDLE_LOG_INFO, "a letter from %.12s… through pond %.12s… could not be opened (sealed to a key we no longer have?)",
                   from.c_str(), pond.c_str());
        return;
    }
    if (!puddle_json_valid_object(plain.data(), plain.size()))
        return dropped(pond, from, "not a letter");
    std::string text = jsonStr({plain.data(), plain.size()}, "letter"), sigText = jsonStr({plain.data(), plain.size()}, "sig");
    char kid[65];
    unsigned char sig[64];
    puddle_chain_head head;
    const char *code;
    if (text.empty() || puddle_event_sig_parse(sigText.c_str(), kid, sig) != 0)
        return dropped(pond, from, "no signature");
    if (!chainOf(ctx, pond, from, false, head, &code))
        return dropped(pond, from, "their key chain could not be had");
    char have[65];
    puddle_key_id(head.sign_pub, have);
    if (strcmp(have, kid) != 0 && (!chainOf(ctx, pond, from, true, head, &code) || (puddle_key_id(head.sign_pub, have), strcmp(have, kid) != 0)))
        return dropped(pond, from, "signed by a key that is not theirs now");
    if (!puddle_text_verify(head.sign_pub, kDomain, text.data(), text.size(), sig))
        return dropped(pond, from, "a bad signature");
    puddle_span L = {text.data(), text.size()};
    if (!puddle_json_valid_object(L.p, L.len) || jsonNum(L, "v") != 1 || jsonStr(L, "from") != from || jsonStr(L, "to") != mine ||
        jsonStr(L, "pond") != pond || jsonStr(L, "id") != id)
        return dropped(pond, from, "meant for someone else, or through another Pond");
    std::string kind = jsonStr(L, "kind");
    pdm::Store *st = pdm::of(ctx);
    bool blocked = st->blocked(from) || (ctx->streams && puddle_streams_is_blocked(ctx->streams, from.c_str()));

    if (kind == "receipt") {
        puddle_span arr, item;
        size_t cur = 0;
        if (puddle_json_member(L.p, L.len, "ids", &arr) != 0)
            return;
        while (puddle_json_array_next(arr, &cur, &item) == 1) {
            char lid[65];
            if (puddle_json_string(item, lid, sizeof lid) != 0)
                continue;
            auto l = st->letter(lid);
            if (!l || l->to != from || l->pond != pond || l->state != Letter::Waiting)
                continue;
            if (l->files.empty()) {
                st->removeLetter(l->id);
            } else { /* kept, so its files can still be fetched */
                l->state = Letter::Delivered, l->reason.clear();
                st->put(*l);
            }
        }
        if (ctx->ponds)
            puddle_ponds_touch(ctx->ponds, pond.c_str());
        return;
    }
    if (kind == "offer") {
        std::string link = jsonStr(L, "link");
        if (!blocked && link.rfind("puddle://", 0) == 0 && link.size() < 1024) {
            pdm::Conv c = convFor(ctx, pond, from);
            c.offer = link;
            st->put(c);
            if (ctx->ponds)
                puddle_ponds_touch(ctx->ponds, pond.c_str());
        }
        receipt(ctx, pond, from, {id});
        return;
    }
    if (kind != "message")
        return;
    std::string event = jsonStr(L, "event"), eventSig = jsonStr(L, "event_sig");
    puddle_message m;
    const char *why;
    std::string chat = pdm::chatId(pond, mine, from);
    char ekid[65];
    unsigned char esig[64];
    if (event.empty() || puddle_chat_event_parse(event.data(), event.size(), &m, &why) != 0)
        return dropped(pond, from, "not a chat event");
    bool ok = chat == m.chat && from == m.sender && puddle_event_sig_parse(eventSig.c_str(), ekid, esig) == 0 && !strcmp(ekid, kid) &&
              puddle_event_verify(head.sign_pub, event.data(), event.size(), esig);
    std::string files = m.files ? m.files : "";
    puddle_message_free(&m);
    char eid[PUDDLE_ID_HEX + 1];
    puddle_sha256_hex(event.data(), event.size(), eid);
    if (!ok || id != eid)
        return dropped(pond, from, "its event is not theirs, or not for this conversation");
    if (!blocked) { /* a blocked person's is dropped unread, and still answered: to them it looks delivered */
        convFor(ctx, pond, from);
        char err[200], stored[PUDDLE_ID_HEX + 1];
        int r = puddle_store_append(ctx->store, chat.c_str(), event.data(), event.size(), eventSig.c_str(), 0, stored, err, sizeof err);
        if (r < 0) {
            puddle_log(PUDDLE_LOG_WARN, "a letter from %.12s… could not be kept: %s", from.c_str(), err);
            return; /* no receipt: it comes again */
        }
        puddle_file_ref *refs;
        size_t n;
        if (r == PUDDLE_APPEND_NEW && !files.empty() && ctx->files && puddle_file_refs_parse(files.data(), files.size(), &refs, &n) == 0) {
            Fetch f;
            f.letter = id, f.pond = pond, f.from = from, f.chat = chat, f.message = stored;
            f.created = puddle_ctx_now_ms(ctx);
            int64_t max = 0;
            bool takes = ctx->ponds && puddle_ponds_dms(ctx->ponds, pond.c_str(), &max) == 1;
            puddle_span keys, k;
            size_t cur = 0;
            bool have_keys = puddle_json_member(L.p, L.len, "keys", &keys) == 0;
            for (size_t i = 0; i < n; i++) {
                char key[64] = "";
                if (!have_keys || puddle_json_array_next(keys, &cur, &k) != 1 || puddle_json_string(k, key, sizeof key) != 0)
                    break;
                puddle_files_ref(ctx->files, refs[i].id, chat.c_str(), stored, f.created); /* kept while the message is */
                if (takes && refs[i].size <= max && puddle_files_state(ctx->files, refs[i].id, nullptr, nullptr) != PUDDLE_FILE_WHOLE)
                    f.files.push_back({(int)i, refs[i].id, key, refs[i].size, false});
            }
            free(refs);
            if (!f.files.empty()) {
                st->put(f);
                Worker *w = worker(ctx);
                std::lock_guard<std::mutex> g(w->mu);
                w->fpoked = true;
                w->fcv.notify_all();
            }
        }
    }
    receipt(ctx, pond, from, {id});
}

/* ---- fetching a letter's files ---- */

/* One piece through the Pond: opened, or false (`away` if their Puddle isn't there now). */
bool piece(puddle_ctx *ctx, const Fetch &f, const Fetch::File &file, const std::string &part, uint64_t index, std::string &out, bool &away)
{
    int status = 0;
    std::string reply, key;
    const char *code;
    away = false;
    if (!unb64(file.key, key, 32) || key.size() != 32)
        return false;
    if (!puddle_pond_call(ctx, f.pond.c_str(), "GET", "/pond/v1/dms/files/" + f.letter + "/" + std::to_string(file.n) + "/" + part + "?from=" + f.from,
                          "", PUDDLE_FILE_CHUNK + 4096, 45, &status, &reply, &code)) {
        away = true;
        return false;
    }
    if (status != 200 || reply.size() < PUDDLE_SEAL_TAG_LEN) {
        away = status == 409 || status == 429;
        return false;
    }
    return openPiece(key, index, file.n, reply, out);
}

/* Fetch what can be of `f`: false if their Puddle is away (try later). */
bool fetchOne(puddle_ctx *ctx, Fetch &f, Worker *w)
{
    for (Fetch::File &file : f.files) {
        if (file.done)
            continue;
        bool away;
        if (puddle_files_state(ctx->files, file.id.c_str(), nullptr, nullptr) != PUDDLE_FILE_WHOLE) {
            std::string hashes;
            if (!piece(ctx, f, file, "hashes", kHashesPiece, hashes, away)) {
                if (away)
                    return false;
                file.done = true; /* it is not theirs to give (any more) */
                continue;
            }
            int64_t size = 0;
            for (int i = 0; i < 8 && hashes.size() >= 8; i++)
                size = (size << 8) | (unsigned char)hashes[i];
            if (hashes.size() < 8 || size != file.size || (hashes.size() - 8) % 32 ||
                puddle_files_fetch_begin(ctx->files, file.id.c_str(), size, (const unsigned char *)hashes.data() + 8,
                                         (uint32_t)((hashes.size() - 8) / 32)) != 0) {
                file.done = true; /* not the file the message names */
                continue;
            }
            int64_t n;
            while ((n = puddle_files_fetch_next(ctx->files, file.id.c_str())) >= 0) {
                {
                    std::lock_guard<std::mutex> g(w->mu);
                    if (w->stopping)
                        return true;
                }
                std::string chunk;
                if (!piece(ctx, f, file, std::to_string(n), (uint64_t)n, chunk, away)) {
                    if (away)
                        return false;
                    break;
                }
                if (puddle_files_fetch_put(ctx->files, file.id.c_str(), (uint32_t)n, (const unsigned char *)chunk.data(), chunk.size()) != 0)
                    break;
            }
            puddle_files_fetch_finish(ctx->files, file.id.c_str());
        }
        file.done = true;
        puddle_files_ref(ctx->files, file.id.c_str(), f.chat.c_str(), f.message.c_str(), puddle_ctx_now_ms(ctx));
    }
    return true;
}

void fetchLoop(puddle_ctx *ctx, Worker *w)
{
    for (;;) {
        int64_t now = puddle_ctx_now_ms(ctx), next = now + 15 * 60 * 1000;
        for (Fetch &f : pdm::of(ctx)->fetches()) {
            {
                std::lock_guard<std::mutex> g(w->mu);
                if (w->stopping)
                    return;
            }
            if (now - f.created > pdm::kKeepMs) {
                pdm::of(ctx)->removeFetch(f.letter);
                continue;
            }
            if (f.next > now) {
                next = std::min(next, f.next);
                continue;
            }
            if (fetchOne(ctx, f, w)) {
                pdm::of(ctx)->removeFetch(f.letter);
                if (ctx->ponds)
                    puddle_ponds_touch(ctx->ponds, f.pond.c_str());
            } else {
                f.attempts++;
                f.next = now + kFetchAgain;
                pdm::of(ctx)->put(f);
                next = std::min(next, f.next);
            }
        }
        std::unique_lock<std::mutex> lk(w->mu);
        w->fcv.wait_for(lk, std::chrono::milliseconds(std::max<int64_t>(next - now, 1000)), [&] { return w->stopping || w->fpoked; });
        w->fpoked = false;
        if (w->stopping)
            return;
    }
}

void sendLoop(puddle_ctx *ctx, Worker *w)
{
    for (;;) {
        std::deque<Job> jobs;
        {
            std::lock_guard<std::mutex> g(w->mu);
            if (w->stopping)
                return;
            jobs.swap(w->jobs);
            w->poked = false;
        }
        for (Job &j : jobs) {
            puddle_span d = {j.data.data(), j.data.size()};
            if (j.kind == "dm") {
                receive(ctx, j.pond, d);
            } else if (j.kind == "dm_back") { /* they're back: what waits for them goes now */
                std::string who = jsonStr(d, "member");
                for (Letter &l : pdm::of(ctx)->letters())
                    if (l.state == Letter::Waiting && l.pond == j.pond && l.to == who && l.next > 0) {
                        l.next = 0;
                        pdm::of(ctx)->put(l);
                    }
                for (Fetch &f : pdm::of(ctx)->fetches())
                    if (f.pond == j.pond && f.from == who) {
                        f.next = 0;
                        pdm::of(ctx)->put(f);
                        std::lock_guard<std::mutex> g(w->mu);
                        w->fpoked = true;
                        w->fcv.notify_all();
                    }
            } else if (j.kind == "dm_want") {
                servePiece(ctx, j.pond, d);
            }
        }
        int64_t now = puddle_ctx_now_ms(ctx), next = now + 15 * 60 * 1000;
        sendDue(ctx, next);
        std::unique_lock<std::mutex> lk(w->mu);
        w->cv.wait_for(lk, std::chrono::milliseconds(std::max<int64_t>(next - now, 50)),
                       [&] { return w->stopping || w->poked || !w->jobs.empty(); });
        if (w->stopping)
            return;
    }
}

void poke(puddle_ctx *ctx)
{
    Worker *w = worker(ctx);
    if (!w)
        return;
    std::lock_guard<std::mutex> g(w->mu);
    w->poked = true;
    w->cv.notify_all();
}

} // namespace

extern "C" {

void puddle_pdm_start(puddle_ctx *ctx)
{
    if (!ctx->pond_dms || ctx->pond_dms_worker)
        return;
    auto *w = new Worker();
    w->ctx = ctx;
    ctx->pond_dms_worker = w;
    try {
        w->sender = std::thread(sendLoop, ctx, w);
        w->fetcher = std::thread(fetchLoop, ctx, w);
    } catch (const std::system_error &) {
        puddle_log(PUDDLE_LOG_ERROR, "cannot start the threads for direct messages through Ponds");
    }
}

void puddle_pdm_stop(puddle_ctx *ctx)
{
    Worker *w = worker(ctx);
    if (!w)
        return;
    {
        std::lock_guard<std::mutex> g(w->mu);
        w->stopping = true;
        w->cv.notify_all();
        w->fcv.notify_all();
    }
    if (w->sender.joinable())
        w->sender.join();
    if (w->fetcher.joinable())
        w->fetcher.join();
    ctx->pond_dms_worker = nullptr;
    delete w;
}

void puddle_pdm_events(puddle_ctx *ctx, const char *pond, const char *json, size_t len)
{
    Worker *w = worker(ctx);
    puddle_span arr, item;
    size_t cur = 0;
    if (!w || puddle_json_member(json, len, "events", &arr) != 0)
        return;
    std::vector<Job> got;
    while (puddle_json_array_next(arr, &cur, &item) == 1) {
        std::string kind = jsonStr(item, "kind");
        puddle_span data;
        if ((kind == "dm" || kind == "dm_back" || kind == "dm_want") && puddle_json_member(item.p, item.len, "data", &data) == 0)
            got.push_back({pond, kind, std::string(data.p, data.len)});
    }
    if (got.empty())
        return;
    std::lock_guard<std::mutex> g(w->mu);
    for (auto &j : got)
        if (w->jobs.size() < 2000)
            w->jobs.push_back(std::move(j));
    w->cv.notify_all();
}

/* Can a letter go to the other side of `c` now? "" if so, else why not. */
static const char *mayWrite(puddle_ctx *ctx, const pdm::Conv &c)
{
    int dms = 1;
    if (!ctx->ponds || puddle_ponds_conn(ctx->ponds, c.pond.c_str(), NULL) != 0)
        return "left";
    if (puddle_ponds_dms(ctx->ponds, c.pond.c_str(), NULL) != 1)
        return "dms_off";
    int r = puddle_ponds_member(ctx->ponds, c.pond.c_str(), c.member.c_str(), NULL, NULL, &dms);
    if (r != 0)
        return "left";
    return dms ? "" : "not_accepted";
}

int puddle_pdm_send_event(puddle_ctx *ctx, const char *chat, const char *event, size_t len, char id[65], const char **code)
{
    pdm::Store *st = pdm::of(ctx);
    auto c = st ? st->conv(chat) : std::nullopt;
    if (!c) {
        *code = "no_conversation";
        return -1;
    }
    if (const char *why = mayWrite(ctx, *c); *why) {
        *code = why;
        return -1;
    }
    Letter l;
    puddle_message m;
    const char *why;
    if (puddle_chat_event_parse(event, len, &m, &why) != 0) {
        *code = "failed";
        return -1;
    }
    std::string files = m.files ? m.files : "";
    puddle_message_free(&m);
    if (!files.empty()) {
        int64_t max = 0;
        puddle_ponds_dms(ctx->ponds, c->pond.c_str(), &max);
        puddle_file_ref *refs;
        size_t n;
        if (max <= 0) {
            *code = "files_off";
            return -1;
        }
        if (puddle_file_refs_parse(files.data(), files.size(), &refs, &n) != 0) {
            *code = "failed";
            return -1;
        }
        for (size_t i = 0; i < n; i++) {
            if (refs[i].size > max) {
                free(refs);
                *code = "too_large";
                return -1;
            }
            unsigned char key[32];
            RAND_bytes(key, sizeof key);
            l.files.push_back({refs[i].id, b64(key, 32)});
        }
        free(refs);
    }
    char sig[PUDDLE_SIG_TEXT_MAX], err[300];
    if (puddle_event_sign(ctx->identity, event, len, sig) != 0 ||
        puddle_store_append(ctx->store, chat, event, len, sig, 0, id, err, sizeof err) < 0) {
        *code = "failed";
        return -1;
    }
    l.id = id, l.chat = chat, l.pond = c->pond, l.to = c->member, l.kind = "message";
    l.event.assign(event, len), l.eventSig = sig;
    l.created = puddle_ctx_now_ms(ctx), l.next = 0;
    if (!st->put(l)) {
        *code = "failed";
        return -1;
    }
    poke(ctx);
    return 0;
}

int puddle_pdm_befriend(puddle_ctx *ctx, const char *chat, const char **code)
{
    pdm::Store *st = pdm::of(ctx);
    auto c = st ? st->conv(chat) : std::nullopt;
    if (!c) {
        *code = "no_conversation";
        return -1;
    }
    if (const char *why = mayWrite(ctx, *c); *why) {
        *code = why;
        return -1;
    }
    puddle_invite inv;
    char err[200], link[512];
    if (!ctx->streams || puddle_streams_invite_create(ctx->streams, puddle_ctx_now_ms(ctx), pdm::kKeepMs, 1, &inv, err, sizeof err) != 0 ||
        !ctx->config || !puddle_make_link(ctx->config, ctx->identity, inv.token, link, sizeof link)) {
        *code = "no_link";
        return -1;
    }
    Letter l;
    l.id = randomHex(32), l.chat = chat, l.pond = c->pond, l.to = c->member, l.kind = "offer";
    l.link = strncmp(link, "puddle://", 9) == 0 ? std::string(link) : "puddle://" + std::string(link);
    l.created = puddle_ctx_now_ms(ctx), l.next = 0;
    c->offered = true;
    if (!st->put(l) || !st->put(*c)) {
        *code = "failed";
        return -1;
    }
    poke(ctx);
    return 0;
}

int puddle_pdm_accept(puddle_ctx *ctx, const char *pond, int accept, const char **code)
{
    int status = 0;
    std::string reply;
    std::string body = accept ? "{\"accept\":true}" : "{\"accept\":false}";
    if (!puddle_pond_call(ctx, pond, "POST", "/pond/v1/dms/settings", body, 4096, 20, &status, &reply, code))
        return -1;
    if (status != 200) {
        *code = status == 404 ? "dms_off" : "failed"; /* a Pond from before direct messages */
        return -1;
    }
    return 0;
}

void puddle_pdm_retry(puddle_ctx *ctx, const char *chat)
{
    pdm::Store *st = pdm::of(ctx);
    if (!st)
        return;
    for (Letter &l : st->letters())
        if (l.chat == chat && l.state != Letter::Delivered) {
            l.state = Letter::Waiting, l.next = 0;
            if (puddle_ctx_now_ms(ctx) - l.created > pdm::kKeepMs)
                l.created = puddle_ctx_now_ms(ctx); /* given another 30 days */
            st->put(l);
        }
    poke(ctx);
}

} // extern "C"
