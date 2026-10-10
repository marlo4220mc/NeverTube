#pragma once

// SPDX-License-Identifier: GPL-3.0-or-later

// MSVC 2026 (VS 18, toolset 14.5x, _MSC_VER >= 1950) REMOVED the symbol
// stdext::make_checked_array_iterator from its STL. Qt 5.15.2 references it
// UNCONDITIONALLY on MSVC from qlist.h:915 (QList::op_eq_impl) via the macro
// QT_MAKE_CHECKED_ARRAY_ITERATOR, so any TU that instantiates the
// operator== of a QList fails with C2653 ('stdext' is not a class) / C3861
// ('make_checked_array_iterator' not found). GitHub windows-latest already runs
// VS2026 and no VS2022 image remains, so it cannot be fixed at the
// runner level: it must be compensated at the code level.
//
// This shim provides a minimal replacement that returns the raw pointer (identical
// to Qt's non-MSVC branch: #define QT_MAKE_CHECKED_ARRAY_ITERATOR(x, size) (x)),
// ignoring bounds. That is why it is guarded by _MSC_VER >= 1950 (only when the
// real symbol no longer exists) and is a no-op on any other toolchain: on
// VS2022/GCC/Clang nobody uses this function, the binary does not change.
//
// It is force-included before every Qt header via /FI from CMakeLists.txt (if(MSVC)).
#if defined(_MSC_VER) && _MSC_VER >= 1950
#include <cstddef>

namespace stdext {

// Qt calls make_checked_array_iterator(ptr, size); here the raw pointer is
// returned. The size only exists in the original header for the checked
// iterator range; returning a plain pointer to std::equal (as Qt does on the
// other platforms) yields identical behavior.
template <typename T>
inline T *make_checked_array_iterator(T *pointer, std::size_t /*size*/)
{
    return pointer;
}

} // namespace stdext
#endif // _MSC_VER >= 1950
