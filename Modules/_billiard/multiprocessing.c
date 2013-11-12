/*
 * Extension module used by multiprocessing package
 *
 * multiprocessing.c
 *
 * Copyright (c) 2006-2008, R Oudkerk
 * Licensed to PSF under a Contributor Agreement.
 */

#include "multiprocessing.h"

#ifdef SCM_RIGHTS
    #define HAVE_FD_TRANSFER 1
#else
    #define HAVE_FD_TRANSFER 0
#endif

PyObject *create_win32_namespace(void);

PyObject *Billiard_pickle_dumps;
PyObject *Billiard_pickle_loads;
PyObject *Billiard_pickle_protocol;
PyObject *Billiard_BufferTooShort;

/*
 * Function which raises exceptions based on error codes
 */

PyObject *
Billiard_SetError(PyObject *Type, int num)
{
    switch (num) {
    case MP_SUCCESS:
        break;
#ifdef MS_WINDOWS
    case MP_STANDARD_ERROR:
        if (Type == NULL)
            Type = PyExc_WindowsError;
        PyErr_SetExcFromWindowsErr(Type, 0);
        break;
    case MP_SOCKET_ERROR:
        if (Type == NULL)
            Type = PyExc_WindowsError;
        PyErr_SetExcFromWindowsErr(Type, WSAGetLastError());
        break;
#else /* !MS_WINDOWS */
    case MP_STANDARD_ERROR:
    case MP_SOCKET_ERROR:
        if (Type == NULL)
            Type = PyExc_OSError;
        PyErr_SetFromErrno(Type);
        break;
#endif /* !MS_WINDOWS */
    case MP_MEMORY_ERROR:
        PyErr_NoMemory();
        break;
    case MP_END_OF_FILE:
        PyErr_SetNone(PyExc_EOFError);
        break;
    case MP_EARLY_END_OF_FILE:
        PyErr_SetString(PyExc_IOError,
                        "got end of file during message");
        break;
    case MP_BAD_MESSAGE_LENGTH:
        PyErr_SetString(PyExc_IOError, "bad message length");
        break;
    case MP_EXCEPTION_HAS_BEEN_SET:
        break;
    default:
        PyErr_Format(PyExc_RuntimeError,
                     "unkown error number %d", num);
    }
    return NULL;
}


/*
 * Windows only
 */

#ifdef MS_WINDOWS

/* On Windows we set an event to signal Ctrl-C; compare with timemodule.c */

HANDLE sigint_event = NULL;

static BOOL WINAPI
ProcessingCtrlHandler(DWORD dwCtrlType)
{
    SetEvent(sigint_event);
    return FALSE;
}

/*
 * Unix only
 */

#else /* !MS_WINDOWS */

#if HAVE_FD_TRANSFER

/* Functions for transferring file descriptors between processes.
   Reimplements some of the functionality of the fdcred
   module at http://www.mca-ltd.com/resources/fdcred_1.tgz. */

static PyObject *
Billiard_multiprocessing_sendfd(PyObject *self, PyObject *args)
{
    int conn, fd, res;
    char dummy_char;
    char buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    struct iovec dummy_iov;
    struct cmsghdr *cmsg;

    if (!PyArg_ParseTuple(args, "ii", &conn, &fd))
        return NULL;

    dummy_iov.iov_base = &dummy_char;
    dummy_iov.iov_len = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);
    msg.msg_iov = &dummy_iov;
    msg.msg_iovlen = 1;
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    msg.msg_controllen = cmsg->cmsg_len;
    *(int*)CMSG_DATA(cmsg) = fd;

    Py_BEGIN_ALLOW_THREADS
    res = sendmsg(conn, &msg, 0);
    Py_END_ALLOW_THREADS

    if (res < 0)
        return PyErr_SetFromErrno(PyExc_OSError);
    Py_RETURN_NONE;
}

static PyObject *
Billiard_multiprocessing_recvfd(PyObject *self, PyObject *args)
{
    int conn, fd, res;
    char dummy_char;
    char buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    struct iovec dummy_iov;
    struct cmsghdr *cmsg;

    if (!PyArg_ParseTuple(args, "i", &conn))
        return NULL;

    dummy_iov.iov_base = &dummy_char;
    dummy_iov.iov_len = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);
    msg.msg_iov = &dummy_iov;
    msg.msg_iovlen = 1;
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    msg.msg_controllen = cmsg->cmsg_len;

    Py_BEGIN_ALLOW_THREADS
    res = recvmsg(conn, &msg, 0);
    Py_END_ALLOW_THREADS

    if (res < 0)
        return PyErr_SetFromErrno(PyExc_OSError);

    fd = *(int*)CMSG_DATA(cmsg);
    return Py_BuildValue("i", fd);
}

#endif /* HAVE_FD_TRANSFER */

#endif /* !MS_WINDOWS */


/*
 * All platforms
 */

static PyObject*
Billiard_multiprocessing_address_of_buffer(PyObject *self, PyObject *obj)
{
    void *buffer;
    Py_ssize_t buffer_len;

    if (PyObject_AsWriteBuffer(obj, &buffer, &buffer_len) < 0)
        return NULL;

    return Py_BuildValue("N" F_PY_SSIZE_T,
                         PyLong_FromVoidPtr(buffer), buffer_len);
}

/* readv() */

#if defined(HAVE_READV)

static Py_ssize_t
_Billiard_iov_setup(struct iovec **iov, Py_buffer **buf, PyObject *seq,
                    int cnt, int type)
{
    int i, j;
    Py_ssize_t blen, total = 0;

    *iov = PyMem_New(struct iovec, cnt);
    if (*iov == NULL) {
        PyErr_NoMemory();
        return total;
    }

    *buf = PyMem_New(Py_buffer, cnt);
    if (*buf == NULL) {
        PyMem_Del(*iov);
        PyErr_NoMemory();
        return total;
    }

    for (i = 0; i < cnt; i++) {
        PyObject *item = PySequence_GetItem(seq, i);
        if (item == NULL)
            goto fail;
        if (PyObject_GetBuffer(item, &(*buf)[i], type) == -1) {
            Py_DECREF(item);
            goto fail;
        }
        Py_DECREF(item);
        (*iov)[i].iov_base = (*buf)[i].buf;
        blen = (*buf)[i].len;
        (*iov)[i].iov_len = blen;
        total += blen;
    }
    return total;

fail:
    PyMem_Del(*iov);
    for (j = 0; j < i; j++) {
        PyBuffer_Release(&(*buf)[j]);
    }
    PyMem_Del(*buf);
    return 0;
}

static void
_Billiard_iov_cleanup(struct iovec *iov, Py_buffer *buf, int cnt)
{
    int i;
    PyMem_Del(iov);
    for (i = 0; i < cnt; i++) {
        PyBuffer_Release(&buf[i]);
    }
    PyMem_Del(buf);
}


static PyObject*
Billiard_readv(PyObject *self, PyObject *args)
{
    int fd, cnt;
    Py_ssize_t n;
    PyObject *seq;
    struct iovec *iov;
    Py_buffer *buf;

    if (!PyArg_ParseTuple(args, "iO", &fd, &seq))
        return NULL;
    if (!PySequence_Check(seq)) {
        PyErr_SetString(PyExc_TypeError,
            "readv() arg 2 must be a sequence");
        return NULL;
    }
    cnt = PySequence_Size(seq);

    if (!_Billiard_iov_setup(&iov, &buf, seq, cnt, PyBUF_WRITABLE))
        return NULL;

    Py_BEGIN_ALLOW_THREADS
    n = readv(fd, iov, cnt);
    Py_END_ALLOW_THREADS

    _Billiard_iov_cleanup(iov, buf, cnt);
    if (n < 0)
        return PyErr_SetFromErrno(PyExc_OSError);
    return PyLong_FromSsize_t(n);
}


#endif /* HAVE_READV */


#if !defined(MS_WINDOWS)

static PyObject *
Billiard_read(PyObject *self, PyObject *args)
{
    int fd;
    Py_buffer view;
    Py_ssize_t buflen, recvlen = 0;

    char *buf = NULL;

    Py_ssize_t n = 0;

    if (!PyArg_ParseTuple(args, "iw*|n", &fd, &view, &recvlen))
        return NULL;
    buflen = view.len;
    buf = view.buf;

    if (recvlen < 0) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "negative len for read");
        return NULL;
    }

    if (recvlen == 0) {
        recvlen = buflen;
    }

    if (buflen < recvlen) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError,
            "Buffer too small for requested bytes");
        return NULL;

    }

    if (buflen < 0 || buflen == 0) {
        errno = EINVAL;
        goto bail;
    }
    // Requires Python 2.7
    //if (!_PyVerify_fd(fd)) goto bail;

    Py_BEGIN_ALLOW_THREADS
    n = read(fd, buf, recvlen);
    Py_END_ALLOW_THREADS
    if (n < 0) goto bail;
    PyBuffer_Release(&view);
    return PyInt_FromSsize_t(n);

bail:
    PyBuffer_Release(&view);
    return PyErr_SetFromErrno(PyExc_OSError);
}

# endif /* !MS_WINDOWS */



/*
 * Function table
 */

static PyMethodDef Billiard_module_methods[] = {
    {"address_of_buffer", Billiard_multiprocessing_address_of_buffer, METH_O,
     "address_of_buffer(obj) -> int\n\n"
     "Return address of obj assuming obj supports buffer inteface"},
#if HAVE_FD_TRANSFER
    {"sendfd", Billiard_multiprocessing_sendfd, METH_VARARGS,
     "sendfd(sockfd, fd) -> None\n\n"
     "Send file descriptor given by fd over the unix domain socket\n"
     "whose file decriptor is sockfd"},
    {"recvfd", Billiard_multiprocessing_recvfd, METH_VARARGS,
     "recvfd(sockfd) -> fd\n\n"
     "Receive a file descriptor over a unix domain socket\n"
     "whose file decriptor is sockfd"},
#endif
#if !defined(MS_WINDOWS)
    {"read", Billiard_read, METH_VARARGS,
     "read(fd, buffer) -> bytes\n\n"
     "Read from file descriptor into buffer."},
#endif
#if HAVE_READV
    {"readv", Billiard_readv, METH_VARARGS,
    "readv(fd, buffers) -> bytesread\n\n"
    "Read from a file descriptor into a number of writable buffers.\n"
    "buffers is an arbitrary sequence of writable buffers.\n\n"
    "Returns the total number of bytes read."},
#endif
    {NULL}
};


/*
 * Initialize
 */

PyMODINIT_FUNC
init_billiard(void)
{
    PyObject *module, *temp, *value;

    /* Initialize module */
    module = Py_InitModule("_billiard", Billiard_module_methods);
    if (!module)
        return;

    /* Get copy of objects from pickle */
    temp = PyImport_ImportModule(PICKLE_MODULE);
    if (!temp)
        return;
    Billiard_pickle_dumps = PyObject_GetAttrString(temp, "dumps");
    Billiard_pickle_loads = PyObject_GetAttrString(temp, "loads");
    Billiard_pickle_protocol = PyObject_GetAttrString(temp, "HIGHEST_PROTOCOL");
    Py_XDECREF(temp);

    /* Get copy of BufferTooShort */
    temp = PyImport_ImportModule("billiard");
    if (!temp)
        return;
    Billiard_BufferTooShort = PyObject_GetAttrString(temp, "BufferTooShort");
    Py_XDECREF(temp);

    /* Add connection type to module */
    if (PyType_Ready(&BilliardConnectionType) < 0)
        return;
    Py_INCREF(&BilliardConnectionType);
    PyModule_AddObject(module, "Connection", (PyObject*)&BilliardConnectionType);

#if defined(MS_WINDOWS) ||                                              \
  (defined(HAVE_SEM_OPEN) && !defined(POSIX_SEMAPHORES_NOT_ENABLED))
    /* Add SemLock type to module */
    if (PyType_Ready(&BilliardSemLockType) < 0)
        return;
    Py_INCREF(&BilliardSemLockType);
    PyDict_SetItemString(BilliardSemLockType.tp_dict, "SEM_VALUE_MAX",
                         Py_BuildValue("i", SEM_VALUE_MAX));
    PyModule_AddObject(module, "SemLock", (PyObject*)&BilliardSemLockType);
#endif

#ifdef MS_WINDOWS
    /* Add PipeConnection to module */
    if (PyType_Ready(&BilliardPipeConnectionType) < 0)
        return;
    Py_INCREF(&BilliardPipeConnectionType);
    PyModule_AddObject(module, "PipeConnection",
                       (PyObject*)&BilliardPipeConnectionType);

    /* Initialize win32 class and add to multiprocessing */
    temp = create_win32_namespace();
    if (!temp)
        return;
    PyModule_AddObject(module, "win32", temp);

    /* Initialize the event handle used to signal Ctrl-C */
    sigint_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!sigint_event) {
        PyErr_SetFromWindowsErr(0);
        return;
    }
    if (!SetConsoleCtrlHandler(ProcessingCtrlHandler, TRUE)) {
        PyErr_SetFromWindowsErr(0);
        return;
    }
#endif

    /* Add configuration macros */
    temp = PyDict_New();
    if (!temp)
        return;
#define ADD_FLAG(name)                                            \
    value = Py_BuildValue("i", name);                             \
    if (value == NULL) { Py_DECREF(temp); return; }               \
    if (PyDict_SetItemString(temp, #name, value) < 0) {           \
        Py_DECREF(temp); Py_DECREF(value); return; }              \
    Py_DECREF(value)

#if defined(HAVE_SEM_OPEN) && !defined(POSIX_SEMAPHORES_NOT_ENABLED)
    ADD_FLAG(HAVE_SEM_OPEN);
#endif
#ifdef HAVE_SEM_TIMEDWAIT
    ADD_FLAG(HAVE_SEM_TIMEDWAIT);
#endif
#ifdef HAVE_FD_TRANSFER
    ADD_FLAG(HAVE_FD_TRANSFER);
#endif
#ifdef HAVE_BROKEN_SEM_GETVALUE
    ADD_FLAG(HAVE_BROKEN_SEM_GETVALUE);
#endif
#ifdef HAVE_BROKEN_SEM_UNLINK
    ADD_FLAG(HAVE_BROKEN_SEM_UNLINK);
#endif
    if (PyModule_AddObject(module, "flags", temp) < 0)
        return;
}
