#include "api/api.h"
#include "api/request.hpp"
#include "objects/file.hpp"
#include "storage/files.h"

#include <cstdlib>
#include <cstring>

/* Files sent in chats (docs/files.md): the owner uploads one a chunk at a time, then sends it in a message
 * (POST conversations/{id}/messages/send with "files"). Reading one back is the same for a file the owner sent and one
 * that arrived. */

using namespace puddle;
using namespace puddle::api;

static Upload uploadOf(const puddle_call *c)
{
    auto u = Upload::get(c->ctx, puddle_param(c, "upload"));
    if (!u)
        throw Error::notFound();
    return *u;
}

static int create_upload(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    auto size = b.optInt("size");
    if (!size)
        throw Error::badRequest("bad_request", "size: the file's length in bytes");
    Upload::create(c->ctx, *size).write(w);
    return 201;
}

static int get_upload(const puddle_call *c, puddle_jw *w)
{
    uploadOf(c).write(w);
    return 200;
}

/* the body is the chunk's bytes, as they are */
static int put_chunk(const puddle_call *c, puddle_jw *w)
{
    uint32_t n = (uint32_t)strtoul(puddle_param(c, "n"), nullptr, 10);
    uploadOf(c).put(n, c->body, c->body_len).write(w);
    return 200;
}

static int finish_upload(const puddle_call *c, puddle_jw *w)
{
    uploadOf(c).finish().write(w);
    return 201;
}

static int cancel_upload(const puddle_call *c, puddle_jw *w)
{
    Upload u = uploadOf(c);
    u.write(w);
    u.cancel();
    return 200;
}

static int get_file(const puddle_call *c, puddle_jw *w)
{
    auto f = File::here(c->ctx, puddle_param(c, "file"));
    if (!f)
        throw Error::notFound();
    f->write(w);
    return 200;
}

/* The bytes of a file that is here whole, read from disk as they are sent; a Range asks for part. Sent as
 * application/octet-stream: what kind of file it is comes with the message, and it is never sniffed. */
static int file_content(const puddle_call *c, puddle_jw *w)
{
    char path[1100];
    if (!c->ctx->files || puddle_files_path(c->ctx->files, puddle_param(c, "file"), path, sizeof path) != 0 ||
        !puddle_answer_file(c, path, "application/octet-stream", 1))
        return puddle_fail(w, 404, "not_found");
    return 200;
}

const puddle_route puddle_file_api_routes[] = {
    { "POST", "/api/v1/files/uploads/create",                           PUDDLE_AUTH_OWNER, 256,               create_upload, NULL },
    { "GET",  "/api/v1/files/uploads/{upload:token}",                   PUDDLE_AUTH_OWNER, 0,                 get_upload,    NULL },
    { "POST", "/api/v1/files/uploads/{upload:token}/chunks/{n:index}",  PUDDLE_AUTH_OWNER, PUDDLE_FILE_CHUNK, put_chunk,     NULL },
    { "POST", "/api/v1/files/uploads/{upload:token}/finish",            PUDDLE_AUTH_OWNER, 64,                finish_upload, NULL },
    { "POST", "/api/v1/files/uploads/{upload:token}/cancel",            PUDDLE_AUTH_OWNER, 64,                cancel_upload, NULL },
    { "GET",  "/api/v1/files/{file:event}",                             PUDDLE_AUTH_OWNER, 0,                 get_file,      NULL },
    { "GET",  "/api/v1/files/{file:event}/content",                     PUDDLE_AUTH_OWNER, 0,                 file_content,  NULL },
};
const size_t puddle_file_api_routes_count = sizeof puddle_file_api_routes / sizeof puddle_file_api_routes[0];
