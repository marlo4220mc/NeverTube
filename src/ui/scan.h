#pragma once

#include <QChar>
#include <QLatin1Char>
#include <QString>
#include <QStringList>

// Source-text scanners shared by the offline self-tests that walk the source
// tree (theme/glyph/i18n). They live here, in one place, because the three
// tests have to agree on what a "literal" is: before, the pair lived in an
// anonymous namespace inside theme.cpp and the i18n test would have needed a
// second copy (which is exactly the kind of divergence the repo avoids).
namespace nt {

// Strips comments from a line (respecting the block state, which is carried
// between lines through `enBloque`) and returns only the code. Without this,
// comments are read as literals: citing a name in a "// this button is called
// X" made a dead selector pass as alive, and citing a key in a comment would
// do the same with the translation keys. An "http://" inside a literal
// truncates that literal (it is left without a closing quote, so it is
// discarded): for these tests it does not matter.
//
// The caller must RESET `enBloque` for every file: an unclosed "/*" would
// leave the whole next file marked as a comment (a silent hole).
inline QString sinComentarios(const QString &linea, bool &enBloque)
{
    QString out;
    for (int i = 0; i < linea.size();) {
        if (enBloque) {
            const int fin = linea.indexOf(QLatin1String("*/"), i);
            if (fin < 0)
                break;
            enBloque = false;
            i = fin + 2;
            continue;
        }
        const QChar c = linea.at(i);
        if (c == QLatin1Char('/') && i + 1 < linea.size()
            && linea.at(i + 1) == QLatin1Char('/'))
            break; // the rest of the line is a comment
        if (c == QLatin1Char('/') && i + 1 < linea.size()
            && linea.at(i + 1) == QLatin1Char('*')) {
            enBloque = true;
            i += 2;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

// Double-quoted literals of a line, with the escapes \", \\, \n, etc.
// already resolved. Those carrying \u or \x are filtered by the caller: there
// the codepoint is explicit on purpose.
inline QStringList literalesDe(const QString &linea)
{
    QStringList out;
    for (int i = 0; i < linea.size(); ++i) {
        if (linea.at(i) != QLatin1Char('"'))
            continue;
        QString lit;
        int j = i + 1;
        bool cerrado = false;
        for (; j < linea.size(); ++j) {
            const QChar c = linea.at(j);
            if (c == QLatin1Char('\\') && j + 1 < linea.size()) {
                lit += c;
                lit += linea.at(j + 1);
                ++j;
                continue;
            }
            if (c == QLatin1Char('"')) {
                cerrado = true;
                break;
            }
            lit += c;
        }
        if (cerrado) {
            out << lit;
            i = j; // continue after the closing quote
        }
    }
    return out;
}

} // namespace nt
