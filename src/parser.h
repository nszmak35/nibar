/* Config parser: the config is Python, run by an embedded interpreter. */
#ifndef NIBAR_PARSER_H
#define NIBAR_PARSER_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

typedef enum {
	NB_SCALAR = 0,
	NB_LIST,
	NB_TABLE
} NbType;

typedef struct NbEntry {
	NbType type;
	const char *key;
	const char *value;
	const char **items;
	size_t count;
	int source;
} NbEntry;

typedef int (*NbEntryCb)(const NbEntry *entry, void *userdata);

typedef struct {
	NbEntryCb cb;
	void *userdata;
	int source;
} Parser;


static PyObject *
stringify(PyObject *value)
{
	if (value == Py_None)
		return PyUnicode_FromString("NULL");
	if (PyBool_Check(value))
		return PyUnicode_FromString(value == Py_True ? "1" : "0");
	return PyObject_Str(value);
}

static int
emit_row(Parser *p, NbType type, const char *key, PyObject *seq)
{
	Py_ssize_t n, i, made = 0;
	const char **items;
	PyObject **strings;
	NbEntry e;
	int rc = -1;

	if (!PyList_Check(seq) && !PyTuple_Check(seq))
		return 0;
	n = PySequence_Size(seq);
	if (n < 0) {
		PyErr_Clear();
		return -1;
	}
	if (n == 0)
		return 0;

	items = calloc((size_t)n, sizeof(*items));
	strings = calloc((size_t)n, sizeof(*strings));
	if (!items || !strings)
		goto out;

	for (i = 0; i < n; i++) {
		PyObject *v = PySequence_GetItem(seq, i);
		if (!v)
			goto out;
		strings[i] = stringify(v);
		Py_DECREF(v);
		if (!strings[i])
			goto out;
		made++;
		items[i] = PyUnicode_AsUTF8(strings[i]);
		if (!items[i])
			goto out;
	}

	memset(&e, 0, sizeof(e));
	e.type = type;
	e.key = key;
	e.items = items;
	e.count = (size_t)n;
	e.source = p->source;
	rc = p->cb(&e, p->userdata);

out:
	for (i = 0; i < made; i++)
		Py_XDECREF(strings[i]);
	free(strings);
	free(items);
	return rc;
}

static int
emit_scalar(Parser *p, const char *key, PyObject *value)
{
	PyObject *s = stringify(value);
	const char *text;
	NbEntry e;
	int rc;

	if (!s)
		return -1;
	text = PyUnicode_AsUTF8(s);
	if (!text) {
		Py_DECREF(s);
		return -1;
	}
	memset(&e, 0, sizeof(e));
	e.type = NB_SCALAR;
	e.key = key;
	e.value = text;
	e.source = p->source;
	rc = p->cb(&e, p->userdata);
	Py_DECREF(s);
	return rc;
}


static int
emit_list(Parser *p, const char *key, PyObject *value)
{
	Py_ssize_t n, i;

	if (!PyList_Check(value) && !PyTuple_Check(value))
		return 0;
	n = PySequence_Size(value);
	if (n < 0) {
		PyErr_Clear();
		return -1;
	}
	for (i = 0; i < n; i++) {
		PyObject *item = PySequence_GetItem(value, i);
		PyObject *row;
		int rc;

		if (!item)
			return -1;
		if (PyList_Check(item) || PyTuple_Check(item)) {
			rc = emit_row(p, NB_TABLE, key, item);
		} else {
			row = PyTuple_Pack(1, item);
			if (!row) {
				Py_DECREF(item);
				return -1;
			}
			rc = emit_row(p, NB_LIST, key, row);
			Py_DECREF(row);
		}
		Py_DECREF(item);
		if (rc)
			return rc;
	}
	return 0;
}


#define BAR_US 0x1f
#define BAR_RS 0x1e

typedef struct {
	char *p;
	size_t n, cap;
} Buf;

typedef struct {
	char **v;
	size_t n, cap;
} Items;

static int
buf_add(Buf *b, const char *s, size_t len)
{
	if (b->n + len + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 64;
		char *np;

		while (nc < b->n + len + 1)
			nc *= 2;
		np = realloc(b->p, nc);
		if (!np)
			return -1;
		b->p = np;
		b->cap = nc;
	}
	memcpy(b->p + b->n, s, len);
	b->n += len;
	b->p[b->n] = '\0';
	return 0;
}

static int
items_add(Items *it, const char *s)
{
	if (it->n + 1 > it->cap) {
		size_t nc = it->cap ? it->cap * 2 : 32;
		char **nv = realloc(it->v, nc * sizeof(char *));

		if (!nv)
			return -1;
		it->v = nv;
		it->cap = nc;
	}
	it->v[it->n] = strdup(s);
	if (!it->v[it->n])
		return -1;
	it->n++;
	return 0;
}

static void
items_free(Items *it)
{
	size_t i;

	for (i = 0; i < it->n; i++)
		free(it->v[i]);
	free(it->v);
	memset(it, 0, sizeof(*it));
}

static int
buf_add_py(Buf *b, PyObject *v)
{
	PyObject *s = stringify(v);
	const char *text;
	Py_ssize_t len;
	int rc;

	if (!s)
		return -1;
	text = PyUnicode_AsUTF8AndSize(s, &len);
	if (!text) {
		Py_DECREF(s);
		return -1;
	}
	rc = buf_add(b, text, (size_t)len);
	Py_DECREF(s);
	return rc;
}

static int
encode_value(PyObject *v, Buf *b)
{
	if (PyList_Check(v) || PyTuple_Check(v)) {
		Py_ssize_t n = PySequence_Size(v), i;

		if (n < 0)
			return -1;
		for (i = 0; i < n; i++) {
			PyObject *item = PySequence_GetItem(v, i);
			char sep = BAR_US;
			int rc;

			if (!item)
				return -1;
			if (i > 0 && buf_add(b, &sep, 1) < 0) {
				Py_DECREF(item);
				return -1;
			}
			rc = buf_add_py(b, item);
			Py_DECREF(item);
			if (rc)
				return rc;
		}
		return 0;
	}
	if (PyDict_Check(v)) {
		PyObject *k, *val;
		Py_ssize_t pos = 0;
		int first = 1;

		while (PyDict_Next(v, &pos, &k, &val)) {
			char sep = BAR_US, rs = BAR_RS;

			if (!first && buf_add(b, &sep, 1) < 0)
				return -1;
			first = 0;
			if (buf_add_py(b, k) < 0 || buf_add(b, &rs, 1) < 0 ||
			    buf_add_py(b, val) < 0)
				return -1;
		}
		return 0;
	}
	return buf_add_py(b, v);
}

static int collect_attrs(PyObject *klass, const char *prefix, Items *out,
                         int depth);

static int
collect_dict(PyObject *klass, const char *prefix, Items *out, int depth)
{
	PyObject *mapping = PyObject_GetAttrString(klass, "__dict__");
	PyObject *keys;
	Py_ssize_t n, i;
	int rc = 0;

	if (!mapping) {
		PyErr_Clear();
		return 0;
	}
	keys = PyMapping_Keys(mapping);
	if (!keys) {
		Py_DECREF(mapping);
		return -1;
	}
	n = PySequence_Size(keys);
	for (i = 0; i < n && !rc; i++) {
		PyObject *key = PySequence_GetItem(keys, i);
		PyObject *value;
		const char *name;
		char full[256];

		if (!key) {
			rc = -1;
			break;
		}
		name = PyUnicode_Check(key) ? PyUnicode_AsUTF8(key) : NULL;
		if (!name || name[0] == '_') {
			PyErr_Clear();
			Py_DECREF(key);
			continue;
		}
		value = PyObject_GetItem(mapping, key);
		Py_DECREF(key);
		if (!value) {
			PyErr_Clear();
			continue;
		}
		if (PyType_Check(value)) {
			if (depth < 3 && strlen(prefix) + strlen(name) + 2 < sizeof(full)) {
				snprintf(full, sizeof(full), "%s%s.", prefix, name);
				rc = collect_attrs(value, full, out, depth + 1);
			}
		} else if (!PyCallable_Check(value) && !PyModule_Check(value)) {
			Buf b = {0};

			if (strlen(prefix) + strlen(name) + 1 > sizeof(full)) {
				Py_DECREF(value);
				continue;
			}
			snprintf(full, sizeof(full), "%s%s", prefix, name);
			if (encode_value(value, &b) < 0 || (!b.p && buf_add(&b, "", 0) < 0) ||
			    items_add(out, full) < 0 || items_add(out, b.p ? b.p : "") < 0)
				rc = -1;
			free(b.p);
		}
		Py_DECREF(value);
	}
	Py_DECREF(keys);
	Py_DECREF(mapping);
	return rc;
}

static int
collect_attrs(PyObject *klass, const char *prefix, Items *out, int depth)
{
	PyObject *mro = PyObject_GetAttrString(klass, "__mro__");
	Py_ssize_t i;
	int rc = 0;

	if (!mro || !PyTuple_Check(mro)) {
		PyErr_Clear();
		Py_XDECREF(mro);
		return collect_dict(klass, prefix, out, depth);
	}
	for (i = PyTuple_GET_SIZE(mro) - 1; i >= 0 && !rc; i--) {
		PyObject *c = PyTuple_GET_ITEM(mro, i);

		if (c == (PyObject *)&PyBaseObject_Type)
			continue;
		rc = collect_dict(c, prefix, out, depth);
	}
	Py_DECREF(mro);
	return rc;
}

static int
emit_element(Parser *p, PyObject *klass)
{
	Items it = {0};
	PyObject *nm = PyObject_GetAttrString(klass, "__name__");
	const char *name = nm ? PyUnicode_AsUTF8(nm) : NULL;
	NbEntry e;
	int rc;

	if (!name) {
		PyErr_Clear();
		name = "element";
	}
	if (items_add(&it, name) < 0) {
		Py_XDECREF(nm);
		items_free(&it);
		return -1;
	}
	Py_XDECREF(nm);
	rc = collect_attrs(klass, "", &it, 0);
	if (!rc) {
		memset(&e, 0, sizeof(e));
		e.type = NB_TABLE;
		e.key = "bar_element";
		e.items = (const char **)it.v;
		e.count = it.n;
		e.source = p->source;
		rc = p->cb(&e, p->userdata);
	}
	items_free(&it);
	return rc;
}

static int
emit_sequence(Parser *p, PyObject *value)
{
	Py_ssize_t n, i;

	if (!PyList_Check(value) && !PyTuple_Check(value))
		return 0;
	n = PySequence_Size(value);
	if (n < 0) {
		PyErr_Clear();
		return -1;
	}
	for (i = 0; i < n; i++) {
		PyObject *item = PySequence_GetItem(value, i);
		int rc = 0;

		if (!item)
			return -1;
		if (PyType_Check(item))
			rc = emit_element(p, item);
		Py_DECREF(item);
		if (rc)
			return rc;
	}
	return 0;
}

static int
emit_scalar_text(Parser *p, const char *key, const char *text)
{
	NbEntry e;

	memset(&e, 0, sizeof(e));
	e.type = NB_SCALAR;
	e.key = key;
	e.value = text;
	e.source = p->source;
	return p->cb(&e, p->userdata);
}

static int
emit_bar(Parser *p, PyObject *klass, int *seq_done)
{
	PyObject *mapping = PyObject_GetAttrString(klass, "__dict__");
	PyObject *keys;
	Py_ssize_t n, i;
	int rc = 0;

	if (!mapping) {
		PyErr_Clear();
		return 0;
	}
	keys = PyMapping_Keys(mapping);
	if (!keys) {
		Py_DECREF(mapping);
		return -1;
	}
	n = PySequence_Size(keys);
	for (i = 0; i < n && !rc; i++) {
		PyObject *key = PySequence_GetItem(keys, i);
		PyObject *value;
		const char *name;
		char full[160];

		if (!key) {
			rc = -1;
			break;
		}
		name = PyUnicode_Check(key) ? PyUnicode_AsUTF8(key) : NULL;
		if (!name || name[0] == '_' || strlen(name) + 5 > sizeof(full)) {
			PyErr_Clear();
			Py_DECREF(key);
			continue;
		}
		value = PyObject_GetItem(mapping, key);
		Py_DECREF(key);
		if (!value) {
			PyErr_Clear();
			continue;
		}
		snprintf(full, sizeof(full), "bar_%s", name);
		if (!strcmp(name, "sequence_elements")) {
			*seq_done = 1;
			rc = emit_sequence(p, value);
		} else if (PyType_Check(value) || PyCallable_Check(value) ||
		           PyDict_Check(value)) {
			rc = 0;
		} else if (PyList_Check(value) || PyTuple_Check(value)) {
			Buf b = {0};

			if (encode_value(value, &b) < 0 || (!b.p && buf_add(&b, "", 0) < 0))
				rc = -1;
			else
				rc = emit_scalar_text(p, full, b.p);
			free(b.p);
		} else {
			rc = emit_scalar(p, full, value);
		}
		Py_DECREF(value);
	}
	Py_DECREF(keys);
	Py_DECREF(mapping);
	return rc;
}


static int
emit_tags(Parser *p, PyObject *value)
{
	if (PyLong_Check(value) && !PyBool_Check(value)) {
		long count = PyLong_AsLong(value), i;

		if (PyErr_Occurred()) {
			PyErr_Clear();
			return -1;
		}
		if (count < 1)
			count = 1;
		if (count > 31)
			count = 31;
		for (i = 1; i <= count; i++) {
			PyObject *s = PyUnicode_FromFormat("%ld", i);
			PyObject *row;
			int rc;

			if (!s)
				return -1;
			row = PyTuple_Pack(1, s);
			Py_DECREF(s);
			if (!row)
				return -1;
			rc = emit_row(p, NB_LIST, "tags", row);
			Py_DECREF(row);
			if (rc)
				return rc;
		}
		return 0;
	}
	return emit_list(p, "tags", value);
}


static int
parse_globals(Parser *p, PyObject *globals)
{
	PyObject *key, *value;
	PyObject *module_seq = NULL;
	Py_ssize_t pos = 0;
	int bar_seq = 0;

	while (PyDict_Next(globals, &pos, &key, &value)) {
		const char *name;
		int rc;

		if (!PyUnicode_Check(key))
			continue;
		name = PyUnicode_AsUTF8(key);
		if (!name || name[0] == '_') {
			PyErr_Clear();
			continue;
		}
		if (PyModule_Check(value) || PyCallable_Check(value)) {
			if (PyType_Check(value) && !strcmp(name, "Bar")) {
				rc = emit_bar(p, value, &bar_seq);
				if (rc)
					return rc;
			}
			continue;
		}
		if (!strcmp(name, "sequence_elements")) {
			module_seq = value;
			continue;
		}
		if (!strcmp(name, "tags"))
			rc = emit_tags(p, value);
		else if (PyList_Check(value) || PyTuple_Check(value) || PyDict_Check(value))
			rc = 0;
		else
			rc = emit_scalar(p, name, value);
		if (rc)
			return rc;
	}
	if (module_seq && !bar_seq)
		return emit_sequence(p, module_seq);
	return 0;
}


static void
py_init_once(void)
{
	if (!Py_IsInitialized()) {
		Py_InitializeEx(0);
	}
}

/* A config may import modules next to it; stale cached ones are dropped on reload. */
static PyObject *
cfgdir_enter(const char *name)
{
	const char *slash;
	PyObject *dir, *path, *mods, *items, *importlib;
	Py_ssize_t i, n, dlen;

	if (!name || name[0] != '/' || !(slash = strrchr(name, '/')))
		return NULL;
	dlen = slash == name ? 1 : (Py_ssize_t)(slash - name);
	dir = PyUnicode_FromStringAndSize(name, dlen);
	if (!dir) {
		PyErr_Clear();
		return NULL;
	}
	path = PySys_GetObject("path");
	if (!path || !PyList_Check(path) || PyList_Insert(path, 0, dir) < 0) {
		PyErr_Clear();
		Py_DECREF(dir);
		return NULL;
	}

	mods = PyImport_GetModuleDict();
	items = mods ? PyDict_Items(mods) : NULL;
	if (items) {
		n = PyList_Size(items);
		for (i = 0; i < n; i++) {
			PyObject *item = PyList_GetItem(items, i);
			PyObject *key, *mod, *file;
			const char *f;

			if (!item || !PyTuple_Check(item) || PyTuple_Size(item) != 2)
				continue;
			key = PyTuple_GetItem(item, 0);
			mod = PyTuple_GetItem(item, 1);
			file = PyObject_GetAttrString(mod, "__file__");
			if (!file) {
				PyErr_Clear();
				continue;
			}
			f = PyUnicode_Check(file) ? PyUnicode_AsUTF8(file) : NULL;
			if (f && !strncmp(f, name, (size_t)dlen) &&
			    (dlen == 1 || f[dlen] == '/')) {
				if (PyDict_DelItem(mods, key) < 0)
					PyErr_Clear();
			} else if (!f) {
				PyErr_Clear();
			}
			Py_DECREF(file);
		}
		Py_DECREF(items);
	} else {
		PyErr_Clear();
	}

	importlib = PyImport_ImportModule("importlib");
	if (importlib) {
		PyObject *r = PyObject_CallMethod(importlib, "invalidate_caches", NULL);
		if (!r)
			PyErr_Clear();
		Py_XDECREF(r);
		Py_DECREF(importlib);
	} else {
		PyErr_Clear();
	}
	return dir;
}

static void
cfgdir_leave(PyObject *dir)
{
	PyObject *path, *r;

	if (!dir)
		return;
	path = PySys_GetObject("path");
	if (path && PyList_Check(path)) {
		r = PyObject_CallMethod(path, "remove", "O", dir);
		if (!r)
			PyErr_Clear();
		Py_XDECREF(r);
	}
	Py_DECREF(dir);
}

static int parse_buffer_impl(const char *src, const char *name, int source,
                             NbEntryCb cb, void *userdata);

static int
nb_parse_buffer(const char *src, const char *name, int source,
                  NbEntryCb cb, void *userdata)
{
	PyObject *dir;
	int rc;

	if (!src || !cb)
		return -1;
	py_init_once();
	dir = source ? cfgdir_enter(name) : NULL;
	rc = parse_buffer_impl(src, name, source, cb, userdata);
	cfgdir_leave(dir);
	return rc;
}

static int
parse_buffer_impl(const char *src, const char *name, int source,
                  NbEntryCb cb, void *userdata)
{
	Parser p;
	PyObject *globals, *code, *result;
	int rc;

	if (!src || !cb)
		return -1;
	py_init_once();

	p.cb = cb;
	p.userdata = userdata;
	p.source = source;

	globals = PyDict_New();
	if (!globals)
		return -1;
	if (PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) < 0) {
		PyErr_Print();
		Py_DECREF(globals);
		return -1;
	}
	{
		PyObject *nm = PyUnicode_FromString("nibar_config");
		if (nm) {
			PyDict_SetItemString(globals, "__name__", nm);
			Py_DECREF(nm);
		}
	}

	code = Py_CompileStringExFlags(src, name ? name : "<config>", Py_file_input,
	                               NULL, -1);
	if (!code) {
		PyErr_Print();
		Py_DECREF(globals);
		return -1;
	}
	result = PyEval_EvalCode(code, globals, globals);
	Py_DECREF(code);
	if (!result) {
		PyErr_Print();
		Py_DECREF(globals);
		return -1;
	}
	Py_DECREF(result);

	rc = parse_globals(&p, globals);
	if (rc && PyErr_Occurred())
		PyErr_Print();
	Py_DECREF(globals);
	return rc ? -1 : 0;
}

static int
nb_parse_file(const char *path, int source, NbEntryCb cb, void *userdata,
                int *found)
{
	FILE *fp;
	long size;
	char *buf;
	int rc;

	if (found)
		*found = 0;
	if (!path || access(path, R_OK) != 0)
		return 0;
	fp = fopen(path, "rb");
	if (!fp)
		return 0;
	if (found)
		*found = 1;
	if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 ||
	    size > 4 * 1024 * 1024 || fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}
	buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(fp);
		return -1;
	}
	if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
		free(buf);
		fclose(fp);
		return -1;
	}
	buf[size] = '\0';
	fclose(fp);
	rc = nb_parse_buffer(buf, path, source, cb, userdata);
	free(buf);
	return rc;
}

#endif
