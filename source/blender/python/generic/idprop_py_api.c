/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup pygen
 */

#include <Python.h>

#include "MEM_guardedalloc.h"

#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "idprop_py_api.h"

#include "BKE_idprop.h"

#include "DNA_ID.h" /* ID property definitions. */
#include "RNA_access.h"
#include "RNA_enum_types.h"

#define USE_STRING_COERCE

#ifdef USE_STRING_COERCE
#  include "py_capi_utils.h"
#endif

#include "python_utildefines.h"

extern bool pyrna_id_FromPyObject(PyObject *obj, ID **id);
extern PyObject *pyrna_id_CreatePyObject(ID *id);
extern bool pyrna_id_CheckPyObject(PyObject *obj);

/* -------------------------------------------------------------------- */
/** \name Python from ID-Property (Internal Conversions)
 *
 * Low level conversion to avoid duplicate code, no type checking.
 * \{ */

static PyObject *idprop_py_from_idp_string(const IDProperty *prop)
{
  if (prop->subtype == IDP_STRING_SUB_BYTE) {
    return PyBytes_FromStringAndSize(IDP_String(prop), prop->len);
  }

#ifdef USE_STRING_COERCE
  return PyC_UnicodeFromByteAndSize(IDP_Array(prop), prop->len - 1);
#else
  return PyUnicode_FromStringAndSize(IDP_String(prop), prop->len - 1);
#endif
}

static PyObject *idprop_py_from_idp_int(const IDProperty *prop)
{
  return PyLong_FromLong((long)IDP_Int(prop));
}

static PyObject *idprop_py_from_idp_float(const IDProperty *prop)
{
  return PyFloat_FromDouble((double)IDP_Float(prop));
}

static PyObject *idprop_py_from_idp_double(const IDProperty *prop)
{
  return PyFloat_FromDouble(IDP_Double(prop));
}

static PyObject *idprop_py_from_idp_group(ID *id, IDProperty *prop, IDProperty *parent)
{
  BPy_IDProperty *group = PyObject_New(BPy_IDProperty, &BPy_IDGroup_Type);
  group->id = id;
  group->prop = prop;
  group->parent = parent; /* can be NULL */
  return (PyObject *)group;
}

static PyObject *idprop_py_from_idp_id(IDProperty *prop)
{
  return pyrna_id_CreatePyObject(prop->data.pointer);
}

static PyObject *idprop_py_from_idp_array(ID *id, IDProperty *prop)
{
  BPy_IDProperty *array = PyObject_New(BPy_IDProperty, &BPy_IDArray_Type);
  array->id = id;
  array->prop = prop;
  return (PyObject *)array;
}

static PyObject *idprop_py_from_idp_idparray(ID *id, IDProperty *prop)
{
  PyObject *seq = PyList_New(prop->len);
  IDProperty *array = IDP_IDPArray(prop);
  int i;

  if (!seq) {
    PyErr_Format(
        PyExc_RuntimeError, "%s: IDP_IDPARRAY: PyList_New(%d) failed", __func__, prop->len);
    return NULL;
  }

  for (i = 0; i < prop->len; i++) {
    PyObject *wrap = BPy_IDGroup_WrapData(id, array++, prop);

    /* BPy_IDGroup_MapDataToPy sets the error */
    if (UNLIKELY(wrap == NULL)) {
      Py_DECREF(seq);
      return NULL;
    }

    PyList_SET_ITEM(seq, i, wrap);
  }

  return seq;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name IDProp Group Access
 * \{ */

/* use for both array and group */
static Py_hash_t BPy_IDGroup_hash(BPy_IDProperty *self)
{
  return _Py_HashPointer(self->prop);
}

static PyObject *BPy_IDGroup_repr(BPy_IDProperty *self)
{
  return PyUnicode_FromFormat("<bpy id prop: owner=\"%s\", name=\"%s\", address=%p>",
                              self->id ? self->id->name : "<NONE>",
                              self->prop->name,
                              self->prop);
}

PyObject *BPy_IDGroup_WrapData(ID *id, IDProperty *prop, IDProperty *parent)
{
  switch (prop->type) {
    case IDP_STRING:
      return idprop_py_from_idp_string(prop);
    case IDP_INT:
      return idprop_py_from_idp_int(prop);
    case IDP_FLOAT:
      return idprop_py_from_idp_float(prop);
    case IDP_DOUBLE:
      return idprop_py_from_idp_double(prop);
    case IDP_GROUP:
      return idprop_py_from_idp_group(id, prop, parent);
    case IDP_ARRAY:
      return idprop_py_from_idp_array(id, prop);
    case IDP_IDPARRAY:
      return idprop_py_from_idp_idparray(id, prop); /* this could be better a internal type */
    case IDP_ID:
      return idprop_py_from_idp_id(prop);
    default:
      Py_RETURN_NONE;
  }
}

/* UNUSED, currently assignment overwrites into new properties, rather than setting in-place. */
#if 0
static int BPy_IDGroup_SetData(BPy_IDProperty *self, IDProperty *prop, PyObject *value)
{
  switch (prop->type) {
    case IDP_STRING: {
      char *st;
      if (!PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "expected a string!");
        return -1;
      }
      /* NOTE: if this code is enabled, bytes support needs to be added */
#  ifdef USE_STRING_COERCE
      {
        int alloc_len;
        PyObject *value_coerce = NULL;

        st = (char *)PyC_UnicodeAsByte(value, &value_coerce);
        alloc_len = strlen(st) + 1;

        st = _PyUnicode_AsString(value);
        IDP_ResizeArray(prop, alloc_len);
        memcpy(IDP_Array(prop), st, alloc_len);
        Py_XDECREF(value_coerce);
      }
#  else
      st = _PyUnicode_AsString(value);
      IDP_ResizeArray(prop, strlen(st) + 1);
      strcpy(IDP_Array(prop), st);
#  endif

      return 0;
    }

    case IDP_INT: {
      int ivalue = PyLong_AsSsize_t(value);
      if (ivalue == -1 && PyErr_Occurred()) {
        PyErr_SetString(PyExc_TypeError, "expected an int type");
        return -1;
      }
      IDP_Int(prop) = ivalue;
      break;
    }
    case IDP_FLOAT: {
      float fvalue = (float)PyFloat_AsDouble(value);
      if (fvalue == -1 && PyErr_Occurred()) {
        PyErr_SetString(PyExc_TypeError, "expected a float");
        return -1;
      }
      IDP_Float(self->prop) = fvalue;
      break;
    }
    case IDP_DOUBLE: {
      double dvalue = PyFloat_AsDouble(value);
      if (dvalue == -1 && PyErr_Occurred()) {
        PyErr_SetString(PyExc_TypeError, "expected a float");
        return -1;
      }
      IDP_Double(self->prop) = dvalue;
      break;
    }
    default:
      PyErr_SetString(PyExc_AttributeError, "attempt to set read-only attribute!");
      return -1;
  }
  return 0;
}
#endif

static PyObject *BPy_IDGroup_GetName(BPy_IDProperty *self, void *UNUSED(closure))
{
  return PyUnicode_FromString(self->prop->name);
}

static int BPy_IDGroup_SetName(BPy_IDProperty *self, PyObject *value, void *UNUSED(closure))
{
  const char *name;
  Py_ssize_t name_size;

  if (!PyUnicode_Check(value)) {
    PyErr_SetString(PyExc_TypeError, "expected a string!");
    return -1;
  }

  name = _PyUnicode_AsStringAndSize(value, &name_size);

  if (name_size >= MAX_IDPROP_NAME) {
    PyErr_SetString(PyExc_TypeError, "string length cannot exceed 63 characters!");
    return -1;
  }

  memcpy(self->prop->name, name, name_size);
  return 0;
}

#if 0
static PyObject *BPy_IDGroup_GetType(BPy_IDProperty *self)
{
  return PyLong_FromLong(self->prop->type);
}
#endif

static PyGetSetDef BPy_IDGroup_getseters[] = {
    {"name",
     (getter)BPy_IDGroup_GetName,
     (setter)BPy_IDGroup_SetName,
     "The name of this Group.",
     NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

static Py_ssize_t BPy_IDGroup_Map_Len(BPy_IDProperty *self)
{
  if (self->prop->type != IDP_GROUP) {
    PyErr_SetString(PyExc_TypeError, "len() of unsized object");
    return -1;
  }

  return self->prop->len;
}

static PyObject *BPy_IDGroup_Map_GetItem(BPy_IDProperty *self, PyObject *item)
{
  IDProperty *idprop;
  const char *name;

  if (self->prop->type != IDP_GROUP) {
    PyErr_SetString(PyExc_TypeError, "unsubscriptable object");
    return NULL;
  }

  name = _PyUnicode_AsString(item);

  if (name == NULL) {
    PyErr_SetString(PyExc_TypeError, "only strings are allowed as keys of ID properties");
    return NULL;
  }

  idprop = IDP_GetPropertyFromGroup(self->prop, name);

  if (idprop == NULL) {
    PyErr_SetString(PyExc_KeyError, "key not in subgroup dict");
    return NULL;
  }

  return BPy_IDGroup_WrapData(self->id, idprop, self->prop);
}

/* returns NULL on success, error string on failure */
static char idp_sequence_type(PyObject *seq_fast)
{
  PyObject **seq_fast_items = PySequence_Fast_ITEMS(seq_fast);
  PyObject *item;
  char type = IDP_INT;

  Py_ssize_t i, len = PySequence_Fast_GET_SIZE(seq_fast);

  for (i = 0; i < len; i++) {
    item = seq_fast_items[i];
    if (PyFloat_Check(item)) {
      if (type == IDP_IDPARRAY) { /* mixed dict/int */
        return -1;
      }
      type = IDP_DOUBLE;
    }
    else if (PyLong_Check(item)) {
      if (type == IDP_IDPARRAY) { /* mixed dict/int */
        return -1;
      }
    }
    else if (PyMapping_Check(item)) {
      if (i != 0 && (type != IDP_IDPARRAY)) { /* mixed dict/int */
        return -1;
      }
      type = IDP_IDPARRAY;
    }
    else {
      return -1;
    }
  }

  return type;
}

static const char *idp_try_read_name(PyObject *name_obj)
{
  const char *name = NULL;
  if (name_obj) {
    Py_ssize_t name_size;
    name = _PyUnicode_AsStringAndSize(name_obj, &name_size);

    if (name == NULL) {
      PyErr_Format(PyExc_KeyError,
                   "invalid id-property key, expected a string, not a %.200s",
                   Py_TYPE(name_obj)->tp_name);
      return NULL;
    }

    if (name_size >= MAX_IDPROP_NAME) {
      PyErr_SetString(PyExc_KeyError,
                      "the length of IDProperty names is limited to 63 characters");
      return NULL;
    }
  }
  else {
    name = "";
  }
  return name;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID-Property from Python (Internal Conversions)
 * \{ */

/**
 * The 'idp_from_Py*' functions expect that the input type has been checked before
 * and return NULL if the IDProperty can't be created.
 */

static IDProperty *idp_from_PyFloat(const char *name, PyObject *ob)
{
  IDPropertyTemplate val = {0};
  val.d = PyFloat_AsDouble(ob);
  return IDP_New(IDP_DOUBLE, &val, name);
}

static IDProperty *idp_from_PyLong(const char *name, PyObject *ob)
{
  IDPropertyTemplate val = {0};
  val.i = PyC_Long_AsI32(ob);
  if (val.i == -1 && PyErr_Occurred()) {
    return NULL;
  }
  return IDP_New(IDP_INT, &val, name);
}

static IDProperty *idp_from_PyUnicode(const char *name, PyObject *ob)
{
  IDProperty *prop;
  IDPropertyTemplate val = {0};
#ifdef USE_STRING_COERCE
  Py_ssize_t value_size;
  PyObject *value_coerce = NULL;
  val.string.str = PyC_UnicodeAsByteAndSize(ob, &value_size, &value_coerce);
  val.string.len = (int)value_size + 1;
  val.string.subtype = IDP_STRING_SUB_UTF8;
  prop = IDP_New(IDP_STRING, &val, name);
  Py_XDECREF(value_coerce);
#else
  val.str = _PyUnicode_AsString(ob);
  prop = IDP_New(IDP_STRING, val, name);
#endif
  return prop;
}

static IDProperty *idp_from_PyBytes(const char *name, PyObject *ob)
{
  IDPropertyTemplate val = {0};
  val.string.str = PyBytes_AS_STRING(ob);
  val.string.len = PyBytes_GET_SIZE(ob);
  val.string.subtype = IDP_STRING_SUB_BYTE;
  return IDP_New(IDP_STRING, &val, name);
}

static int idp_array_type_from_formatstr_and_size(const char *typestr, Py_ssize_t itemsize)
{
  const char format = PyC_StructFmt_type_from_str(typestr);

  if (PyC_StructFmt_type_is_float_any(format)) {
    if (itemsize == 4) {
      return IDP_FLOAT;
    }
    if (itemsize == 8) {
      return IDP_DOUBLE;
    }
  }
  if (PyC_StructFmt_type_is_int_any(format)) {
    if (itemsize == 4) {
      return IDP_INT;
    }
  }

  return -1;
}

static const char *idp_format_from_array_type(int type)
{
  if (type == IDP_INT) {
    return "i";
  }
  if (type == IDP_FLOAT) {
    return "f";
  }
  if (type == IDP_DOUBLE) {
    return "d";
  }
  return NULL;
}

static IDProperty *idp_from_PySequence_Buffer(const char *name, Py_buffer *buffer)
{
  IDProperty *prop;
  IDPropertyTemplate val = {0};

  const int id_type = idp_array_type_from_formatstr_and_size(buffer->format, buffer->itemsize);
  if (id_type == -1) {
    /* should never happen as the type has been checked before */
    return NULL;
  }

  val.array.type = id_type;
  val.array.len = buffer->len / buffer->itemsize;

  prop = IDP_New(IDP_ARRAY, &val, name);
  memcpy(IDP_Array(prop), buffer->buf, buffer->len);
  return prop;
}

static IDProperty *idp_from_PySequence_Fast(const char *name, PyObject *ob)
{
  IDProperty *prop;
  IDPropertyTemplate val = {0};

  PyObject **ob_seq_fast_items;
  PyObject *item;
  int i;

  ob_seq_fast_items = PySequence_Fast_ITEMS(ob);

  if ((val.array.type = idp_sequence_type(ob)) == (char)-1) {
    PyErr_SetString(PyExc_TypeError,
                    "only floats, ints and dicts are allowed in ID property arrays");
    return NULL;
  }

  /* validate sequence and derive type.
   * we assume IDP_INT unless we hit a float
   * number; then we assume it's */

  val.array.len = PySequence_Fast_GET_SIZE(ob);

  switch (val.array.type) {
    case IDP_DOUBLE: {
      double *prop_data;
      prop = IDP_New(IDP_ARRAY, &val, name);
      prop_data = IDP_Array(prop);
      for (i = 0; i < val.array.len; i++) {
        item = ob_seq_fast_items[i];
        if (((prop_data[i] = PyFloat_AsDouble(item)) == -1.0) && PyErr_Occurred()) {
          return NULL;
        }
      }
      break;
    }
    case IDP_INT: {
      int *prop_data;
      prop = IDP_New(IDP_ARRAY, &val, name);
      prop_data = IDP_Array(prop);
      for (i = 0; i < val.array.len; i++) {
        item = ob_seq_fast_items[i];
        if (((prop_data[i] = PyC_Long_AsI32(item)) == -1) && PyErr_Occurred()) {
          return NULL;
        }
      }
      break;
    }
    case IDP_IDPARRAY: {
      prop = IDP_NewIDPArray(name);
      for (i = 0; i < val.array.len; i++) {
        item = ob_seq_fast_items[i];
        if (BPy_IDProperty_Map_ValidateAndCreate(NULL, prop, item) == false) {
          return NULL;
        }
      }
      break;
    }
    default:
      /* should never happen */
      PyErr_SetString(PyExc_RuntimeError, "internal error with idp array.type");
      return NULL;
  }
  return prop;
}

static IDProperty *idp_from_PySequence(const char *name, PyObject *ob)
{
  Py_buffer buffer;
  bool use_buffer = false;

  if (PyObject_CheckBuffer(ob)) {
    PyObject_GetBuffer(ob, &buffer, PyBUF_SIMPLE | PyBUF_FORMAT);
    const char format = PyC_StructFmt_type_from_str(buffer.format);
    if (PyC_StructFmt_type_is_float_any(format) ||
        (PyC_StructFmt_type_is_int_any(format) && buffer.itemsize == 4)) {
      use_buffer = true;
    }
    else {
      PyBuffer_Release(&buffer);
    }
  }

  if (use_buffer) {
    IDProperty *prop = idp_from_PySequence_Buffer(name, &buffer);
    PyBuffer_Release(&buffer);
    return prop;
  }

  PyObject *ob_seq_fast = PySequence_Fast(ob, "py -> idprop");
  if (ob_seq_fast != NULL) {
    IDProperty *prop = idp_from_PySequence_Fast(name, ob_seq_fast);
    Py_DECREF(ob_seq_fast);
    return prop;
  }

  return NULL;
}

static IDProperty *idp_from_PyMapping(const char *name, PyObject *ob)
{
  IDProperty *prop;
  const IDPropertyTemplate val = {0};

  PyObject *keys, *vals, *key, *pval;
  int i, len;
  /* yay! we get into recursive stuff now! */
  keys = PyMapping_Keys(ob);
  vals = PyMapping_Values(ob);

  /* we allocate the group first; if we hit any invalid data,
   * we can delete it easily enough.*/
  prop = IDP_New(IDP_GROUP, &val, name);
  len = PyMapping_Length(ob);
  for (i = 0; i < len; i++) {
    key = PySequence_GetItem(keys, i);
    pval = PySequence_GetItem(vals, i);
    if (BPy_IDProperty_Map_ValidateAndCreate(key, prop, pval) == false) {
      IDP_FreeProperty(prop);
      Py_XDECREF(keys);
      Py_XDECREF(vals);
      Py_XDECREF(key);
      Py_XDECREF(pval);
      /* error is already set */
      return NULL;
    }
    Py_XDECREF(key);
    Py_XDECREF(pval);
  }
  Py_XDECREF(keys);
  Py_XDECREF(vals);
  return prop;
}

static IDProperty *idp_from_DatablockPointer(const char *name, PyObject *ob)
{
  IDPropertyTemplate val = {0};
  pyrna_id_FromPyObject(ob, &val.id);
  return IDP_New(IDP_ID, &val, name);
}

static IDProperty *idp_from_PyObject(PyObject *name_obj, PyObject *ob)
{
  const char *name = idp_try_read_name(name_obj);
  if (name == NULL) {
    return NULL;
  }

  if (PyFloat_Check(ob)) {
    return idp_from_PyFloat(name, ob);
  }
  if (PyLong_Check(ob)) {
    return idp_from_PyLong(name, ob);
  }
  if (PyUnicode_Check(ob)) {
    return idp_from_PyUnicode(name, ob);
  }
  if (PyBytes_Check(ob)) {
    return idp_from_PyBytes(name, ob);
  }
  if (PySequence_Check(ob)) {
    return idp_from_PySequence(name, ob);
  }
  if (ob == Py_None || pyrna_id_CheckPyObject(ob)) {
    return idp_from_DatablockPointer(name, ob);
  }
  if (PyMapping_Check(ob)) {
    return idp_from_PyMapping(name, ob);
  }

  PyErr_Format(
      PyExc_TypeError, "invalid id-property type %.200s not supported", Py_TYPE(ob)->tp_name);
  return NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mapping Get/Set (Internal Access)
 * \{ */

/**
 * \note group can be a pointer array or a group.
 * assume we already checked key is a string.
 *
 * \return success.
 */
bool BPy_IDProperty_Map_ValidateAndCreate(PyObject *name_obj, IDProperty *group, PyObject *ob)
{
  IDProperty *prop = idp_from_PyObject(name_obj, ob);
  if (prop == NULL) {
    return false;
  }

  if (group->type == IDP_IDPARRAY) {
    IDP_AppendArray(group, prop);
    /* IDP_AppendArray does a shallow copy (memcpy), only free memory */
    MEM_freeN(prop);
  }
  else {
    IDProperty *prop_exist;

    /* avoid freeing when types match in case they are referenced by the UI, see: T37073
     * obviously this isn't a complete solution, but helps for common cases. */
    prop_exist = IDP_GetPropertyFromGroup(group, prop->name);
    if ((prop_exist != NULL) && (prop_exist->type == prop->type) &&
        (prop_exist->subtype == prop->subtype)) {
      /* Preserve prev/next links!!! See T42593. */
      prop->prev = prop_exist->prev;
      prop->next = prop_exist->next;
      prop->flag = prop_exist->flag;

      IDP_FreePropertyContent(prop_exist);
      *prop_exist = *prop;
      MEM_freeN(prop);
    }
    else {
      IDP_ReplaceInGroup_ex(group, prop, prop_exist);
    }
  }

  return true;
}

int BPy_Wrap_SetMapItem(IDProperty *prop, PyObject *key, PyObject *val)
{
  if (prop->type != IDP_GROUP) {
    PyErr_SetString(PyExc_TypeError, "unsubscriptable object");
    return -1;
  }

  if (val == NULL) { /* del idprop[key] */
    IDProperty *pkey;
    const char *name = _PyUnicode_AsString(key);

    if (name == NULL) {
      PyErr_Format(PyExc_KeyError, "expected a string, not %.200s", Py_TYPE(key)->tp_name);
      return -1;
    }

    pkey = IDP_GetPropertyFromGroup(prop, name);
    if (pkey) {
      IDP_FreeFromGroup(prop, pkey);
      return 0;
    }

    PyErr_SetString(PyExc_KeyError, "property not found in group");
    return -1;
  }

  bool ok;

  ok = BPy_IDProperty_Map_ValidateAndCreate(key, prop, val);
  if (ok == false) {
    return -1;
  }

  return 0;
}

static int BPy_IDGroup_Map_SetItem(BPy_IDProperty *self, PyObject *key, PyObject *val)
{
  return BPy_Wrap_SetMapItem(self->prop, key, val);
}

static PyObject *BPy_IDGroup_iter(BPy_IDProperty *self)
{
  BPy_IDGroup_Iter *iter = PyObject_New(BPy_IDGroup_Iter, &BPy_IDGroup_Iter_Type);
  iter->group = self;
  iter->mode = IDPROP_ITER_KEYS;
  iter->cur = self->prop->data.group.first;
  Py_XINCREF(iter);
  return (PyObject *)iter;
}

/* for simple, non nested types this is the same as BPy_IDGroup_WrapData */
static PyObject *BPy_IDGroup_MapDataToPy(IDProperty *prop)
{
  switch (prop->type) {
    case IDP_STRING:
      return idprop_py_from_idp_string(prop);
    case IDP_INT:
      return idprop_py_from_idp_int(prop);
    case IDP_FLOAT:
      return idprop_py_from_idp_float(prop);
    case IDP_DOUBLE:
      return idprop_py_from_idp_double(prop);
    case IDP_ID:
      return idprop_py_from_idp_id(prop);
    case IDP_ARRAY: {
      PyObject *seq = PyList_New(prop->len);
      int i;

      if (!seq) {
        PyErr_Format(
            PyExc_RuntimeError, "%s: IDP_ARRAY: PyList_New(%d) failed", __func__, prop->len);
        return NULL;
      }

      switch (prop->subtype) {
        case IDP_FLOAT: {
          const float *array = (float *)IDP_Array(prop);
          for (i = 0; i < prop->len; i++) {
            PyList_SET_ITEM(seq, i, PyFloat_FromDouble(array[i]));
          }
          break;
        }
        case IDP_DOUBLE: {
          const double *array = (double *)IDP_Array(prop);
          for (i = 0; i < prop->len; i++) {
            PyList_SET_ITEM(seq, i, PyFloat_FromDouble(array[i]));
          }
          break;
        }
        case IDP_INT: {
          const int *array = (int *)IDP_Array(prop);
          for (i = 0; i < prop->len; i++) {
            PyList_SET_ITEM(seq, i, PyLong_FromLong(array[i]));
          }
          break;
        }
        default:
          PyErr_Format(
              PyExc_RuntimeError, "%s: invalid/corrupt array type '%d'!", __func__, prop->subtype);
          Py_DECREF(seq);
          return NULL;
      }

      return seq;
    }
    case IDP_IDPARRAY: {
      PyObject *seq = PyList_New(prop->len);
      IDProperty *array = IDP_IDPArray(prop);
      int i;

      if (!seq) {
        PyErr_Format(
            PyExc_RuntimeError, "%s: IDP_IDPARRAY: PyList_New(%d) failed", __func__, prop->len);
        return NULL;
      }

      for (i = 0; i < prop->len; i++) {
        PyObject *wrap = BPy_IDGroup_MapDataToPy(array++);

        /* BPy_IDGroup_MapDataToPy sets the error */
        if (UNLIKELY(wrap == NULL)) {
          Py_DECREF(seq);
          return NULL;
        }

        PyList_SET_ITEM(seq, i, wrap);
      }
      return seq;
    }
    case IDP_GROUP: {
      PyObject *dict = _PyDict_NewPresized(prop->len);
      IDProperty *loop;

      for (loop = prop->data.group.first; loop; loop = loop->next) {
        PyObject *wrap = BPy_IDGroup_MapDataToPy(loop);

        /* BPy_IDGroup_MapDataToPy sets the error */
        if (UNLIKELY(wrap == NULL)) {
          Py_DECREF(dict);
          return NULL;
        }

        PyDict_SetItemString(dict, loop->name, wrap);
        Py_DECREF(wrap);
      }
      return dict;
    }
  }

  PyErr_Format(PyExc_RuntimeError,
               "%s ERROR: '%s' property exists with a bad type code '%d'!",
               __func__,
               prop->name,
               prop->type);
  return NULL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID-Property Group Methods
 * \{ */

PyDoc_STRVAR(
    BPy_IDGroup_pop_doc,
    ".. method:: pop(key, default)\n"
    "\n"
    "   Remove an item from the group, returning a Python representation.\n"
    "\n"
    "   :raises KeyError: When the item doesn't exist.\n"
    "\n"
    "   :arg key: Name of item to remove.\n"
    "   :type key: string\n"
    "   :arg default: Value to return when key isn't found, otherwise raise an exception.\n"
    "   :type default: Undefined\n");
static PyObject *BPy_IDGroup_pop(BPy_IDProperty *self, PyObject *args)
{
  IDProperty *idprop;
  PyObject *pyform;

  char *key;
  PyObject *def = NULL;

  if (!PyArg_ParseTuple(args, "s|O:get", &key, &def)) {
    return NULL;
  }

  idprop = IDP_GetPropertyFromGroup(self->prop, key);
  if (idprop == NULL) {
    if (def == NULL) {
      PyErr_SetString(PyExc_KeyError, "item not in group");
      return NULL;
    }
    return Py_INCREF_RET(def);
  }

  pyform = BPy_IDGroup_MapDataToPy(idprop);
  if (pyform == NULL) {
    /* ok something bad happened with the #PyObject,
     * so don't remove the prop from the group.  if pyform is
     * NULL, then it already should have raised an exception.*/
    return NULL;
  }

  IDP_RemoveFromGroup(self->prop, idprop);
  return pyform;
}

PyDoc_STRVAR(
    BPy_IDGroup_iter_items_doc,
    ".. method:: iteritems()\n"
    "\n"
    "   Iterate through the items in the dict; behaves like dictionary method iteritems.\n");
static PyObject *BPy_IDGroup_iter_items(BPy_IDProperty *self)
{
  BPy_IDGroup_Iter *iter = PyObject_New(BPy_IDGroup_Iter, &BPy_IDGroup_Iter_Type);
  iter->group = self;
  iter->mode = IDPROP_ITER_ITEMS;
  iter->cur = self->prop->data.group.first;
  Py_XINCREF(iter);
  return (PyObject *)iter;
}

/* utility function */
static void BPy_IDGroup_CorrectListLen(IDProperty *prop, PyObject *seq, int len, const char *func)
{
  int j;

  printf("%s: ID Property Error found and corrected!\n", func);

  /* fill rest of list with valid references to None */
  for (j = len; j < prop->len; j++) {
    PyList_SET_ITEM(seq, j, Py_INCREF_RET(Py_None));
  }

  /*set correct group length*/
  prop->len = len;
}

PyObject *BPy_Wrap_GetKeys(IDProperty *prop)
{
  PyObject *list = PyList_New(prop->len);
  IDProperty *loop;
  int i;

  for (i = 0, loop = prop->data.group.first; loop && (i < prop->len); loop = loop->next, i++) {
    PyList_SET_ITEM(list, i, PyUnicode_FromString(loop->name));
  }

  /* if the id prop is corrupt, count the remaining */
  for (; loop; loop = loop->next, i++) {
    /* pass */
  }

  if (i != prop->len) { /* if the loop didn't finish, we know the length is wrong */
    BPy_IDGroup_CorrectListLen(prop, list, i, __func__);
    Py_DECREF(list); /*free the list*/
    /*call self again*/
    return BPy_Wrap_GetKeys(prop);
  }

  return list;
}

PyObject *BPy_Wrap_GetValues(ID *id, IDProperty *prop)
{
  PyObject *list = PyList_New(prop->len);
  IDProperty *loop;
  int i;

  for (i = 0, loop = prop->data.group.first; loop; loop = loop->next, i++) {
    PyList_SET_ITEM(list, i, BPy_IDGroup_WrapData(id, loop, prop));
  }

  if (i != prop->len) {
    BPy_IDGroup_CorrectListLen(prop, list, i, __func__);
    Py_DECREF(list); /*free the list*/
    /*call self again*/
    return BPy_Wrap_GetValues(id, prop);
  }

  return list;
}

PyObject *BPy_Wrap_GetItems(ID *id, IDProperty *prop)
{
  PyObject *seq = PyList_New(prop->len);
  IDProperty *loop;
  int i;

  for (i = 0, loop = prop->data.group.first; loop; loop = loop->next, i++) {
    PyObject *item = PyTuple_New(2);
    PyTuple_SET_ITEMS(
        item, PyUnicode_FromString(loop->name), BPy_IDGroup_WrapData(id, loop, prop));
    PyList_SET_ITEM(seq, i, item);
  }

  if (i != prop->len) {
    BPy_IDGroup_CorrectListLen(prop, seq, i, __func__);
    Py_DECREF(seq); /*free the list*/
    /*call self again*/
    return BPy_Wrap_GetItems(id, prop);
  }

  return seq;
}

PyDoc_STRVAR(BPy_IDGroup_keys_doc,
             ".. method:: keys()\n"
             "\n"
             "   Return the keys associated with this group as a list of strings.\n");
static PyObject *BPy_IDGroup_keys(BPy_IDProperty *self)
{
  return BPy_Wrap_GetKeys(self->prop);
}

PyDoc_STRVAR(BPy_IDGroup_values_doc,
             ".. method:: values()\n"
             "\n"
             "   Return the values associated with this group.\n");
static PyObject *BPy_IDGroup_values(BPy_IDProperty *self)
{
  return BPy_Wrap_GetValues(self->id, self->prop);
}

PyDoc_STRVAR(BPy_IDGroup_items_doc,
             ".. method:: items()\n"
             "\n"
             "   Return the items associated with this group.\n");
static PyObject *BPy_IDGroup_items(BPy_IDProperty *self)
{
  return BPy_Wrap_GetItems(self->id, self->prop);
}

static int BPy_IDGroup_Contains(BPy_IDProperty *self, PyObject *value)
{
  const char *name = _PyUnicode_AsString(value);

  if (!name) {
    PyErr_Format(PyExc_TypeError, "expected a string, not a %.200s", Py_TYPE(value)->tp_name);
    return -1;
  }

  return IDP_GetPropertyFromGroup(self->prop, name) ? 1 : 0;
}

PyDoc_STRVAR(BPy_IDGroup_update_doc,
             ".. method:: update(other)\n"
             "\n"
             "   Update key, values.\n"
             "\n"
             "   :arg other: Updates the values in the group with this.\n"
             "   :type other: :class:`IDPropertyGroup` or dict\n");
static PyObject *BPy_IDGroup_update(BPy_IDProperty *self, PyObject *value)
{
  PyObject *pkey, *pval;
  Py_ssize_t i = 0;

  if (BPy_IDGroup_Check(value)) {
    BPy_IDProperty *other = (BPy_IDProperty *)value;
    if (UNLIKELY(self->prop == other->prop)) {
      Py_RETURN_NONE;
    }

    /* XXX, possible one is inside the other */
    IDP_MergeGroup(self->prop, other->prop, true);
  }
  else if (PyDict_Check(value)) {
    while (PyDict_Next(value, &i, &pkey, &pval)) {
      BPy_IDGroup_Map_SetItem(self, pkey, pval);
      if (PyErr_Occurred()) {
        return NULL;
      }
    }
  }
  else {
    PyErr_Format(PyExc_TypeError,
                 "expected a dict or an IDPropertyGroup type, not a %.200s",
                 Py_TYPE(value)->tp_name);
    return NULL;
  }

  Py_RETURN_NONE;
}

PyDoc_STRVAR(BPy_IDGroup_to_dict_doc,
             ".. method:: to_dict()\n"
             "\n"
             "   Return a purely python version of the group.\n");
static PyObject *BPy_IDGroup_to_dict(BPy_IDProperty *self)
{
  return BPy_IDGroup_MapDataToPy(self->prop);
}

PyDoc_STRVAR(BPy_IDGroup_clear_doc,
             ".. method:: clear()\n"
             "\n"
             "   Clear all members from this group.\n");
static PyObject *BPy_IDGroup_clear(BPy_IDProperty *self)
{
  IDP_ClearProperty(self->prop);
  Py_RETURN_NONE;
}

PyDoc_STRVAR(BPy_IDGroup_get_doc,
             ".. method:: get(key, default=None)\n"
             "\n"
             "   Return the value for key, if it exists, else default.\n");
static PyObject *BPy_IDGroup_get(BPy_IDProperty *self, PyObject *args)
{
  IDProperty *idprop;
  const char *key;
  PyObject *def = Py_None;

  if (!PyArg_ParseTuple(args, "s|O:get", &key, &def)) {
    return NULL;
  }

  idprop = IDP_GetPropertyFromGroup(self->prop, key);
  if (idprop) {
    PyObject *pyobj = BPy_IDGroup_WrapData(self->id, idprop, self->prop);
    if (pyobj) {
      return pyobj;
    }
  }

  Py_INCREF(def);
  return def;
}

static bool check_ui_data_value(PyObject *py_object)
{
  if (ELEM(py_object, NULL, Py_None)) {
    return false;
  }

  if (PyLong_Check(py_object) || PyFloat_Check(py_object)) {
    return true;
  }

  PyErr_SetString(PyExc_TypeError, "Property ui data values must be integers or floats");
  return false;
}

static int int_from_py_int_or_double(PyObject *py_object)
{
  if (PyLong_Check(py_object)) {
    return PyC_Long_AsI32(py_object);
  }
  if (PyFloat_Check(py_object)) {
    return (int)PyFloat_AsDouble(py_object);
  }

  BLI_assert(false);
  return 0;
}

static void idprop_update_rna_ui_data_int(IDProperty *idprop,
                                          PyObject *py_min,
                                          PyObject *py_max,
                                          PyObject *py_soft_min,
                                          PyObject *py_soft_max,
                                          PyObject *py_step,
                                          PyObject *py_default_value)
{
  IDPropertyUIDataInt *ui_data = (IDPropertyUIDataInt *)idprop->ui_data;
  if (check_ui_data_value(py_min)) {
    ui_data->min = int_from_py_int_or_double(py_min);
    ui_data->soft_min = MAX2(ui_data->soft_min, ui_data->min);
  }
  if (check_ui_data_value(py_max)) {
    ui_data->max = int_from_py_int_or_double(py_max);
    ui_data->soft_max = MIN2(ui_data->soft_max, ui_data->max);
  }
  if (check_ui_data_value(py_soft_min)) {
    ui_data->soft_min = int_from_py_int_or_double(py_soft_min);
    ui_data->soft_min = MAX2(ui_data->soft_min, ui_data->min);
  }
  if (check_ui_data_value(py_soft_max)) {
    ui_data->soft_max = int_from_py_int_or_double(py_soft_max);
    ui_data->soft_max = MIN2(ui_data->soft_max, ui_data->max);
  }
  if (check_ui_data_value(py_step)) {
    ui_data->step = int_from_py_int_or_double(py_step);
  }
  if (!ELEM(py_default_value, NULL, Py_None)) {
    if (PySequence_Check(py_default_value)) {
      if (idprop->type != IDP_ARRAY) {
        PyErr_SetString(PyExc_TypeError, "Only array properties can have array default values");
      }
      else {
        MEM_SAFE_FREE(ui_data->default_array);
        PyObject **ob_seq_fast_items = PySequence_Fast_ITEMS(py_default_value);
        Py_ssize_t len = PySequence_Fast_GET_SIZE(py_default_value);

        ui_data->default_array_len = len;
        ui_data->default_array = MEM_malloc_arrayN(len, sizeof(int), __func__);
        for (Py_ssize_t i = 0; i < len; i++) {
          PyObject *item = ob_seq_fast_items[i];
          if (check_ui_data_value(item)) {
            ui_data->default_array[i] = int_from_py_int_or_double(item);
          }
        }
      }
    }
    else if (check_ui_data_value(py_default_value)) {
      ui_data->default_value = PyC_Long_AsI32(py_default_value);
    }
  }
}

static double double_from_py_int_or_double(PyObject *py_object)
{
  if (PyLong_Check(py_object)) {
    return (double)PyC_Long_AsI32(py_object);
  }
  if (PyFloat_Check(py_object)) {
    return PyFloat_AsDouble(py_object);
  }

  BLI_assert(false);
  return 0.0;
}

static void idprop_update_rna_ui_data_float(IDProperty *idprop,
                                            PyObject *py_min,
                                            PyObject *py_max,
                                            PyObject *py_soft_min,
                                            PyObject *py_soft_max,
                                            PyObject *py_step,
                                            PyObject *py_precision,
                                            PyObject *py_default_value)
{
  IDPropertyUIDataFloat *ui_data = (IDPropertyUIDataFloat *)idprop->ui_data;
  if (check_ui_data_value(py_min)) {
    ui_data->min = double_from_py_int_or_double(py_min);
    ui_data->soft_min = MAX2(ui_data->soft_min, ui_data->min);
  }
  if (check_ui_data_value(py_max)) {
    ui_data->max = double_from_py_int_or_double(py_max);
    ui_data->soft_max = MIN2(ui_data->soft_max, ui_data->max);
  }
  if (check_ui_data_value(py_soft_min)) {
    ui_data->soft_min = double_from_py_int_or_double(py_soft_min);
    ui_data->soft_min = MAX2(ui_data->soft_min, ui_data->min);
  }
  if (check_ui_data_value(py_soft_max)) {
    ui_data->soft_max = double_from_py_int_or_double(py_soft_max);
    ui_data->soft_max = MIN2(ui_data->soft_max, ui_data->max);
  }
  if (check_ui_data_value(py_step)) {
    ui_data->step = (float)double_from_py_int_or_double(py_step);
  }
  if (check_ui_data_value(py_precision)) {
    ui_data->precision = (float)double_from_py_int_or_double(py_precision);
  }
  if (!ELEM(py_default_value, NULL, Py_None)) {
    if (PySequence_Check(py_default_value)) {
      if (idprop->type != IDP_ARRAY) {
        PyErr_SetString(PyExc_TypeError, "Only array properties can have array default values");
      }
      else {
        MEM_SAFE_FREE(ui_data->default_array);
        PyObject **ob_seq_fast_items = PySequence_Fast_ITEMS(py_default_value);
        Py_ssize_t len = PySequence_Fast_GET_SIZE(py_default_value);

        ui_data->default_array_len = len;
        ui_data->default_array = MEM_malloc_arrayN(len, sizeof(double), __func__);
        for (Py_ssize_t i = 0; i < len; i++) {
          PyObject *item = ob_seq_fast_items[i];
          if (check_ui_data_value(item)) {
            ui_data->default_array[i] = double_from_py_int_or_double(item);
          }
        }
      }
    }
    else if (check_ui_data_value(py_default_value)) {
      ui_data->default_value = double_from_py_int_or_double(py_default_value);
    }
  }
}
static void idprop_update_rna_ui_data_string(IDProperty *idprop, PyObject *py_default_value)
{
  IDPropertyUIDataString *ui_data = (IDPropertyUIDataString *)idprop->ui_data;

  if (!ELEM(py_default_value, NULL, Py_None)) {
    if (PyUnicode_Check(py_default_value)) {
      ui_data->default_value = BLI_strdup(_PyUnicode_AsString(py_default_value));
    }
    else {
      PyErr_SetString(PyExc_TypeError, "String property defult value must have a string type");
    }
  }
}

PyDoc_STRVAR(BPy_IDGroup_update_rna_doc,
             ".. method:: update_rna(key, "
             "                       subtype=None "
             "                       min=None, "
             "                       max=None, "
             "                       soft_min=None, "
             "                       soft_max=None, "
             "                       precision=None, "
             "                       step=None, "
             "                       default_value=None, "
             "                       description=None)\n"
             "\n"
             "   Update the RNA type information of the IDProperty used for interaction and\n"
             "drawing in the user interface. The property specified by the key must be a direct\n"
             "child of the group. The required types for many of the keyword arguments depend on\n"
             "the type of the property.\n ");
static PyObject *BPy_IDGroup_update_rna(BPy_IDProperty *self, PyObject *args, PyObject *kwargs)
{
  const char *key;
  PyObject *rna_subtype = NULL;
  PyObject *min = NULL;
  PyObject *max = NULL;
  PyObject *soft_min = NULL;
  PyObject *soft_max = NULL;
  PyObject *precision = NULL;
  PyObject *step = NULL;
  PyObject *default_value = NULL;
  PyObject *description = NULL;

  static const char *kwlist[] = {"key",
                                 "subtype",
                                 "min",
                                 "max",
                                 "soft_min",
                                 "soft_max",
                                 "precision",
                                 "step",
                                 "default",
                                 "description",
                                 NULL};

  if (!PyArg_ParseTupleAndKeywords(args,
                                   kwargs,
                                   "s|OOOOOOOOO:update_rna",
                                   (char **)kwlist,
                                   &key,
                                   &rna_subtype,
                                   &min,
                                   &max,
                                   &soft_min,
                                   &soft_max,
                                   &precision,
                                   &step,
                                   &default_value,
                                   &description)) {
    return NULL;
  }

  IDProperty *idprop = IDP_GetPropertyFromGroup(self->prop, key);
  if (idprop == NULL) {
    PyErr_Format(PyExc_KeyError, "Property \"%s\" not found in IDProperty group", key);
    return NULL;
  }

  if (!IDP_supports_ui_data(idprop)) {
    PyErr_SetString(
        PyExc_ValueError,
        "RNA UI data is only supported for string, integer, float, or double properties");
    return NULL;
  }

  IDP_ui_data_ensure(idprop);

  if (!ELEM(rna_subtype, NULL, Py_None)) {
    if (PyUnicode_Check(rna_subtype)) {
      const char *subtype_string = _PyUnicode_AsString(rna_subtype);
      int result = PROP_NONE;
      RNA_enum_value_from_id(rna_enum_property_subtype_items, subtype_string, &result);
      idprop->ui_data->rna_subtype = result;
    }
    else if (rna_subtype != Py_None) {
      PyErr_SetString(PyExc_TypeError, "RNA subtype must be a string object");
    }
  }

  if (!ELEM(description, NULL, Py_None)) {
    if (PyUnicode_Check(description)) {
      idprop->ui_data->description = BLI_strdup(_PyUnicode_AsString(description));
    }
    else if (description != Py_None) {
      PyErr_SetString(PyExc_TypeError, "Property description must be a string object");
    }
  }

  /* Type specific data. */
  switch (IDP_ui_data_type(idprop)) {
    case IDP_UI_DATA_TYPE_STRING:
      idprop_update_rna_ui_data_string(idprop, default_value);
      break;
    case IDP_UI_DATA_TYPE_INT:
      idprop_update_rna_ui_data_int(idprop, min, max, soft_min, soft_max, step, default_value);
      break;
    case IDP_UI_DATA_TYPE_FLOAT:
      idprop_update_rna_ui_data_float(
          idprop, min, max, soft_min, soft_max, step, precision, default_value);
      break;
    case IDP_UI_DATA_TYPE_UNSUPPORTED:
      BLI_assert(false);
      break;
  }

  Py_RETURN_NONE;
}

static void idprop_ui_data_to_dict_int(IDProperty *idprop, PyObject *dict)
{
  IDPropertyUIDataInt *ui_data = (IDPropertyUIDataInt *)idprop->ui_data;
  PyObject *item;

  PyDict_SetItemString(dict, "min", item = PyLong_FromLong(ui_data->min));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "max", item = PyLong_FromLong(ui_data->max));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "soft_min", item = PyLong_FromLong(ui_data->soft_min));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "soft_max", item = PyLong_FromLong(ui_data->soft_max));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "step", item = PyLong_FromLong(ui_data->step));
  Py_DECREF(item);
  if (idprop->type == IDP_ARRAY) {
    PyObject *list = PyList_New(ui_data->default_array_len);
    for (int i = 0; i < ui_data->default_array_len; i++) {
      PyList_SET_ITEM(list, i, PyLong_FromLong(ui_data->default_array[i]));
    }
    PyDict_SetItemString(dict, "default_value", list);
    Py_DECREF(list);
  }
  else {
    PyDict_SetItemString(dict, "default_value", item = PyLong_FromLong(ui_data->step));
    Py_DECREF(item);
  }
}

static void idprop_ui_data_to_dict_float(IDProperty *idprop, PyObject *dict)
{
  IDPropertyUIDataFloat *ui_data = (IDPropertyUIDataFloat *)idprop->ui_data;
  PyObject *item;

  PyDict_SetItemString(dict, "min", item = PyFloat_FromDouble(ui_data->min));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "max", item = PyFloat_FromDouble(ui_data->max));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "soft_min", item = PyFloat_FromDouble(ui_data->soft_min));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "soft_max", item = PyFloat_FromDouble(ui_data->soft_max));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "step", item = PyFloat_FromDouble((double)ui_data->step));
  Py_DECREF(item);
  PyDict_SetItemString(dict, "precision", item = PyFloat_FromDouble((double)ui_data->precision));
  Py_DECREF(item);
  if (idprop->type == IDP_ARRAY) {
    PyObject *list = PyList_New(ui_data->default_array_len);
    for (int i = 0; i < ui_data->default_array_len; i++) {
      PyList_SET_ITEM(list, i, PyFloat_FromDouble(ui_data->default_array[i]));
    }
    PyDict_SetItemString(dict, "default_value", list);
    Py_DECREF(list);
  }
  else {
    PyDict_SetItemString(dict, "default_value", item = PyFloat_FromDouble(ui_data->step));
    Py_DECREF(item);
  }
}

static void idprop_ui_data_to_dict_string(IDProperty *idprop, PyObject *dict)
{
  IDPropertyUIDataString *ui_data = (IDPropertyUIDataString *)idprop->ui_data;
  PyObject *item;

  PyDict_SetItemString(dict, "default_value", item = PyUnicode_FromString(ui_data->default_value));
  Py_DECREF(item);
}

PyDoc_STRVAR(BPy_IDGroup_rna_ui_data_doc,
             ".. method:: rna_data(key)\n"
             "\n"
             "   Return a dictionary of the property's RNA UI data. The fields in the "
             "returned dictionary and their types will depend on the property's type.\n");
static PyObject *BPy_IDGroup_rna_ui_data(BPy_IDProperty *self, PyObject *args)
{
  const char *key;

  if (!PyArg_ParseTuple(args, "s:rna_data", &key)) {
    Py_RETURN_NONE;
  }

  IDProperty *idprop = IDP_GetPropertyFromGroup(self->prop, key);
  if (idprop == NULL) {
    PyErr_SetString(PyExc_KeyError, "Property not found in IDProperty group");
    Py_RETURN_NONE;
  }

  IDPropertyUIData *ui_data = IDP_ui_data_ensure(idprop);
  BLI_assert(ui_data != NULL);

  PyObject *dict = PyDict_New();

  /* RNA subtype. */
  {
    const char *subtype_id = NULL;
    RNA_enum_identifier(rna_enum_property_subtype_items, ui_data->rna_subtype, &subtype_id);
    PyObject *item = PyUnicode_FromString(subtype_id);
    PyDict_SetItemString(dict, "subtype", item);
    Py_DECREF(item);
  }

  /* Description. */
  if (ui_data->description != NULL) {
    PyObject *item = PyUnicode_FromString(ui_data->description);
    PyDict_SetItemString(dict, "description", item);
    Py_DECREF(item);
  }

  /* Type specific data. */
  switch (IDP_ui_data_type(idprop)) {
    case IDP_UI_DATA_TYPE_STRING:
      idprop_ui_data_to_dict_string(idprop, dict);
      break;
    case IDP_UI_DATA_TYPE_INT:
      idprop_ui_data_to_dict_int(idprop, dict);
      break;
    case IDP_UI_DATA_TYPE_FLOAT:
      idprop_ui_data_to_dict_float(idprop, dict);
      break;
    case IDP_UI_DATA_TYPE_UNSUPPORTED:
      BLI_assert(false);
      break;
  }

  return dict;
}

PyDoc_STRVAR(BPy_IDGroup_rna_ui_data_clear_doc,
             ".. method:: clear_rna(key)\n"
             "\n"
             "   Remove the RNA UI data from this IDProperty.\n"
             "\n"
             "   :raises KeyError: If no property with the name is in the group.\n");
static PyObject *BPy_IDGroup_rna_ui_data_clear(BPy_IDProperty *self, PyObject *args)
{
  const char *key;

  if (!PyArg_ParseTuple(args, "s:clear_rna", &key)) {
    Py_RETURN_NONE;
  }

  IDProperty *idprop = IDP_GetPropertyFromGroup(self->prop, key);
  if (idprop == NULL) {
    PyErr_SetString(PyExc_KeyError, "Property not found in IDProperty group");
    Py_RETURN_NONE;
  }

  if (idprop->ui_data) {
    IDP_free_ui_data(idprop);
  }

  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    BPy_IDGroup_rna_ui_data_copy_doc,
    ".. method:: clear_rna(source_group, key_source, key_destination)\n"
    "\n"
    "   Copy UI data from an IDProperty in the source group to a property in this group.\n "
    "If the source property has no UI data, the target UI data will be reset if it exists.\n"
    "\n"
    "   :raises KeyError: If either the source or destination item doesn't exist.\n"
    "   :raises TypeError: If the types of the two properties don't match.\n");
static PyObject *BPy_IDGroup_rna_ui_data_copy(BPy_IDProperty *self, PyObject *args)
{
  BPy_IDProperty *group_src;
  const char *key_src;
  const char *key_dest;

  if (!PyArg_ParseTuple(
          args, "O!ss:clear_rna", &BPy_IDGroup_Type, &group_src, &key_src, &key_dest)) {
    Py_RETURN_NONE;
  }

  const IDProperty *idprop_src = IDP_GetPropertyFromGroup(self->prop, key_src);
  IDProperty *idprop_dest = IDP_GetPropertyFromGroup(group_src->prop, key_dest);
  if (ELEM(NULL, idprop_src, idprop_dest)) {
    PyErr_SetString(PyExc_KeyError, "Property not found in IDProperty group");
    Py_RETURN_NONE;
  }

  if (idprop_src->ui_data == NULL) {
    if (idprop_dest->ui_data != NULL) {
      IDP_free_ui_data(idprop_dest);
    }
    Py_RETURN_NONE;
  }

  if (IDP_ui_data_type(idprop_src) != IDP_ui_data_type(idprop_dest)) {
    PyErr_SetString(PyExc_TypeError, "Source and destination UI data types do not match");
    Py_RETURN_NONE;
  }

  if (idprop_dest->ui_data != NULL) {
    IDP_free_ui_data(idprop_dest);
  }

  idprop_dest->ui_data = IDP_copy_ui_data(idprop_src);

  Py_RETURN_NONE;
}

static struct PyMethodDef BPy_IDGroup_methods[] = {
    {"pop", (PyCFunction)BPy_IDGroup_pop, METH_VARARGS, BPy_IDGroup_pop_doc},
    {"iteritems", (PyCFunction)BPy_IDGroup_iter_items, METH_NOARGS, BPy_IDGroup_iter_items_doc},
    {"keys", (PyCFunction)BPy_IDGroup_keys, METH_NOARGS, BPy_IDGroup_keys_doc},
    {"values", (PyCFunction)BPy_IDGroup_values, METH_NOARGS, BPy_IDGroup_values_doc},
    {"items", (PyCFunction)BPy_IDGroup_items, METH_NOARGS, BPy_IDGroup_items_doc},
    {"update", (PyCFunction)BPy_IDGroup_update, METH_O, BPy_IDGroup_update_doc},
    {"get", (PyCFunction)BPy_IDGroup_get, METH_VARARGS, BPy_IDGroup_get_doc},
    {"to_dict", (PyCFunction)BPy_IDGroup_to_dict, METH_NOARGS, BPy_IDGroup_to_dict_doc},
    {"clear", (PyCFunction)BPy_IDGroup_clear, METH_NOARGS, BPy_IDGroup_clear_doc},
    {"update_rna",
     (PyCFunction)BPy_IDGroup_update_rna,
     METH_VARARGS | METH_KEYWORDS,
     BPy_IDGroup_update_rna_doc},
    {"rna_data", (PyCFunction)BPy_IDGroup_rna_ui_data, METH_VARARGS, BPy_IDGroup_rna_ui_data_doc},
    {"clear_rna",
     (PyCFunction)BPy_IDGroup_rna_ui_data_clear,
     METH_VARARGS,
     BPy_IDGroup_rna_ui_data_clear_doc},
    {"copy_rna",
     (PyCFunction)BPy_IDGroup_rna_ui_data_copy,
     METH_VARARGS,
     BPy_IDGroup_rna_ui_data_copy_doc},
    {NULL, NULL, 0, NULL},
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID-Property Group Type
 * \{ */

static PySequenceMethods BPy_IDGroup_Seq = {
    (lenfunc)BPy_IDGroup_Map_Len, /* lenfunc sq_length */
    NULL,                         /* binaryfunc sq_concat */
    NULL,                         /* ssizeargfunc sq_repeat */
    NULL,
    /* ssizeargfunc sq_item */ /* TODO - setting this will allow PySequence_Check to return True */
    NULL,                      /* intintargfunc ***was_sq_slice*** */
    NULL,                      /* intobjargproc sq_ass_item */
    NULL,                      /* ssizeobjargproc ***was_sq_ass_slice*** */
    (objobjproc)BPy_IDGroup_Contains, /* objobjproc sq_contains */
    NULL,                             /* binaryfunc sq_inplace_concat */
    NULL,                             /* ssizeargfunc sq_inplace_repeat */
};

static PyMappingMethods BPy_IDGroup_Mapping = {
    (lenfunc)BPy_IDGroup_Map_Len,           /*inquiry mp_length */
    (binaryfunc)BPy_IDGroup_Map_GetItem,    /*binaryfunc mp_subscript */
    (objobjargproc)BPy_IDGroup_Map_SetItem, /*objobjargproc mp_ass_subscript */
};

PyTypeObject BPy_IDGroup_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    /*  For printing, in format "<module>.<name>" */
    "IDPropertyGroup",      /* char *tp_name; */
    sizeof(BPy_IDProperty), /* int tp_basicsize; */
    0,                      /* tp_itemsize;  For allocation */

    /* Methods to implement standard operations */

    NULL, /* destructor tp_dealloc; */
#if PY_VERSION_HEX >= 0x03080000
    0, /* tp_vectorcall_offset */
#else
    (printfunc)NULL, /* printfunc tp_print */
#endif
    NULL,                       /* getattrfunc tp_getattr; */
    NULL,                       /* setattrfunc tp_setattr; */
    NULL,                       /* cmpfunc tp_compare; */
    (reprfunc)BPy_IDGroup_repr, /* reprfunc tp_repr; */

    /* Method suites for standard classes */

    NULL,                 /* PyNumberMethods *tp_as_number; */
    &BPy_IDGroup_Seq,     /* PySequenceMethods *tp_as_sequence; */
    &BPy_IDGroup_Mapping, /* PyMappingMethods *tp_as_mapping; */

    /* More standard operations (here for binary compatibility) */

    (hashfunc)BPy_IDGroup_hash, /* hashfunc tp_hash; */
    NULL,                       /* ternaryfunc tp_call; */
    NULL,                       /* reprfunc tp_str; */
    NULL,                       /* getattrofunc tp_getattro; */
    NULL,                       /* setattrofunc tp_setattro; */

    /* Functions to access object as input/output buffer */
    NULL, /* PyBufferProcs *tp_as_buffer; */

    /*** Flags to define presence of optional/expanded features ***/
    Py_TPFLAGS_DEFAULT, /* long tp_flags; */

    NULL, /*  char *tp_doc;  Documentation string */
    /*** Assigned meaning in release 2.0 ***/
    /* call function for all accessible objects */
    NULL, /* traverseproc tp_traverse; */

    /* delete references to contained objects */
    NULL, /* inquiry tp_clear; */

    /***  Assigned meaning in release 2.1 ***/
    /*** rich comparisons ***/
    NULL, /* richcmpfunc tp_richcompare; */

    /***  weak reference enabler ***/
    0, /* long tp_weaklistoffset; */

    /*** Added in release 2.2 ***/
    /*   Iterators */
    (getiterfunc)BPy_IDGroup_iter, /* getiterfunc tp_iter; */
    NULL,                          /* iternextfunc tp_iternext; */
    /*** Attribute descriptor and subclassing stuff ***/
    BPy_IDGroup_methods,   /* struct PyMethodDef *tp_methods; */
    NULL,                  /* struct PyMemberDef *tp_members; */
    BPy_IDGroup_getseters, /* struct PyGetSetDef *tp_getset; */
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID Array Methods
 * \{ */

static PyTypeObject *idp_array_py_type(BPy_IDArray *self, bool *r_is_double)
{
  switch (self->prop->subtype) {
    case IDP_FLOAT:
      *r_is_double = false;
      return &PyFloat_Type;
    case IDP_DOUBLE:
      *r_is_double = true;
      return &PyFloat_Type;
    case IDP_INT:
      *r_is_double = false;
      return &PyLong_Type;
    default:
      *r_is_double = false;
      return NULL;
  }
}

static PyObject *BPy_IDArray_repr(BPy_IDArray *self)
{
  return PyUnicode_FromFormat("<bpy id property array [%d]>", self->prop->len);
}

PyDoc_STRVAR(BPy_IDArray_get_typecode_doc,
             "The type of the data in the array {'f': float, 'd': double, 'i': int}.");
static PyObject *BPy_IDArray_get_typecode(BPy_IDArray *self)
{
  switch (self->prop->subtype) {
    case IDP_FLOAT:
      return PyUnicode_FromString("f");
    case IDP_DOUBLE:
      return PyUnicode_FromString("d");
    case IDP_INT:
      return PyUnicode_FromString("i");
  }

  PyErr_Format(
      PyExc_RuntimeError, "%s: invalid/corrupt array type '%d'!", __func__, self->prop->subtype);

  return NULL;
}

static PyGetSetDef BPy_IDArray_getseters[] = {
    /* matches pythons array.typecode */
    {"typecode",
     (getter)BPy_IDArray_get_typecode,
     (setter)NULL,
     BPy_IDArray_get_typecode_doc,
     NULL},
    {NULL, NULL, NULL, NULL, NULL},
};

PyDoc_STRVAR(BPy_IDArray_to_list_doc,
             ".. method:: to_list()\n"
             "\n"
             "   Return the array as a list.\n");
static PyObject *BPy_IDArray_to_list(BPy_IDArray *self)
{
  return BPy_IDGroup_MapDataToPy(self->prop);
}

static PyMethodDef BPy_IDArray_methods[] = {
    {"to_list", (PyCFunction)BPy_IDArray_to_list, METH_NOARGS, BPy_IDArray_to_list_doc},
    {NULL, NULL, 0, NULL},
};

static int BPy_IDArray_Len(BPy_IDArray *self)
{
  return self->prop->len;
}

static PyObject *BPy_IDArray_GetItem(BPy_IDArray *self, int index)
{
  if (index < 0 || index >= self->prop->len) {
    PyErr_SetString(PyExc_IndexError, "index out of range!");
    return NULL;
  }

  switch (self->prop->subtype) {
    case IDP_FLOAT:
      return PyFloat_FromDouble(((float *)IDP_Array(self->prop))[index]);
    case IDP_DOUBLE:
      return PyFloat_FromDouble(((double *)IDP_Array(self->prop))[index]);
    case IDP_INT:
      return PyLong_FromLong((long)((int *)IDP_Array(self->prop))[index]);
  }

  PyErr_Format(
      PyExc_RuntimeError, "%s: invalid/corrupt array type '%d'!", __func__, self->prop->subtype);

  return NULL;
}

static int BPy_IDArray_SetItem(BPy_IDArray *self, int index, PyObject *value)
{
  if (index < 0 || index >= self->prop->len) {
    PyErr_SetString(PyExc_RuntimeError, "index out of range!");
    return -1;
  }

  switch (self->prop->subtype) {
    case IDP_FLOAT: {
      const float f = (float)PyFloat_AsDouble(value);
      if (f == -1 && PyErr_Occurred()) {
        return -1;
      }
      ((float *)IDP_Array(self->prop))[index] = f;
      break;
    }
    case IDP_DOUBLE: {
      const double d = PyFloat_AsDouble(value);
      if (d == -1 && PyErr_Occurred()) {
        return -1;
      }
      ((double *)IDP_Array(self->prop))[index] = d;
      break;
    }
    case IDP_INT: {
      const int i = PyC_Long_AsI32(value);
      if (i == -1 && PyErr_Occurred()) {
        return -1;
      }

      ((int *)IDP_Array(self->prop))[index] = i;
      break;
    }
  }
  return 0;
}

static PySequenceMethods BPy_IDArray_Seq = {
    (lenfunc)BPy_IDArray_Len,             /* inquiry sq_length */
    NULL,                                 /* binaryfunc sq_concat */
    NULL,                                 /* intargfunc sq_repeat */
    (ssizeargfunc)BPy_IDArray_GetItem,    /* intargfunc sq_item */
    NULL,                                 /* intintargfunc sq_slice */
    (ssizeobjargproc)BPy_IDArray_SetItem, /* intobjargproc sq_ass_item */
    NULL,                                 /* intintobjargproc sq_ass_slice */
    NULL,                                 /* objobjproc sq_contains */
    /* Added in release 2.0 */
    NULL, /* binaryfunc sq_inplace_concat */
    NULL, /* intargfunc sq_inplace_repeat */
};

/* sequence slice (get): idparr[a:b] */
static PyObject *BPy_IDArray_slice(BPy_IDArray *self, int begin, int end)
{
  IDProperty *prop = self->prop;
  PyObject *tuple;
  int count;

  CLAMP(begin, 0, prop->len);
  if (end < 0) {
    end = prop->len + end + 1;
  }
  CLAMP(end, 0, prop->len);
  begin = MIN2(begin, end);

  tuple = PyTuple_New(end - begin);

  switch (prop->subtype) {
    case IDP_FLOAT: {
      const float *array = (float *)IDP_Array(prop);
      for (count = begin; count < end; count++) {
        PyTuple_SET_ITEM(tuple, count - begin, PyFloat_FromDouble(array[count]));
      }
      break;
    }
    case IDP_DOUBLE: {
      const double *array = (double *)IDP_Array(prop);
      for (count = begin; count < end; count++) {
        PyTuple_SET_ITEM(tuple, count - begin, PyFloat_FromDouble(array[count]));
      }
      break;
    }
    case IDP_INT: {
      const int *array = (int *)IDP_Array(prop);
      for (count = begin; count < end; count++) {
        PyTuple_SET_ITEM(tuple, count - begin, PyLong_FromLong(array[count]));
      }
      break;
    }
  }

  return tuple;
}
/* sequence slice (set): idparr[a:b] = value */
static int BPy_IDArray_ass_slice(BPy_IDArray *self, int begin, int end, PyObject *seq)
{
  IDProperty *prop = self->prop;
  bool is_double;
  const PyTypeObject *py_type = idp_array_py_type(self, &is_double);
  const size_t elem_size = is_double ? sizeof(double) : sizeof(float);
  size_t alloc_len;
  size_t size;
  void *vec;

  CLAMP(begin, 0, prop->len);
  CLAMP(end, 0, prop->len);
  begin = MIN2(begin, end);

  size = (end - begin);
  alloc_len = size * elem_size;

  /* NOTE: we count on int/float being the same size here */
  vec = MEM_mallocN(alloc_len, "array assignment");

  if (PyC_AsArray(vec, seq, size, py_type, is_double, "slice assignment: ") == -1) {
    MEM_freeN(vec);
    return -1;
  }

  memcpy((void *)(((char *)IDP_Array(prop)) + (begin * elem_size)), vec, alloc_len);

  MEM_freeN(vec);
  return 0;
}

static PyObject *BPy_IDArray_subscript(BPy_IDArray *self, PyObject *item)
{
  if (PyIndex_Check(item)) {
    Py_ssize_t i;
    i = PyNumber_AsSsize_t(item, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred()) {
      return NULL;
    }
    if (i < 0) {
      i += self->prop->len;
    }
    return BPy_IDArray_GetItem(self, i);
  }
  if (PySlice_Check(item)) {
    Py_ssize_t start, stop, step, slicelength;

    if (PySlice_GetIndicesEx(item, self->prop->len, &start, &stop, &step, &slicelength) < 0) {
      return NULL;
    }

    if (slicelength <= 0) {
      return PyTuple_New(0);
    }
    if (step == 1) {
      return BPy_IDArray_slice(self, start, stop);
    }

    PyErr_SetString(PyExc_TypeError, "slice steps not supported with vectors");
    return NULL;
  }

  PyErr_Format(PyExc_TypeError,
               "vector indices must be integers, not %.200s",
               __func__,
               Py_TYPE(item)->tp_name);
  return NULL;
}

static int BPy_IDArray_ass_subscript(BPy_IDArray *self, PyObject *item, PyObject *value)
{
  if (PyIndex_Check(item)) {
    Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred()) {
      return -1;
    }
    if (i < 0) {
      i += self->prop->len;
    }
    return BPy_IDArray_SetItem(self, i, value);
  }
  if (PySlice_Check(item)) {
    Py_ssize_t start, stop, step, slicelength;

    if (PySlice_GetIndicesEx(item, self->prop->len, &start, &stop, &step, &slicelength) < 0) {
      return -1;
    }

    if (step == 1) {
      return BPy_IDArray_ass_slice(self, start, stop, value);
    }

    PyErr_SetString(PyExc_TypeError, "slice steps not supported with vectors");
    return -1;
  }

  PyErr_Format(
      PyExc_TypeError, "vector indices must be integers, not %.200s", Py_TYPE(item)->tp_name);
  return -1;
}

static PyMappingMethods BPy_IDArray_AsMapping = {
    (lenfunc)BPy_IDArray_Len,
    (binaryfunc)BPy_IDArray_subscript,
    (objobjargproc)BPy_IDArray_ass_subscript,
};

static int itemsize_by_idarray_type(int array_type)
{
  if (array_type == IDP_INT) {
    return sizeof(int);
  }
  if (array_type == IDP_FLOAT) {
    return sizeof(float);
  }
  if (array_type == IDP_DOUBLE) {
    return sizeof(double);
  }
  return -1; /* should never happen */
}

static int BPy_IDArray_getbuffer(BPy_IDArray *self, Py_buffer *view, int flags)
{
  IDProperty *prop = self->prop;
  const int itemsize = itemsize_by_idarray_type(prop->subtype);
  const int length = itemsize * prop->len;

  if (PyBuffer_FillInfo(view, (PyObject *)self, IDP_Array(prop), length, false, flags) == -1) {
    return -1;
  }

  view->itemsize = itemsize;
  view->format = (char *)idp_format_from_array_type(prop->subtype);

  Py_ssize_t *shape = MEM_mallocN(sizeof(Py_ssize_t), __func__);
  shape[0] = prop->len;
  view->shape = shape;

  return 0;
}

static void BPy_IDArray_releasebuffer(BPy_IDArray *UNUSED(self), Py_buffer *view)
{
  MEM_freeN(view->shape);
}

static PyBufferProcs BPy_IDArray_Buffer = {
    (getbufferproc)BPy_IDArray_getbuffer,
    (releasebufferproc)BPy_IDArray_releasebuffer,
};

/* -------------------------------------------------------------------- */
/** \name ID Array Type
 * \{ */

PyTypeObject BPy_IDArray_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    /*  For printing, in format "<module>.<name>" */
    "IDPropertyArray",   /* char *tp_name; */
    sizeof(BPy_IDArray), /* int tp_basicsize; */
    0,                   /* tp_itemsize;  For allocation */

    /* Methods to implement standard operations */

    NULL, /* destructor tp_dealloc; */
#if PY_VERSION_HEX >= 0x03080000
    0, /* tp_vectorcall_offset */
#else
    (printfunc)NULL, /* printfunc tp_print */
#endif
    NULL,                       /* getattrfunc tp_getattr; */
    NULL,                       /* setattrfunc tp_setattr; */
    NULL,                       /* cmpfunc tp_compare; */
    (reprfunc)BPy_IDArray_repr, /* reprfunc tp_repr; */

    /* Method suites for standard classes */

    NULL,                   /* PyNumberMethods *tp_as_number; */
    &BPy_IDArray_Seq,       /* PySequenceMethods *tp_as_sequence; */
    &BPy_IDArray_AsMapping, /* PyMappingMethods *tp_as_mapping; */

    /* More standard operations (here for binary compatibility) */

    NULL, /* hashfunc tp_hash; */
    NULL, /* ternaryfunc tp_call; */
    NULL, /* reprfunc tp_str; */
    NULL, /* getattrofunc tp_getattro; */
    NULL, /* setattrofunc tp_setattro; */

    /* Functions to access object as input/output buffer */
    &BPy_IDArray_Buffer, /* PyBufferProcs *tp_as_buffer; */

    /*** Flags to define presence of optional/expanded features ***/
    Py_TPFLAGS_DEFAULT, /* long tp_flags; */

    NULL, /*  char *tp_doc;  Documentation string */
    /*** Assigned meaning in release 2.0 ***/
    /* call function for all accessible objects */
    NULL, /* traverseproc tp_traverse; */

    /* delete references to contained objects */
    NULL, /* inquiry tp_clear; */

    /***  Assigned meaning in release 2.1 ***/
    /*** rich comparisons ***/
    NULL, /* richcmpfunc tp_richcompare; */

    /***  weak reference enabler ***/
    0, /* long tp_weaklistoffset; */

    /*** Added in release 2.2 ***/
    /*   Iterators */
    NULL, /* getiterfunc tp_iter; */
    NULL, /* iternextfunc tp_iternext; */

    /*** Attribute descriptor and subclassing stuff ***/
    BPy_IDArray_methods,   /* struct PyMethodDef *tp_methods; */
    NULL,                  /* struct PyMemberDef *tp_members; */
    BPy_IDArray_getseters, /* struct PyGetSetDef *tp_getset; */
    NULL,                  /* struct _typeobject *tp_base; */
    NULL,                  /* PyObject *tp_dict; */
    NULL,                  /* descrgetfunc tp_descr_get; */
    NULL,                  /* descrsetfunc tp_descr_set; */
    0,                     /* long tp_dictoffset; */
    NULL,                  /* initproc tp_init; */
    NULL,                  /* allocfunc tp_alloc; */
    NULL,                  /* newfunc tp_new; */
    /*  Low-level free-memory routine */
    NULL, /* freefunc tp_free;  */
    /* For PyObject_IS_GC */
    NULL, /* inquiry tp_is_gc;  */
    NULL, /* PyObject *tp_bases; */
    /* method resolution order */
    NULL, /* PyObject *tp_mro;  */
    NULL, /* PyObject *tp_cache; */
    NULL, /* PyObject *tp_subclasses; */
    NULL, /* PyObject *tp_weaklist; */
    NULL,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name ID-Property Group Iterator Type
 * \{ */

static PyObject *IDGroup_Iter_repr(BPy_IDGroup_Iter *self)
{
  return PyUnicode_FromFormat("(ID Property Group Iter \"%s\")", self->group->prop->name);
}

static PyObject *BPy_Group_Iter_Next(BPy_IDGroup_Iter *self)
{
  if (self->cur) {
    PyObject *ret;
    IDProperty *cur;

    cur = self->cur;
    self->cur = self->cur->next;

    if (self->mode == IDPROP_ITER_ITEMS) {
      ret = PyTuple_New(2);
      PyTuple_SET_ITEMS(ret,
                        PyUnicode_FromString(cur->name),
                        BPy_IDGroup_WrapData(self->group->id, cur, self->group->prop));
      return ret;
    }

    return PyUnicode_FromString(cur->name);
  }

  PyErr_SetNone(PyExc_StopIteration);
  return NULL;
}

PyTypeObject BPy_IDGroup_Iter_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    /*  For printing, in format "<module>.<name>" */
    "IDPropertyGroupIter",    /* char *tp_name; */
    sizeof(BPy_IDGroup_Iter), /* int tp_basicsize; */
    0,                        /* tp_itemsize;  For allocation */

    /* Methods to implement standard operations */

    NULL, /* destructor tp_dealloc; */
#if PY_VERSION_HEX >= 0x03080000
    0, /* tp_vectorcall_offset */
#else
    (printfunc)NULL, /* printfunc tp_print */
#endif
    NULL,                        /* getattrfunc tp_getattr; */
    NULL,                        /* setattrfunc tp_setattr; */
    NULL,                        /* cmpfunc tp_compare; */
    (reprfunc)IDGroup_Iter_repr, /* reprfunc tp_repr; */

    /* Method suites for standard classes */

    NULL, /* PyNumberMethods *tp_as_number; */
    NULL, /* PySequenceMethods *tp_as_sequence; */
    NULL, /* PyMappingMethods *tp_as_mapping; */

    /* More standard operations (here for binary compatibility) */

    NULL, /* hashfunc tp_hash; */
    NULL, /* ternaryfunc tp_call; */
    NULL, /* reprfunc tp_str; */
    NULL, /* getattrofunc tp_getattro; */
    NULL, /* setattrofunc tp_setattro; */

    /* Functions to access object as input/output buffer */
    NULL, /* PyBufferProcs *tp_as_buffer; */

    /*** Flags to define presence of optional/expanded features ***/
    Py_TPFLAGS_DEFAULT, /* long tp_flags; */

    NULL, /*  char *tp_doc;  Documentation string */
    /*** Assigned meaning in release 2.0 ***/
    /* call function for all accessible objects */
    NULL, /* traverseproc tp_traverse; */

    /* delete references to contained objects */
    NULL, /* inquiry tp_clear; */

    /***  Assigned meaning in release 2.1 ***/
    /*** rich comparisons ***/
    NULL, /* richcmpfunc tp_richcompare; */

    /***  weak reference enabler ***/
    0, /* long tp_weaklistoffset; */

    /*** Added in release 2.2 ***/
    /*   Iterators */
    PyObject_SelfIter,                 /* getiterfunc tp_iter; */
    (iternextfunc)BPy_Group_Iter_Next, /* iternextfunc tp_iternext; */
};

void IDProp_Init_Types(void)
{
  PyType_Ready(&BPy_IDGroup_Type);
  PyType_Ready(&BPy_IDGroup_Iter_Type);
  PyType_Ready(&BPy_IDArray_Type);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Module 'idprop.types'
 * \{ */

static struct PyModuleDef IDProp_types_module_def = {
    PyModuleDef_HEAD_INIT,
    "idprop.types", /* m_name */
    NULL,           /* m_doc */
    0,              /* m_size */
    NULL,           /* m_methods */
    NULL,           /* m_reload */
    NULL,           /* m_traverse */
    NULL,           /* m_clear */
    NULL,           /* m_free */
};

static PyObject *BPyInit_idprop_types(void)
{
  PyObject *submodule;

  submodule = PyModule_Create(&IDProp_types_module_def);

  IDProp_Init_Types();

#define MODULE_TYPE_ADD(s, t) \
  PyModule_AddObject(s, t.tp_name, (PyObject *)&t); \
  Py_INCREF((PyObject *)&t)

  /* bmesh_py_types.c */
  MODULE_TYPE_ADD(submodule, BPy_IDGroup_Type);
  MODULE_TYPE_ADD(submodule, BPy_IDGroup_Iter_Type);
  MODULE_TYPE_ADD(submodule, BPy_IDArray_Type);

#undef MODULE_TYPE_ADD

  return submodule;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Module 'idprop'
 * \{ */

static PyMethodDef IDProp_methods[] = {
    {NULL, NULL, 0, NULL},
};

PyDoc_STRVAR(IDProp_module_doc,
             "This module provides access id property types (currently mainly for docs).");
static struct PyModuleDef IDProp_module_def = {
    PyModuleDef_HEAD_INIT,
    "idprop",          /* m_name */
    IDProp_module_doc, /* m_doc */
    0,                 /* m_size */
    IDProp_methods,    /* m_methods */
    NULL,              /* m_reload */
    NULL,              /* m_traverse */
    NULL,              /* m_clear */
    NULL,              /* m_free */
};

PyObject *BPyInit_idprop(void)
{
  PyObject *mod;
  PyObject *submodule;
  PyObject *sys_modules = PyImport_GetModuleDict();

  mod = PyModule_Create(&IDProp_module_def);

  /* idprop.types */
  PyModule_AddObject(mod, "types", (submodule = BPyInit_idprop_types()));
  PyDict_SetItem(sys_modules, PyModule_GetNameObject(submodule), submodule);

  return mod;
}

/** \} */
