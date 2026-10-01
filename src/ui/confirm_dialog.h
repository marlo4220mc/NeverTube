#pragma once

#include <functional>

class QString;
class QWidget;

namespace nt {

void confirm(QWidget *parent, const QString &title, const QString &text,
             const QString &acceptText, std::function<void()> onAccept);

bool inputText(QWidget *parent, const QString &title, const QString &prompt,
               const QString &initialText, const QString &okText,
               QString *outText);

} // namespace nt
