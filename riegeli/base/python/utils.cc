// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// From https://docs.python.org/3/c-api/intro.html:
// Since Python may define some pre-processor definitions which affect the
// standard headers on some systems, you must include Python.h before any
// standard headers are included.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "riegeli/base/python/utils.h"

#include <stddef.h>
#include <stdint.h>
#include <limits>
#include <string>

#include "absl/base/optimization.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/utility/utility.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"

namespace riegeli {
namespace python {

Exception& Exception::operator=(const Exception& that) noexcept {
  PythonLock lock;
  Py_XINCREF(that.type_.get());
  type_.reset(that.type_.get());
  Py_XINCREF(that.value_.get());
  value_.reset(that.value_.get());
  Py_XINCREF(that.traceback_.get());
  traceback_.reset(that.traceback_.get());
  return *this;
}

Exception Exception::Fetch() {
  PythonLock::AssertHeld();
  PyObject* type;
  PyObject* value;
  PyObject* traceback;
  PyErr_Fetch(&type, &value, &traceback);
  PyErr_NormalizeException(&type, &value, &traceback);
  return Exception(type, value, traceback);
}

PyObject* Exception::Restore() const& {
  PythonLock::AssertHeld();
  Py_XINCREF(type_.get());
  Py_XINCREF(value_.get());
  Py_XINCREF(traceback_.get());
  PyErr_Restore(type_.get(), value_.get(), traceback_.get());
  return nullptr;
}

PyObject* Exception::Restore() && {
  PythonLock::AssertHeld();
  PyErr_Restore(type_.release(), value_.release(), traceback_.release());
  return nullptr;
}

std::string Exception::message() const {
  if (ok()) return "Healthy";
  PythonLock lock;
  RIEGELI_ASSERT(PyExceptionClass_Check(type_.get()))
      << "Expected an exception class, not " << Py_TYPE(type_.get())->tp_name;
  std::string message = PyExceptionClass_Name(type_.get());
  if (value_ == nullptr) return message;
  const PythonPtr str_result(PyObject_Str(value_.get()));
  if (ABSL_PREDICT_FALSE(str_result == nullptr)) {
    PyErr_Clear();
    absl::StrAppend(&message, ": <str() failed>");
    return message;
  }
  TextOrBytes str;
  if (ABSL_PREDICT_FALSE(!str.FromPython(str_result.get()))) {
    PyErr_Clear();
    absl::StrAppend(&message, ": <TextOrBytes::FromPython() failed>");
    return message;
  }
  if (!str.data().empty()) absl::StrAppend(&message, ": ", str.data());
  return message;
}

void SetRiegeliError(absl::string_view message) {
  PythonLock::AssertHeld();
  static constexpr ImportedConstant kRiegeliError(
      "riegeli.base.python.riegeli_error", "RiegeliError");
  if (ABSL_PREDICT_FALSE(!kRiegeliError.Verify())) return;
  PyObject* const type = kRiegeliError.get();
  PythonPtr value = StringToPython(message);
  if (ABSL_PREDICT_FALSE(value == nullptr)) return;
  Py_INCREF(type);
  PyErr_Restore(type, value.release(), nullptr);
}

namespace internal {

namespace {

// A linked list of all objects of type StaticObject which have value_
// allocated, chained by their next_ fields. This is used to free the objects
// on Python interpreter shutdown.
const StaticObject* all_static_objects = nullptr;

}  // namespace

void FreeStaticObjectsImpl() {
  const StaticObject* static_object =
      absl::exchange(all_static_objects, nullptr);
  while (static_object != nullptr) {
    Py_DECREF(static_object->value_);
    static_object->value_ = nullptr;
    static_object = absl::exchange(static_object->next_, nullptr);
  }
}

namespace {

// extern "C" for a calling convention compatible with Py_AtExit().
extern "C" void FreeStaticObjects() { FreeStaticObjectsImpl(); }

}  // namespace

void StaticObject::RegisterThis() const {
  PythonLock::AssertHeld();
  if (all_static_objects == nullptr) {
    // This is the first registered StaticObject since Py_Initialize().
    Py_AtExit(FreeStaticObjects);
  }
  next_ = absl::exchange(all_static_objects, this);
}

bool ImportedCapsuleBase::ImportValue() const {
  // For some reason PyImport_ImportModule() is sometimes required before
  // PyCapsule_Import() for a module with a nested name.
  const size_t dot = absl::string_view(capsule_name_).rfind('.');
  RIEGELI_ASSERT_NE(dot, absl::string_view::npos)
      << "Capsule name does not contain a dot: " << capsule_name_;
  RIEGELI_CHECK(
      PyImport_ImportModule(std::string(capsule_name_, dot).c_str()) != nullptr)
      << Exception::Fetch().message();
  value_ = PyCapsule_Import(capsule_name_, false);
  return value_ != nullptr;
}

}  // namespace internal

bool Identifier::AllocateValue() const {
  value_ = StringToPython(name_).release();
  if (ABSL_PREDICT_FALSE(value_ == nullptr)) return false;
#if PY_MAJOR_VERSION >= 3
  PyUnicode_InternInPlace(&value_);
#else
  PyString_InternInPlace(&value_);
#endif
  RegisterThis();
  return true;
}

bool ImportedConstant::AllocateValue() const {
  const PythonPtr module_name = StringToPython(module_name_);
  if (ABSL_PREDICT_FALSE(module_name == nullptr)) return false;
  const PythonPtr module(PyImport_Import(module_name.get()));
  if (ABSL_PREDICT_FALSE(module == nullptr)) return false;
  const PythonPtr attr_name = StringToPython(attr_name_);
  if (ABSL_PREDICT_FALSE(attr_name == nullptr)) return false;
  value_ = PyObject_GetAttr(module.get(), attr_name.get());
  if (ABSL_PREDICT_FALSE(value_ == nullptr)) return false;
  RegisterThis();
  return true;
}

bool ExportCapsule(PyObject* module, const char* capsule_name,
                   const void* ptr) {
  PythonPtr capsule(
      PyCapsule_New(const_cast<void*>(ptr), capsule_name, nullptr));
  if (ABSL_PREDICT_FALSE(capsule == nullptr)) return false;
  const size_t dot = absl::string_view(capsule_name).rfind('.');
  RIEGELI_ASSERT_NE(dot, absl::string_view::npos)
      << "Capsule name does not contain a dot: " << capsule_name;
  RIEGELI_ASSERT(PyModule_Check(module))
      << "Expected a module, not " << Py_TYPE(module)->tp_name;
  RIEGELI_ASSERT_EQ(absl::string_view(PyModule_GetName(module)),
                    absl::string_view(capsule_name, dot))
      << "Module name mismatch";
  if (ABSL_PREDICT_FALSE(PyModule_AddObject(module, capsule_name + dot + 1,
                                            capsule.release()) < 0)) {
    return false;
  }
  return true;
}

bool TextOrBytes::FromPython(PyObject* object) {
  // TODO: Change this depending on how
  // https://bugs.python.org/issue35295 is resolved.
  if (PyUnicode_Check(object)) {
#if PY_VERSION_HEX >= 0x03030000
    Py_ssize_t length;
    const char* data = PyUnicode_AsUTF8AndSize(object, &length);
    if (ABSL_PREDICT_FALSE(data == nullptr)) return false;
    data_ = absl::string_view(data, length);
    return true;
#else
    utf8_.reset(PyUnicode_AsUTF8String(object));
    if (ABSL_PREDICT_FALSE(utf8_ == nullptr)) return false;
    object = utf8_.get();
#endif
  } else if (ABSL_PREDICT_FALSE(!PyBytes_Check(object))) {
    PyErr_Format(PyExc_TypeError,
                 "Expected "
#if PY_MAJOR_VERSION >= 3
                 "str or bytes"
#else
                 "str or unicode"
#endif
                 ", not %s",
                 Py_TYPE(object)->tp_name);
    return false;
  }
  data_ =
      absl::string_view(PyBytes_AS_STRING(object), PyBytes_GET_SIZE(object));
  return true;
}

PythonPtr ChainToPython(const Chain& value) {
  PythonPtr bytes(
      PyBytes_FromStringAndSize(nullptr, IntCast<Py_ssize_t>(value.size())));
  if (ABSL_PREDICT_FALSE(bytes == nullptr)) return nullptr;
  value.CopyTo(PyBytes_AS_STRING(bytes.get()));
  return bytes;
}

bool ChainFromPython(PyObject* object, Chain* value) {
  Py_buffer buffer;
  if (ABSL_PREDICT_FALSE(PyObject_GetBuffer(object, &buffer, PyBUF_CONTIG_RO) <
                         0)) {
    return false;
  }
  value->Clear();
  value->Append(absl::string_view(static_cast<const char*>(buffer.buf),
                                  IntCast<size_t>(buffer.len)),
                IntCast<size_t>(buffer.len));
  PyBuffer_Release(&buffer);
  return true;
}

PythonPtr SizeToPython(size_t value) {
#if PY_MAJOR_VERSION < 3
  if (ABSL_PREDICT_TRUE(
          value <= IntCast<unsigned long>(std::numeric_limits<long>::max()))) {
    return PythonPtr(PyInt_FromLong(IntCast<long>(value)));
  }
#endif
  if (ABSL_PREDICT_FALSE(value >
                         std::numeric_limits<unsigned long long>::max())) {
    PyErr_Format(PyExc_OverflowError, "Size out of range: %zu", value);
    return nullptr;
  }
  return PythonPtr(
      PyLong_FromUnsignedLongLong(IntCast<unsigned long long>(value)));
}

bool SizeFromPython(PyObject* object, size_t* value) {
  const PythonPtr index(PyNumber_Index(object));
  if (ABSL_PREDICT_FALSE(index == nullptr)) return false;
#if PY_MAJOR_VERSION < 3
  if (ABSL_PREDICT_TRUE(PyInt_Check(index.get()))) {
    const long index_value = PyInt_AS_LONG(index.get());
    if (ABSL_PREDICT_FALSE(index_value < 0)) {
      PyErr_Format(PyExc_OverflowError, "Size out of range: %ld", index_value);
      return false;
    }
    if (ABSL_PREDICT_FALSE(IntCast<unsigned long>(index_value) >
                           std::numeric_limits<size_t>::max())) {
      PyErr_Format(PyExc_OverflowError, "Size out of range: %ld", index_value);
      return false;
    }
    *value = IntCast<size_t>(index_value);
    return true;
  }
#endif
  RIEGELI_ASSERT(PyLong_Check(index.get()))
      << "PyNumber_Index() returned an unexpected type: "
      << Py_TYPE(index.get())->tp_name;
  unsigned long long index_value = PyLong_AsUnsignedLongLong(index.get());
  if (ABSL_PREDICT_FALSE(index_value == static_cast<unsigned long long>(-1)) &&
      PyErr_Occurred()) {
    return false;
  }
  if (ABSL_PREDICT_FALSE(index_value > std::numeric_limits<size_t>::max())) {
    PyErr_Format(PyExc_OverflowError, "Size out of range: %llu", index_value);
    return false;
  }
  *value = IntCast<size_t>(index_value);
  return true;
}

PythonPtr PositionToPython(Position value) {
#if PY_MAJOR_VERSION < 3
  if (ABSL_PREDICT_TRUE(
          value <= IntCast<unsigned long>(std::numeric_limits<long>::max()))) {
    return PythonPtr(PyInt_FromLong(IntCast<long>(value)));
  }
#endif
  if (ABSL_PREDICT_FALSE(value >
                         std::numeric_limits<unsigned long long>::max())) {
    PyErr_Format(PyExc_OverflowError, "Position out of range: %ju",
                 uintmax_t{value});
    return nullptr;
  }
  return PythonPtr(
      PyLong_FromUnsignedLongLong(IntCast<unsigned long long>(value)));
}

bool PositionFromPython(PyObject* object, Position* value) {
  const PythonPtr index(PyNumber_Index(object));
  if (ABSL_PREDICT_FALSE(index == nullptr)) return false;
#if PY_MAJOR_VERSION < 3
  if (ABSL_PREDICT_TRUE(PyInt_Check(index.get()))) {
    const long index_value = PyInt_AS_LONG(index.get());
    if (ABSL_PREDICT_FALSE(index_value < 0)) {
      PyErr_Format(PyExc_OverflowError, "Position out of range: %ld",
                   index_value);
      return false;
    }
    if (ABSL_PREDICT_FALSE(IntCast<unsigned long>(index_value) >
                           std::numeric_limits<Position>::max())) {
      PyErr_Format(PyExc_OverflowError, "Position out of range: %ld",
                   index_value);
      return false;
    }
    *value = IntCast<Position>(index_value);
    return true;
  }
#endif
  RIEGELI_ASSERT(PyLong_Check(index.get()))
      << "PyNumber_Index() returned an unexpected type: "
      << Py_TYPE(index.get())->tp_name;
  unsigned long long index_value = PyLong_AsUnsignedLongLong(index.get());
  if (ABSL_PREDICT_FALSE(index_value == static_cast<unsigned long long>(-1)) &&
      PyErr_Occurred()) {
    return false;
  }
  if (ABSL_PREDICT_FALSE(index_value > std::numeric_limits<Position>::max())) {
    PyErr_Format(PyExc_OverflowError, "Position out of range: %llu",
                 index_value);
    return false;
  }
  *value = IntCast<Position>(index_value);
  return true;
}

}  // namespace python
}  // namespace riegeli
