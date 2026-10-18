// glibc compatibility implementations (see src/glibc_compat.h).
//
// This translation unit intentionally includes NO system or C++ standard
// headers. The symbols below are referenced (with modern glibc version
// bindings) by the prebuilt libc++.a objects that are statically linked into
// the plugin. Because this shared object defines them locally, the linker
// binds those versioned references to these definitions instead of to
// libc/libpthread, so the plugin runs on glibc as old as 2.17.
//
// Keeping this file free of glibc headers also avoids the __isoc23_* asm
// renames that glibc headers apply, which would otherwise turn the wrapper
// bodies into infinite recursion.

extern "C" {

// ---------------------------------------------------------------------------
// Classic C library functions (plain declarations: these symbols were never
// re-versioned, so they bind to GLIBC_2.2.5 / GLIBC_2.3.3).
// ---------------------------------------------------------------------------

typedef __builtin_va_list va_list_;
typedef __WCHAR_TYPE__ wchar_t_;
typedef __SIZE_TYPE__ size_t_;

extern long strtol(const char *, char **, int);
extern long long strtoll(const char *, char **, int);
extern unsigned long strtoul(const char *, char **, int);
extern unsigned long long strtoull(const char *, char **, int);
extern long long strtoll_l(const char *, char **, int, void *);
extern unsigned long long strtoull_l(const char *, char **, int, void *);
extern long wcstol(const wchar_t_ *, wchar_t_ **, int);
extern long long wcstoll(const wchar_t_ *, wchar_t_ **, int);
extern unsigned long wcstoul(const wchar_t_ *, wchar_t_ **, int);
extern unsigned long long wcstoull(const wchar_t_ *, wchar_t_ **, int);
extern int vsscanf(const char *, const char *, va_list_);
extern long syscall(long, ...);

// ---------------------------------------------------------------------------
// __isoc23_* wrappers (glibc >= 2.38 headers renamed these; behaviour only
// differs for C23 binary literals, which are irrelevant to this plugin).
// ---------------------------------------------------------------------------

long __isoc23_strtol(const char *nptr, char **endptr, int base)
{
    return strtol(nptr, endptr, base);
}

long long __isoc23_strtoll(const char *nptr, char **endptr, int base)
{
    return strtoll(nptr, endptr, base);
}

unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base)
{
    return strtoul(nptr, endptr, base);
}

unsigned long long __isoc23_strtoull(const char *nptr, char **endptr, int base)
{
    return strtoull(nptr, endptr, base);
}

long long __isoc23_strtoll_l(const char *nptr, char **endptr, int base, void *loc)
{
    return strtoll_l(nptr, endptr, base, loc);
}

unsigned long long __isoc23_strtoull_l(const char *nptr, char **endptr, int base, void *loc)
{
    return strtoull_l(nptr, endptr, base, loc);
}

long __isoc23_wcstol(const wchar_t_ *nptr, wchar_t_ **endptr, int base)
{
    return wcstol(nptr, endptr, base);
}

long long __isoc23_wcstoll(const wchar_t_ *nptr, wchar_t_ **endptr, int base)
{
    return wcstoll(nptr, endptr, base);
}

unsigned long __isoc23_wcstoul(const wchar_t_ *nptr, wchar_t_ **endptr, int base)
{
    return wcstoul(nptr, endptr, base);
}

unsigned long long __isoc23_wcstoull(const wchar_t_ *nptr, wchar_t_ **endptr, int base)
{
    return wcstoull(nptr, endptr, base);
}

int __isoc23_sscanf(const char *str, const char *format, ...)
{
    va_list_ args;
    __builtin_va_start(args, format);
    int result = vsscanf(str, format, args);
    __builtin_va_end(args);
    return result;
}

int __isoc23_vsscanf(const char *str, const char *format, va_list_ args)
{
    return vsscanf(str, format, args);
}

// ---------------------------------------------------------------------------
// pthread shims (glibc >= 2.34 re-versioned these when libpthread merged
// into libc; prebuilt libc++.a references the new versions). These are
// self-contained implementations with no glibc dependency.
// ---------------------------------------------------------------------------

// pthread_once_t is a plain int in glibc: 0 = initial, 1 = in progress,
// 2 = complete.
int pthread_once(int *once_control, void (*init_routine)(void))
{
    for (;;) {
        int current = __atomic_load_n(once_control, __ATOMIC_ACQUIRE);
        if (current == 2) {
            return 0;
        }
        if (current == 0 && __atomic_compare_exchange_n(once_control, &current, 1, 0,
                                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            init_routine();
            __atomic_store_n(once_control, 2, __ATOMIC_RELEASE);
            return 0;
        }
        // Another thread is running the initializer; yield.
        syscall(24);  // SYS_sched_yield on x86_64
    }
}

// pthread_key_t is unsigned int in glibc.
typedef void (*key_dtor_t)(void *);

constexpr unsigned kMaxKeys = 512;
static key_dtor_t g_key_dtors[kMaxKeys];
static int g_key_count = 0;
static _Thread_local void *g_key_values[kMaxKeys];

int pthread_key_create(unsigned *key, key_dtor_t destructor)
{
    int next = __atomic_fetch_add(&g_key_count, 1, __ATOMIC_RELAXED);
    if (next < 0 || static_cast<unsigned>(next) >= kMaxKeys) {
        return 12;  // EAGAIN
    }
    g_key_dtors[next] = destructor;
    *key = static_cast<unsigned>(next);
    return 0;
}

void *pthread_getspecific(unsigned key)
{
    return key < kMaxKeys ? g_key_values[key] : static_cast<void *>(nullptr);
}

int pthread_setspecific(unsigned key, const void *value)
{
    if (key >= kMaxKeys) {
        return 22;  // EINVAL
    }
    g_key_values[key] = const_cast<void *>(value);
    return 0;
}

}  // extern "C"
