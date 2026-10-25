#pragma once
// Windows: resolve the Python C API at runtime from the interpreter DLL the
// host process (Endstone) already loaded. The plugin links no python3xx
// import library, so it can never spawn a second interpreter or fail to
// load on a version mismatch; if resolution fails, the Python module is
// simply not registered and the C++ API stays fully available.
//
// This header must be included AFTER <Python.h>: the macro redirects at the
// bottom rewrite every later use of these API names into dispatches through
// the function pointers declared here, and they would otherwise break the
// declarations Python.h itself makes.
//
// The build must define Py_NO_ENABLE_SHARED so pyport.h keeps plain extern
// declarations (no dllimport, no #pragma comment(lib) auto-link).

#include <Windows.h>
#include <TlHelp32.h>

namespace structbridge::pyrt {

// ---------------------------------------------------------------------------
// Resolved entry points (null until init() succeeds).
// ---------------------------------------------------------------------------
inline int (*PyArg_ParseTuple)(PyObject *, const char *, ...) = nullptr;
inline int (*PyArg_ParseTupleAndKeywords)(PyObject *, PyObject *, const char *, char **, ...) = nullptr;
inline PyObject *(*PyBool_FromLong)(long) = nullptr;
inline PyObject *(*Py_BuildValue)(const char *, ...) = nullptr;
inline int (*PyCallable_Check)(PyObject *) = nullptr;
inline int (*PyDict_SetItemString)(PyObject *, const char *, PyObject *) = nullptr;
inline PyObject *(*PyErr_Occurred)() = nullptr;
inline void (*PyErr_Print)() = nullptr;
inline void (*PyErr_SetString)(PyObject *, const char *) = nullptr;
inline PyGILState_STATE (*PyGILState_Ensure)() = nullptr;
inline void (*PyGILState_Release)(PyGILState_STATE) = nullptr;
inline PyObject *(*PyImport_GetModuleDict)() = nullptr;
inline PyObject *(*PyList_New)(Py_ssize_t) = nullptr;
inline int (*PyList_SetItem)(PyObject *, Py_ssize_t, PyObject *) = nullptr;
inline PyObject *(*PyLong_FromUnsignedLongLong)(unsigned long long) = nullptr;
inline PyObject *(*PyModule_Create2)(PyModuleDef *, int) = nullptr;
inline PyObject *(*PyObject_CallObject)(PyObject *, PyObject *) = nullptr;
inline int (*PyObject_IsTrue)(PyObject *) = nullptr;
inline PyObject *(*PyUnicode_FromString)(const char *) = nullptr;

// Data symbols. _Py_NoneStruct & co. are PyObject instances, PyExc_* are
// PyObject* variables; the pointers below hold their addresses as returned
// by GetProcAddress.
inline PyObject *p_Py_NoneStruct = nullptr;
inline PyObject *p_Py_TrueStruct = nullptr;
inline PyObject *p_Py_FalseStruct = nullptr;
inline PyObject **pp_PyExc_RuntimeError = nullptr;
inline PyObject **pp_PyExc_TypeError = nullptr;

// Locate the interpreter DLL among the loaded modules of this process and
// resolve every entry point. Idempotent; returns false when the host runs
// without an embedded interpreter or one too old to export the full C API.
inline bool init()
{
    if (PyModule_Create2 != nullptr) {
        return true;  // already resolved
    }

    // python*.dll candidates are probed with PyModule_Create2: the
    // stable-ABI stub python3.dll exports only the limited API and must be
    // skipped, while a full python3xx.dll always has it.
    HMODULE py = nullptr;
    {
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE) {
            return false;
        }
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                const wchar_t *name = me.szModule;
                const size_t len = wcslen(name);
                if (len > 10 && _wcsnicmp(name, L"python", 6) == 0 && _wcsicmp(name + len - 4, L".dll") == 0 &&
                    GetProcAddress(me.hModule, "PyModule_Create2") != nullptr) {
                    py = me.hModule;
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    if (py == nullptr) {
        return false;
    }

#define PYRT_RESOLVE(fn)                                                                              \
    do {                                                                                              \
        if ((fn = reinterpret_cast<decltype(fn)>(reinterpret_cast<void *>(GetProcAddress(py, #fn)))) == nullptr) { \
            return false;                                                                             \
        }                                                                                             \
    } while (0)
#define PYRT_RESOLVE_DATA(ptr, sym)                                                                   \
    do {                                                                                              \
        if ((ptr = reinterpret_cast<decltype(ptr)>(GetProcAddress(py, sym))) == nullptr) {             \
            return false;                                                                             \
        }                                                                                             \
    } while (0)

    PYRT_RESOLVE(PyArg_ParseTuple);
    PYRT_RESOLVE(PyArg_ParseTupleAndKeywords);
    PYRT_RESOLVE(PyBool_FromLong);
    PYRT_RESOLVE(Py_BuildValue);
    PYRT_RESOLVE(PyCallable_Check);
    PYRT_RESOLVE(PyDict_SetItemString);
    PYRT_RESOLVE(PyErr_Occurred);
    PYRT_RESOLVE(PyErr_Print);
    PYRT_RESOLVE(PyErr_SetString);
    PYRT_RESOLVE(PyGILState_Ensure);
    PYRT_RESOLVE(PyGILState_Release);
    PYRT_RESOLVE(PyImport_GetModuleDict);
    PYRT_RESOLVE(PyList_New);
    PYRT_RESOLVE(PyList_SetItem);
    PYRT_RESOLVE(PyLong_FromUnsignedLongLong);
    PYRT_RESOLVE(PyModule_Create2);
    PYRT_RESOLVE(PyObject_CallObject);
    PYRT_RESOLVE(PyObject_IsTrue);
    PYRT_RESOLVE(PyUnicode_FromString);

    PYRT_RESOLVE_DATA(p_Py_NoneStruct, "_Py_NoneStruct");
    PYRT_RESOLVE_DATA(p_Py_TrueStruct, "_Py_TrueStruct");
    PYRT_RESOLVE_DATA(p_Py_FalseStruct, "_Py_FalseStruct");
    PYRT_RESOLVE_DATA(pp_PyExc_RuntimeError, "PyExc_RuntimeError");
    PYRT_RESOLVE_DATA(pp_PyExc_TypeError, "PyExc_TypeError");

#undef PYRT_RESOLVE
#undef PYRT_RESOLVE_DATA
    return true;
}

}  // namespace structbridge::pyrt

// ---------------------------------------------------------------------------
// Macro redirects: every later use of these names dispatches through the
// pointers above (self-referencing macros do not expand recursively, so the
// qualified names inside the replacements resolve to the pointer variables).
// ---------------------------------------------------------------------------
#define PyArg_ParseTuple ::structbridge::pyrt::PyArg_ParseTuple
#define PyArg_ParseTupleAndKeywords ::structbridge::pyrt::PyArg_ParseTupleAndKeywords
#define PyBool_FromLong ::structbridge::pyrt::PyBool_FromLong
#define Py_BuildValue ::structbridge::pyrt::Py_BuildValue
#define PyCallable_Check ::structbridge::pyrt::PyCallable_Check
#define PyDict_SetItemString ::structbridge::pyrt::PyDict_SetItemString
#define PyErr_Occurred ::structbridge::pyrt::PyErr_Occurred
#define PyErr_Print ::structbridge::pyrt::PyErr_Print
#define PyErr_SetString ::structbridge::pyrt::PyErr_SetString
#define PyGILState_Ensure ::structbridge::pyrt::PyGILState_Ensure
#define PyGILState_Release ::structbridge::pyrt::PyGILState_Release
#define PyImport_GetModuleDict ::structbridge::pyrt::PyImport_GetModuleDict
#define PyList_New ::structbridge::pyrt::PyList_New
#define PyList_SetItem ::structbridge::pyrt::PyList_SetItem
#define PyLong_FromUnsignedLongLong ::structbridge::pyrt::PyLong_FromUnsignedLongLong
#define PyModule_Create2 ::structbridge::pyrt::PyModule_Create2
#define PyObject_CallObject ::structbridge::pyrt::PyObject_CallObject
#define PyObject_IsTrue ::structbridge::pyrt::PyObject_IsTrue
#define PyUnicode_FromString ::structbridge::pyrt::PyUnicode_FromString

#define _Py_NoneStruct (*::structbridge::pyrt::p_Py_NoneStruct)
#define _Py_TrueStruct (*::structbridge::pyrt::p_Py_TrueStruct)
#define _Py_FalseStruct (*::structbridge::pyrt::p_Py_FalseStruct)
#define PyExc_RuntimeError (*::structbridge::pyrt::pp_PyExc_RuntimeError)
#define PyExc_TypeError (*::structbridge::pyrt::pp_PyExc_TypeError)
