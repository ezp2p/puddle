// The app's Backend against the real core, without a UI: the first-run state machine.
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/gui/app/backend.h"

extern "C" {
#include "core/identity.h"
#include "core/phrase.h"
}

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static bool port_open(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    bool ok = connect(fd, (sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

// is `needle` in any file under `dir`?
static bool anywhere(const QString &dir, const QByteArray &needle)
{
    QDirIterator it(dir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile f(it.next());
        if (f.open(QIODevice::ReadOnly) && f.readAll().contains(needle))
            return true;
    }
    return false;
}

static QByteArray chain_of(const QString &dataDir)
{
    QFile f(dataDir + "/identity/chain.jsonl");
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    app.setOrganizationName("sloporation-test");

    QTemporaryDir home;
    CHECK(home.isValid());
    const QString dataDir = home.path() + "/data";
    const int port = 20000 + (int)(getpid() % 20000);
    qputenv("HOME", home.path().toUtf8());
    QDir().mkpath(home.path() + "/.config/puddle");
    auto writeConfig = [&](const QString &text) {
        QFile f(home.path() + "/.config/puddle/puddle.yaml");
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(text.toUtf8());
    };
    writeConfig(QString("address: 127.0.0.1\nport: %1\ndata_dir: %2\nprofiles:\n").arg(port).arg(dataDir));

    QString firstPhrase, secondPhrase, thirdPhrase;

    {   // ---- a brand new install ----
        Backend b;
        CHECK(b.needsSetup() && !b.setupRedo() && !b.running() && b.error().isEmpty());
        CHECK(!port_open(port));                              // nothing is listening while setting up

        CHECK(!b.finishSetup());                              // can't finish what was never made
        CHECK(b.needsSetup() && !b.running() && !b.error().isEmpty());

        firstPhrase = b.createIdentity();
        CHECK(puddle_phrase_parse(firstPhrase.toUtf8().constData(), nullptr) == PUDDLE_PHRASE_OK);
        CHECK(b.error().isEmpty());
        CHECK(puddle_identity_exists(dataDir.toUtf8()) && puddle_identity_unconfirmed(dataDir.toUtf8()));
        CHECK(b.needsSetup() && !b.running() && !port_open(port)); // made, but not yet confirmed: still off

        secondPhrase = b.createIdentity();                    // "Make my identity" again: a fresh one replaces it
        CHECK(!secondPhrase.isEmpty() && secondPhrase != firstPhrase);
        CHECK(puddle_identity_unconfirmed(dataDir.toUtf8()));
    }   // the owner quits here, without confirming

    {   // ---- the next launch ----
        Backend b;
        CHECK(b.needsSetup() && b.setupRedo());                // it knows setup was left unfinished
        CHECK(!b.running() && !port_open(port));               // and did not start on an identity nobody confirmed
        CHECK(puddle_identity_unconfirmed(dataDir.toUtf8()));

        thirdPhrase = b.createIdentity();
        CHECK(!thirdPhrase.isEmpty() && thirdPhrase != secondPhrase && thirdPhrase != firstPhrase);

        CHECK(b.finishSetup());                                // "I have written it down" -> Continue
        CHECK(!b.needsSetup() && !b.setupRedo() && b.running() && b.error().isEmpty());
        CHECK(b.listenUrl() == QString("https://127.0.0.1:%1").arg(port));
        CHECK(!puddle_identity_unconfirmed(dataDir.toUtf8()));
        CHECK(port_open(port));
        CHECK(b.createIdentity().isEmpty());                   // nothing more to set up

        QCoreApplication::processEvents();
        QString log;
        QAbstractItemModel *m = qobject_cast<QAbstractItemModel *>(b.log());
        for (int i = 0; m && i < m->rowCount(); i++)
            log += m->data(m->index(i, 0)).toString() + "\n";
        CHECK(log.contains("identity "));                      // the log shows the id...
        for (const QString &p : { firstPhrase, secondPhrase, thirdPhrase })
            CHECK(!log.contains(p, Qt::CaseInsensitive));      // ...and never a phrase
    }
    CHECK(!port_open(port));                                   // quitting stopped the server

    QByteArray chainBefore = chain_of(dataDir);
    {   // ---- every launch after that ----
        Backend b;
        CHECK(!b.needsSetup() && b.running() && b.error().isEmpty() && port_open(port));
        CHECK(b.createIdentity().isEmpty() && !b.finishSetup()); // can't be tricked into making a second one
    }
    CHECK(chain_of(dataDir) == chainBefore);                   // the identity is untouched

    // no phrase, in any spelling, on disk anywhere under the app's home
    for (const QString &p : { firstPhrase, secondPhrase, thirdPhrase }) {
        QString bare = p;
        bare.remove('-');
        CHECK(!anywhere(home.path(), p.toUtf8()));
        CHECK(!anywhere(home.path(), bare.toUtf8()));
        CHECK(!anywhere(home.path(), bare.toLower().toUtf8()));
    }

    {   // ---- theme.yaml: plain `key: value` lines ----
        const QVariantMap t = Backend::parseTheme("# my theme\ncanvas: \"#101418\"\n card: '#1b2026'  \naccent: #7aa2f7 # blue\ncardRadius: 28\nbroken line\nempty:\n");
        CHECK(t.size() == 4);
        CHECK(t.value("canvas") == "#101418" && t.value("card") == "#1b2026" && t.value("cardRadius") == "28");
        CHECK(t.value("accent") == "#7aa2f7");                // an unquoted # right after the colour is the colour; " #" starts a comment
        CHECK(Backend::parseTheme("").isEmpty());
    }

    {   // ---- something wrong with the settings is an error to show, not a setup screen ----
        writeConfig("port: nope\n");
        Backend b;
        CHECK(!b.needsSetup() && !b.running() && b.error().contains("port"));
    }

    return failures ? 1 : 0;
}
