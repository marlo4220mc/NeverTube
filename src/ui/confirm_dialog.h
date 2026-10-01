#pragma once

#include <functional>

class QString;
class QWidget;

namespace nt {

// Diálogo de confirmación con el ASPECTO de la app. Sustituye a QMessageBox,
// que se pinta con el tema del escritorio: el último que quedaba vivo era el
// de "Borrar historial" de la página Buscar, y salía como un diálogo del
// sistema en medio de una app que no tiene ni un solo diálogo así.
//
// El marco visible es un QFrame#popupDialog (el mismo que el popup de Ajustes y
// el de "Nueva playlist" de Biblioteca), así que los tres se ven igual. A
// diferencia de aquellos dos NO es un popover: es modal y sin marco, porque es
// una acción destructiva y tiene que bloquear la página mientras se decide.
//
// Decisión de teclado: se enfoca CANCELAR y no hay botón por defecto, así que
// ni Enter ni un Enter perdido borran nada; Escape cierra (comportamiento de
// QDialog).
void confirm(QWidget *parent, const QString &title, const QString &text,
             const QString &acceptText, std::function<void()> onAccept);

} // namespace nt