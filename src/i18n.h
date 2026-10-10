#pragma once

#include <QList>
#include <QString>

class QAction;
class QAbstractButton;
class QLabel;
class QLineEdit;
class QWidget;

// Multilanguage support, designed so that adding a language is dropping a file.
//
// The catalog lives in <root>/localization/<code>.json (one file per language,
// embedded in the binary through localization/localization.qrc). Each file has a
// "language" object with the code and the NATIVE name that the Settings selector
// shows, and a "strings" object of stable keys -> text:
//
//   { "language": { "code": "en", "name": "English" },
//     "strings": { "nav.home": "Home", ... } }
//
// The BASE language is Spanish (the source language of the app): any key missing
// from the current language falls back to it, and a key missing everywhere is
// returned as-is (so it is visible in the UI instead of blank).
//
// The DEFAULT language (fresh install, no "ui_lang" saved) is English, except
// when the system language is detected: see systemPreferredLanguage(). The base
// stays Spanish because that is the wording of the source, not because it is the
// default UI choice.
//
// The keys are semantic (nav.home, settings.quality...), never the Spanish text:
// adding a language is a new file with the same key set and the Spanish wording
// can change without breaking any translation. --selftest-i18n checks offline
// that every language has exactly the same keys and that every key used in the
// source exists.
namespace nt {

struct LanguageInfo
{
    QString code; // "es", "en"...
    QString name; // native name shown in Settings ("Español", "English")
};

// Code of the active language (defaults to English). The application selects it
// at startup from the "ui_lang" setting and, when there is none, from the system
// language (systemPreferredLanguage()); MainWindow applies it before building
// the pages.
QString language();
QString baseLanguage();

// Preferred UI language for a fresh install: the system language if its catalog
// exists (a Spanish system picks "es"), otherwise the default (English). It does
// NOT read or write any setting; MainWindow calls it only when "ui_lang" is
// unset, so the user's explicit choice always wins.
QString systemPreferredLanguage();

// Loads the language's file and re-applies every live binding. Returns false
// (leaving the current language untouched) if that code has no file: an
// unknown code never leaves the UI half-translated.
bool setLanguage(const QString &code);

// Languages found in :/localization (each *.json with valid metadata).
QList<LanguageInfo> availableLanguages();

// Translation of a key in the active language (base fallback, then the key).
QString tr(const char *key);
QString tr(const QString &key);

// LIVE-bound text. Each helper applies the translation NOW and registers the
// widget/action so setLanguage() re-applies it: that is what makes the switch
// instant without a per-page retranslate(). The object is tracked with a
// QPointer, so destroyed widgets (menus, dialogs, rebuilt sections) are dropped
// on the next pass.
//
// Only STATIC chrome may be bound: if a label's text is also set dynamically
// somewhere else, do NOT bind it (bindings are re-applied on every language
// change and would overwrite the dynamic value).
void bindText(QLabel *w, const char *key);
void bindText(QAbstractButton *w, const char *key);
void bindTooltip(QWidget *w, const char *key);
void bindPlaceholder(QLineEdit *w, const char *key);
void bindAction(QAction *a, const char *key);

// Re-applies every live binding in the active language. Called by setLanguage().
void retranslate();

// OFFLINE self-test (no network or GPU): language files (metadata, key parity
// between languages, no empty values, no duplicated key), the keys used in the
// source (they must exist in the base language) and the live-binding mechanism.
// Called from --selftest-i18n.
bool i18nSelfTest();

} // namespace nt
