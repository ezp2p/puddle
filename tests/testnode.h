// Shared by the app tests: a real Puddle (server thread, identity, its own Chats) in a temporary folder.
#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <functional>
#include <memory>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/gui/app/chats.h"

extern "C" {
#include "core/config.h"
#include "core/identity.h"
#include "core/phrase.h"
#include "service/server.h"
}

inline bool until(const std::function<bool()> &cond, int ms = 15000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(5000);
    }
    return true;
}

inline unsigned free_port()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (sockaddr *)&a, sizeof a);
    socklen_t l = sizeof a;
    getsockname(fd, (sockaddr *)&a, &l);
    close(fd);
    return ntohs(a.sin_port);
}

struct Node {
    QString dir, name;
    puddle_config cfg{};
    puddle_server *srv = nullptr;
    std::thread th;
    QString uid, link;
    std::unique_ptr<Chats> chats;

    bool up(const QString &root, const QString &who)
    {
        name = who;
        dir = root + "/" + who;
        QDir().mkpath(dir);
        QFile pf(dir + "/profiles.yaml");
        if (!pf.open(QIODevice::WriteOnly)) return false;
        pf.write(QString("profiles:\n  - id: main\n    display_name: %1\n").arg(who).toUtf8());
        pf.close();
        snprintf(cfg.dir, sizeof cfg.dir, "%s", dir.toUtf8().constData());
        snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s/data", dir.toUtf8().constData());
        snprintf(cfg.address, sizeof cfg.address, "127.0.0.1");
        cfg.port = (uint16_t)free_port();
        snprintf(cfg.profile_files[0], PUDDLE_PATH_MAX, "%s", pf.fileName().toUtf8().constData());
        cfg.nprofile_files = 1;
        char err[300], phrase[PUDDLE_PHRASE_LEN + 1], id[PUDDLE_USER_ID_LEN + 1];
        puddle_identity *ident = puddle_identity_create(cfg.data_dir, phrase, err, sizeof err);
        if (!ident)
            return false;
        puddle_identity_id(ident, id);
        puddle_identity_close(ident);
        puddle_identity_confirm(cfg.data_dir);
        uid = id;
        srv = puddle_server_create(&cfg, err, sizeof err);
        if (!srv) {
            fprintf(stderr, "%s: %s\n", who.toUtf8().constData(), err);
            return false;
        }
        th = std::thread([s = srv] { puddle_server_run(s); });
        link = QString("127.0.0.1:%1").arg(cfg.port);
        chats = std::make_unique<Chats>();
        chats->setInterval(100);
        return chats->attach(QUrl(QString("https://127.0.0.1:%1").arg(cfg.port)), dir + "/data/state/owner-token", dir + "/cert.pem");
    }

    // Take the Puddle off the network, and bring it back (same identity, certificate and port).
    void stop()
    {
        if (!srv)
            return;
        puddle_server_stop(srv);
        th.join();
        puddle_server_destroy(srv);
        srv = nullptr;
    }
    bool start()
    {
        char err[300];
        srv = puddle_server_create(&cfg, err, sizeof err);
        if (!srv)
            return false;
        th = std::thread([s = srv] { puddle_server_run(s); });
        return true;
    }

    ~Node()
    {
        chats.reset();
        stop();
    }
};

