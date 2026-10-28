#include "objects/file.hpp"

#include "core/error.hpp"
#include "storage/files.h"
#include "core/transfers.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace puddle {

static const char *stateName(File::State s)
{
    switch (s) {
    case File::State::Available: return "available";
    case File::State::Downloading: return "downloading";
    case File::State::TooLarge: return "too_large";
    case File::State::Failed: return "failed";
    case File::State::Waiting: break;
    }
    return "waiting";
}

File File::of(puddle_ctx *ctx, const puddle_file_ref &ref, const std::string &chat, const std::string &message)
{
    File f;
    f.id_ = ref.id;
    f.ref_ = ref;
    f.described_ = true;
    int64_t size = 0, have = 0;
    puddle_file_state st = ctx->files ? puddle_files_state(ctx->files, ref.id, &size, &have) : PUDDLE_FILE_ABSENT;
    f.have_ = have;
    char why[200] = "";
    puddle_tr_state tr = ctx->transfers ? puddle_transfers_state(ctx->transfers, ref.id, why) : PUDDLE_TR_NONE;
    f.reason_ = why;
    if (st == PUDDLE_FILE_WHOLE)
        f.state_ = State::Available;
    else if (tr == PUDDLE_TR_FAILED)
        f.state_ = State::Failed;
    else if (tr != PUDDLE_TR_NONE || st == PUDDLE_FILE_PARTIAL)
        f.state_ = State::Downloading;
    else if (ref.size > puddle_files_max_size(ctx->config))
        f.state_ = State::TooLarge; // more than this Puddle takes: never fetched
    if (f.state_ == State::TooLarge)
        f.max_ = puddle_files_max_size(ctx->config);
    if (ctx->files && !chat.empty() && !message.empty()) {
        // notes are "<kind> ..." lines (federation/files.cpp); only this message's matter here
        auto each = [](void *u, const char *line) {
            auto *pr = static_cast<std::pair<File *, std::string> *>(u);
            const std::string &cm = pr->second; // "<chat> <message>"
            char kind[16], a[80], b[80], c[80];
            long long n = 0;
            int got = sscanf(line, "%15s %79s %79s %79s %lld", kind, a, b, c, &n);
            if (!strcmp(kind, "refused") && got == 5 && cm == std::string(b) + " " + c)
                pr->first->refused_.push_back({ a, (int64_t)n });
            else if (!strcmp(kind, "request") && got == 5 && cm == std::string(b) + " " + c)
                pr->first->askers_.push_back(a);
            else if (!strcmp(kind, "asked") && got >= 3 && cm == std::string(a) + " " + b)
                pr->first->asked_ = true;
            return 0;
        };
        std::pair<File *, std::string> u{ &f, chat + " " + message };
        puddle_files_note_each(ctx->files, ref.id, "", each, &u);
    }
    return f;
}

std::vector<FileRequest> FileRequest::all(puddle_ctx *ctx)
{
    struct Acc {
        puddle_ctx *ctx;
        std::vector<FileRequest> out;
        std::string id;
    } acc{ ctx, {}, {} };
    if (!ctx->files)
        return {};
    puddle_files_each(ctx->files, [](void *u, const char *id, puddle_file_state st) {
        auto *a = static_cast<Acc *>(u);
        if (st != PUDDLE_FILE_WHOLE)
            return 0;
        a->id = id;
        puddle_files_note_each(a->ctx->files, id, "request ", [](void *u2, const char *line) {
            auto *a2 = static_cast<Acc *>(u2);
            char user[80], chat[80], message[80];
            long long ts = 0;
            if (sscanf(line, "request %79s %79s %79s %lld", user, chat, message, &ts) == 4)
                a2->out.push_back({ a2->id, user, chat, message, (int64_t)ts });
            return 0;
        }, a);
        return 0;
    }, &acc);
    std::sort(acc.out.begin(), acc.out.end(), [](const FileRequest &x, const FileRequest &y) { return x.ts < y.ts; });
    return acc.out;
}

std::optional<File> File::here(puddle_ctx *ctx, const std::string &id)
{
    int64_t size = 0, have = 0;
    puddle_file_state st = ctx->files ? puddle_files_state(ctx->files, id.c_str(), &size, &have) : PUDDLE_FILE_ABSENT;
    if (st == PUDDLE_FILE_ABSENT)
        return std::nullopt;
    File f;
    f.id_ = id;
    snprintf(f.ref_.id, sizeof f.ref_.id, "%s", id.c_str());
    f.ref_.size = size;
    f.have_ = have;
    f.state_ = st == PUDDLE_FILE_WHOLE ? State::Available : State::Downloading;
    return f;
}

void File::write(puddle_jw *w) const
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id_.c_str());
    if (described_) {
        puddle_jw_str(w, "name", ref_.name);
        puddle_jw_str(w, "mime", ref_.mime);
    }
    puddle_jw_num(w, "size", ref_.size);
    if (ref_.w) {
        puddle_jw_num(w, "w", ref_.w);
        puddle_jw_num(w, "h", ref_.h);
    }
    if (ref_.duration_ms)
        puddle_jw_num(w, "duration_ms", ref_.duration_ms);
    if (ref_.preview[0])
        puddle_jw_str(w, "preview", ref_.preview);
    puddle_jw_str(w, "state", stateName(state_));
    if (!reason_.empty() && state_ != State::Available)
        puddle_jw_str(w, "reason", reason_.c_str());
    puddle_jw_num(w, "received", have_);
    if (max_)
        puddle_jw_num(w, "max", max_);
    if (!refused_.empty()) {
        puddle_jw_key(w, "refused");
        puddle_jw_array(w);
        for (const auto &r : refused_) {
            puddle_jw_object(w);
            puddle_jw_str(w, "user", r.user.c_str());
            puddle_jw_num(w, "max", r.max);
            puddle_jw_end_object(w);
        }
        puddle_jw_end_array(w);
    }
    if (!askers_.empty()) {
        puddle_jw_key(w, "requests");
        puddle_jw_array(w);
        for (const auto &a : askers_)
            puddle_jw_string(w, a.c_str());
        puddle_jw_end_array(w);
    }
    if (asked_)
    {
        puddle_jw_key(w, "asked");
        puddle_jw_bool(w, 1);
    }
    puddle_jw_end_object(w);
}

// ---- Upload ----

Upload Upload::create(puddle_ctx *ctx, int64_t size)
{
    if (!ctx->files)
        throw Error::internal("files are not kept here");
    if (size < 1 || size > puddle_files_max_size(ctx->config))
        throw Error::badRequest("too_large", "files can be at most " + std::to_string(puddle_files_max_size(ctx->config)) + " bytes here");
    char token[PUDDLE_UPLOAD_TOKEN_LEN + 1];
    if (puddle_files_upload_create(ctx->files, size, token) != 0)
        throw Error::internal("cannot start the upload");
    auto u = get(ctx, token);
    if (!u)
        throw Error::internal();
    return *u;
}

std::optional<Upload> Upload::get(puddle_ctx *ctx, const std::string &token)
{
    Upload u;
    if (!ctx->files || puddle_files_upload_info(ctx->files, token.c_str(), &u.size_, &u.have_, &u.count_) != 0)
        return std::nullopt;
    u.ctx_ = ctx;
    u.token_ = token;
    return u;
}

Upload Upload::put(uint32_t n, const unsigned char *data, size_t len) const
{
    int rc = puddle_files_upload_put(ctx_->files, token_.c_str(), n, data, len);
    if (rc == 1)
        throw Error::notFound();
    if (rc == -1)
        throw Error::badRequest("bad_chunk", "chunk " + std::to_string(n) + " of this file is " +
                                                 std::to_string(puddle_file_chunk_len(size_, n)) + " bytes");
    if (rc != 0)
        throw Error::internal("cannot keep the chunk");
    auto u = get(ctx_, token_);
    if (!u)
        throw Error::notFound();
    return *u;
}

File Upload::finish() const
{
    char id[PUDDLE_FILE_ID_LEN + 1];
    int rc = puddle_files_upload_finish(ctx_->files, token_.c_str(), id);
    if (rc == 1)
        throw Error::notFound();
    if (rc == 2)
        throw Error::conflict("incomplete", std::to_string(count_ - have_) + " chunks have not arrived");
    if (rc != 0)
        throw Error::internal("cannot keep the file");
    auto f = File::here(ctx_, id);
    if (!f)
        throw Error::internal();
    return *f;
}

void Upload::cancel() const
{
    puddle_files_upload_cancel(ctx_->files, token_.c_str());
}

void Upload::write(puddle_jw *w) const
{
    puddle_jw_object(w);
    puddle_jw_str(w, "upload", token_.c_str());
    puddle_jw_num(w, "size", size_);
    puddle_jw_num(w, "chunk_size", PUDDLE_FILE_CHUNK);
    puddle_jw_num(w, "chunks", count_);
    puddle_jw_num(w, "received", have_);
    puddle_jw_end_object(w);
}

} // namespace puddle
