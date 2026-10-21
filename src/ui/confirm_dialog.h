#pragma once

#include <functional>

class QString;
class QWidget;

namespace nt {

// `checkText` is the label of a CHECK BOX that the dialog shows under the
// text (empty = no check box). `checkState` is its state: the one it has
// when the dialog opens, and the one the person leaves it in (it is only
// written when the dialog is ACCEPTED).
//
// The check box is what the delete of a download uses: "also delete it from
// the disk" is UNCHECKED by default, so the action that happens with a
// single click is the safe one.
void confirm(QWidget *parent, const QString &title, const QString &text,
             const QString &acceptText, std::function<void()> onAccept,
             const QString &checkText = QString(), bool *checkState = nullptr);

bool inputText(QWidget *parent, const QString &title, const QString &prompt,
               const QString &initialText, const QString &okText,
               QString *outText);

} // namespace nt
