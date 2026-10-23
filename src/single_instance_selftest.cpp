#include "single_instance_selftest.h"

#include "single_instance.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>

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

    QStringList got;
    QEventLoop loop;
    QObject::connect(&first, &SingleInstance::argumentsReceived, &loop,
                     [&got, &loop](const QStringList &args) {
                         got = args;
                         loop.quit();
                     });

    // A timeout, so a failure is a FAIL and not a test that never ends.
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);

    // --- the second process: it must NOT own the name, and its arguments must
    // arrive at the first one, unchanged (argv[0] included).
    const QStringList sent{QStringLiteral("C:/x/nevertube_qt2.exe"),
                           QStringLiteral("--download"),
                           QStringLiteral("dQw4w9WgXcQ"),
                           QStringLiteral("1080")};
    {
        SingleInstance second(name);
        check(!second.start(sent), "el segundo NO toma el nombre");
    }
    guard.start(3000);
    if (got.isEmpty())
        loop.exec();
    check(got == sent, "los argumentos llegan al primero, enteros");

    // --- and the first one KEEPS answering: a connection that closes must not
    // take the listener down (that is what makes the second link of the day
    // work).
    got.clear();
    {
        SingleInstance third(name);
        check(!third.start(QStringList{QStringLiteral("C:/x/nevertube_qt2.exe"),
                                       QStringLiteral("--search"),
                                       QStringLiteral("hola")}),
              "el tercero tampoco toma el nombre");
    }
    guard.start(3000);
    if (got.isEmpty())
        loop.exec();
    check(got.size() == 3 && got.at(1) == QLatin1String("--search"),
          "el primero sigue contestando");

    return ok;
}
