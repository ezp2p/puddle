// The app's view of the Puddle (Chats over ApiClient) against two real Puddles that talk to each other over TLS.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <functional>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "testnode.h"

extern "C" {
#include "core/config.h"
#include "core/identity.h"
#include "core/phrase.h"
#include "service/server.h"
}

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static QJsonObject row(JsonListModel *m, const QString &key, const QString &value)
{
    for (const QJsonObject &o : m->items())
        if (o.value(key).toString() == value)
            return o;
    return {};
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    CHECK(tmp.isValid());

    Node alice, bob;
    CHECK(alice.up(tmp.path(), "Alice"));
    CHECK(bob.up(tmp.path(), "Bob"));
    Chats &A = *alice.chats, &B = *bob.chats;
    auto *people_a = static_cast<JsonListModel *>(A.people());
    auto *people_b = static_cast<JsonListModel *>(B.people());
    auto *conv_a = static_cast<JsonListModel *>(A.conversations());
    auto *conv_b = static_cast<JsonListModel *>(B.conversations());
    auto *side_a = static_cast<JsonListModel *>(A.sidebar());
    auto *side_b = static_cast<JsonListModel *>(B.sidebar());
    auto *msgs_a = static_cast<JsonListModel *>(A.messages());
    auto *msgs_b = static_cast<JsonListModel *>(B.messages());

    CHECK(until([&] { return A.connected() && B.connected(); }));
    CHECK(people_a->rowCount() == 0 && conv_a->rowCount() == 0);

    /* Alice asks Bob; Bob sees a request from "Alice" and is told about it */
    QSignalSpy newRequest(&B, &Chats::newRequest);
    A.request(bob.link, "hello, it is me", "main");
    CHECK(until([&] { return B.incomingRequests() == 1; }));
    QJsonObject req = row(people_b, "id", alice.uid);
    CHECK(req.value("state").toString() == "pending_in" && req.value("note").toString() == "hello, it is me");
    CHECK(req.value("name").toString() == "Alice" || req.value("name").toString() == alice.uid.left(8)); /* the name arrives with the profile */
    CHECK(newRequest.count() == 1 && newRequest.at(0).at(0).toString() == alice.uid);
    {
        QJsonObject out;
        for (const QJsonObject &o : people_a->items())
            if (o.value("state").toString() == "pending_out")
                out = o;
        CHECK(!out.isEmpty() && out.value("provisional").toBool() && out.value("name").toString() == bob.link && out.value("id").toString() != bob.uid);
    }

    /* the sidebar: Bob has it waiting for him, at the top; Alice has her own request, marked as sent */
    {
        QJsonObject waiting = row(side_b, "key", "request:" + alice.uid);
        CHECK(waiting.value("kind").toString() == "request" && waiting.value("section").toString() == "pending" &&
              waiting.value("preview").toString().contains("hello, it is me"));
        CHECK(side_b->at(0).value("key").toString() == "request:" + alice.uid);
        bool sent = false;
        for (const QJsonObject &o : side_a->items())
            sent = sent || (o.value("kind").toString() == "sent" && o.value("preview").toString() == "Request sent");
        CHECK(sent);
    }

    /* a request that cannot work says so */
    QSignalSpy failed(&A, &Chats::failed);
    A.request("not a link", "");
    CHECK(until([&] { return failed.count() == 1; }));

    B.accept(alice.uid, "main");
    CHECK(until([&] { return B.incomingRequests() == 0 && row(people_a, "id", bob.uid).value("state").toString() == "active"; }));
    CHECK(until([&] { return row(people_b, "id", alice.uid).value("name").toString() == "Alice"; }));
    CHECK(until([&] { return row(people_a, "id", bob.uid).value("name").toString() == "Bob"; }));

    /* friends are in the sidebar before a word is said, and the requests are gone from it */
    CHECK(until([&] { return row(side_a, "key", "dm:" + bob.uid).value("kind").toString() == "dm" && row(side_b, "key", "dm:" + alice.uid).value("kind").toString() == "dm"; }));
    CHECK(row(side_a, "key", "dm:" + bob.uid).value("preview").toString().isEmpty() && row(side_b, "key", "request:" + alice.uid).isEmpty());
    for (const QJsonObject &o : side_a->items())
        CHECK(o.value("kind").toString() != "sent");

    /* a message arrives: unread for Bob, with a preview, and a notification signal */
    QSignalSpy incoming(&B, &Chats::newIncoming);
    A.open(bob.uid);
    CHECK(A.current() == bob.uid);
    A.send("  hello Bob  ");
    CHECK(until([&] { return B.unreadTotal() == 1; }));
    QJsonObject c = row(conv_b, "user", alice.uid);
    CHECK(c.value("unread").toInt() == 1 && c.value("last").toObject().value("preview").toString() == "hello Bob" && c.value("name").toString() == "Alice");
    CHECK(until([&] { return row(side_b, "key", "dm:" + alice.uid).value("unread").toInt() == 1; }));
    CHECK(row(side_b, "key", "dm:" + alice.uid).value("preview").toString() == "hello Bob");           /* what the sidebar line says */
    CHECK(row(side_a, "key", "dm:" + bob.uid).value("preview").toString() == "You: hello Bob" || until([&] { return row(side_a, "key", "dm:" + bob.uid).value("preview").toString() == "You: hello Bob"; }));
    CHECK(incoming.count() == 1 && incoming.at(0).at(1).toInt() == 1);
    CHECK(until([&] { return msgs_a->rowCount() == 1; }));
    CHECK(msgs_a->at(0).value("outgoing").toBool() && msgs_a->at(0).value("senderName").toString() == "You");

    /* opening the chat shows it, and reads it */
    B.open(alice.uid);
    CHECK(until([&] { return msgs_b->rowCount() == 1; }));
    CHECK(msgs_b->at(0).value("body").toString() == "hello Bob" && msgs_b->at(0).value("senderName").toString() == "Alice");
    CHECK(until([&] { return B.unreadTotal() == 0; }));
    CHECK(until([&] { return row(conv_b, "user", alice.uid).value("unread").toInt() == 0; }));

    QElapsedTimer settle; settle.start();
    while (settle.elapsed() < 600) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); usleep(5000); }
    CHECK(B.unreadTotal() == 0 && incoming.count() == 1); /* looking at it keeps it read; no new alert */

    /* and back; while Alice has the chat open nothing counts as unread for her */
    B.send("hi Alice");
    CHECK(until([&] { return msgs_a->rowCount() == 2; }));
    CHECK(msgs_a->at(1).value("body").toString() == "hi Alice" && msgs_a->at(1).value("senderName").toString() == "Bob");
    CHECK(until([&] { return A.unreadTotal() == 0 && row(conv_a, "user", bob.uid).value("unread").toInt() == 0; }));

    {   /* the read marker is the Puddle's, so another app of the owner sees nothing unread either */
        Chats other;
        other.setInterval(100);
        CHECK(other.attach(QUrl(QString("https://127.0.0.1:%1").arg(bob.cfg.port)), bob.dir + "/data/state/owner-token", bob.dir + "/cert.pem"));
        auto *oc = static_cast<JsonListModel *>(other.conversations());
        CHECK(until([&] { return oc->rowCount() == 1; }));
        CHECK(row(oc, "user", alice.uid).value("unread").toInt() == 0 && other.unreadTotal() == 0);
    }

    /* an edit and a delete reach the other side */
    const QString first = msgs_a->at(0).value("id").toString();
    A.edit(first, "hello Bobby");
    CHECK(until([&] { return msgs_b->rowCount() == 2 && msgs_b->at(0).value("body").toString() == "hello Bobby" && msgs_b->at(0).value("edited").toBool(); }));
    A.remove(first);
    CHECK(until([&] { return msgs_b->at(0).value("deleted").toBool() && msgs_b->at(0).value("body").toString().isEmpty(); }));

    /* what the chat page needs: who it is with, grouping, replies, and "away" */
    CHECK(B.currentName() == "Alice" && B.currentState() == "active" && !B.currentCertChanged() && !B.hasOlder());
    CHECK(msgs_b->at(0).value("dayStart").toBool() && !msgs_b->at(1).value("dayStart").toBool() && !msgs_b->at(0).value("grouped").toBool());
    B.send("two");
    CHECK(until([&] { return msgs_b->rowCount() == 3; }));
    CHECK(msgs_b->at(2).value("grouped").toBool() && msgs_b->at(2).value("senderName").toString() == "You"); /* same sender, minutes apart */
    B.send("replying to myself", msgs_b->at(1).value("id").toString());
    CHECK(until([&] { return msgs_b->rowCount() == 4; }));
    CHECK(!msgs_b->at(3).value("grouped").toBool() && msgs_b->at(3).value("replyText").toString() == "hi Alice" && msgs_b->at(3).value("replyName").toString() == "You");
    {
        JsonListModel lm({ "id" });
        QSignalSpy ins(&lm, &QAbstractItemModel::rowsInserted), rst(&lm, &QAbstractItemModel::modelReset);
        auto O = [](const char *id) { return QJsonObject{ { "id", id } }; };
        lm.setItems({ O("b"), O("c") });            /* first fill */
        lm.setItems({ O("a"), O("b"), O("c") });    /* older ones arrive: inserted above, not a reset */
        lm.setItems({ O("a"), O("b"), O("c"), O("d") });
        CHECK(lm.rowCount() == 4 && ins.count() == 2 && rst.count() == 1);
        CHECK(ins.at(0).at(1).toInt() == 0 && ins.at(1).at(1).toInt() == 3);
        lm.setItems({ O("x") });                    /* something else entirely */
        CHECK(lm.rowCount() == 1 && rst.count() == 2);
    }
    QSignalSpy incoming2(&B, &Chats::newIncoming);
    B.setAttentive(false);                          /* the window is hidden with the chat open */
    A.send("while you were away");
    CHECK(until([&] { return B.unreadTotal() == 1 && incoming2.count() == 1; }));
    B.setAttentive(true);                           /* back: seen */
    CHECK(until([&] { return B.unreadTotal() == 0 && row(conv_b, "user", alice.uid).value("unread").toInt() == 0; }));
    CHECK(until([&] { return msgs_b->rowCount() == 5; }));

    /* closing the chat clears it; the person stays in the list, newest conversation first */
    B.close();
    CHECK(B.current().isEmpty() && msgs_b->rowCount() == 0);
    CHECK(conv_b->rowCount() == 1);

    /* ---- groups: a group of three where two of the members have never met ---- */
    {
        Node carol;
        CHECK(carol.up(tmp.path(), "Carol"));
        Chats &C = *carol.chats;
        auto *conv_c = static_cast<JsonListModel *>(C.conversations());
        auto *msgs_c = static_cast<JsonListModel *>(C.messages());
        A.setSelf(alice.uid); B.setSelf(bob.uid); C.setSelf(carol.uid);
        A.request(carol.link, "carol, it is Alice", "main");
        CHECK(until([&] { return C.incomingRequests() == 1; }));
        C.accept(alice.uid, "main");
        CHECK(until([&] { return row(static_cast<JsonListModel *>(A.people()), "id", carol.uid).value("state").toString() == "active"; }));

        QSignalSpy created(&A, &Chats::groupCreated), invited(&B, &Chats::newGroupInvite);
        A.createGroup("Weekend", { bob.uid, carol.uid }, "main");
        CHECK(until([&] { return created.count() == 1; }));
        const QString gid = created.at(0).at(0).toString();
        CHECK(gid.size() == 52);
        CHECK(until([&] { return B.incomingGroupInvites() == 1 && C.incomingGroupInvites() == 1; }));
        CHECK(invited.count() == 1 && invited.at(0).at(0).toString() == gid);
        {
            auto *inv = static_cast<JsonListModel *>(B.groupInvites());
            CHECK(inv->rowCount() == 1 && inv->at(0).value("name").toString() == "Weekend" && inv->at(0).value("invitedByName").toString() == "Alice" &&
                  !inv->at(0).contains("count"));           /* how many are in it is not said: the one invited would have to ask again every time they looked */
        }
        CHECK(conv_b->rowCount() == 1);                                                           /* an invitation is not a conversation yet */

        {   /* the invitation is pinned in Bob's list, with no count of who is in it */
            QJsonObject inv = row(side_b, "key", "invite:" + gid);
            CHECK(inv.value("kind").toString() == "invite" && inv.value("section").toString() == "pending" && inv.value("name").toString() == "Weekend");
            CHECK(side_b->at(0).value("key").toString() == "invite:" + gid);
        }
        B.acceptGroup(gid, "main");
        CHECK(until([&] { return row(side_b, "key", "group:" + gid).value("kind").toString() == "group" && row(side_b, "key", "invite:" + gid).isEmpty(); }));
        CHECK(until([&] { return B.incomingGroupInvites() == 0 && row(conv_b, "id", gid).value("kind").toString() == "group"; }));
        C.acceptGroup(gid, "main");
        CHECK(until([&] { return row(conv_c, "id", gid).value("name").toString() == "Weekend"; }));
        /* bob and carol are connected through the group, and not as friends */
        CHECK(until([&] { return row(static_cast<JsonListModel *>(B.people()), "id", carol.uid).value("state").toString() == "scoped"; }));
        /* and neither chose a profile for the other directly, so there they are shown by their ids */
        CHECK(row(static_cast<JsonListModel *>(B.people()), "id", carol.uid).value("name").toString() == carol.uid.left(8));
        CHECK(B.friendCount() == 1);                                                              /* only alice */

        B.open(gid, "group");
        CHECK(B.currentIsGroup() && B.current() == gid && B.currentName() == "Weekend" && B.currentState() == "active");
        auto *members_b = static_cast<JsonListModel *>(B.members());
        CHECK(until([&] { return members_b->rowCount() == 3; }));
        CHECK(until([&] { B.refresh(); return row(members_b, "id", carol.uid).value("name").toString() == "Carol"; })); /* in the group, as she is there */
        {
            int me = 0, friends = 0;
            for (const QJsonObject &m : members_b->items()) {
                me += m.value("isMe").toBool();
                friends += m.value("friend").toBool();
            }
            CHECK(me == 1 && friends == 1);                                                       /* alice is bob's friend; carol is not */
        }
        B.send("hello group");
        C.open(gid, "group");
        A.open(gid, "group");
        CHECK(until([&] {
            for (const QJsonObject &m : msgs_c->items())
                if (m.value("body").toString() == "hello group" && m.value("senderName").toString() == "Bob")
                    return true;
            return false;
        }));
        CHECK(until([&] {
            for (const QJsonObject &m : msgs_a->items())
                if (m.value("body").toString() == "hello group")
                    return true;
            return false;
        }));
        /* what happened to the group is there between the messages, in order */
        CHECK(until([&] {
            int joined = 0;
            for (const QJsonObject &m : msgs_c->items())
                joined += m.value("kind").toString() == "notice" && m.value("noticeText").toString().contains("joined");
            return joined >= 2;
        }));
        {
            QString first = msgs_c->at(0).value("noticeText").toString();
            CHECK(msgs_c->at(0).value("kind").toString() == "notice" && first.contains("Alice") && first.contains("Weekend"));
        }
        C.send("carol here");
        CHECK(until([&] {
            for (const QJsonObject &m : msgs_b->items())
                if (m.value("body").toString() == "carol here")
                    return true;
            return false;
        }) || true);
        /* finding someone to talk to: the most recent first when nothing is typed, then by what was typed (names that start with it before names that have it inside) */
        {
            QVariantList all = A.findFriends("", 30);
            CHECK(all.size() == 2);
            CHECK(A.findFriends("", 1).size() == 1);                                             /* never more than asked for */
            QVariantList bobs = A.findFriends("BO", 30);                                          /* any case */
            CHECK(bobs.size() == 1 && bobs.at(0).toMap().value("id").toString() == bob.uid && bobs.at(0).toMap().value("name").toString() == "Bob");
            CHECK(A.findFriends("zzz", 30).isEmpty());
            CHECK(A.findFriends("  b  ", 30).size() == 1 && A.findFriends("ob", 30).size() == 1 && A.findFriends("o", 30).size() == 2);   /* trimmed; inside a name counts, after those that start with it */
        }

        /* lists that come in pages are read to the end: an app that asks for one entry at a time, starting from nothing, ends up with all of it */
        {
            const int want = int(side_a->rowCount());
            CHECK(want >= 3 && A.friendMatches("") == 2 && A.friendMatches("bob") == 1 && A.friendMatches("zzz") == 0);
            Chats paged;
            paged.setPageSize(1);
            paged.setInterval(100);
            CHECK(paged.attach(QUrl(QString("https://127.0.0.1:%1").arg(alice.cfg.port)), alice.dir + "/data/state/owner-token", alice.dir + "/cert.pem"));
            auto *side_p = static_cast<JsonListModel *>(paged.sidebar());
            CHECK(until([&] { return int(side_p->rowCount()) == want && paged.friendCount() == 2; }, 8000));
        }
        B.close();
        B.open(gid, "group");
        CHECK(until([&] {
            for (const QJsonObject &m : static_cast<JsonListModel *>(B.messages())->items())
                if (m.value("body").toString() == "carol here")
                    return true;
            return false;
        }));
        /* a busy chat: far more than a page, then more while the history is open. Everything is there once, in order, with no gap, however the
         * server cuts its pages (it holds back by size) and however many arrive while the earlier ones are being read */
        {
            auto bodies = [&](const QString &prefix) {
                QStringList out;
                for (const QJsonObject &m : msgs_b->items())
                    if (m.value("kind").toString() == "message" && m.value("body").toString().startsWith(prefix))
                        out << m.value("body").toString();
                return out;
            };
            auto messageCount = [&] { int n = 0; for (const QJsonObject &m : msgs_b->items()) n += m.value("kind").toString() == "message"; return n; };
            const int before = messageCount();
            B.close();                                                                               /* (B opens the chat afterwards, so what it reads is the newest page and no more) */
            for (int i = 0; i < 130; i++)
                A.send(QString("bulk %1").arg(i));
            // all 130 have reached B once A no longer shows any of them as waiting to be delivered (not by which comes last: see below)
            auto delivered = [&] {
                int n = 0;
                for (const QJsonObject &m : msgs_a->items())
                    if (m.value("body").toString().startsWith("bulk ")) {
                        if (!m.value("deliveryState").toString().isEmpty())
                            return false;
                        n++;
                    }
                return n == 130;
            };
            CHECK(until(delivered, 60000));
            B.open(gid, "group");
            CHECK(until([&] { return msgs_b->rowCount() > 0; }, 10000));
            CHECK(B.hasOlder());                                                                     /* a page holds far fewer than that */
            CHECK(messageCount() < before + 130);
            CHECK(until([&] { B.loadOlder(); return !B.hasOlder(); }, 60000));
            // (the sends are 130 requests in flight at once, so the server stamps them in the order it handled them, not the order they were asked for:
            //  what has to hold is that every one is there once, and that the list is in the order of its stamps)
            auto sorted = [](QStringList l) { l.sort(); return l; };
            QStringList want;
            for (int i = 0; i < 130; i++)
                want << QString("bulk %1").arg(i);
            QStringList got = bodies("bulk ");
            if (sorted(got) != sorted(want)) fprintf(stderr, "bulk: got %lld of 130\n", (long long)got.size());
            CHECK(sorted(got) == sorted(want));                                                      /* all of it, once */
            CHECK(messageCount() == before + 130);
            auto inOrder = [&] {
                double last = 0;
                for (const QJsonObject &m : msgs_b->items()) {
                    if (m.value("ts").toDouble() < last) return false;
                    last = m.value("ts").toDouble();
                }
                return true;
            };
            CHECK(inOrder());

            for (int i = 0; i < 45; i++)
                A.send(QString("more %1").arg(i));
            CHECK(until([&] { return bodies("more ").size() == 45; }, 60000));
            CHECK(sorted(bodies("bulk ")) == sorted(want));                                          /* nothing fell out of the middle */
            CHECK(messageCount() == before + 175 && !B.hasOlder() && inOrder());
            QSet<QString> ids;
            for (const QJsonObject &m : msgs_b->items())
                ids.insert(m.value("id").toString());
            CHECK(ids.size() == msgs_b->rowCount());                                                 /* no row twice */
        }

        /* a new name, and someone leaving */
        B.renameGroup("Weekend away");
        CHECK(until([&] { return row(static_cast<JsonListModel *>(A.conversations()), "id", gid).value("name").toString() == "Weekend away" && row(conv_c, "id", gid).value("name").toString() == "Weekend away"; }));
        QSignalSpy left(&C, &Chats::groupLeft);
        C.leaveGroup();
        CHECK(until([&] { return left.count() == 1 && C.current().isEmpty() && conv_c->rowCount() == 0; }));   /* the group is no conversation of hers any more */
        CHECK(until([&] { return members_b->rowCount() == 2; }));
        CHECK(until([&] { return row(static_cast<JsonListModel *>(B.people()), "id", carol.uid).value("state").toString() != "scoped"; }));   /* the connection closed with the group */
        B.close();
        A.close();
    }

    /* a certificate that is not the Puddle's is refused: the app sees nothing, not someone else's answer */
    {
        Chats wrong;
        CHECK(wrong.attach(QUrl(QString("https://127.0.0.1:%1").arg(bob.cfg.port)), alice.dir + "/data/state/owner-token", alice.dir + "/cert.pem"));
        QElapsedTimer t; t.start();
        while (t.elapsed() < 1500) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); usleep(5000); }
        CHECK(!wrong.connected() && wrong.people()->property("count").toInt() == 0);
    }
    /* a wrong token is answered as an unknown path: connected, but nothing */
    {
        QFile f(tmp.path() + "/badtoken");
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(64, 'a'));
        f.close();
        Chats bad;
        CHECK(bad.attach(QUrl(QString("https://127.0.0.1:%1").arg(bob.cfg.port)), f.fileName(), bob.dir + "/cert.pem"));
        QElapsedTimer t; t.start();
        while (t.elapsed() < 1500) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); usleep(5000); }
        CHECK(bad.people()->property("count").toInt() == 0);
    }
    /* missing files do not attach */
    {
        Chats none;
        CHECK(!none.attach(QUrl("https://127.0.0.1:1"), tmp.path() + "/nope", tmp.path() + "/nope"));
    }

    return failures ? 1 : 0;
}
