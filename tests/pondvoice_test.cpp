// Voice channels in Ponds (docs/ponds.md, "Voice channels"; roadmap 9.2), the Puddle's side, against a stand-in Pond: a real
// Puddle joins it through the owner API as the window does (the window's own Ponds model), and the Pond here speaks the
// Pond's voice routes (the Pond's docs/voice.md) with libdatachannel, as the real one does. Joining only a voice channel,
// the connection coming up, the owner's frames reaching the Pond, the others' coming back marked with whose they are (by
// slot, from the Pond's events), muted and deafened, the Pond letting us go, leaving; and "already in a call" both ways with
// a friend. A real Pond and real Puddles meet in the pond repository's e2e test. Skipped (77) without calls built.

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include "testnode.h"

#include "federation/calls.h"

#ifndef PUDDLE_HAVE_CALLS
int main()
{
    printf("pondvoice_test: skipped (built without calls)\n");
    return 77;
}
#else

#include <rtc/rtc.hpp>

extern "C" {
#include "base/encoding.h"
#include "base/hash.h"
#include "base/json.h"
#include "core/streams.h"
#include "http/http.h"
#include "http/serve.h"
#include "http/tls.h"
}

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <poll.h>
#include <string>
#include <vector>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ids as real ones are made: base32 of a SHA-256 (52 characters, the last one carrying a single bit) */
static std::string id_of(const char *seed)
{
    unsigned char h[32];
    char out[PUDDLE_USER_ID_LEN + 8];
    puddle_sha256((const unsigned char *)seed, strlen(seed), h);
    puddle_base32_encode(h, 32, out, sizeof out);
    return out;
}
static const std::string BEN_ID = id_of("ben"), POND_ID_S = id_of("pond");
static const char *BEN = BEN_ID.c_str();          /* a member already in the voice channel, slot 1 */
static const char *POND_ID = POND_ID_S.c_str();

static std::string field(const std::string &json, const char *key)
{
    puddle_span v;
    if (puddle_json_member(json.data(), json.size(), key, &v) != 0)
        return "";
    std::string out(v.len + 1, '\0');
    if (puddle_json_string(v, out.data(), out.size()) == 0)
        return out.c_str();
    return std::string(v.p, v.len); /* not a string: as it is */
}

static std::string quoted(const std::string &s)
{
    std::string buf(s.size() * 6 + 8, '\0');
    puddle_jw w;
    puddle_jw_init(&w, buf.data(), buf.size());
    puddle_jw_string(&w, s.c_str());
    buf.resize(puddle_jw_finish(&w));
    return buf;
}

// ---- a stand-in Pond: HTTPS, a thread a connection, and voice with libdatachannel ----

struct FakePond {
    SSL_CTX *ctx = nullptr;
    int listen_fd = -1, stop_fd[2] = { -1, -1 };
    unsigned port = 0, udp = 0;
    std::thread acceptor;
    std::mutex mu;
    std::condition_variable news;
    std::string me;                       /* the Puddle that joined */
    std::vector<std::string> events;      /* event n is events[n - 1] */
    // our Puddle's voice connection
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> voice, watch_keep[2]; /* our Puddle's voice; its camera and screen (held: tracks are held weakly) */
    std::shared_ptr<rtc::DataChannel> dc;
    std::atomic<bool> dc_open{false}, connected{false};
    std::string session;
    bool in_voice = false, muted = false, deafened = false;
    std::atomic<int> frames{0}, joins{0}, leaves{0}, camera_in{0}, screen_in{0};
    std::shared_ptr<rtc::Track> out[17];  /* v1 .. v16: what our Puddle watches */
    std::string watch_body;               /* the last watch it asked for */
    bool ben_camera = false, ben_screen = false;
    std::string last_state, created;     /* the last state sent, and the last channel an admin asked for */
    bool refuse_state = false;            /* answer state with a 404: it has let us go */

    std::string voice_list()
    {
        std::string out = "[{\"channel\":2,\"member\":\"" + std::string(BEN) + "\",\"slot\":1,\"muted\":false,\"deafened\":false,\"camera\":" +
                          (ben_camera ? "true" : "false") + ",\"screen\":" + (ben_screen ? "true" : "false") + "}";
        if (in_voice && !me.empty())
            out += ",{\"channel\":2,\"member\":\"" + me + "\",\"slot\":2,\"muted\":" + (muted ? "true" : "false") + ",\"deafened\":" +
                   (deafened ? "true" : "false") + ",\"camera\":false}";
        return out + "]";
    }

    std::string info()
    {
        return "{\"protocol\":4,\"features\":[\"files\",\"roles\",\"voice\"],\"pond\":{\"id\":\"" + std::string(POND_ID) +
               "\",\"name\":\"Voice Pond\",\"description\":\"\",\"files\":{\"max_size\":0,\"chunk_size\":1048576,\"per_message\":10}},"
               "\"me\":{\"id\":\"" + me + "\",\"profile\":null,\"joined\":1,\"role\":\"operator\"},\"offer\":null,"
               "\"channels\":[{\"id\":1,\"name\":\"general\",\"topic\":\"\",\"position\":0,\"kind\":\"text\"},"
               "{\"id\":2,\"name\":\"talk\",\"topic\":\"\",\"position\":1,\"kind\":\"voice\"}],\"voice\":" + voice_list() +
               ",\"cursor\":" + std::to_string(events.size()) + "}";
    }

    void event(const std::string &kind, const std::string &data) /* lock held */
    {
        events.push_back("{\"id\":" + std::to_string(events.size() + 1) + ",\"kind\":\"" + kind + "\",\"data\":" + data + "}");
        news.notify_all();
    }

    std::string me_in_voice() /* lock held */
    {
        return "{\"channel\":2,\"member\":\"" + me + "\",\"slot\":2,\"muted\":" + (muted ? "true" : "false") + ",\"deafened\":" +
               (deafened ? "true" : "false") + ",\"camera\":false}";
    }

    // a voice join: answer the offer, as the Pond's src/voice.cpp does
    std::string join_voice(const std::string &offer, int their_udp, int *status)
    {
        rtc::Configuration conf;
        conf.enableIceUdpMux = true;
        conf.disableAutoNegotiation = true;
        conf.portRangeBegin = conf.portRangeEnd = (uint16_t)udp;
        conf.bindAddress = "127.0.0.1";
        auto p = std::make_shared<rtc::PeerConnection>(conf);
        auto seen = std::make_shared<std::atomic<int>>(0);
        p->onStateChange([this](rtc::PeerConnection::State s) {
            if (s != rtc::PeerConnection::State::Connected)
                return;
            connected = true;
            std::lock_guard<std::mutex> g(mu);
            in_voice = true;
            event("voice", me_in_voice());
        });
        p->onTrack([this, seen](std::shared_ptr<rtc::Track> t) {
            const std::string mid = t->mid();
            if (mid == "audio") {
                t->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
                t->onFrame([this](rtc::binary data, rtc::FrameInfo) {
                    if (data.size() >= 3 && data[0] == std::byte{'A'})
                        frames++;
                });
                std::lock_guard<std::mutex> g(mu);
                voice = t;
            } else if (mid == "video" || mid == "screen") {
                t->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::LongStartSequence));
                const bool screen = mid == "screen";
                t->onFrame([this, screen](rtc::binary, rtc::FrameInfo) { (screen ? screen_in : camera_in)++; });
                std::lock_guard<std::mutex> g(mu);
                (screen ? watch_keep[1] : watch_keep[0]) = t;
            } else if (mid.size() >= 2 && mid[0] == 'v') {
                int n = atoi(mid.c_str() + 1);
                auto d = t->description();
                d.addSSRC(7000 + (uint32_t)n * 2 + 1, std::string("pond"));
                t->setDescription(std::move(d));
                auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(7000 + (uint32_t)n * 2 + 1, "pond", 96, rtc::H264RtpPacketizer::ClockRate);
                t->setMediaHandler(std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp, 1200));
                std::lock_guard<std::mutex> g(mu);
                if (n >= 1 && n <= 16)
                    out[n] = t;
            }
            (*seen)++;
        });
        p->onDataChannel([this](std::shared_ptr<rtc::DataChannel> d) {
            d->onOpen([this] { dc_open = true; });
            std::lock_guard<std::mutex> g(mu);
            dc = d;
        });
        p->setRemoteDescription(rtc::Description(offer, "offer"));
        p->addRemoteCandidate(rtc::Candidate("a=candidate:9 1 UDP 2122000000 127.0.0.1 " + std::to_string(their_udp) + " typ host", "audio"));
        int lines = 0;
        for (size_t at = 0; (at = offer.find("\nm=", at)) != std::string::npos; at += 3)
            lines += offer.compare(at + 1, 13, "m=application") != 0;
        for (int i = 0; i < 500 && seen->load() < lines; i++)
            usleep(10000);
        p->setLocalDescription();
        for (int i = 0; i < 300 && p->gatheringState() != rtc::PeerConnection::GatheringState::Complete; i++)
            usleep(10000);
        std::lock_guard<std::mutex> g(mu);
        pc = p;
        session = "sessionsessionsessionsess" + std::to_string(++joins % 10);
        *status = 200;
        return "{\"session\":\"" + session + "\",\"sdp\":" + quoted(std::string(*p->localDescription())) + ",\"udp\":" + std::to_string(udp) +
               ",\"slot\":2}";
    }

    // Ben turns his camera or screen on or off: everyone is told
    void ben(bool camera, bool screen)
    {
        std::lock_guard<std::mutex> g(mu);
        ben_camera = camera, ben_screen = screen;
        event("voice", "{\"channel\":2,\"member\":\"" + std::string(BEN) + "\",\"slot\":1,\"muted\":false,\"deafened\":false,\"camera\":" +
                           (camera ? "true" : "false") + ",\"screen\":" + (screen ? "true" : "false") + "}");
    }

    // a picture on track v<n>, as the Pond forwards a camera or screen watched there
    bool show_on(int n, const std::string &au, uint32_t ts)
    {
        std::shared_ptr<rtc::Track> t;
        {
            std::lock_guard<std::mutex> g(mu);
            t = out[n];
        }
        if (!t || !t->isOpen())
            return false;
        rtc::binary b(au.size());
        memcpy(b.data(), au.data(), au.size());
        try {
            t->sendFrame(b, rtc::FrameInfo(ts));
        } catch (const std::exception &) {
            return false;
        }
        return true;
    }

    // the Pond forwarding a member's frame: [slot][ts][opus]
    bool send_from(int slot, const std::string &frame, uint32_t ts)
    {
        std::shared_ptr<rtc::DataChannel> d;
        {
            std::lock_guard<std::mutex> g(mu);
            d = dc;
        }
        if (!d || !dc_open)
            return false;
        rtc::binary m(6 + frame.size());
        m[0] = std::byte(slot >> 8), m[1] = std::byte(slot & 0xff);
        for (int i = 0; i < 4; i++)
            m[2 + i] = std::byte(ts >> (24 - 8 * i));
        memcpy(m.data() + 6, frame.data(), frame.size());
        try {
            d->send(m);
        } catch (const std::exception &) {
            return false;
        }
        return true;
    }

    std::string answer(const std::string &method, const std::string &target, const std::string &body, const std::string &signature,
                       int *status)
    {
        *status = 200;
        if (method == "POST" && target == "/pond/v1/join") {
            size_t at = signature.find("id=");
            std::lock_guard<std::mutex> g(mu);
            me = at == std::string::npos ? "" : signature.substr(at + 3, 52);
            return info();
        }
        if (target == "/pond/v1/info") {
            std::lock_guard<std::mutex> g(mu);
            return info();
        }
        if (target == "/pond/v1/members") {
            std::lock_guard<std::mutex> g(mu);
            return "{\"members\":[{\"id\":\"" + me + "\",\"profile\":null,\"joined\":1,\"role\":\"operator\"},{\"id\":\"" + std::string(BEN) +
                   "\",\"profile\":{\"display_name\":\"Ben\"},\"joined\":1,\"role\":\"member\"}]}";
        }
        if (target.rfind("/pond/v1/channels/", 0) == 0 && target.find("/messages") != std::string::npos)
            return "{\"messages\":[]}";
        if (target.rfind("/pond/v1/events?after=", 0) == 0) {
            size_t after = strtoul(target.c_str() + 22, nullptr, 10);
            std::unique_lock<std::mutex> lk(mu);
            news.wait_for(lk, std::chrono::milliseconds(300), [&] { return events.size() > after; });
            std::string out = "{\"events\":[";
            for (size_t i = after; i < events.size(); i++)
                out += (i > after ? "," : "") + events[i];
            return out + "],\"cursor\":" + std::to_string(std::max(after, events.size())) + "}";
        }
        if (method == "POST" && target == "/pond/v1/channels/2/voice/join")
            return join_voice(field(body, "sdp"), atoi(field(body, "udp").c_str()), status);
        if (method == "POST" && target.rfind("/pond/v1/channels/", 0) == 0 && target.find("/voice/join") != std::string::npos) {
            *status = 404;
            return "{\"error\":\"no_channel\"}";
        }
        if (method == "POST" && target.rfind("/pond/v1/voice/", 0) == 0) {
            std::lock_guard<std::mutex> g(mu);
            if (target.find(session) == std::string::npos || (refuse_state && target.find("/state") != std::string::npos)) {
                *status = 404;
                return "{\"error\":\"no_session\"}";
            }
            if (target.find("/state") != std::string::npos) {
                last_state = body;
                muted = body.find("\"muted\":true") != std::string::npos;
                deafened = body.find("\"deafened\":true") != std::string::npos;
                event("voice", me_in_voice());
                *status = 204;
                return "";
            }
            if (target.find("/watch") != std::string::npos) {
                watch_body = body;
                *status = 204;
                return "";
            }
            if (target.find("/leave") != std::string::npos) {
                leaves++;
                in_voice = false;
                event("voice_left", "{\"channel\":2,\"member\":\"" + me + "\"}");
                *status = 204;
                return "";
            }
        }
        if (method == "POST" && target == "/pond/v1/admin/channels/create") {
            std::lock_guard<std::mutex> g(mu);
            created = body;
            *status = 204;
            return "";
        }
        if (method == "POST") { /* the profile, keys and the rest: taken */
            *status = 204;
            return "";
        }
        *status = 404;
        return "{\"error\":\"not_found\"}";
    }

    static int owner_check(void *, const char *) { return 0; }
    static size_t limit(void *, const puddle_request *, int) { return 200000; }
    static size_t route(void *user, const puddle_request *r, int, const unsigned char *body, size_t n, char *out, size_t cap, int *status)
    {
        auto *self = static_cast<FakePond *>(user);
        std::string reply = self->answer(r->method, r->target, std::string((const char *)body, n), r->signature, status);
        return puddle_http_response(*status, NULL, reply.data(), reply.size(), out, cap);
    }
    static int io_read(void *io, void *buf, size_t cap)
    {
        int n = SSL_read(static_cast<SSL *>(io), buf, (int)cap);
        return n > 0 ? n : 0;
    }
    static int io_write(void *io, const void *buf, size_t len) { return SSL_write(static_cast<SSL *>(io), buf, (int)len) > 0 ? 0 : -1; }

    void serve(int fd)
    {
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) == 1) {
            puddle_http_handler h{};
            h.owner_check = owner_check, h.body_limit = limit, h.route = route, h.user = this, h.deadline_seconds = 10;
            puddle_io io = { io_read, io_write, ssl };
            puddle_http_serve(&h, &io, NULL);
            SSL_shutdown(ssl);
        }
        SSL_free(ssl);
        close(fd);
    }

    bool start(const std::string &dir)
    {
        char err[200];
        puddle_mkdir_p(dir.c_str());
        ctx = puddle_tls_context(dir.c_str(), err, sizeof err);
        if (!ctx)
            return false;
        port = free_port();
        udp = free_port();
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)port);
        if (bind(listen_fd, (sockaddr *)&a, sizeof a) != 0 || listen(listen_fd, 16) != 0 || pipe(stop_fd) != 0)
            return false;
        acceptor = std::thread([this] {
            for (;;) {
                pollfd p[2] = { { listen_fd, POLLIN, 0 }, { stop_fd[0], POLLIN, 0 } };
                if (poll(p, 2, -1) < 0)
                    continue;
                if (p[1].revents)
                    break;
                int fd = accept(listen_fd, NULL, NULL);
                if (fd >= 0)
                    std::thread([this, fd] { serve(fd); }).detach();
            }
        });
        return true;
    }

    void close_voice()
    {
        std::shared_ptr<rtc::PeerConnection> p;
        {
            std::lock_guard<std::mutex> g(mu);
            p = pc;
            pc.reset(), dc.reset(), voice.reset(), watch_keep[0].reset(), watch_keep[1].reset();
            for (auto &t : out)
                t.reset();
        }
        if (p) {
            p->resetCallbacks();
            p->close();
        }
        dc_open = false;
    }

    ~FakePond()
    {
        close_voice();
        if (stop_fd[1] >= 0 && write(stop_fd[1], "x", 1) == 1 && acceptor.joinable())
            acceptor.join();
        if (listen_fd >= 0)
            close(listen_fd);
    }
};

// ---- the owner's device: frames as they arrive ----

struct Heard {
    std::mutex mu;
    std::map<std::string, int> from; /* sender -> frames */
    std::map<std::string, int> seen; /* sender -> pictures (a camera, or "<id>/screen") */
    std::string call;
};

static void on_picture(void *user, const char *, const char *from, const unsigned char *, size_t len, uint32_t)
{
    auto *h = static_cast<Heard *>(user);
    if (len < 100)
        return;
    std::lock_guard<std::mutex> g(h->mu);
    h->seen[from]++;
}

static int seen_from(Heard &h, const std::string &who)
{
    std::lock_guard<std::mutex> g(h.mu);
    return h.seen.count(who) ? h.seen[who] : 0;
}

/* an H.264-shaped access unit (SPS, PPS, IDR) of about 3 KB */
static std::string picture(int k)
{
    std::string au("\x00\x00\x00\x01\x67\x42\xc0\x1e\x00\x00\x00\x01\x68\xce\x3c\x80\x00\x00\x00\x01\x65", 21);
    au += char(k + 1);
    while (au.size() < 3000)
        au += char(1 + au.size() % 250);
    return au;
}

static void on_frame(void *user, const char *call, const char *from, const unsigned char *frame, size_t len, uint32_t)
{
    auto *h = static_cast<Heard *>(user);
    if (len < 2 || frame[0] != 'B')
        return;
    std::lock_guard<std::mutex> g(h->mu);
    h->from[from]++;
    h->call = call;
}

static int heard_from(Heard &h, const std::string &who)
{
    std::lock_guard<std::mutex> g(h.mu);
    return h.from.count(who) ? h.from[who] : 0;
}

static bool stream_is(Node &n, const QString &peer, puddle_stream_state want)
{
    puddle_stream st;
    if (puddle_streams_get(puddle_server_streams(n.srv), peer.toUtf8().constData(), &st) != 0)
        return false;
    bool ok = st.state == want;
    puddle_stream_free(&st);
    return ok;
}

static void say(puddle_calls *calls, int n)
{
    for (int k = 0; k < n; k++) {
        unsigned char f[60] = { 'A', (unsigned char)k };
        puddle_voice_send_audio(calls, f, sizeof f);
        usleep(20000);
    }
}

int main(int argc, char **argv)
{
    qputenv("PUDDLE_TEST_NO_AUDIO", "1");
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    Node ann, bob;
    CHECK(ann.up(tmp.path(), "Ann") && bob.up(tmp.path(), "Bob"));
    puddle_calls *calls = puddle_server_calls(ann.srv);
    CHECK(calls != nullptr);
    Heard heard;
    puddle_calls_on_audio(calls, on_frame, &heard);
    puddle_calls_on_video(calls, on_picture, &heard);

    FakePond pond;
    CHECK(pond.start((tmp.path() + "/pond").toStdString()));
    Ponds *ponds = ann.chats->pondList();
    ponds->setInterval(100);
    ponds->start();

    /* ---- joining the Pond, as the window does; its voice channel and who is in it ---- */
    ponds->join(QString("pond://127.0.0.1:%1?invite=aaaaaaaaaaaaaaaaaaaaaaaaaa").arg(pond.port), "main");
    CHECK(until([&] { return static_cast<JsonListModel *>(ponds->list())->rowCount() == 1; }));
    ponds->open(POND_ID);
    auto *channels = static_cast<JsonListModel *>(ponds->channels());
    auto talk = [&] {
        for (const QJsonObject &c : channels->items())
            if (c.value("name").toString() == "talk")
                return c;
        return QJsonObject();
    };
    CHECK(until([&] { return talk().value("kind").toString() == "voice"; }));
    CHECK(until([&] { return talk().value("voice").toArray().size() == 1; }));
    CHECK(talk().value("voice").toArray().at(0).toObject().value("name").toString() == "Ben"); /* by name, as the members say */

    /* ---- making a voice channel, as Server Settings does (the Pond decides who may) ---- */
    CHECK(until([&] { return ponds->canVoice(); }));
    ponds->createChannel("chat", "", "voice");
    CHECK(until([&] {
        std::lock_guard<std::mutex> g(pond.mu);
        return pond.created.find("\"kind\":\"voice\"") != std::string::npos && pond.created.find("\"name\":\"chat\"") != std::string::npos;
    }));
    ponds->createChannel("notes", "");
    CHECK(until([&] {
        std::lock_guard<std::mutex> g(pond.mu);
        return pond.created.find("\"kind\":\"text\"") != std::string::npos;
    }));

    /* ---- only a voice channel: a text one says so ---- */
    QStringList failed;
    QObject::connect(ponds, &Ponds::failed, [&](const QString &why) { failed << why; });
    ponds->joinVoice("1");
    CHECK(until([&] { return !failed.isEmpty(); }) && failed.value(0) == "That isn't a voice channel.");
    CHECK(ponds->voiceState().isEmpty());

    /* ---- joining the voice channel: connecting, then in, and the Pond tells everyone ---- */
    ponds->joinVoice("2");
    CHECK(until([&] { return ponds->voiceState() == "active"; }));
    CHECK(ponds->voicePond() == POND_ID && ponds->voiceChannel() == "2" && ponds->voiceName() == "talk" && ponds->voicePondName() == "Voice Pond");
    CHECK(pond.connected && pond.joins == 1);
    CHECK(until([&] { return talk().value("voice").toArray().size() == 2; })); /* us too, from the Pond's event */

    /* ---- our voice reaches the Pond ---- */
    CHECK(until([&] { say(calls, 5); return pond.frames >= 3; }));

    /* ---- the others' come back, as whose they are ---- */
    CHECK(until([&] { return pond.dc_open.load(); }));
    for (int k = 0; k < 10; k++)
        pond.send_from(1, std::string("B") + char('0' + k), 1000 + k * 960);
    pond.send_from(7, "B?", 5000); /* a slot nobody has told us of yet */
    CHECK(until([&] { return heard_from(heard, BEN) >= 8; }));
    CHECK(until([&] { return heard_from(heard, "slot:7") == 1; }));
    {
        std::lock_guard<std::mutex> g(heard.mu);
        CHECK(heard.call == "voice");
    }

    /* ---- our camera and screen: on, the Pond is told, and they go up their own tracks; off, nothing goes ---- */
    {
        const std::string au = picture(0);
        CHECK(puddle_voice_send_video(calls, (const unsigned char *)au.data(), au.size(), 0) == -1); /* the camera is off */
        ponds->setVoiceCamera(true);
        CHECK(until([&] { return pond.last_state.find("\"camera\":true") != std::string::npos && ponds->voiceCamera(); }));
        ponds->setVoiceScreen(true);
        CHECK(until([&] { return pond.last_state.find("\"screen\":true") != std::string::npos && ponds->voiceScreen(); }));
        CHECK(until([&] {
            puddle_voice_send_video(calls, (const unsigned char *)au.data(), au.size(), 0);
            puddle_voice_send_video(calls, (const unsigned char *)au.data(), au.size(), 1);
            return pond.camera_in >= 2 && pond.screen_in >= 2;
        }));
        ponds->setVoiceCamera(false);
        ponds->setVoiceScreen(false);
        CHECK(until([&] { return pond.last_state.find("\"camera\":false,\"screen\":false") != std::string::npos; }));
        CHECK(puddle_voice_send_video(calls, (const unsigned char *)au.data(), au.size(), 1) == -1);
    }

    /* ---- everyone else's, watched by themselves: each on a track of its own, which it keeps ---- */
    pond.ben(true, true);
    CHECK(until([&] {
        std::lock_guard<std::mutex> g(pond.mu);
        return pond.watch_body.find("\"v1\":\"" + std::string(BEN) + "\"") != std::string::npos &&
               pond.watch_body.find("\"v2\":\"" + std::string(BEN) + "/screen\"") != std::string::npos;
    }));
    CHECK(until([&] {
        pond.show_on(1, picture(1), 9000);
        pond.show_on(2, picture(2), 9000);
        return seen_from(heard, BEN) >= 1 && seen_from(heard, std::string(BEN) + "/screen") >= 1;
    }));
    pond.ben(false, true); /* his camera goes off: v1 is let go, and his screen stays on v2 */
    CHECK(until([&] {
        std::lock_guard<std::mutex> g(pond.mu);
        return pond.watch_body == "{\"tracks\":{\"v1\":null}}";
    }));
    pond.ben(false, false);

    /* ---- muted: nothing of ours goes, and the Pond is told; deafened: nothing comes ---- */
    ponds->setVoiceMuted(true);
    CHECK(until([&] { return pond.last_state.find("\"muted\":true") != std::string::npos; }));
    CHECK(ponds->voiceMuted());
    CHECK(until([&] { return talk().value("voice").toArray().size() == 2 && talk().value("voice").toArray().at(1).toObject().value("muted").toBool(); }));
    {
        unsigned char f[4] = { 'A', 9 };
        CHECK(puddle_voice_send_audio(calls, f, sizeof f) == -1);
    }
    ponds->setVoiceMuted(false);
    ponds->setVoiceDeafened(true);
    CHECK(until([&] { return pond.last_state.find("\"deafened\":true") != std::string::npos && !ponds->voiceMuted(); }));
    int before = heard_from(heard, BEN);
    for (int k = 0; k < 5; k++)
        pond.send_from(1, "Bx", 20000 + k * 960);
    until([] { return false; }, 300);
    CHECK(heard_from(heard, BEN) == before);
    ponds->setVoiceDeafened(false);
    CHECK(until([&] { return !ponds->voiceDeafened(); }));

    /* ---- already in a call: a friend's ring is answered busy, and they are told so; nor can we start one ---- */
    ann.chats->request(bob.link, "hi", "main");
    CHECK(until([&] { return stream_is(bob, ann.uid, PUDDLE_STREAM_PENDING_IN); }, 20000));
    bob.chats->accept(ann.uid, "main");
    CHECK(until([&] { return stream_is(ann, bob.uid, PUDDLE_STREAM_ACTIVE); }, 20000));
    Calls *bobCalls = bob.chats->callModel();
    bobCalls->start();
    bobCalls->call(ann.uid);
    CHECK(until([&] { return bobCalls->error() == "Ann is already in a call."; }));
    CHECK(bobCalls->state().isEmpty());
    Calls *annCalls = ann.chats->callModel();
    annCalls->call(bob.uid);
    CHECK(until([&] { return annCalls->error() == "You're already in a call."; }));
    CHECK(ponds->voiceState() == "active"); /* still in the voice channel */

    /* ---- leaving: the Pond is told; then a call can be made, and while it rings, a voice channel is busy ---- */
    ponds->leaveVoice();
    CHECK(until([&] { return pond.leaves == 1; }));
    CHECK(until([&] { return ponds->voiceState().isEmpty(); }));
    annCalls->call(bob.uid);
    CHECK(until([&] { return annCalls->state() == "calling"; }));
    failed.clear();
    ponds->joinVoice("2");
    CHECK(until([&] { return !failed.isEmpty(); }) && failed.value(0) == "You're already in a call.");
    annCalls->hangUp();
    CHECK(until([&] { return annCalls->state().isEmpty(); }));

    /* ---- the Pond lets us go: we say so, and joining again works ---- */
    pond.close_voice();
    ponds->joinVoice("2");
    CHECK(until([&] { return ponds->voiceState() == "active"; }));
    CHECK(pond.joins == 2);
    {
        std::lock_guard<std::mutex> g(pond.mu);
        pond.refuse_state = true;
    }
    ponds->setVoiceMuted(true);
    CHECK(until([&] { return ponds->voiceState() == "ended" && ponds->voiceReason() == "removed"; }));
    ponds->leaveVoice(); /* dismissed */
    CHECK(until([&] { return ponds->voiceState().isEmpty(); }));

    ponds->stop();
    bobCalls->stop();
    if (failures) {
        fprintf(stderr, "pondvoice_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("pondvoice_test: ok\n");
    return 0;
}

#endif
