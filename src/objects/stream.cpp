#include "objects/stream.hpp"

#include "core/error.hpp"
#include "core/link.h"
#include "objects/user.hpp"
#include "core/outbox.h"

#include <algorithm>
#include <cstring>

namespace puddle {

namespace {

struct Transition {
    unsigned allowed; // states it may be applied to, a bit each
    puddle_stream_state to;
    puddle_owed owed;
    int64_t now;
};

int applyTransition(void *user, puddle_stream *st, int exists)
{
    auto *t = static_cast<Transition *>(user);
    if (!exists)
        return 2;
    if (!(t->allowed & (1u << st->state)))
        return 3;
    st->state = t->to;
    st->owed = t->owed;
    st->updated_ms = t->now;
    return 0;
}

// withdrawing a request to an address: nobody there knows us yet as far as we can tell, so there is nothing to tell them;
// the record stays (closed) so that an answer that still comes can be recognised and closed on their side too
int closeQuietly(void *, puddle_stream *st, int exists)
{
    if (!exists || !st->provisional || st->state != PUDDLE_STREAM_PENDING_OUT)
        return 1;
    st->state = PUDDLE_STREAM_CLOSED;
    return 0;
}

constexpr unsigned bit(puddle_stream_state s) { return 1u << s; }

} // namespace

Stream::Stream(Stream &&other) noexcept : ctx_(other.ctx_), rec_(other.rec_)
{
    other.rec_.chain = nullptr;
}

Stream &Stream::operator=(Stream &&other) noexcept
{
    if (this != &other) {
        puddle_stream_free(&rec_);
        ctx_ = other.ctx_;
        rec_ = other.rec_;
        other.rec_.chain = nullptr;
    }
    return *this;
}

Stream::~Stream()
{
    puddle_stream_free(&rec_);
}

std::optional<Stream> Stream::get(puddle_ctx *ctx, const std::string &id)
{
    puddle_stream st;
    if (puddle_streams_get(ctx->streams, id.c_str(), &st) != 0)
        return std::nullopt;
    return Stream(ctx, st);
}

std::vector<Stream> Stream::all(puddle_ctx *ctx, std::optional<puddle_stream_state> state)
{
    puddle_stream *list;
    size_t n;
    if (puddle_streams_list(ctx->streams, &list, &n) != 0)
        throw Error::internal();
    std::vector<Stream> out;
    for (size_t i = 0; i < n; i++) {
        bool keep = !list[i].scoped_req && (!state || list[i].state == *state); // a group request of ours is not someone to show
        if (keep) {
            out.push_back(Stream(ctx, list[i]));
            list[i].chain = nullptr; // the record's buffer now belongs to the Stream
        }
    }
    puddle_stream_list_free(list, n);
    std::sort(out.begin(), out.end(), [](const Stream &a, const Stream &b) {
        if (a.rec_.updated_ms != b.rec_.updated_ms)
            return a.rec_.updated_ms > b.rec_.updated_ms;
        return strcmp(a.rec_.id, b.rec_.id) < 0;
    });
    return out;
}

Stream Stream::request(puddle_ctx *ctx, const std::string &target, const std::string &note, const std::string &profile)
{
    puddle_link link;
    char lerr[100];
    if (puddle_link_parse(target.c_str(), &link, lerr, sizeof lerr) != 0)
        throw Error::badRequest("bad_link", lerr);
    char id[PUDDLE_USER_ID_LEN + 1];
    const char *code, *msg;
    int status = ctx->courier->request(ctx, &link, note.c_str(), profile.empty() ? NULL : profile.c_str(), id, &code, &msg);
    if (status != 0)
        throw Error(status, code, msg ? msg : "");
    auto s = get(ctx, id);
    if (!s)
        throw Error::internal();
    return std::move(*s);
}

Stream Stream::transition(unsigned allowed, puddle_stream_state to, puddle_owed owed, const char *conflict)
{
    char err[300];
    Transition t = { allowed, to, owed, puddle_ctx_now_ms(ctx_) };
    int rc = puddle_streams_update(ctx_->streams, rec_.id, applyTransition, &t, err, sizeof err);
    if (rc == 2)
        throw Error::notFound();
    if (rc == 3)
        throw Error::conflict(conflict);
    if (rc != 0)
        throw Error::internal(err);
    if (owed != PUDDLE_OWED_NONE)
        ctx_->courier->owe(ctx_, rec_.id, owed); // queue the news; the record says it is owed until it lands
    auto s = get(ctx_, rec_.id);
    if (!s)
        throw Error::internal();
    return std::move(*s);
}

Stream Stream::accept()
{
    Stream s = transition(bit(PUDDLE_STREAM_PENDING_IN), PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_APPROVED, "not_waiting");
    ctx_->courier->media_want(ctx_, rec_.id); // and, after the answer, ask for the pictures in their profile
    return s;
}

Stream Stream::decline()
{
    return transition(bit(PUDDLE_STREAM_PENDING_IN), PUDDLE_STREAM_CLOSED, PUDDLE_OWED_DECLINED, "not_waiting");
}

Stream Stream::remove()
{
    char err[200];
    if (puddle_streams_update(ctx_->streams, rec_.id, closeQuietly, NULL, err, sizeof err) == 0) {
        auto s = get(ctx_, rec_.id);
        if (s)
            return std::move(*s);
    }
    return transition(bit(PUDDLE_STREAM_ACTIVE) | bit(PUDDLE_STREAM_PENDING_OUT), PUDDLE_STREAM_CLOSED, PUDDLE_OWED_CLOSED, "not_open");
}

Stream Stream::trust()
{
    char err[300];
    int rc = puddle_streams_trust_fingerprint(ctx_->streams, rec_.id, err, sizeof err);
    if (rc == 1)
        throw Error::conflict("nothing_to_trust");
    if (rc != 0)
        throw Error::internal(err);
    if (ctx_->outbox)
        puddle_outbox_release(ctx_->outbox, rec_.id);
    auto s = get(ctx_, rec_.id);
    if (!s)
        throw Error::internal();
    return std::move(*s);
}

Stream Stream::showProfile(const std::optional<std::string> &profileId)
{
    Profile::showAt(ctx_, std::string("dm:") + rec_.id, profileId); // an open stream hears at once; a request carries it
    auto s = get(ctx_, rec_.id);
    if (!s)
        throw Error::internal();
    return std::move(*s);
}

void Stream::write(puddle_jw *w) const
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", rec_.id);
    if (rec_.provisional)
        puddle_jw_flag(w, "provisional", 1);
    puddle_jw_key(w, "user");
    User::fromStream(ctx_, rec_).write(w);
    puddle_jw_str(w, "state", puddle_stream_state_name(rec_.state));
    if (rec_.naddr > 0)
        puddle_jw_str(w, "address", rec_.addresses[0]);
    if (rec_.error[0])
        puddle_jw_str(w, "error", rec_.error);
    if (rec_.tls_fp_seen[0])
        puddle_jw_flag(w, "certificate_changed", 1);
    if (rec_.note[0])
        puddle_jw_str(w, "message", rec_.note);
    if (auto mine = Profile::shownAt(ctx_, std::string("dm:") + rec_.id))
        puddle_jw_str(w, "profile", mine->c_str());
    puddle_jw_num(w, "created_at", rec_.created_ms);
    puddle_jw_num(w, "updated_at", rec_.updated_ms);
    puddle_jw_end_object(w);
}

} // namespace puddle
