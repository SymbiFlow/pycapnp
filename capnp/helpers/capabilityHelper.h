#pragma once

#include "capnp/dynamic.h"
#include <stdexcept>
#include "Python.h"
#include <iostream>

extern "C" {
   PyObject * wrap_remote_call(PyObject * func, capnp::Response<capnp::DynamicStruct> &);
   PyObject * wrap_dynamic_struct_reader(capnp::DynamicStruct::Reader &);
   ::kj::Promise<void> * call_server_method(PyObject * py_server, char * name, capnp::CallContext< capnp::DynamicStruct, capnp::DynamicStruct> & context);
   PyObject * wrap_kj_exception(kj::Exception &);
   PyObject * wrap_kj_exception_for_reraise(kj::Exception &);
   PyObject * get_exception_info(PyObject *, PyObject *, PyObject *);
   PyObject * convert_array_pyobject(kj::Array<PyObject *>&);
   ::kj::Promise<PyObject *> * extract_promise(PyObject *);
 }

void reraise_kj_exception() {
  try {
    if (PyErr_Occurred())
      ; // let the latest Python exn pass through and ignore the current one
    else
      throw;
  }
  catch (kj::Exception& exn) {
    auto obj = wrap_kj_exception_for_reraise(exn);
    PyErr_SetObject((PyObject*)obj->ob_type, obj);
  }
  catch (const std::exception& exn) {
    PyErr_SetString(PyExc_RuntimeError, exn.what());
  }
  catch (...)
  {
    PyErr_SetString(PyExc_RuntimeError, "Unknown exception");
  }
}

void check_py_error() {
    PyObject * err = PyErr_Occurred();
    if(err) {
        PyObject * ptype, *pvalue, *ptraceback;
        PyErr_Fetch(&ptype, &pvalue, &ptraceback);
        if(ptype == NULL || pvalue == NULL || ptraceback == NULL)
          throw kj::Exception(kj::Exception::Nature::OTHER, kj::Exception::Durability::PERMANENT, kj::heapString("capabilityHelper.h"), 44, kj::heapString("Unknown error occurred"));

        PyObject * info = get_exception_info(ptype, pvalue, ptraceback);

        PyObject * py_filename = PyTuple_GetItem(info, 0);
        kj::String filename(kj::heapString(PyBytes_AsString(py_filename)));

        PyObject * py_line = PyTuple_GetItem(info, 1);
        int line = PyInt_AsLong(py_line);

        PyObject * py_description = PyTuple_GetItem(info, 2);
        kj::String description(kj::heapString(PyBytes_AsString(py_description)));

        Py_DECREF(ptype);
        Py_DECREF(pvalue);
        Py_DECREF(ptraceback);
        Py_DECREF(info);
        PyErr_Clear();

        throw kj::Exception(kj::Exception::Nature::OTHER, kj::Exception::Durability::PERMANENT, kj::mv(filename), line, kj::mv(description));
    }
}

// TODO: need to decref error_func as well on successful run
kj::Promise<PyObject *> wrapPyFunc(PyObject * func, PyObject * arg) {
    PyObject * result = PyObject_CallFunctionObjArgs(func, arg, NULL);
    Py_DECREF(func);

    check_py_error();

    auto promise = extract_promise(result);
    if(promise != NULL)
      return kj::mv(*promise);
    return result;
}

kj::Promise<PyObject *> wrapPyFuncNoArg(PyObject * func) {
    PyObject * result = PyObject_CallFunctionObjArgs(func, NULL);
    Py_DECREF(func);

    check_py_error();

    auto promise = extract_promise(result);
    if(promise != NULL)
      return kj::mv(*promise);
    return result;
}

kj::Promise<PyObject *> wrapRemoteCall(PyObject * func, capnp::Response<capnp::DynamicStruct> & arg) {
    PyObject * ret = wrap_remote_call(func, arg);

    check_py_error();

    auto promise = extract_promise(ret);
    if(promise != NULL)
      return kj::mv(*promise);
    return ret;
}

::kj::Promise<PyObject *> then(kj::Promise<PyObject *> & promise, PyObject * func, PyObject * error_func) {
  if(error_func == Py_None)
    return promise.then([func](PyObject * arg) { return wrapPyFunc(func, arg); } );
  else
    return promise.then([func](PyObject * arg) { return wrapPyFunc(func, arg); } 
                                     , [error_func](kj::Exception arg) { return wrapPyFunc(error_func, wrap_kj_exception(arg)); } );
}

::kj::Promise<PyObject *> then(::capnp::RemotePromise< ::capnp::DynamicStruct> & promise, PyObject * func, PyObject * error_func) {
  if(error_func == Py_None)
    return promise.then([func](capnp::Response<capnp::DynamicStruct>&& arg) { return wrapRemoteCall(func, arg); } );
  else
    return promise.then([func](capnp::Response<capnp::DynamicStruct>&& arg) { return  wrapRemoteCall(func, arg); } 
                                     , [error_func](kj::Exception arg) { return wrapPyFunc(error_func, wrap_kj_exception(arg)); } );
}

::kj::Promise<PyObject *> then(kj::Promise<void> & promise, PyObject * func, PyObject * error_func) {
  if(error_func == Py_None)
    return promise.then([func]() { return wrapPyFuncNoArg(func); } );
  else
    return promise.then([func]() { return wrapPyFuncNoArg(func); } 
                                     , [error_func](kj::Exception arg) { return wrapPyFunc(error_func, wrap_kj_exception(arg)); } );
}

::kj::Promise<PyObject *> then(kj::Promise<kj::Array<PyObject *> > && promise) {
  return promise.then([](kj::Array<PyObject *>&& arg) { return convert_array_pyobject(arg); } );
}

class PythonInterfaceDynamicImpl final: public capnp::DynamicCapability::Server {
public:
  PyObject * py_server;

  PythonInterfaceDynamicImpl(capnp::InterfaceSchema & schema, PyObject * _py_server)
      : capnp::DynamicCapability::Server(schema), py_server(_py_server) {
        Py_INCREF(_py_server);
      }

  ~PythonInterfaceDynamicImpl() {
    Py_DECREF(py_server);
  }

  kj::Promise<void> call(capnp::InterfaceSchema::Method method,
                         capnp::CallContext< capnp::DynamicStruct, capnp::DynamicStruct> context) {
    auto methodName = method.getProto().getName();

    kj::Promise<void> * promise = call_server_method(py_server, const_cast<char *>(methodName.cStr()), context);

    check_py_error();

    if(promise == nullptr)
      return kj::READY_NOW;

    kj::Promise<void> ret(kj::mv(*promise));
    delete promise;
    return ret;
  }
};

capnp::DynamicCapability::Client new_client(capnp::InterfaceSchema & schema, PyObject * server) {
  return capnp::DynamicCapability::Client(kj::heap<PythonInterfaceDynamicImpl>(schema, server));
}
capnp::DynamicValue::Reader new_server(capnp::InterfaceSchema & schema, PyObject * server) {
  return capnp::DynamicValue::Reader(kj::heap<PythonInterfaceDynamicImpl>(schema, server));
}

capnp::Capability::Client server_to_client(capnp::InterfaceSchema & schema, PyObject * server) {
  return kj::heap<PythonInterfaceDynamicImpl>(schema, server);
}

::kj::Promise<PyObject *> convert_to_pypromise(capnp::RemotePromise<capnp::DynamicStruct> & promise) {
    return promise.then([](capnp::Response<capnp::DynamicStruct>&& response) { return wrap_dynamic_struct_reader(response); } );
}

::kj::Promise<PyObject *> convert_to_pypromise(kj::Promise<void> & promise) {
    return promise.then([]() { Py_RETURN_NONE;} );
}

template<class T>
::kj::Promise<void> convert_to_voidpromise(kj::Promise<T> & promise) {
    return promise.then([](T) { } );
}