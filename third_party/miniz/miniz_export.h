/* Stand-in for miniz's CMake-generated export header (miniz_export.h).
   Upstream generates it during its own CMake build; we vendor the raw sources.
   Static build => all export/decoration macros expand to nothing. */
#pragma once

#ifndef MINIZ_EXPORT
#define MINIZ_EXPORT
#endif
#ifndef MINIZ_NO_EXPORT
#define MINIZ_NO_EXPORT
#endif
#ifndef MINIZ_DEPRECATED
#define MINIZ_DEPRECATED
#endif
#ifndef MINIZ_DEPRECATED_EXPORT
#define MINIZ_DEPRECATED_EXPORT MINIZ_EXPORT MINIZ_DEPRECATED
#endif
#ifndef MINIZ_DEPRECATED_NO_EXPORT
#define MINIZ_DEPRECATED_NO_EXPORT MINIZ_NO_EXPORT MINIZ_DEPRECATED
#endif
#ifndef MINIZ_STATIC_DEFINE
#define MINIZ_STATIC_DEFINE
#endif
