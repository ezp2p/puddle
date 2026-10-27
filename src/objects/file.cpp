#include "objects/file.hpp"

#include "core/error.hpp"
#include "storage/files.h"
#include "core/transfers.h"

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

File File::of(puddle_ctx *ctx, const puddle_file_ref &ref)
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
    return f;
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
