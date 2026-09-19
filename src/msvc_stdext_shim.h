#pragma once

// SPDX-License-Identifier: GPL-3.0-or-later

// MSVC 2026 (VS 18, toolset 14.5x, _MSC_VER >= 1950) ELIMINÓ de su STL el
// símbolo stdext::make_checked_array_iterator. Qt 5.15.2 lo referencia de forma
// INCONDICIONAL en MSVC desde qlist.h:915 (QList::op_eq_impl) vía la macro
// QT_MAKE_CHECKED_ARRAY_ITERATOR, así que cualquier TU que instancie el
// operator== de un QList falla con C2653 ('stdext' no es clase) / C3861
// ('make_checked_array_iterator' no encontrado). GitHub windows-latest ya corre
// VS2026 y no queda imagen con VS2022, por lo que no se puede arreglar a nivel
// de runner: hay que compensarlo a nivel de código.
//
// Este shim provee un reemplazo mínimo que devuelve el puntero crudo (idéntico
// a la rama no-MSVC de Qt: #define QT_MAKE_CHECKED_ARRAY_ITERATOR(x, size) (x)),
// ignorando bounds. Por eso va guardado en _MSC_VER >= 1950 (solo cuando el
// símbolo real ya no existe) y es un no-op en cualquier otro toolchain: en
// VS2022/GCC/Clang nadie usa esta función, no cambia el binario.
//
// Se fuerza antes de todo cabecera Qt vía /FI desde CMakeLists.txt (if(MSVC)).
#if defined(_MSC_VER) && _MSC_VER >= 1950
#include <cstddef>

namespace stdext {

// Qt llama make_checked_array_iterator(ptr, size); aquí se devuelve el puntero
// crudo. El tamaño solo existe en la cabecera original para el rango de
// iteradores comprobados; al devolver un puntero puro a std::equal (como hace
// Qt en el resto de plataformas) se obtiene comportamiento idéntico.
template <typename T>
inline T *make_checked_array_iterator(T *pointer, std::size_t /*size*/)
{
    return pointer;
}

} // namespace stdext
#endif // _MSC_VER >= 1950
