#include "single_instance_selftest.h"

#include "single_instance.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QProcess>
#include <QTimer>

#include <cstdio>

namespace {

// The result of one run of the "second instance".
struct Second
{
    bool started = false;
    bool finished = false;
    int exitCode = -1;
};

} // namespace

// WHY THE SECOND INSTANCE IS A REAL PROCESS AND NOT A SECOND SingleInstance
// OBJECT OF THIS ONE.
//
// The handover is what a second process does: it connects, writes and exits.
// Done from this process, the client BLOCKS the event loop while it waits for
// the ack, and the loop that has to answer is this same one, so nothing is
// exercised at all: the test measured an artificial case, and on Windows that
// case failed while the pipe was working (the first instance was listening the
// whole time). With a child process the loop runs, the server accepts, reads,
// answers and closes, exactly like with the app open.
//
// The child is this same binary with `--handover <name> <argv...>` (see
// main.cpp): exit 0 = the arguments were handed over, 2 = it took the name.
bool singleInstanceSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::printf("  SINGLE-INSTANCE %s: %s\n", cond ? "OK" : "FAIL", what);
        if (!cond)
            ok = false;
    };

    // A name of its own: a running app (or a second run of the tests) must not
    // be disturbed, and this test must not be refused because the app is open.
    const QString name = QStringLiteral("nevertube_qt2-selftest-%1")
                             .arg(QCoreApplication::applicationPid());

    SingleInstance first(name);
    check(first.start(QStringList()), "el primero toma el nombre");

    // Every event that arrives, and the payload of the last one. An EMPTY
    // payload is a real event (the app opened with no arguments, which brings
    // the window to the front), so the count is kept apart from the list.
    int events = 0;
    QStringList got;
    QObject::connect(&first, &SingleInstance::argumentsReceived,
                     [&events, &got](const QStringList &args) {
                         ++events;
                         got = args;
                     });

    // Runs the second process and waits for it with the event loop RUNNING.
    auto runSecond = [&name](const QStringList &payload) {
        Second second;
        QStringList childArgs{QStringLiteral("--handover"), name};
        childArgs += payload;

        QProcess proc;
        QEventLoop loop;
        QTimer guard;
        guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(
            &proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            &loop, [&loop](int, QProcess::ExitStatus) { loop.quit(); });

        proc.start(QCoreApplication::applicationFilePath(), childArgs);
        second.started = proc.waitForStarted(5000);
        if (!second.started) {
            std::printf("  (no arranco el segundo proceso: %s)\n",
                        qPrintable(proc.errorString()));
            return second;
        }
        // A timeout, so a failure is a FAIL and not a test that never ends.
        guard.start(20000);
        if (proc.state() != QProcess::NotRunning)
            loop.exec();
        second.finished = proc.state() == QProcess::NotRunning;
        second.exitCode = proc.exitCode();
        return second;
    };

    // --- the second process: it must NOT own the name, and its arguments must
    // arrive at the first one, unchanged (argv[0] included).
    const QStringList sent{QStringLiteral("C:/x/nevertube_qt2.exe"),
                           QStringLiteral("--download"),
                           QStringLiteral("dQw4w9WgXcQ"),
                           QStringLiteral("1080")};
    const Second second = runSecond(sent);
    check(second.finished, "el segundo termina (no se queda esperando)");
    check(second.exitCode == 0, "el segundo NO toma el nombre");
    check(got == sent, "los argumentos llegan al primero, enteros");
    check(events == 1, "el primero recibe UN aviso (no dos)");

    // --- and the first one KEEPS answering: a connection that closes must not
    // take the listener down (that is what makes the second link of the day
    // work).
    got.clear();
    const int eventsBefore = events;
    const Second third = runSecond(QStringList{
        QStringLiteral("C:/x/nevertube_qt2.exe"),
        QStringLiteral("--search"), QStringLiteral("hola")});
    check(third.exitCode == 0, "el tercero tampoco toma el nombre");
    check(events == eventsBefore + 1 && got.size() == 3
              && got.at(1) == QLatin1String("--search"),
          "el primero sigue contestando");

    // --- the app opened with NO arguments (the icon of the menu): the second
    // process still hands over, and what that means is "come to the front".
    got.clear();
    const int beforeBare = events;
    const Second bare = runSecond(QStringList());
    check(bare.exitCode == 0, "abrirla otra vez sin argumentos no abre otra ventana");
    check(events == beforeBare + 1 && got.isEmpty(),
          "sin argumentos igual avisa (la ventana viene al frente)");

    return ok;
}
