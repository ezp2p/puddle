#include "core/pond_dms.hpp"

#include "base/encoding.h"
#include "base/hash.h"
#include "base/json.h"
#include "base/log.h"
#include "platform/fsutil.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace puddle::pdm {

namespace {

bool idOk(const std::string &s, size_t len)
{
    if (s.size() != len)
        return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return false;
    return true;
}

std::string readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeFile(const std::string &dir, const std::string &name, const std::string &data)
{
    std::string path = dir + "/" + name;
    return puddle_write_file_atomic(dir.c_str(), path.c_str(), data.data(), data.size()) == 0;
}

// a JSON text built with puddle_jw, growing until it fits
template <class F> std::string build(size_t guess, F fill)
{
    for (size_t cap = guess; cap < (64u << 20); cap *= 2) {
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

struct Reader {
    puddle_span all;
    std::string str(const char *key) const
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
    int64_t num(const char *key) const
    {
        puddle_span v;
        int64_t n = 0;
        if (puddle_json_member(all.p, all.len, key, &v) != 0 || puddle_json_int64(v, &n) != 0)
            return 0;
        return n;
    }
    bool flag(const char *key) const
    {
        puddle_span v;
        return puddle_json_member(all.p, all.len, key, &v) == 0 && v.len == 4 && !memcmp(v.p, "true", 4);
    }
    template <class F> void each(const char *key, F f) const
    {
        puddle_span arr, item;
        size_t cur = 0;
        if (puddle_json_member(all.p, all.len, key, &arr) != 0)
            return;
        while (puddle_json_array_next(arr, &cur, &item) == 1)
            f(Reader{item});
    }
};

std::string convJson(const Conv &c)
{
    return build(1024, [&](puddle_jw *w) {
        puddle_jw_object(w);
        puddle_jw_str(w, "chat", c.chat.c_str());
        puddle_jw_str(w, "pond", c.pond.c_str());
        puddle_jw_str(w, "member", c.member.c_str());
        puddle_jw_str(w, "pond_name", c.pondName.c_str());
        puddle_jw_str(w, "offer", c.offer.c_str());
        puddle_jw_flag(w, "offered", c.offered);
        puddle_jw_num(w, "created", c.created);
        puddle_jw_end_object(w);
    });
}

std::optional<Conv> convOf(const std::string &text)
{
    if (!puddle_json_valid_object(text.data(), text.size()))
        return std::nullopt;
    Reader r{{text.data(), text.size()}};
    Conv c;
    c.chat = r.str("chat"), c.pond = r.str("pond"), c.member = r.str("member"), c.pondName = r.str("pond_name");
    c.offer = r.str("offer"), c.offered = r.flag("offered"), c.created = r.num("created");
    if (!idOk(c.chat, PUDDLE_USER_ID_LEN) || !idOk(c.pond, PUDDLE_USER_ID_LEN) || !idOk(c.member, PUDDLE_USER_ID_LEN))
        return std::nullopt;
    return c;
}

std::string letterJson(const Letter &l)
{
    return build(l.event.size() * 2 + 4096, [&](puddle_jw *w) {
        puddle_jw_object(w);
        puddle_jw_str(w, "id", l.id.c_str());
        puddle_jw_str(w, "chat", l.chat.c_str());
        puddle_jw_str(w, "pond", l.pond.c_str());
        puddle_jw_str(w, "to", l.to.c_str());
        puddle_jw_str(w, "kind", l.kind.c_str());
        puddle_jw_key(w, "event");
        puddle_jw_string_n(w, l.event.data(), l.event.size());
        puddle_jw_str(w, "event_sig", l.eventSig.c_str());
        puddle_jw_str(w, "link", l.link.c_str());
        puddle_jw_key(w, "ids");
        puddle_jw_array(w);
        for (auto &i : l.ids)
            puddle_jw_string(w, i.c_str());
        puddle_jw_end_array(w);
        puddle_jw_key(w, "files");
        puddle_jw_array(w);
        for (auto &f : l.files) {
            puddle_jw_object(w);
            puddle_jw_str(w, "id", f.id.c_str());
            puddle_jw_str(w, "key", f.key.c_str());
            puddle_jw_end_object(w);
        }
        puddle_jw_end_array(w);
        puddle_jw_num(w, "created", l.created);
        puddle_jw_num(w, "next", l.next);
        puddle_jw_num(w, "attempts", l.attempts);
        puddle_jw_num(w, "state", l.state);
        puddle_jw_str(w, "reason", l.reason.c_str());
        puddle_jw_end_object(w);
    });
}

std::optional<Letter> letterOf(const std::string &text)
{
    if (!puddle_json_valid_object(text.data(), text.size()))
        return std::nullopt;
    Reader r{{text.data(), text.size()}};
    Letter l;
    l.id = r.str("id"), l.chat = r.str("chat"), l.pond = r.str("pond"), l.to = r.str("to"), l.kind = r.str("kind");
    l.event = r.str("event"), l.eventSig = r.str("event_sig"), l.link = r.str("link");
    r.each("ids", [&](Reader i) {
        char id[65];
        if (puddle_json_string(i.all, id, sizeof id) == 0)
            l.ids.push_back(id);
    });
    r.each("files", [&](Reader f) { l.files.push_back({f.str("id"), f.str("key")}); });
    l.created = r.num("created"), l.next = r.num("next"), l.attempts = (int)r.num("attempts");
    int64_t st = r.num("state");
    l.state = st == 1 ? Letter::Delivered : st == 2 ? Letter::Failed : Letter::Waiting;
    l.reason = r.str("reason");
    if (!idOk(l.id, 64) || !idOk(l.pond, PUDDLE_USER_ID_LEN) || !idOk(l.to, PUDDLE_USER_ID_LEN))
        return std::nullopt;
    return l;
}

std::string fetchJson(const Fetch &f)
{
    return build(4096, [&](puddle_jw *w) {
        puddle_jw_object(w);
        puddle_jw_str(w, "letter", f.letter.c_str());
        puddle_jw_str(w, "pond", f.pond.c_str());
        puddle_jw_str(w, "from", f.from.c_str());
        puddle_jw_str(w, "chat", f.chat.c_str());
        puddle_jw_str(w, "message", f.message.c_str());
        puddle_jw_key(w, "files");
        puddle_jw_array(w);
        for (auto &x : f.files) {
            puddle_jw_object(w);
            puddle_jw_num(w, "n", x.n);
            puddle_jw_str(w, "id", x.id.c_str());
            puddle_jw_str(w, "key", x.key.c_str());
            puddle_jw_num(w, "size", x.size);
            puddle_jw_flag(w, "done", x.done);
            puddle_jw_end_object(w);
        }
        puddle_jw_end_array(w);
        puddle_jw_num(w, "created", f.created);
        puddle_jw_num(w, "next", f.next);
        puddle_jw_num(w, "attempts", f.attempts);
        puddle_jw_end_object(w);
    });
}

std::optional<Fetch> fetchOf(const std::string &text)
{
    if (!puddle_json_valid_object(text.data(), text.size()))
        return std::nullopt;
    Reader r{{text.data(), text.size()}};
    Fetch f;
    f.letter = r.str("letter"), f.pond = r.str("pond"), f.from = r.str("from"), f.chat = r.str("chat"), f.message = r.str("message");
    r.each("files", [&](Reader x) {
        Fetch::File ff;
        ff.n = (int)x.num("n"), ff.id = x.str("id"), ff.key = x.str("key"), ff.size = x.num("size"), ff.done = x.flag("done");
        f.files.push_back(ff);
    });
    f.created = r.num("created"), f.next = r.num("next"), f.attempts = (int)r.num("attempts");
    if (!idOk(f.letter, 64) || !idOk(f.pond, PUDDLE_USER_ID_LEN) || !idOk(f.from, PUDDLE_USER_ID_LEN))
        return std::nullopt;
    return f;
}

template <class T, class P> std::vector<T> readAll(const std::string &dir, P parse)
{
    std::vector<T> out;
    puddle_dir *d = puddle_dir_open(dir.c_str());
    if (!d)
        return out;
    const char *name;
    while ((name = puddle_dir_next(d))) {
        std::string n = name;
        if (n.size() < 6 || n.compare(n.size() - 5, 5, ".json") != 0)
            continue;
        if (auto v = parse(readFile(dir + "/" + n)))
            out.push_back(std::move(*v));
    }
    puddle_dir_close(d);
    return out;
}

} // namespace

std::string chatId(const std::string &pond, const std::string &a, const std::string &b)
{
    if (!idOk(pond, PUDDLE_USER_ID_LEN) || !idOk(a, PUDDLE_USER_ID_LEN) || !idOk(b, PUDDLE_USER_ID_LEN))
        return "";
    const std::string &lo = a < b ? a : b, &hi = a < b ? b : a;
    std::string in = "pdm" + pond + lo + hi;
    unsigned char raw[32];
    puddle_sha256(in.data(), in.size(), raw);
    char out[PUDDLE_USER_ID_LEN + 1];
    if (puddle_base32_encode(raw, 32, out, sizeof out) != PUDDLE_USER_ID_LEN)
        return "";
    return out;
}

std::unique_ptr<Store> Store::open(const std::string &dataDir, std::string &err)
{
    std::string dir = dataDir + "/state/pond_dms";
    for (const char *sub : {"", "/conversations", "/out", "/in"})
        if (puddle_mkdir_p((dir + sub).c_str()) != 0) {
            err = "cannot make " + dir + sub;
            return nullptr;
        }
    std::unique_ptr<Store> s(new Store(dir));
    std::istringstream lines(readFile(dir + "/blocks.tsv"));
    std::string line;
    while (std::getline(lines, line)) {
        size_t tab = line.find('\t');
        std::string id = line.substr(0, tab);
        if (idOk(id, PUDDLE_USER_ID_LEN))
            s->blocks_.push_back({id, tab == std::string::npos ? 0 : strtoll(line.c_str() + tab + 1, nullptr, 10)});
    }
    return s;
}

std::optional<Conv> Store::conv(const std::string &chat)
{
    if (!idOk(chat, PUDDLE_USER_ID_LEN))
        return std::nullopt;
    std::lock_guard<std::mutex> g(mu_);
    return convOf(readFile(dir_ + "/conversations/" + chat + ".json"));
}

std::optional<Conv> Store::convWith(const std::string &pond, const std::string &member)
{
    for (Conv &c : convs())
        if (c.pond == pond && c.member == member)
            return c;
    return std::nullopt;
}

std::vector<Conv> Store::convs()
{
    std::lock_guard<std::mutex> g(mu_);
    return readAll<Conv>(dir_ + "/conversations", convOf);
}

bool Store::put(const Conv &c)
{
    std::string text = convJson(c);
    std::lock_guard<std::mutex> g(mu_);
    return !text.empty() && writeFile(dir_ + "/conversations", c.chat + ".json", text);
}

bool Store::put(const Letter &l)
{
    std::string text = letterJson(l);
    std::lock_guard<std::mutex> g(mu_);
    return !text.empty() && writeFile(dir_ + "/out", l.id + ".json", text);
}

std::optional<Letter> Store::letter(const std::string &id)
{
    if (!idOk(id, 64))
        return std::nullopt;
    std::lock_guard<std::mutex> g(mu_);
    return letterOf(readFile(dir_ + "/out/" + id + ".json"));
}

std::vector<Letter> Store::letters()
{
    std::lock_guard<std::mutex> g(mu_);
    return readAll<Letter>(dir_ + "/out", letterOf);
}

bool Store::removeLetter(const std::string &id)
{
    if (!idOk(id, 64))
        return false;
    std::lock_guard<std::mutex> g(mu_);
    return puddle_remove_file((dir_ + "/out/" + id + ".json").c_str()) == 0;
}

bool Store::put(const Fetch &f)
{
    std::string text = fetchJson(f);
    std::lock_guard<std::mutex> g(mu_);
    return !text.empty() && writeFile(dir_ + "/in", f.letter + ".json", text);
}

std::vector<Fetch> Store::fetches()
{
    std::lock_guard<std::mutex> g(mu_);
    return readAll<Fetch>(dir_ + "/in", fetchOf);
}

bool Store::removeFetch(const std::string &letter)
{
    if (!idOk(letter, 64))
        return false;
    std::lock_guard<std::mutex> g(mu_);
    return puddle_remove_file((dir_ + "/in/" + letter + ".json").c_str()) == 0;
}

bool Store::saveBlocks()
{
    std::string text;
    for (auto &[id, t] : blocks_)
        text += id + "\t" + std::to_string(t) + "\n";
    return writeFile(dir_, "blocks.tsv", text);
}

int Store::block(const std::string &id, int64_t now)
{
    if (!idOk(id, PUDDLE_USER_ID_LEN))
        return -1;
    std::lock_guard<std::mutex> g(mu_);
    for (auto &b : blocks_)
        if (b.first == id)
            return 1;
    blocks_.push_back({id, now});
    return saveBlocks() ? 0 : -1;
}

int Store::unblock(const std::string &id)
{
    std::lock_guard<std::mutex> g(mu_);
    for (size_t i = 0; i < blocks_.size(); i++)
        if (blocks_[i].first == id) {
            blocks_.erase(blocks_.begin() + i);
            return saveBlocks() ? 0 : -1;
        }
    return 1;
}

bool Store::blocked(const std::string &id)
{
    std::lock_guard<std::mutex> g(mu_);
    for (auto &b : blocks_)
        if (b.first == id)
            return true;
    return false;
}

std::vector<std::pair<std::string, int64_t>> Store::blocks()
{
    std::lock_guard<std::mutex> g(mu_);
    return blocks_;
}

} // namespace puddle::pdm
