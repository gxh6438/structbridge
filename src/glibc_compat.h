// glibc 2.17 compatibility layer for the Linux build (see also
// src/glibc_compat_impl.cpp).
//
// The plugin is compiled against a modern glibc (2.39) but must load on
// servers running glibc as old as 2.17 (CentOS 7 era). Modern glibc headers
// emit references that old glibc cannot satisfy:
//
//   1. stat/lstat/fstat are exported as direct symbols since glibc 2.33;
//      older glibc only provides the __xstat/__lxstat/__fxstat family with
//      an extra version argument. The wrappers below define stat/lstat/
//      fstat locally so the versioned references from the statically
//      linked libc++ bind to them instead.
//
//   2. dl* (dlopen/dlsym/dlerror/dladdr/dlclose) moved into libc.so.6 with
//      new GLIBC_2.34 default versions in glibc 2.34; older glibc only has
//      the GLIBC_2.2.5 versions in libdl.so.2. The .symver directives below
//      pin the references to the old version (which modern libc also exports
//      as compat symbols), and scripts/build_linux.sh adds libdl.so.2 to the
//      NEEDED set so the symbols resolve on pre-2.34 systems as well.
//
// The __isoc23_* and pthread_* references coming from prebuilt libc++.a
// objects are handled in glibc_compat_impl.cpp (they require definitions,
// not renames, because the referencing objects are already compiled).

#pragma once

#if defined(__linux__) && defined(__GLIBC__)

#include <dlfcn.h>
#include <sys/stat.h>

__asm__(".symver dlopen,dlopen@GLIBC_2.2.5");
__asm__(".symver dlclose,dlclose@GLIBC_2.2.5");
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver dlerror,dlerror@GLIBC_2.2.5");
__asm__(".symver dladdr,dladdr@GLIBC_2.2.5");

// x86_64: _STAT_VER == 1, struct stat layout stable since glibc 2.3.
extern "C" {
int __xstat(int version, const char *path, struct stat *buf);
int __lxstat(int version, const char *path, struct stat *buf);
int __fxstat(int version, int fd, struct stat *buf);

constexpr int kStatVerX86_64 = 1;

int stat(const char *path, struct stat *buf) noexcept
{
    return __xstat(kStatVerX86_64, path, buf);
}

int lstat(const char *path, struct stat *buf) noexcept
{
    return __lxstat(kStatVerX86_64, path, buf);
}

int fstat(int fd, struct stat *buf) noexcept
{
    return __fxstat(kStatVerX86_64, fd, buf);
}
}  // extern "C"

#endif  // defined(__linux__) && defined(__GLIBC__)
