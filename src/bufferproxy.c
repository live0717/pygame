/*
  pygame - Python Game Library
  Module adapted from bufferproxy.c, Copyright (C) 2007  Marcus von Appen

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Library General Public
  License as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Library General Public License for more details.

  You should have received a copy of the GNU Library General Public
  License along with this library; if not, write to the Free
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

/*
  This module exports a proxy object that exposes another object's
  data throught the Python buffer protocol or the array interface.
  The new buffer protocol is available for Python 3.x. For Python 2.x
  only the old protocol is implemented (for PyPy compatibility).
  Both the C level array structure - __array_struct__ - interface and
  Python level - __array_interface__ - are exposed.
 */

#define PYGAMEAPI_BUFPROXY_INTERNAL
#include "pygame.h"
#include "pgcompat.h"
#include "pgbufferproxy.h"

/* No build will support the new and old buffer protocols simultaneously. */
#if HAVE_OLD_BUFPROTO
#define PG_ENABLE_OLDBUF 1
#else
#define PG_ENABLE_OLDBUF 0
#endif


#if SDL_BYTEORDER == SDL_LIL_ENDIAN
#define BUFPROXY_MY_ENDIAN '<'
#define BUFPROXY_OTHER_ENDIAN '>'
#else
#define BUFPROXY_MY_ENDIAN '>'
#define BUFPROXY_OTHER_ENDIAN '<'
#endif

#define PROXY_MODNAME "bufferproxy"
#define PROXY_TYPE_NAME "BufferProxy"
#define PROXY_TYPE_FULLNAME (IMPPREFIX PROXY_MODNAME "." PROXY_TYPE_NAME)

typedef struct PgBufproxyObject_s {
    PyObject_HEAD
    PyObject *obj;                             /* Wrapped object              */
    Pg_buffer *view_p;                         /* For array interface export  */
#if PG_ENABLE_OLDBUF
    Py_ssize_t segcount;                       /* bf_getsegcount return value */
    Py_ssize_t seglen;                         /* bf_getsegcount len argument */
#endif
    pg_getbufferfunc get_buffer;               /* Pg_buffer get callback      */
    PyObject *dict;                            /* Allow arbitrary attributes  */
    PyObject *weakrefs;                        /* Reference cycles can happen */
} PgBufproxyObject;

typedef struct Pg_buffer_d_s {
    Pg_buffer view;
    PyObject *dict;
} Pg_buffer_d;

static int PgBufproxy_Trip(PyObject *);
static Py_buffer *_proxy_get_view (PgBufproxyObject*);

/* $$ Transitional stuff */
#warning Transitional stuff: must disappear!

#define NOTIMPLEMENTED(rcode) \
    PyErr_Format(PyExc_NotImplementedError, \
                 "Not ready yet. (line %i in %s)", __LINE__, __FILE__); \
    return rcode

/* Use Dict_AsView alternative with flags arg. */
static void _release_buffer_from_dict(Py_buffer *);

static int
_get_buffer_from_dict(PyObject *dict, Pg_buffer *pg_view_p, int flags) {
    PyObject *obj;
    Py_buffer *view_p = (Py_buffer *)pg_view_p;
    Pg_buffer *pg_dict_view_p;
    Py_buffer *dict_view_p;
    PyObject *py_callback;
    PyObject *py_rval;

    assert(dict && PyDict_Check(dict));
    assert(view_p);
    view_p->obj = 0;
    pg_dict_view_p = PyMem_New(Pg_buffer, 1);
    if (!pg_dict_view_p) {
        PyErr_NoMemory();
        return -1;
    }
    pg_dict_view_p->consumer = pg_view_p->consumer;
    if (PgDict_AsBuffer(pg_dict_view_p, dict, flags)) {
        PyMem_Free(pg_dict_view_p);
        return -1;
    }
    dict_view_p = (Py_buffer *)pg_dict_view_p;
    obj = PyDict_GetItemString(dict, "parent");
    if (!obj) {
        obj = Py_None;
    }
    Py_INCREF(obj);
    py_callback = PyDict_GetItemString(dict, "before");
    if (py_callback) {
        Py_INCREF(py_callback);
        py_rval = PyObject_CallFunctionObjArgs(py_callback, obj, NULL);
        Py_DECREF(py_callback);
        if (!py_rval) {
            PgBuffer_Release(pg_dict_view_p);
            Py_DECREF(obj);
            return -1;
        }
        Py_DECREF(py_rval);
    }
    Py_INCREF(dict);
    dict_view_p->obj = dict;
    view_p->obj = obj;
    view_p->buf = dict_view_p->buf;
    view_p->len = dict_view_p->len;
    view_p->readonly = dict_view_p->readonly;
    view_p->itemsize = dict_view_p->itemsize;
    view_p->format = dict_view_p->format;
    view_p->ndim = dict_view_p->ndim;
    view_p->shape = dict_view_p->shape;
    view_p->strides = dict_view_p->strides;
    view_p->suboffsets = dict_view_p->suboffsets;
    view_p->internal = pg_dict_view_p;
    pg_view_p->release_buffer = _release_buffer_from_dict;
    return 0;
}

/* This will need changes */
static void
_release_buffer_from_dict(Py_buffer *view_p)
{
    Py_buffer *dict_view_p;
    PyObject *dict;
    PyObject *obj;
    PyObject *py_callback;
    PyObject *py_rval;

    assert(view_p && view_p->internal);
    obj = view_p->obj;
    dict_view_p = (Py_buffer *)view_p->internal;
    dict = dict_view_p->obj;
    assert(dict && PyDict_Check(dict));
    py_callback = PyDict_GetItemString(dict, "after");
    if (py_callback) {
        Py_INCREF(py_callback);
        py_rval = PyObject_CallFunctionObjArgs(py_callback, obj, NULL);
        if (py_rval) {
            Py_DECREF(py_rval);
        }
        else {
            PyErr_Clear();
        }
        Py_DECREF(py_callback);
    }
    PgBuffer_Release((Pg_buffer *)dict_view_p);
    PyMem_Free(dict_view_p);
    view_p->obj = 0;
    Py_DECREF(obj);
}

/* Stub functions */
static PyObject *proxy_get_raw(PgBufproxyObject *, PyObject *);

/* End transitional stuff */

static PyObject *
_proxy_subtype_new(PyTypeObject *type,
                   PyObject *obj,
                   pg_getbufferfunc get_buffer)
{
    PgBufproxyObject *self = (PgBufproxyObject *)type->tp_alloc(type, 0);

    if (!self) {
        return 0;
    }
    Py_XINCREF(obj);
    self->obj = obj;
    self->get_buffer = get_buffer;
    return (PyObject *)self;
}

static Py_buffer *
_proxy_get_view(PgBufproxyObject *proxy) {
    Pg_buffer *view_p = proxy->view_p;

    if (!view_p) {
        view_p = PyMem_New(Pg_buffer, 1);
        if (!view_p) {
            PyErr_NoMemory();
            return 0;
        }
        view_p->consumer = (PyObject *)proxy;
        if (proxy->get_buffer(proxy->obj, view_p, PyBUF_RECORDS)) {
            PyMem_Free(view_p);
            return 0;
        }
        proxy->view_p = view_p;
    }
    assert(((Py_buffer *)view_p)->len && ((Py_buffer *)view_p)->itemsize);
    return (Py_buffer *)view_p;
}

static void
_proxy_release_view(PgBufproxyObject *proxy) {
    Pg_buffer *view_p = proxy->view_p;

    if (view_p) {
        proxy->view_p = 0;
        PgBuffer_Release(view_p);
        PyMem_Free(view_p);
    }
}

static int
_proxy_zombie_get_buffer(PyObject *obj, Pg_buffer *pg_view_p, int flags)
{
    PyObject *proxy = pg_view_p->consumer;

    ((Py_buffer *)pg_view_p)->obj = 0;
    PyErr_Format (PyExc_RuntimeError,
                  "Attempted buffer export on <%s at %p, parent=<%s at %p>> "
                  "while deallocating it",
                  Py_TYPE(proxy)->tp_name, (void *)proxy,
                  Py_TYPE(obj)->tp_name, (void *)obj);
    return -1;
}

/**
 * Return a new PgBufproxyObject (Python level constructor).
 */
static PyObject *
proxy_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *obj = 0;
    pg_getbufferfunc get_buffer = PgObject_GetBuffer;

    if (!PyArg_ParseTuple(args, "O:Bufproxy", &obj)) {
        return 0;
    }
    if (PyDict_Check(obj)) {
        get_buffer = _get_buffer_from_dict;
    }
    return _proxy_subtype_new(type, obj, get_buffer);
}

/**
 * Deallocates the PgBufproxyObject and its members.
 * Is reentrant.
 */
static void
proxy_dealloc(PgBufproxyObject *self)
{
    /* Prevent infinite recursion from a reentrant call */
    if (self->get_buffer == _proxy_zombie_get_buffer) {
        return;
    }
    self->get_buffer = _proxy_zombie_get_buffer;

    /* Non reentrant call; deallocate */
    PyObject_GC_UnTrack(self);
    _proxy_release_view(self);
    Py_XDECREF(self->obj);
    Py_XDECREF(self->dict);
    if (self->weakrefs) {
        PyObject_ClearWeakRefs((PyObject *)self);
    }
    Py_TYPE(self)->tp_free(self);
}

static int
proxy_traverse(PgBufproxyObject *self, visitproc visit, void *arg) {
    if (self->obj) {
        Py_VISIT(self->obj);
    }
    if (self->view_p && ((Py_buffer *)self->view_p)->obj) /* conditional && */ {
        Py_VISIT(((Py_buffer *)self->view_p)->obj);
    }
    if (self->dict) {
        Py_VISIT(self->dict);
    }
    return 0;
}

/**** Getter and setter access ****/
static PyObject *
proxy_get_arraystruct(PgBufproxyObject *self, PyObject *closure)
{
    Py_buffer *view_p = _proxy_get_view(self);
    PyObject *capsule;

    if (!view_p) {
        return 0;
    }
    capsule = PgBuffer_AsArrayStruct(view_p);
    if (!capsule) {
        _proxy_release_view(self);
    }
    return capsule;
}

static PyObject *
proxy_get_arrayinterface(PgBufproxyObject *self, PyObject *closure)
{
    Py_buffer *view_p = _proxy_get_view(self);
    PyObject *dict;

    if (!view_p) {
        return 0;
    }
    dict = PgBuffer_AsArrayInterface(view_p);
    if (!dict) {
        _proxy_release_view(self);
    }
    return dict;
}

static PyObject *
proxy_get_parent(PgBufproxyObject *self, PyObject *closure)
{
    Py_buffer *view_p = _proxy_get_view(self);
    PyObject *obj;

    if (!view_p) {
        return 0;
    }
    obj = view_p->obj ? view_p->obj : Py_None;
    Py_INCREF(obj);
    return obj;
}

static PyObject *
proxy_get___dict__(PgBufproxyObject *self, PyObject *closure)
{
    if (!self->dict) {
        self->dict = PyDict_New();
        if (!self->dict) {
            return 0;
        }
    }
    Py_INCREF(self->dict);
    return self->dict;
}

static PyObject *
proxy_get_raw(PgBufproxyObject *self, PyObject *closure)
{
    Py_buffer *view_p = _proxy_get_view(self);

    if (!view_p) {
        return 0;
    }
    if (!PyBuffer_IsContiguous(view_p, 'A')) {
        PyErr_SetString(PyExc_ValueError, "the bytes are not contiguous");
        return 0;
    }
    return Bytes_FromStringAndSize((char *)view_p->buf, view_p->len);
}

static PyObject *
proxy_get_length(PgBufproxyObject *self, PyObject *closure)
{
    Py_buffer *view_p = _proxy_get_view(self);

    return view_p ? PyInt_FromSsize_t(view_p->len) : 0;
}

/**** Methods ****/

/**
 * Representation method.
 */
static PyObject *
proxy_repr (PgBufproxyObject *self)
{
    return Text_FromFormat("<%s(%p)>", Py_TYPE(self)->tp_name, self);
}

/**
 * Writes raw data to the buffer.
 */
static PyObject *
proxy_write(PgBufproxyObject *buffer, PyObject *args, PyObject *kwds)
{
    NOTIMPLEMENTED(0);
}

static struct PyMethodDef proxy_methods[] = {
    {"write", (PyCFunction)proxy_write, METH_VARARGS | METH_KEYWORDS,
     "write raw bytes to object buffer"},
    {0, 0, 0, 0}
};

/**
 * Getters and setters for the PgBufproxyObject.
 */
static PyGetSetDef proxy_getsets[] =
{
    {"__array_struct__", (getter)proxy_get_arraystruct, 0, 0, 0},
    {"__array_interface__", (getter)proxy_get_arrayinterface, 0, 0, 0},
    {"parent", (getter)proxy_get_parent, 0, 0, 0},
    {"__dict__", (getter)proxy_get___dict__, 0, 0, 0},
    {"raw", (getter)proxy_get_raw, 0, 0, 0},
    {"length", (getter)proxy_get_length, 0, 0, 0},
    {0, 0, 0, 0, 0}
};


#if PG_ENABLE_NEWBUF || PG_ENABLE_OLDBUF

#if PG_ENABLE_NEWBUF
static int
proxy_getbuffer(PgBufproxyObject *self, Py_buffer *view_p, int flags)
{
    Pg_buffer *pg_obj_view_p = PyMem_New(Pg_buffer, 1);
    Py_buffer *obj_view_p = (Py_buffer *)pg_obj_view_p;

    view_p->obj = 0;
    if (!pg_obj_view_p) {
        PyErr_NoMemory();
        return -1;
    }
    pg_obj_view_p->consumer = (PyObject *)self;
    if (self->get_buffer(self->obj, pg_obj_view_p, flags)) {
        PyMem_Free(pg_obj_view_p);
        return -1;
    }
    Py_INCREF(self);
    view_p->obj = (PyObject *)self;
    view_p->buf = obj_view_p->buf;
    view_p->len = obj_view_p->len;
    view_p->readonly = obj_view_p->readonly;
    view_p->itemsize = obj_view_p->itemsize;
    view_p->format = obj_view_p->format;
    view_p->ndim = obj_view_p->ndim;
    view_p->shape = obj_view_p->shape;
    view_p->strides = obj_view_p->strides;
    view_p->suboffsets = obj_view_p->suboffsets;
    view_p->internal = obj_view_p;
    return 0;
}

static void
proxy_releasebuffer(PgBufproxyObject *self, Py_buffer *view_p)
{
    PgBuffer_Release((Pg_buffer *)view_p->internal);
    PyMem_Free(view_p->internal);
}

#endif /* #if PG_ENABLE_NEWBUF */


#if PG_ENABLE_OLDBUF
static int
_is_byte_view(Py_buffer *view_p) {
    const char *format = view_p->format;

    /* Conditional ||'s */
    return ((!format)                                                   ||
            (format[0] == 'B' && format[1] == '\0')                     ||
            (format[0] == '=' && format[1] == 'B' && format[2] == '\0') ||
            (format[0] == '<' && format[1] == 'B' && format[2] == '\0') ||
            (format[0] == '>' && format[1] == 'B' && format[2] == '\0') ||
            (format[0] == '@' && format[1] == 'B' && format[2] == '\0') ||
            (format[0] == '!' && format[1] == 'B' && format[2] == '\0')    );
}

static Py_ssize_t
proxy_getreadbuf(PgBufproxyObject *self, Py_ssize_t _index, void **ptr)
{
    Py_buffer *view_p = (Py_buffer *)self->view_p;
    Py_ssize_t offset = 0;
    Py_ssize_t dim;

    if (_index < 0 || _index >= self->segcount) {
        if (_index == 0 && self->segcount == 0) {
            *ptr = 0;
            return 0;
        }
        PyErr_SetString(PyExc_IndexError, "segment index out of range");
        return -1;
    }
    if (!view_p) {
        *ptr = 0;
        return 0;
    }
    if (self->segcount == 1) {
        assert(_index == 0);
        *ptr = view_p->buf;
        return view_p->len;
    }
    /* Segments will be strictly in C contiguous order, which may
       differ from the actual order in memory. It can affect buffer
       copying. This may never be an issue, though, since Python
       never directly supported multi-segment buffers. And besides,
       the old buffer is deprecated. */
    for (dim = view_p->ndim - 1; dim != -1; --dim) {
        offset += _index % view_p->shape[dim] * view_p->strides[dim];
        _index /= view_p->shape[dim];
    }
    *ptr = (char *)view_p->buf + offset;
    return view_p->itemsize;
}

static Py_ssize_t
proxy_getwritebuf(PgBufproxyObject *self, Py_ssize_t _index, void **ptr)
{
    void *p;
    Py_ssize_t seglen = proxy_getreadbuf(self, _index, &p);

    if (seglen < 0) {
        return -1;
    }
    if (seglen > 0 && ((Py_buffer *)self->view_p)->readonly) /* cond. && */ {
        PyErr_SetString(PyExc_ValueError, "buffer is not writeable");
        return -1;
    }
    *ptr = p;
    return seglen;
}

static Py_ssize_t
proxy_getsegcount(PgBufproxyObject *self, Py_ssize_t *lenp)
{
    Py_buffer *view_p = _proxy_get_view(self);

    if (!view_p) {
        PyErr_Clear();
        self->seglen = 0;
        self->segcount = 0;
    }
    else if (view_p->ndim == 0 ||
             (view_p->ndim == 1 && _is_byte_view(view_p))) {
        self->seglen = view_p->len;
        self->segcount = 1;
    }
    else {
        self->seglen = view_p->len;
        self->segcount = view_p->len / view_p->itemsize;
    }
    if (lenp) {
        *lenp = self->seglen;
    }
    return self->segcount;
}

#endif /* #if PG_ENABLE_OLDBUF */


#define PROXY_BUFFERPROCS (&proxy_bufferprocs)

static PyBufferProcs proxy_bufferprocs = {
#if PG_ENABLE_OLDBUF
    (readbufferproc)proxy_getreadbuf,
    (writebufferproc)proxy_getwritebuf,
    (segcountproc)proxy_getsegcount,
    0
#elif HAVE_OLD_BUFPROTO
    0,
    0,
    0,
    0
#endif

#if HAVE_OLD_BUFPROTO && HAVE_NEW_BUFPROTO
     ,
#endif

#if PG_ENABLE_NEWBUF
    (getbufferproc)proxy_getbuffer,
    (releasebufferproc)proxy_releasebuffer
#elif HAVE_NEW_BUFPROTO
    0,
    0
#endif
};

#endif /* #if PG_ENABLE_NEWBUF || PG_ENABLE_OLDBUF */


#if !defined(PROXY_BUFFERPROCS)
#define PROXY_BUFFERPROCS 0
#endif

#if PY2 && PG_ENABLE_NEWBUF
#define PROXY_TPFLAGS \
    (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC | \
     Py_TPFLAGS_HAVE_NEWBUFFER)
#else
#define PROXY_TPFLAGS \
    (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC)
#endif

static PyTypeObject PgBufproxy_Type =
{
    TYPE_HEAD(NULL, 0)
    PROXY_TYPE_FULLNAME,        /* tp_name */
    sizeof (PgBufproxyObject),  /* tp_basicsize */
    0,                          /* tp_itemsize */
    (destructor)proxy_dealloc,  /* tp_dealloc */
    0,                          /* tp_print */
    0,                          /* tp_getattr */
    0,                          /* tp_setattr */
    0,                          /* tp_compare */
    (reprfunc)proxy_repr,       /* tp_repr */
    0,                          /* tp_as_number */
    0,                          /* tp_as_sequence */
    0,                          /* tp_as_mapping */
    0,                          /* tp_hash */
    0,                          /* tp_call */
    0,                          /* tp_str */
    0,                          /* tp_getattro */
    0,                          /* tp_setattro */
    PROXY_BUFFERPROCS,          /* tp_as_buffer */
    PROXY_TPFLAGS,              /* tp_flags */
    "Object bufproxy as an array struct\n",
    (traverseproc)proxy_traverse,  /* tp_traverse */
    0,                          /* tp_clear */
    0,                          /* tp_richcompare */
    offsetof(PgBufproxyObject, weakrefs),  /* tp_weaklistoffset */
    0,                          /* tp_iter */
    0,                          /* tp_iternext */
    proxy_methods,              /* tp_methods */
    0,                          /* tp_members */
    proxy_getsets,              /* tp_getset */
    0,                          /* tp_base */
    0,                          /* tp_dict */
    0,                          /* tp_descr_get */
    0,                          /* tp_descr_set */
    offsetof(PgBufproxyObject, dict),  /* tp_dictoffset */
    0,                          /* tp_init */
    PyType_GenericAlloc,        /* tp_alloc */
    proxy_new,                  /* tp_new */
    PyObject_GC_Del,            /* tp_free */
#ifndef __SYMBIAN32__
    0,                          /* tp_is_gc */
    0,                          /* tp_bases */
    0,                          /* tp_mro */
    0,                          /* tp_cache */
    0,                          /* tp_subclasses */
    0,                          /* tp_weaklist */
    0                           /* tp_del */
#endif
};

/**** Module methods ***/

#if PG_ENABLE_OLDBUF
static PyObject *
get_read_buffer(PyObject *self, PyObject *args, PyObject *kwds)
{
    long segment = 0;
    PyObject *obj = 0;
    Py_ssize_t len = 0;
    void *ptr = 0;
    readbufferproc getreadbuffer = 0;
    static char *keywords[] = {"obj", "segment", 0};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Ol", keywords,
                                     &obj, &segment)) {
        return 0;
    }
    if (!Py_TYPE(obj)->tp_as_buffer) {
        PyErr_SetString(PyExc_ValueError, "No tp_as_buffer struct");
        return 0;
    }
    getreadbuffer = Py_TYPE(obj)->tp_as_buffer->bf_getreadbuffer;
    if (!getreadbuffer) {
        PyErr_SetString(PyExc_ValueError, "No bf_getreadbuffer slot function");
        return 0;
    }
    len = getreadbuffer(obj, segment, &ptr);
    if (len < 0) {
        return 0;
    }
    return Py_BuildValue("ll", (long)len, (long)ptr);
}

static PyObject *
get_write_buffer(PyObject *self, PyObject *args, PyObject *kwds)
{
    long segment = 0;
    PyObject *obj = 0;
    Py_ssize_t len = 0;
    void *ptr = 0;
    writebufferproc getwritebuffer = 0;
    static char *keywords[] = {"obj", "segment", 0};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Ol", keywords,
                                     &obj, &segment)) {
        return 0;
    }
    if (!Py_TYPE(obj)->tp_as_buffer) {
        PyErr_SetString(PyExc_ValueError, "No tp_as_buffer struct");
        return 0;
    }
    getwritebuffer = Py_TYPE(obj)->tp_as_buffer->bf_getwritebuffer;
    if (!getwritebuffer) {
        PyErr_SetString(PyExc_ValueError, "No bf_getwritebuffer slot function");
        return 0;
    }
    len = getwritebuffer(obj, segment, &ptr);
    if (len < 0) {
        return 0;
    }
    return Py_BuildValue("ll", (long)len, (long)ptr);
}

static PyObject *
get_segcount(PyObject *self, PyObject *obj)
{
    Py_ssize_t segcount = 0;
    Py_ssize_t len = 0;
    segcountproc getsegcount = 0;

    if (!Py_TYPE(obj)->tp_as_buffer) {
        PyErr_SetString(PyExc_ValueError, "No tp_as_buffer struct");
        return 0;
    }
    getsegcount = Py_TYPE(obj)->tp_as_buffer->bf_getsegcount;
    if (!getsegcount) {
        PyErr_SetString(PyExc_ValueError, "No bf_getsegcount slot function");
        return 0;
    }
    segcount = getsegcount(obj, &len);
    return Py_BuildValue("ll", (long)segcount, (long)len);
}

#endif

static PyMethodDef bufferproxy_methods[] = {
#if PG_ENABLE_OLDBUF
    {"get_read_buffer", (PyCFunction)get_read_buffer,
     METH_VARARGS | METH_KEYWORDS, "call bf_getreadbuffer slot function"},
    {"get_write_buffer", (PyCFunction)get_write_buffer,
     METH_VARARGS | METH_KEYWORDS, "call bf_getwritebuffer slot function"},
    {"get_segcount", (PyCFunction)get_segcount,
     METH_O, "call bf_getsegcount slot function"},
#endif
    {0, 0, 0, 0}
};

/**** Public C api ***/

static PyObject *
PgBufproxy_New(PyObject *obj, pg_getbufferfunc get_buffer)
{
    if (!get_buffer) {
        if (!obj) {
            PyErr_SetString(PyExc_ValueError,
                            "One of arguments 'obj' or 'get_buffer' is "
                            "required: both NULL instead");
            return 0;
        }
        get_buffer = PgObject_GetBuffer;
    }
    return _proxy_subtype_new(&PgBufproxy_Type, obj, get_buffer);
}

static PyObject *
PgBufproxy_GetParent(PyObject *obj)
{
    if (!PyObject_IsInstance (obj, (PyObject *)&PgBufproxy_Type)) {
        PyErr_Format(PyExc_TypeError,
                     "Expected a BufferProxy object: got %s instance instead",
                     Py_TYPE(obj)->tp_name);
        return 0;
    }
    return proxy_get_parent((PgBufproxyObject *)obj, 0);
}

static int
PgBufproxy_Trip(PyObject *obj)
{
    if (!PyObject_IsInstance (obj, (PyObject *)&PgBufproxy_Type)) {
        PyErr_Format(PyExc_TypeError,
                     "Expected a BufferProxy object: got %s instance instead",
                     Py_TYPE(obj)->tp_name);
        return -1;
    }
    return _proxy_get_view((PgBufproxyObject *)obj) ? 0 : -1;
}

/*DOC*/ static char bufferproxy_doc[] =
/*DOC*/    "exports BufferProxy, a generic wrapper object for an py_buffer";

MODINIT_DEFINE(bufferproxy)
{
    PyObject *module;
    PyObject *apiobj;
    static void* c_api[PYGAMEAPI_BUFPROXY_NUMSLOTS];

#if PY3
    static struct PyModuleDef _module = {
        PyModuleDef_HEAD_INIT,
        PROXY_MODNAME,
        bufferproxy_doc,
        -1,
        bufferproxy_methods,
        NULL, NULL, NULL, NULL
    };
#endif

    /* imported needed apis */
    import_pygame_base();
    if (PyErr_Occurred()) {
        MODINIT_ERROR;
    }

    /* prepare exported types */
    if (PyType_Ready(&PgBufproxy_Type) < 0) {
        MODINIT_ERROR;
    }

#define bufferproxy_docs ""

    /* create the module */
#if PY3
    module = PyModule_Create(&_module);
#else
    module = Py_InitModule3(MODPREFIX PROXY_MODNAME, bufferproxy_methods,
                            bufferproxy_doc);
#endif

    Py_INCREF(&PgBufproxy_Type);
    if (PyModule_AddObject(module,
                           PROXY_TYPE_NAME,
                           (PyObject *)&PgBufproxy_Type)) {
        Py_DECREF(&PgBufproxy_Type);
        DECREF_MOD(module);
        MODINIT_ERROR;
    }
#if PYGAMEAPI_BUFPROXY_NUMSLOTS != 4
#error export slot count mismatch
#endif
    c_api[0] = &PgBufproxy_Type;
    c_api[1] = PgBufproxy_New;
    c_api[2] = PgBufproxy_GetParent;
    c_api[3] = PgBufproxy_Trip;
    apiobj = encapsulate_api(c_api, PROXY_MODNAME);
    if (apiobj == NULL) {
        DECREF_MOD(module);
        MODINIT_ERROR;
    }
    if (PyModule_AddObject(module, PYGAMEAPI_LOCAL_ENTRY, apiobj)) {
        Py_DECREF(apiobj);
        DECREF_MOD(module);
        MODINIT_ERROR;
    }
    MODINIT_RETURN(module);
}
