#define OP_API
#define _In_
#define _In_reads_bytes_(x)
#define _Out_
#define _Inout_
%module pyop

%{
    #define OP_API
#include <exception>
#include "../include/libop.h"
%}

%include "std_string.i"
%include "std_wstring.i"
%include "typemaps.i"
%apply long long { LONG_PTR };
%apply std::wstring &OUTPUT {std::wstring &}
%apply long *OUTPUT {long *}
%apply long long *OUTPUT {LONG_PTR *}
%apply size_t *OUTPUT {size_t *}

%typemap(in) const void * (Py_buffer view) {
    if (PyObject_GetBuffer($input, &view, PyBUF_SIMPLE) != 0) {
        SWIG_exception_fail(SWIG_TypeError, "expected a bytes-like object");
    }
    $1 = view.buf;
}

%typemap(freearg) const void * {
    PyBuffer_Release(&view$argnum);
}

%extend op::Op {
    void RunApp(const wchar_t *cmdline, long mode, long *ret) {
        unsigned long pid = 0;
        $self->RunApp(cmdline, mode, &pid, ret);
    }
}

// C++ 异常→Python 异常：防止 C++ 异常逃逸导致 Python 解释器崩溃
// （修改本文件后需在有 SWIG 的环境执行 `swig -c++ -python -o op_wrap.cxx op.i`
//   重新生成 wrapper，否则改动不生效）
%exception {
    try {
        $action
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        SWIG_fail;
    } catch (...) {
        PyErr_SetString(PyExc_RuntimeError, "Unknown C++ exception");
        SWIG_fail;
    }
}

%include "../include/libop.h"

