#include "core/backup.h"

#include "base/encoding.h"
#include "base/json.h"
#include "base/seal.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char *MAGIC = "PUDDLE-BACKUP 1\n";
constexpr const char *LABEL = "puddle-backup-v1";
constexpr size_t RECORD = PUDDLE_SEAL_RECORD;
constexpr uint32_t MAX_SEALED = RECORD + PUDDLE_SEAL_TAG_LEN;

void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* a UTF-8 path, the same on every system */
fs::path utf8_path(const std::string &s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string info_of(const char *id, uint64_t seq, int64_t made)
{
    return std::string(LABEL) + "\n" + id + "\n" + std::to_string(seq) + "\n" + std::to_string(made);
}

/* a path inside a backup: relative, '/'-separated, no "." or ".." parts, nothing odd */
bool safe_path(const std::string &p)
{
    if (p.empty() || p.size() > 1024 || p[0] == '/' || p.find('\\') != std::string::npos || p.find('\0') != std::string::npos ||
        p.find(':') != std::string::npos)
        return false;
    size_t at = 0;
    while (at <= p.size()) {
        size_t end = p.find('/', at);
        if (end == std::string::npos)
            end = p.size();
        std::string part = p.substr(at, end - at);
        if (part.empty() || part == "." || part == "..")
            return false;
        at = end + 1;
    }
    return true;
}

/* ---- writing: the payload goes out in sealed records as it is made ---- */

struct Writer {
    std::ofstream out;
    unsigned char key[32];
    uint64_t index = 0;
    std::vector<unsigned char> buf, sealed;
    bool ok = true;

    void flush(bool last)
    {
        sealed.resize(buf.size() + PUDDLE_SEAL_TAG_LEN);
        if (puddle_seal_record(key, index++, 0, last ? 1 : 0, buf.data(), buf.size(), sealed.data()) != 0) {
            ok = false;
            return;
        }
        unsigned char len[4] = { (unsigned char)(sealed.size() >> 24), (unsigned char)(sealed.size() >> 16),
                                 (unsigned char)(sealed.size() >> 8), (unsigned char)sealed.size() };
        out.write((const char *)len, 4);
        out.write((const char *)sealed.data(), (std::streamsize)sealed.size());
        ok = ok && out.good();
        buf.clear();
    }
    void put(const void *p, size_t n)
    {
        const unsigned char *c = (const unsigned char *)p;
        while (n && ok) {
            size_t take = std::min(n, RECORD - buf.size());
            buf.insert(buf.end(), c, c + take);
            c += take;
            n -= take;
            if (buf.size() == RECORD)
                flush(false);
        }
    }
    void file_head(const std::string &path, uint64_t size)
    {
        unsigned char h[3] = { 'F', (unsigned char)(path.size() >> 8), (unsigned char)path.size() };
        put(h, 3);
        put(path.data(), path.size());
        unsigned char s[8];
        for (int i = 0; i < 8; i++)
            s[i] = (unsigned char)(size >> (56 - 8 * i));
        put(s, 8);
    }
};

bool add_bytes(Writer &w, const std::string &path, const char *bytes, size_t len)
{
    w.file_head(path, len);
    w.put(bytes, len);
    return w.ok;
}

bool add_file(Writer &w, const std::string &path, const fs::path &from, puddle_backup_replace_fn extra, void *user, char *err,
              size_t errlen)
{
    if (extra) {
        char *bytes = nullptr;
        size_t len = 0;
        if (extra(path.c_str(), &bytes, &len, user) == 1) {
            bool ok = add_bytes(w, path, bytes, len);
            free(bytes);
            return ok;
        }
    }
    std::ifstream in(from, std::ios::binary);
    std::error_code ec;
    uint64_t size = fs::file_size(from, ec);
    if (!in || ec) {
        seterr(err, errlen, "cannot read %s", from.string().c_str());
        return false;
    }
    w.file_head(path, size);
    std::vector<char> chunk(RECORD);
    uint64_t left = size;
    while (left && w.ok) {
        size_t want = (size_t)std::min<uint64_t>(left, chunk.size());
        in.read(chunk.data(), (std::streamsize)want);
        if ((size_t)in.gcount() != want) {
            seterr(err, errlen, "%s changed while it was being read", from.string().c_str());
            return false;
        }
        w.put(chunk.data(), want);
        left -= want;
    }
    return w.ok;
}

/* every regular file under `root`, as "<prefix>/<relative path>", leaving out what `skip` says */
template <typename Skip>
bool add_tree(Writer &w, const fs::path &root, const std::string &prefix, Skip skip, puddle_backup_replace_fn extra, void *user,
              char *err, size_t errlen)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return true;
    std::vector<std::pair<std::string, fs::path>> files;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            seterr(err, errlen, "cannot list %s: %s", root.string().c_str(), ec.message().c_str());
            return false;
        }
        if (it->is_symlink(ec) || !it->is_regular_file(ec))
            continue;
        std::string rel = fs::relative(it->path(), root, ec).generic_string();
        if (ec || skip(rel, it->path()))
            continue;
        files.emplace_back(prefix + "/" + rel, it->path());
    }
    std::sort(files.begin(), files.end());
    for (auto &[path, from] : files)
        if (!add_file(w, path, from, extra, user, err, errlen))
            return false;
    return true;
}

bool same_file(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    return fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec);
}

/* ---- reading ---- */

bool read_header(std::ifstream &in, puddle_backup_info *info, unsigned char enc_pub[32], unsigned char enc[32], char *err, size_t errlen)
{
    std::string magic(strlen(MAGIC), '\0');
    in.read(magic.data(), (std::streamsize)magic.size());
    if (!in || magic != MAGIC) {
        seterr(err, errlen, "that isn't a Puddle backup");
        return false;
    }
    std::string line;
    if (!std::getline(in, line) || line.size() > 4096) {
        seterr(err, errlen, "that backup is damaged");
        return false;
    }
    puddle_span v;
    int64_t seq = -1, made = 0;
    char hex[65] = "", b64[64] = "";
    size_t n = 0;
    if (puddle_json_member(line.data(), line.size(), "id", &v) != 0 || puddle_json_string(v, info->id, sizeof info->id) != 0 ||
        puddle_json_member(line.data(), line.size(), "seq", &v) != 0 || puddle_json_int64(v, &seq) != 0 || seq < 0 ||
        puddle_json_member(line.data(), line.size(), "made", &v) != 0 || puddle_json_int64(v, &made) != 0 ||
        puddle_json_member(line.data(), line.size(), "enc_pub", &v) != 0 || puddle_json_string(v, hex, sizeof hex) != 0 ||
        puddle_hex_decode(hex, enc_pub, 32) != 0 || puddle_json_member(line.data(), line.size(), "enc", &v) != 0 ||
        puddle_json_string(v, b64, sizeof b64) != 0 || puddle_base64_decode(b64, enc, 32, &n) != 0 || n != 32) {
        seterr(err, errlen, "that backup is damaged");
        return false;
    }
    info->seq = (uint64_t)seq;
    info->made_ms = made;
    return true;
}

} // namespace

extern "C" int puddle_backup_write(const puddle_config *cfg, puddle_identity *me, const char *path, puddle_backup_replace_fn extra,
                                   void *user, char *err, size_t errlen)
{
    char id[PUDDLE_USER_ID_LEN + 1];
    unsigned char enc_pub[32], enc[32];
    uint64_t seq = 0;
    puddle_identity_id(me, id);
    puddle_identity_public(me, nullptr, enc_pub, &seq);
    const int64_t made =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string info = info_of(id, seq, made);

    Writer w;
    puddle_hpke ctx;
    if (puddle_hpke_sender(enc_pub, info.data(), info.size(), enc, &ctx) != 0 ||
        puddle_hpke_export(&ctx, LABEL, strlen(LABEL), w.key, 32) != 0) {
        seterr(err, errlen, "cannot seal a backup");
        return -1;
    }
    OPENSSL_cleanse(&ctx, sizeof ctx);

    const fs::path final_path = utf8_path(path), tmp = utf8_path(std::string(path) + ".part");
    w.out.open(tmp, std::ios::binary | std::ios::trunc);
    if (!w.out) {
        seterr(err, errlen, "cannot write %s", tmp.string().c_str());
        return -1;
    }
    char hex[65], b64[64];
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", enc_pub[i]);
    puddle_base64_encode(enc, 32, b64, sizeof b64);
    w.out << MAGIC << "{\"id\":\"" << id << "\",\"seq\":" << seq << ",\"made\":" << made << ",\"enc_pub\":\"" << hex << "\",\"enc\":\""
          << b64 << "\"}\n";

    const fs::path cfg_dir = utf8_path(cfg->dir), data = utf8_path(cfg->data_dir), profiles = cfg_dir / "profiles";
    bool ok = true;
    /* the settings that travel: profiles, the theme, and puddle.yaml (restored beside, not over, the new computer's) */
    if (fs::exists(cfg_dir / "puddle.yaml"))
        ok = add_file(w, "puddle.yaml", cfg_dir / "puddle.yaml", extra, user, err, errlen);
    if (ok && fs::exists(cfg_dir / "theme.yaml"))
        ok = add_file(w, "theme.yaml", cfg_dir / "theme.yaml", extra, user, err, errlen);
    if (ok)
        ok = add_tree(w, profiles, "profiles", [](const std::string &, const fs::path &) { return false; }, extra, user, err, errlen);
    /* the data folder, but not a new computer's to make: its owner token, and (where the two folders are one) the TLS files
     * and the settings already taken */
    if (ok)
        ok = add_tree(
            w, data, "data",
            [&](const std::string &rel, const fs::path &p) {
                if (rel == "state/owner-token" || (rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".tmp") == 0) ||
                    (rel.size() > 5 && rel.compare(rel.size() - 5, 5, ".part") == 0))
                    return true;
                for (const char *f : { "cert.pem", "key.pem", "puddle.yaml", "theme.yaml" })
                    if (same_file(p, cfg_dir / f))
                        return true;
                std::error_code ec;
                std::string under = fs::relative(p, profiles, ec).generic_string();
                return !ec && !under.empty() && under.rfind("..", 0) != 0;
            },
            extra, user, err, errlen);
    if (ok) {
        const unsigned char e = 'E';
        w.put(&e, 1);
        w.flush(true);
        w.out.close();
        ok = w.ok && !w.out.fail();
        if (!ok)
            seterr(err, errlen, "cannot write %s", tmp.string().c_str());
    }
    OPENSSL_cleanse(w.key, sizeof w.key);
    std::error_code ec;
    if (ok) {
        fs::rename(tmp, final_path, ec);
        if (ec) {
            seterr(err, errlen, "cannot write %s: %s", path, ec.message().c_str());
            ok = false;
        }
    }
    if (!ok) {
        if (w.out.is_open())
            w.out.close();
        fs::remove(tmp, ec);
        if (err && !*err)
            seterr(err, errlen, "cannot write %s", path);
        return -1;
    }
    return 0;
}

extern "C" int puddle_backup_info_read(const char *path, puddle_backup_info *out, char *err, size_t errlen)
{
    std::ifstream in(utf8_path(path), std::ios::binary);
    if (!in) {
        seterr(err, errlen, "cannot read %s", path);
        return -1;
    }
    unsigned char enc_pub[32], enc[32];
    return read_header(in, out, enc_pub, enc, err, errlen) ? 0 : -1;
}

extern "C" int puddle_backup_is(const char *path)
{
    std::ifstream in(utf8_path(path), std::ios::binary);
    std::string magic(strlen(MAGIC), '\0');
    return in.read(magic.data(), (std::streamsize)magic.size()) && magic == MAGIC;
}

extern "C" int puddle_backup_restore(const puddle_config *cfg, const char *path, const char *phrase, puddle_backup_info *out,
                                     char *err, size_t errlen)
{
    if (puddle_identity_exists(cfg->data_dir)) {
        seterr(err, errlen, "there is already an identity in %s, and it is not overwritten", cfg->data_dir);
        return -1;
    }
    std::ifstream in(utf8_path(path), std::ios::binary);
    if (!in) {
        seterr(err, errlen, "cannot read %s", path);
        return -1;
    }
    puddle_backup_info info;
    memset(&info, 0, sizeof info);
    unsigned char enc_pub[32], enc[32], sk[32], pk[32], key[32];
    if (!read_header(in, &info, enc_pub, enc, err, errlen))
        return -1;
    if (puddle_identity_phrase_enc(phrase, sk, pk, err, errlen) != 0)
        return -1;
    if (memcmp(pk, enc_pub, 32) != 0) {
        OPENSSL_cleanse(sk, sizeof sk);
        seterr(err, errlen,
               "that recovery phrase doesn't open this backup. It opens with the phrase that was current when it was made (key "
               "generation %llu); after a key change, an older backup needs the older phrase",
               (unsigned long long)info.seq);
        return -1;
    }
    const std::string hinfo = info_of(info.id, info.seq, info.made_ms);
    puddle_hpke ctx;
    int rc = puddle_hpke_receiver(enc, sk, hinfo.data(), hinfo.size(), &ctx) || puddle_hpke_export(&ctx, LABEL, strlen(LABEL), key, 32);
    OPENSSL_cleanse(sk, sizeof sk);
    OPENSSL_cleanse(&ctx, sizeof ctx);
    if (rc) {
        seterr(err, errlen, "cannot open that backup");
        return -1;
    }

    /* everything goes into a folder beside the data folder first, and into place only once the whole file has opened */
    const fs::path cfg_dir = utf8_path(cfg->dir), data = utf8_path(cfg->data_dir);
    const fs::path stage = data.parent_path() / (data.filename().string() + ".restoring");
    std::error_code ec;
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    if (!ec)
        fs::permissions(stage, fs::perms::owner_all, ec);
    if (ec) {
        OPENSSL_cleanse(key, sizeof key);
        seterr(err, errlen, "cannot make %s: %s", stage.string().c_str(), ec.message().c_str());
        return -1;
    }

    std::vector<unsigned char> plain, sealed;
    size_t at = 0; /* read position in `plain` */
    uint64_t index = 0;
    bool last = false, failed = false;
    std::string why;
    /* more payload: the next record, opened */
    auto more = [&]() -> bool {
        if (last)
            return false;
        unsigned char len[4];
        if (!in.read((char *)len, 4)) {
            why = "that backup was cut short";
            return false;
        }
        uint32_t n = (uint32_t)len[0] << 24 | (uint32_t)len[1] << 16 | (uint32_t)len[2] << 8 | len[3];
        if (n < PUDDLE_SEAL_TAG_LEN || n > MAX_SEALED) {
            why = "that backup is damaged";
            return false;
        }
        sealed.resize(n);
        if (!in.read((char *)sealed.data(), n)) {
            why = "that backup was cut short";
            return false;
        }
        plain.erase(plain.begin(), plain.begin() + (std::ptrdiff_t)at);
        at = 0;
        size_t had = plain.size();
        plain.resize(had + n - PUDDLE_SEAL_TAG_LEN);
        if (puddle_open_record(key, index, 0, 0, sealed.data(), n, plain.data() + had) == 0) {
        } else if (puddle_open_record(key, index, 0, 1, sealed.data(), n, plain.data() + had) == 0) {
            last = true;
        } else {
            why = "that backup is damaged, or was changed";
            return false;
        }
        index++;
        return true;
    };
    auto need = [&](size_t n) -> bool {
        while (plain.size() - at < n)
            if (!more())
                return false;
        return true;
    };

    std::vector<std::string> placed; /* paths in the backup, in order */
    for (;;) {
        if (!need(1)) {
            failed = true;
            break;
        }
        unsigned char kind = plain[at++];
        if (kind == 'E')
            break;
        if (kind != 'F' || !need(2)) {
            why = why.empty() ? "that backup is damaged" : why;
            failed = true;
            break;
        }
        size_t plen = (size_t)plain[at] << 8 | plain[at + 1];
        at += 2;
        if (!need(plen + 8)) {
            failed = true;
            break;
        }
        std::string p((const char *)plain.data() + at, plen);
        at += plen;
        uint64_t size = 0;
        for (int i = 0; i < 8; i++)
            size = size << 8 | plain[at + i];
        at += 8;
        if (!safe_path(p)) {
            why = "that backup names a file it may not";
            failed = true;
            break;
        }
        fs::path to = stage / utf8_path(p);
        fs::create_directories(to.parent_path(), ec);
        std::ofstream o(to, std::ios::binary | std::ios::trunc);
        if (!o) {
            why = "cannot write " + to.string();
            failed = true;
            break;
        }
        while (size) {
            if (plain.size() == at && !more()) {
                failed = true;
                break;
            }
            size_t take = (size_t)std::min<uint64_t>(size, plain.size() - at);
            o.write((const char *)plain.data() + at, (std::streamsize)take);
            at += take;
            size -= take;
        }
        o.close();
        fs::permissions(to, fs::perms::owner_read | fs::perms::owner_write, ec); /* keys and chats: the owner's alone */
        if (failed || o.fail()) {
            why = why.empty() ? "cannot write " + to.string() : why;
            failed = true;
            break;
        }
        placed.push_back(p);
    }
    if (!failed && (!last || at != plain.size() || in.peek() != EOF)) {
        why = "that backup is damaged";
        failed = true;
    }
    OPENSSL_cleanse(key, sizeof key);
    if (failed || !fs::is_directory(stage / "data" / "identity")) {
        fs::remove_all(stage, ec);
        seterr(err, errlen, "%s", why.empty() ? "that backup has no identity in it" : why.c_str());
        return -1;
    }

    /* into place: the data folder (its identity last, so a Puddle sees one only once everything else is there), profiles, the
     * theme if there is none, and puddle.yaml beside this computer's own */
    std::vector<std::pair<fs::path, fs::path>> moves;
    for (const std::string &p : placed) {
        fs::path from = stage / utf8_path(p), to;
        if (p.rfind("data/", 0) == 0)
            to = data / utf8_path(p.substr(5));
        else if (p.rfind("profiles/", 0) == 0)
            to = cfg_dir / utf8_path(p);
        else if (p == "theme.yaml")
            to = fs::exists(cfg_dir / "theme.yaml") ? fs::path() : cfg_dir / "theme.yaml";
        else if (p == "puddle.yaml")
            to = cfg_dir / "puddle.yaml.from-backup";
        if (!to.empty())
            moves.emplace_back(from, to);
    }
    std::stable_partition(moves.begin(), moves.end(), [&](const auto &m) {
        std::error_code e;
        std::string rel = fs::relative(m.second, data / "identity", e).generic_string();
        return e || rel.empty() || rel.rfind("..", 0) == 0; /* not under identity/: first */
    });
    for (auto &[from, to] : moves) {
        fs::create_directories(to.parent_path(), ec);
        fs::rename(from, to, ec);
        if (ec) { /* another file system: copy instead */
            ec.clear();
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            seterr(err, errlen, "cannot put %s in place: %s", to.string().c_str(), ec.message().c_str());
            return -1;
        }
    }
    fs::remove_all(stage, ec);
    if (out)
        *out = info;
    return 0;
}
