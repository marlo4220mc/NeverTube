#include "i18n.h"

#include <QAbstractButton>
#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <cstdio>

#include "ui/scan.h"

namespace nt {

namespace {

// Base (source) language of the app: the one with the complete key set and the
// fallback for any missing translation.
const char *const kBaseCode = "es";

// Default UI language of a fresh install (no "ui_lang" saved and no match with
// the system language): English. Kept apart from the base on purpose.
const char *const kDefaultCode = "en";

enum BindingKind { KText, KTooltip, KPlaceholder, KAction, KAccessibleName };

struct Binding
{
    QPointer<QObject> obj;
    QString key;
    BindingKind kind;
};

QList<Binding> g_bindings;

QString g_lang;                 // active code
QHash<QString, QString> g_cur;  // active translations
QHash<QString, QString> g_base; // base translations
bool g_inited = false;

QString fileFor(const QString &code)
{
    return QStringLiteral(":/localization/") + code + QStringLiteral(".json");
}

// Loads :/localization/<code>.json. Fills `out` with the strings, `name` with
// the native name. Returns false if the file is missing or malformed.
bool loadMap(const QString &code, QHash<QString, QString> *out,
             QString *name = nullptr, QStringList *dupKeys = nullptr)
{
    QFile f(fileFor(code));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray raw = f.readAll();

    // Duplicated keys: QJsonObject silently keeps the last one, so the raw text
    // is scanned too (one key per line in these files). A duplicated key would
    // otherwise be a silent hole in the translation.
    if (dupKeys) {
        QSet<QString> seen;
        for (const QString &line : QString::fromUtf8(raw).split(QLatin1Char('\n'))) {
            const QString t = line.trimmed();
            if (!t.startsWith(QLatin1Char('"')))
                continue;
            const int close = t.indexOf(QLatin1Char('"'), 1);
            if (close < 2)
                continue;
            const QString key = t.mid(1, close - 1);
            if (seen.contains(key))
                *dupKeys << key;
            else
                seen.insert(key);
        }
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    if (name)
        *name = root.value(QStringLiteral("language"))
                    .toObject()
                    .value(QStringLiteral("name"))
                    .toString();
    out->clear();
    const QJsonObject strings =
        root.value(QStringLiteral("strings")).toObject();
    for (auto it = strings.begin(); it != strings.end(); ++it)
        out->insert(it.key(), it.value().toString());
    return true;
}

void bootstrap()
{
    if (g_inited)
        return;
    g_inited = true;
    loadMap(QString::fromLatin1(kBaseCode), &g_base);
    // The default is applied from the first nt::tr() call, before MainWindow has
    // had a chance to set anything (so a page built too early shows English, not
    // the base). If the default catalog is missing, fall back to the base.
    g_lang = QString::fromLatin1(kDefaultCode);
    if (!loadMap(g_lang, &g_cur)) {
        g_lang = QString::fromLatin1(kBaseCode);
        g_cur = g_base;
    }
}

void applyOne(const Binding &b)
{
    if (!b.obj)
        return;
    const QString t = tr(b.key);
    switch (b.kind) {
    case KText:
        if (auto *l = qobject_cast<QLabel *>(b.obj.data()))
            l->setText(t);
        else if (auto *btn = qobject_cast<QAbstractButton *>(b.obj.data()))
            btn->setText(t);
        break;
    case KTooltip:
        if (auto *w = qobject_cast<QWidget *>(b.obj.data()))
            w->setToolTip(t);
        break;
    case KPlaceholder:
        if (auto *e = qobject_cast<QLineEdit *>(b.obj.data()))
            e->setPlaceholderText(t);
        break;
    case KAction:
        if (auto *a = qobject_cast<QAction *>(b.obj.data()))
            a->setText(t);
        break;
    case KAccessibleName:
        if (auto *w = qobject_cast<QWidget *>(b.obj.data()))
            w->setAccessibleName(t);
        break;
    }
}

void addBinding(QObject *obj, const char *key, BindingKind kind)
{
    bootstrap();
    g_bindings.append(Binding{QPointer<QObject>(obj), QString::fromUtf8(key), kind});
    applyOne(g_bindings.last());
}

// Keys of the nt::tr()/nt::bind*() calls on one (comment-stripped) line.
QStringList keysInLine(const QString &code)
{
    static const char *const kPrefixes[] = {
        "nt::tr(",           "nt::bindText(", "nt::bindTooltip(",
        "nt::bindAccessibleName(", "nt::bindPlaceholder(", "nt::bindAction(",
    };
    QStringList out;
    for (const char *rawPrefix : kPrefixes) {
        const QString prefix = QString::fromLatin1(rawPrefix);
        int from = 0;
        while (true) {
            const int at = code.indexOf(prefix, from);
            if (at < 0)
                break;
            from = at + prefix.size();
            // First literal of the call. The bind helpers receive the widget
            // first (nt::bindText(btn, "key")), so the scan walks forward, but
            // it stops at the end of the statement: when the argument is a
            // variable (nt::tr(key)) there is no key to check here.
            int i = from;
            while (i < code.size() && code.at(i) != QLatin1Char('"')) {
                const QChar c = code.at(i);
                if (c == QLatin1Char(';') || c == QLatin1Char(')'))
                    break;
                ++i;
            }
            if (i >= code.size() || code.at(i) != QLatin1Char('"'))
                continue;
            QString lit;
            ++i;
            bool closed = false;
            for (; i < code.size(); ++i) {
                const QChar c = code.at(i);
                if (c == QLatin1Char('\\') && i + 1 < code.size()) {
                    lit += code.at(i + 1);
                    ++i;
                    continue;
                }
                if (c == QLatin1Char('"')) {
                    closed = true;
                    break;
                }
                lit += c;
            }
            if (closed && !lit.isEmpty())
                out << lit;
        }
    }
    return out;
}

} // namespace

QString baseLanguage()
{
    bootstrap();
    return QString::fromLatin1(kBaseCode);
}

QString language()
{
    bootstrap();
    return g_lang;
}

QString tr(const QString &key)
{
    bootstrap();
    const auto cur = g_cur.constFind(key);
    if (cur != g_cur.constEnd() && !cur.value().isEmpty())
        return cur.value();
    const auto base = g_base.constFind(key);
    if (base != g_base.constEnd())
        return base.value();
    return key; // untranslated everywhere: show the key so it is noticed
}

QString tr(const char *key)
{
    return tr(QString::fromUtf8(key));
}

bool setLanguage(const QString &code)
{
    bootstrap();
    if (code == g_lang && !g_cur.isEmpty())
        return true;
    QHash<QString, QString> map;
    if (!loadMap(code, &map))
        return false;
    g_lang = code;
    g_cur = map;
    retranslate();
    return true;
}

QList<LanguageInfo> availableLanguages()
{
    bootstrap();
    static QList<LanguageInfo> cache;
    if (!cache.isEmpty())
        return cache;
    const QDir dir(QStringLiteral(":/localization"));
    const QStringList files = dir.entryList(
        QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString &fn : files) {
        const QString code = fn.left(fn.size() - int(qstrlen(".json")));
        QHash<QString, QString> tmp;
        QString name;
        if (loadMap(code, &tmp, &name))
            cache.append(LanguageInfo{code, name.isEmpty() ? code : name});
    }
    return cache;
}

QString systemPreferredLanguage()
{
    bootstrap();
    // QLocale::system().name() gives "es_ES", "en_US"... Try the full name and
    // then the language part; the first catalog that matches wins. A Spanish
    // system picks "es"; English or any other (French, "C", ...) leaves the
    // default, English.
    const QString sys = QLocale::system().name();
    QStringList candidates;
    if (!sys.isEmpty()) {
        candidates << sys;
        const int sep = sys.indexOf(QLatin1Char('_'));
        if (sep > 0)
            candidates << sys.left(sep);
    }
    const QList<LanguageInfo> langs = availableLanguages();
    for (const QString &c : candidates)
        for (const LanguageInfo &li : langs)
            if (li.code.compare(c, Qt::CaseInsensitive) == 0)
                return li.code;
    return QString::fromLatin1(kDefaultCode);
}

void bindText(QLabel *w, const char *key)
{
    if (w)
        addBinding(w, key, KText);
}

void bindText(QAbstractButton *w, const char *key)
{
    if (w)
        addBinding(w, key, KText);
}

void bindTooltip(QWidget *w, const char *key)
{
    if (w)
        addBinding(w, key, KTooltip);
}

void bindAccessibleName(QWidget *w, const char *key)
{
    if (w)
        addBinding(w, key, KAccessibleName);
}

void bindPlaceholder(QLineEdit *w, const char *key)
{
    if (w)
        addBinding(w, key, KPlaceholder);
}

void bindAction(QAction *a, const char *key)
{
    if (a)
        addBinding(a, key, KAction);
}

void retranslate()
{
    for (int i = g_bindings.size() - 1; i >= 0; --i) {
        if (!g_bindings.at(i).obj) {
            g_bindings.removeAt(i); // destroyed widget: drop the binding
            continue;
        }
        applyOne(g_bindings.at(i));
    }
}

bool i18nSelfTest()
{
    int fail = 0;
    const auto chk = [&fail](bool ok, const QByteArray &what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "OK  " : "FAIL", what.constData());
        if (!ok)
            ++fail;
    };

    bootstrap();

    // 0. The UI default is English; the base stays Spanish for the fallback.
    //    Nothing has chosen a language yet in this process.
    chk(language() == QLatin1String(kDefaultCode),
        "el idioma predeterminado de la UI es ingles (en)");

    // 1. Base language present and non-empty.
    chk(!g_base.isEmpty(), "el idioma base (es) tiene cadenas");

    // 2. Every language has exactly the same key set as the base, has no empty
    //    value and no duplicated key.
    const QList<LanguageInfo> langs = availableLanguages();
    chk(langs.size() >= 2, "hay al menos dos idiomas en localization/");
    bool hasBase = false;
    for (const LanguageInfo &li : langs) {
        if (li.code == baseLanguage())
            hasBase = true;
    }
    chk(hasBase, "el idioma base aparece en localization/");
    for (const LanguageInfo &li : langs) {
        QHash<QString, QString> map;
        QString name;
        QStringList dups;
        const bool ok = loadMap(li.code, &map, &name, &dups);
        chk(ok && !map.isEmpty(), QByteArray("el idioma ") + li.code.toUtf8()
                                      + " carga y no esta vacio");
        chk(!name.isEmpty(), QByteArray("el idioma ") + li.code.toUtf8()
                                 + " trae su nombre visible");
        chk(dups.isEmpty(), QByteArray("el idioma ") + li.code.toUtf8()
                                + " no repite claves ("
                                + dups.join(QLatin1String(", ")).toUtf8() + ")");
        QStringList missing;
        QStringList extra;
        for (const QString &k : g_base.keys())
            if (!map.contains(k))
                missing << k;
        for (const QString &k : map.keys())
            if (!g_base.contains(k))
                extra << k;
        if (li.code != baseLanguage()) {
            chk(missing.isEmpty(),
                QByteArray("el idioma ") + li.code.toUtf8()
                    + " tiene TODAS las claves del base (faltan: "
                    + missing.join(QLatin1String(", ")).toUtf8() + ")");
            chk(extra.isEmpty(),
                QByteArray("el idioma ") + li.code.toUtf8()
                    + " no tiene claves de mas (sobran: "
                    + extra.join(QLatin1String(", ")).toUtf8() + ")");
        } else {
            // The base is the reference, but it must not lack its own keys.
            chk(missing.isEmpty() && extra.isEmpty(),
                "el idioma base es coherente consigo mismo");
        }
        QStringList emptyVals;
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (it.value().trimmed().isEmpty())
                emptyVals << it.key();
        }
        chk(emptyVals.isEmpty(),
            QByteArray("el idioma ") + li.code.toUtf8()
                + " no tiene traducciones vacias ("
                + emptyVals.join(QLatin1String(", ")).toUtf8() + ")");
    }

    // 3. Every key used in the source exists in the base language. This is the
    //    test that catches a typo in a key (which would otherwise just show the
    //    key in the UI).
    {
        // This file is <root>/src/i18n.cpp.
        QDir root = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
        root.cdUp();
        const QString srcDir = root.filePath(QStringLiteral("src"));
        if (!QFileInfo(srcDir).isDir()) {
            std::fprintf(stderr,
                         "  [info] sin arbol de fuentes junto al binario: no se "
                         "comprueban las claves usadas en el codigo\n");
        } else {
            int scannedFiles = 0;
            int usedKeys = 0;
            QStringList untranslated;
            QStringList pila{srcDir};
            while (!pila.isEmpty()) {
                const QString dir = pila.takeLast();
                const QFileInfoList entradas = QDir(dir).entryInfoList(
                    QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
                for (const QFileInfo &fi : entradas) {
                    if (fi.isDir()) {
                        pila << fi.filePath();
                        continue;
                    }
                    const QString sufijo = fi.suffix();
                    if (sufijo != QLatin1String("cpp")
                        && sufijo != QLatin1String("h"))
                        continue;
                    // The engine itself: its prefix table contains the literals
                    // "nt::tr(" / "nt::bindText(" so the scanner would read
                    // garbage keys out of its own source. Nothing user-facing
                    // lives here, so it is skipped.
                    if (fi.fileName() == QLatin1String("i18n.cpp"))
                        continue;
                    QFile f(fi.filePath());
                    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                        continue;
                    ++scannedFiles;
                    bool enBloque = false;
                    const QStringList lineas =
                        QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
                    for (const QString &ln : lineas) {
                        const QStringList keys =
                            keysInLine(sinComentarios(ln, enBloque));
                        for (const QString &k : keys) {
                            ++usedKeys;
                            if (!g_base.contains(k)
                                && !untranslated.contains(k))
                                untranslated << k;
                        }
                    }
                }
            }
            chk(scannedFiles > 20,
                QByteArray("recorrido el arbol de fuentes (")
                    + QByteArray::number(scannedFiles) + " ficheros)");
            chk(usedKeys > 10,
                QByteArray("encontradas ")
                    + QByteArray::number(usedKeys)
                    + " claves usadas en el codigo");
            // A key used in the UI but missing from the base catalog would show
            // up as the raw key on screen: it must fail here, not silently.
            chk(untranslated.isEmpty(),
                QByteArray("toda clave usada en el codigo esta en el idioma base"
                           " (sin traducir: ")
                    + untranslated.join(QLatin1String(", ")).toUtf8() + ")");
        }
    }

    // 4. Live binding: a bound widget must change with the language and come
    //    back, and an unknown key must not blank the UI.
    {
        QLabel probe;
        bindText(&probe, "nav.home");
        const bool hasOther = langs.size() >= 2;
        QString other;
        for (const LanguageInfo &li : langs)
            if (li.code != baseLanguage())
                other = li.code;
        // Start from the base explicitly: the initial language is the DEFAULT
        // (English), not the base, so comparing against whatever it is now would
        // make "other" (English) look like "no change".
        setLanguage(baseLanguage());
        const QString baseText = probe.text();
        if (hasOther) {
            setLanguage(other);
            const QString otherText = probe.text();
            chk(!otherText.isEmpty() && otherText != baseText,
                "un texto enlazado cambia con el idioma");
        }
        setLanguage(baseLanguage());
        chk(probe.text() == baseText, "el texto enlazado vuelve al idioma base");
        chk(tr("no.such.key.xyz") == QLatin1String("no.such.key.xyz"),
            "una clave desconocida se muestra tal cual (no se queda en blanco)");
    }

    std::fprintf(stderr, "I18N %s (%d idiomas, %d claves)\n", fail == 0 ? "OK" : "FAIL",
                 int(langs.size()), int(g_base.size()));
    return fail == 0;
}

} // namespace nt
