/*
 * PyGres, version 2.2 A Python interface for PostgreSQL database. Written by
 * D'Arcy J.M. Cain, (darcy@druid.net).  Based heavily on code written by
 * Pascal Andre, andre@chimay.via.ecp.fr. Copyright (c) 1995, Pascal Andre
 * (andre@via.ecp.fr).
 * 
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written
 * agreement is hereby granted, provided that the above copyright notice and
 * this paragraph and the following two paragraphs appear in all copies or in
 * any new file that contains a substantial portion of this file.
 * 
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT,
 * SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS,
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF THE
 * AUTHOR HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * THE AUTHOR SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS, AND THE
 * AUTHOR HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE, SUPPORT, UPDATES,
 * ENHANCEMENTS, OR MODIFICATIONS.
 * 
 * Further modifications copyright 1997 by D'Arcy J.M. Cain (darcy@druid.net)
 * subject to the same terms and conditions as above.
 * 
 */

#include <Python.h>
#include <libpq-fe.h>
#include <libpq/libpq-fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* really bad stuff here - I'm so naughty */
/* If you need to you can run mkdefines to get */
/* current defines but it should not have changed */
#define	INT2OID		21
#define	INT4OID		23
#define	OIDOID		26
#define	FLOAT4OID	700
#define	FLOAT8OID	701
#define	CASHOID		790

static PyObject		*PGError;
static const char	*PyPgVersion = "2.3";

/* taken from fileobject.c */
#define BUF(v) PyString_AS_STRING((PyStringObject *)(v))

#define CHECK_OPEN        1
#define CHECK_CLOSE       2

#define MAX_BUFFER_SIZE   8192	/* maximum transaction size */

#ifndef NO_DIRECT
#define DIRECT_ACCESS     1		/* enables direct access functions */
#endif							/* NO_DIRECT */

#ifndef NO_LARGE
#define LARGE_OBJECTS     1		/* enables large objects support */
#endif							/* NO_LARGE */

#ifndef NO_DEF_VAR
#define DEFAULT_VARS      1		/* enables default variables use */
#endif							/* NO_DEF_VAR */

/* --------------------------------------------------------------------- */

/* MODULE GLOBAL VARIABLES */

#ifdef DEFAULT_VARS

static PyObject	*pg_default_host;		/* default database host */
static PyObject	*pg_default_base;		/* default database name */
static PyObject	*pg_default_opt;		/* default connection options */
static PyObject	*pg_default_tty;		/* default debug tty */
static PyObject	*pg_default_port;		/* default connection port */
static PyObject	*pg_default_user;		/* default username */
static PyObject	*pg_default_passwd;		/* default password */

#endif							/* DEFAULT_VARS */

/* --------------------------------------------------------------------- */

/* OBJECTS DECLARATION */

/* pg connection object */

typedef struct
{
	PyObject_HEAD
	int             valid;			/* validity flag */
	PGconn         *cnx;			/* PostGres connection handle */
}               pgobject;

staticforward PyTypeObject PgType;

#define is_pgobject(v) ((v)->ob_type == &PgType)

/* pg query object */

typedef struct
{
	PyObject_HEAD
	PGresult       *last_result;	/* last result content */
}               pgqueryobject;

staticforward PyTypeObject PgQueryType;

#define is_pgqueryobject(v) ((v)->ob_type == &PgQueryType)

#ifdef LARGE_OBJECTS
/* pg large object */

typedef struct
{
	PyObject_HEAD
	pgobject * pgcnx;
	Oid             lo_oid;
	int             lo_fd;
}               pglargeobject;

staticforward PyTypeObject PglargeType;

#define is_pglargeobject(v) ((v)->ob_type == &PglargeType)
#endif							/* LARGE_OBJECTS */

/* --------------------------------------------------------------------- */

/* INTERNAL FUNCTIONS */

#ifdef LARGE_OBJECTS
/* validity check (large object) */
static int
check_lo(pglargeobject * self, int level)
{
	if (!self->lo_oid)
	{
		PyErr_SetString(PGError, "object is not valid (null oid).");
		return 0;
	}

	if (level & CHECK_OPEN)
	{
		if (self->lo_fd < 0)
		{
			PyErr_SetString(PyExc_IOError, "object is not opened.");
			return 0;
		}
	}

	if (level & CHECK_CLOSE)
	{
		if (self->lo_fd >= 0)
		{
			PyErr_SetString(PyExc_IOError, "object is already opened.");
			return 0;
		}
	}

	return 1;
}

#endif							/* LARGE_OBJECTS */

/* --------------------------------------------------------------------- */

#ifdef LARGE_OBJECTS
/* PG CONNECTION OBJECT IMPLEMENTATION */

/* pglargeobject initialisation (from pgobject) */

/* creates large object */
static PyObject  *
pg_locreate(pgobject * self, PyObject * args)
{
	int             mode;
	pglargeobject  *npglo;

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "i", &mode))
	{
		PyErr_SetString(PyExc_TypeError,
				"locreate(mode), with mode (integer).");
		return NULL;
	}

	if ((npglo = PyObject_NEW(pglargeobject, &PglargeType)) == NULL)
		return NULL;

	npglo->pgcnx = self;
	Py_XINCREF(self);
	npglo->lo_fd = -1;
	npglo->lo_oid = lo_creat(self->cnx, mode);

	/* checks result validity */
	if (npglo->lo_oid == 0)
	{
		PyErr_SetString(PGError, "can't create large object.");
		Py_XDECREF(npglo);
		return NULL;
	}

	return (PyObject *) npglo;
}

/* init from already known oid */
static PyObject *
pg_getlo(pgobject * self, PyObject * args)
{
	int             lo_oid;
	pglargeobject  *npglo;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "i", &lo_oid))
	{
		PyErr_SetString(PyExc_TypeError, "loopen(oid), with oid (integer).");
		return NULL;
	}

	if (!lo_oid)
	{
		PyErr_SetString(PyExc_ValueError, "the object oid can't be null.");
		return NULL;
	}

	/* creates object */
	if ((npglo = PyObject_NEW(pglargeobject, &PglargeType)) == NULL)
		return NULL;

	npglo->pgcnx = self;
	Py_XINCREF(self);
	npglo->lo_fd = -1;
	npglo->lo_oid = lo_oid;

	return (PyObject *) npglo;
}

/* import unix file */
static PyObject *
pg_loimport(pgobject * self, PyObject * args)
{
	char           *name;
	pglargeobject  *npglo;

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "s", &name))
	{
		PyErr_SetString(PyExc_TypeError, "loimport(name), with name (string).");
		return NULL;
	}

	if ((npglo = PyObject_NEW(pglargeobject, &PglargeType)) == NULL)
		return NULL;

	npglo->pgcnx = self;
	Py_XINCREF(self);
	npglo->lo_fd = -1;
	npglo->lo_oid = lo_import(self->cnx, name);

	/* checks result validity */
	if (npglo->lo_oid == 0)
	{
		PyErr_SetString(PGError, "can't create large object.");
		Py_XDECREF(npglo);
		return NULL;
	}

	return (PyObject *) npglo;
}

/* pglargeobject methods */

/* destructor */
static void
pglarge_dealloc(pglargeobject * self)
{
	if (self->lo_fd >= 0 && self->pgcnx->valid == 1)
		lo_close(self->pgcnx->cnx, self->lo_fd);

	Py_XDECREF(self->pgcnx);
	PyMem_DEL(self);
}

/* opens large object */
static PyObject *
pglarge_open(pglargeobject * self, PyObject * args)
{
	int             mode, fd;

	/* check validity */
	if (!check_lo(self, CHECK_CLOSE))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "i", &mode))
	{
		PyErr_SetString(PyExc_TypeError, "open(mode), with mode(integer).");
		return NULL;
	}

	/* opens large object */
	if ((fd = lo_open(self->pgcnx->cnx, self->lo_oid, mode)) < 0)
	{
		PyErr_SetString(PyExc_IOError, "can't open large object.");
		return NULL;
	}
	self->lo_fd = fd;

	/* no error : returns Py_None */
	Py_INCREF(Py_None);
	return Py_None;
}

/* close large object */
static PyObject *
pglarge_close(pglargeobject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method close() takes no parameters.");
		return NULL;
	}

	/* checks validity */
	if (!check_lo(self, CHECK_OPEN))
		return NULL;

	/* closes large object */
	if (lo_close(self->pgcnx->cnx, self->lo_fd))
	{
		PyErr_SetString(PyExc_IOError, "error while closing large object fd.");
		return NULL;
	}
	self->lo_fd = -1;

	/* no error : returns Py_None */
	Py_INCREF(Py_None);
	return Py_None;
}

/* reads from large object */
static PyObject *
pglarge_read(pglargeobject * self, PyObject * args)
{
	int             size;
	PyObject       *buffer;

	/* checks validity */
	if (!check_lo(self, CHECK_OPEN))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "i", &size))
	{
		PyErr_SetString(PyExc_TypeError, "read(size), wih size (integer).");
		return NULL;
	}

	if (size <= 0)
	{
		PyErr_SetString(PyExc_ValueError, "size must be positive.");
		return NULL;
	}

	/* allocate buffer and runs read */
	buffer = PyString_FromStringAndSize((char *) NULL, size);

	if ((size = lo_read(self->pgcnx->cnx, self->lo_fd, BUF(buffer), size)) < 0)
	{
		PyErr_SetString(PyExc_IOError, "error while reading.");
		Py_XDECREF(buffer);
		return NULL;
	}

	/* resize buffer and returns it */
	_PyString_Resize(&buffer, size);
	return buffer;
}

/* write to large object */
static PyObject *
pglarge_write(pglargeobject * self, PyObject * args)
{
	char           *buffer;
	int             size;

	/* checks validity */
	if (!check_lo(self, CHECK_OPEN))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "s", &buffer))
	{
		PyErr_SetString(PyExc_TypeError,
				"write(buffer), with buffer (sized string).");
		return NULL;
	}

	/* sends query */
	if ((size = lo_write(self->pgcnx->cnx, self->lo_fd, buffer,
					strlen(buffer))) <  strlen(buffer))
	{
		PyErr_SetString(PyExc_IOError, "buffer truncated during write.");
		return NULL;
	}

	/* no error : returns Py_None */
	Py_INCREF(Py_None);
	return Py_None;
}

/* go to position in large object */
static PyObject *
pglarge_lseek(pglargeobject * self, PyObject * args)
{
	int             ret, offset, whence;

	/* checks validity */
	if (!check_lo(self, CHECK_OPEN))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "ii", &offset, &whence))
	{
		PyErr_SetString(PyExc_TypeError,
				"lseek(offset, whence), with offset and whence (integers).");
		return NULL;
	}

	/* sends query */
	if ((ret = lo_lseek(self->pgcnx->cnx, self->lo_fd, offset, whence)) == -1)
	{
		PyErr_SetString(PyExc_IOError, "error while moving cursor.");
		return NULL;
	}

	/* returns position */
	return PyInt_FromLong(ret);
}

/* gets large object size */
static PyObject *
pglarge_size(pglargeobject * self, PyObject * args)
{
	int             start, end;

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method size() takes no parameters.");
		return NULL;
	}

	/* checks validity */
	if (!check_lo(self, CHECK_OPEN))
		return NULL;

	/* gets current position */
	if ((start = lo_tell(self->pgcnx->cnx, self->lo_fd)) == -1)
	{
		PyErr_SetString(PyExc_IOError, "error while getting current position.");
		return NULL;
	}

	/* gets end position */
	if ((end = lo_lseek(self->pgcnx->cnx, self->lo_fd, 0, SEEK_END)) == -1)
	{
		PyErr_SetString(PyExc_IOError, "error while getting end position.");
		return NULL;
	}

	/* move back to start position */
	if ((start = lo_lseek(self->pgcnx->cnx,self->lo_fd,start,SEEK_SET)) == -1)
	{
		PyErr_SetString(PyExc_IOError,
				"error while moving back to first position.");
		return NULL;
	}

	/* returns size */
	return PyInt_FromLong(end);
}

/* gets large object cursor position */
static PyObject *
pglarge_tell(pglargeobject * self, PyObject * args)
{
	int             start;

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method tell() takes no parameters.");
		return NULL;
	}

	/* checks validity */
	if (!check_lo(self, CHECK_OPEN))
		return NULL;

	/* gets current position */
	if ((start = lo_tell(self->pgcnx->cnx, self->lo_fd)) == -1)
	{
		PyErr_SetString(PyExc_IOError, "error while getting position.");
		return NULL;
	}

	/* returns size */
	return PyInt_FromLong(start);
}

/* exports large object as unix file */
static PyObject *
pglarge_export(pglargeobject * self, PyObject * args)
{
	char           *name;

	/* checks validity */
	if (!check_lo(self, CHECK_CLOSE))
		return NULL;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "s", &name))
	{
		PyErr_SetString(PyExc_TypeError,
				"export(filename), with filename (string).");
		return NULL;
	}

	/* runs command */
	if (!lo_export(self->pgcnx->cnx, self->lo_oid, name))
	{
		PyErr_SetString(PyExc_IOError, "error while exporting large object.");
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

/* deletes a large object */
static PyObject *
pglarge_unlink(pglargeobject * self, PyObject * args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method unlink() takes no parameters.");
		return NULL;
	}

	/* checks validity */
	if (!check_lo(self, CHECK_CLOSE))
		return NULL;

	/* deletes the object, invalidate it on success */
	if (!lo_unlink(self->pgcnx->cnx, self->lo_oid))
	{
		PyErr_SetString(PyExc_IOError, "error while unlinking large object");
		return NULL;
	}
	self->lo_oid = 0;

	Py_INCREF(Py_None);
	return Py_None;
}

/* large object methods */
static struct PyMethodDef pglarge_methods[] = {
	{"open",	(PyCFunction) pglarge_open, 1},	/* opens large object */
	{"close",	(PyCFunction) pglarge_close, 1},/* closes large object */
	{"read",	(PyCFunction) pglarge_read, 1},	/* reads from large object */
	{"write",	(PyCFunction) pglarge_write, 1},/* writes to large object */
	{"seek",	(PyCFunction) pglarge_lseek, 1},/* seeks position */
	{"size",	(PyCFunction) pglarge_size, 1},	/* gives object size */
	{"tell",	(PyCFunction) pglarge_tell, 1},	/* gives position in lobj */
	{"export",	(PyCFunction) pglarge_export, 1},/* exports to unix file */
	{"unlink",	(PyCFunction) pglarge_unlink, 1},/* deletes a large object */
	{NULL, NULL}								/* sentinel */
};

/* get attribute */
static PyObject *
pglarge_getattr(pglargeobject * self, char *name)
{
	/* list postgreSQL large object fields */

	/* associated pg connection object */
	if (!strcmp(name, "pgcnx"))
	{
		if (check_lo(self, 0))
		{
			Py_INCREF(self->pgcnx);
			return (PyObject *) (self->pgcnx);
		}

		Py_INCREF(Py_None);
		return Py_None;
	}

	/* large object oid */
	if (!strcmp(name, "oid"))
	{
		if (check_lo(self, 0))
			return PyInt_FromLong(self->lo_oid);

		Py_INCREF(Py_None);
		return Py_None;
	}

	/* error (status) message */
	if (!strcmp(name, "error"))
		return PyString_FromString(PQerrorMessage(self->pgcnx->cnx));

	/* attributes list */
	if (!strcmp(name, "__members__"))
	{
		PyObject       *list = PyList_New(3);

		if (list)
		{
			PyList_SetItem(list, 0, PyString_FromString("oid"));
			PyList_SetItem(list, 1, PyString_FromString("pgcnx"));
			PyList_SetItem(list, 2, PyString_FromString("error"));
		}

		return list;
	}

	return Py_FindMethod(pglarge_methods, (PyObject *) self, name);
}

/* object type definition */
staticforward PyTypeObject PglargeType = {
	PyObject_HEAD_INIT(NULL)
	0,								/* ob_size */
	"pglarge",						/* tp_name */
	sizeof(pglargeobject),			/* tp_basicsize */
	0,								/* tp_itemsize */

	/* methods */
	(destructor) pglarge_dealloc,	/* tp_dealloc */
	0,								/* tp_print */
	(getattrfunc) pglarge_getattr,	/* tp_getattr */
	0,								/* tp_setattr */
	0,								/* tp_compare */
	0,								/* tp_repr */
	0,								/* tp_as_number */
	0,								/* tp_as_sequence */
	0,								/* tp_as_mapping */
	0,								/* tp_hash */
};

#endif							/* LARGE_OBJECTS */


/* --------------------------------------------------------------------- */

/* PG CONNECTION OBJECT IMPLEMENTATION */

/* pgobject initialisation (from module) */

static PyObject *
pgconnect(pgobject *self, PyObject *args, PyObject *dict)
{
	static const char	*kwlist[] = { "dbname", "host", "port", "opt",
									"tty", "user", "passwd" , NULL };
	char           *pghost, *pgopt, *pgtty, *pgdbname, *pguser, *pgpasswd;
	int             pgport;
	char            port_buffer[20];
	pgobject       *npgobj;

	pghost = pgopt = pgtty = pgdbname = pguser = pgpasswd = NULL;
	pgport = -1;

	/* parses standard arguments */
	if (!PyArg_ParseTupleAndKeywords(args, dict, "|zzlzzzz", kwlist,
			&pgdbname, &pghost, &pgport, &pgopt, &pgtty, &pguser, &pgpasswd))
		return NULL;

#ifdef DEFAULT_VARS
	/* handles defaults variables (for unintialised vars) */
	if ((!pghost) && (pg_default_host != Py_None))
		pghost = PyString_AsString(pg_default_host);

	if ((pgport == -1) && (pg_default_port != Py_None))
		pgport = PyInt_AsLong(pg_default_port);

	if ((!pgopt) && (pg_default_opt != Py_None))
		pgopt = PyString_AsString(pg_default_opt);

	if ((!pgtty) && (pg_default_tty != Py_None))
		pgtty = PyString_AsString(pg_default_tty);

	if ((!pgdbname) && (pg_default_base != Py_None))
		pgdbname = PyString_AsString(pg_default_base);

	if ((!pguser) && (pg_default_user != Py_None))
		pguser = PyString_AsString(pg_default_user);

	if ((!pgpasswd) && (pg_default_passwd != Py_None))
		pgpasswd = PyString_AsString(pg_default_passwd);
#endif							/* DEFAULT_VARS */

	if ((npgobj = PyObject_NEW(pgobject, &PgType)) == NULL)
		return NULL;

	if (pgport != -1)
	{
		bzero(port_buffer, sizeof(port_buffer));
		sprintf(port_buffer, "%d", pgport);
		npgobj->cnx = PQsetdbLogin(pghost, port_buffer, pgopt, pgtty, pgdbname,
					pguser, pgpasswd);
	}
	else
		npgobj->cnx = PQsetdbLogin(pghost, NULL, pgopt, pgtty, pgdbname,
					pguser, pgpasswd);

	if (PQstatus(npgobj->cnx) == CONNECTION_BAD)
	{
		PyErr_SetString(PGError, PQerrorMessage(npgobj->cnx));
		Py_XDECREF(npgobj);
		return NULL;
	}

	return (PyObject *) npgobj;
}

/* pgobject methods */

/* destructor */
static void
pg_dealloc(pgobject * self)
{
	if (self->cnx)
		PQfinish(self->cnx);

	PyMem_DEL(self);
}

/* close without deleting */
static PyObject *
pg_close(pgobject *self, PyObject *args)
{
	/* gets args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_TypeError, "close().");
		return NULL;
	}

	if (self->cnx)
		PQfinish(self->cnx);

	self->cnx = NULL;
	Py_INCREF(Py_None);
	return Py_None;
}   

static void
pg_querydealloc(pgqueryobject * self)
{
	if (self->last_result)
		PQclear(self->last_result);

	PyMem_DEL(self);
}

/* resets connection */
static PyObject *
pg_reset(pgobject * self, PyObject * args)
{
	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method reset() takes no parameters.");
		return NULL;
	}

	/* resets the connection */
	PQreset(self->cnx);
	Py_INCREF(Py_None);
	return Py_None;
}

/* get connection socket */
static PyObject *
pg_fileno(pgobject * self, PyObject * args)
{
	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method fileno() takes no parameters.");
		return NULL;
	}

#ifdef		NO_PQSOCKET
	return PyInt_FromLong((long) self->cnx->sock);
#else
	return PyInt_FromLong((long) PQsocket(self->cnx));
#endif
}

/* list fields names from query result */
static PyObject *
pg_listfields(pgqueryobject * self, PyObject * args)
{
	int             i, n;
	char           *name;
	PyObject       *fieldstuple, *str;

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method listfields() takes no parameters.");
		return NULL;
	}

	/* builds tuple */
	n = PQnfields(self->last_result);
	fieldstuple = PyTuple_New(n);

	for (i = 0; i < n; i++)
	{
		name = PQfname(self->last_result, i);
		str = PyString_FromString(name);
		PyTuple_SetItem(fieldstuple, i, str);
	}

	return fieldstuple;
}

/* get field name from last result */
static PyObject *
pg_fieldname(pgqueryobject * self, PyObject * args)
{
	int             i;
	char           *name;

	/* gets args */
	if (!PyArg_ParseTuple(args, "i", &i))
	{
		PyErr_SetString(PyExc_TypeError,
				"fieldname(number), with number(integer).");
		return NULL;
	}

	/* checks number validity */
	if (i >= PQnfields(self->last_result))
	{
		PyErr_SetString(PyExc_ValueError, "invalid field number.");
		return NULL;
	}

	/* gets fields name and builds object */
	name = PQfname(self->last_result, i);
	return PyString_FromString(name);
}

/* gets fields number from name in last result */
static PyObject *
pg_fieldnum(pgqueryobject * self, PyObject * args)
{
	char           *name;
	int             num;

	/* gets args */
	if (!PyArg_ParseTuple(args, "s", &name))
	{
		PyErr_SetString(PyExc_TypeError, "fieldnum(name), with name (string).");
		return NULL;
	}

	/* gets field number */
	if ((num = PQfnumber(self->last_result, name)) == -1)
	{
		PyErr_SetString(PyExc_ValueError, "Unknown field.");
		return NULL;
	}

	return PyInt_FromLong(num);
}

/* retrieves last result */
static PyObject *
pg_getresult(pgqueryobject * self, PyObject * args)
{
	PyObject       *rowtuple, *reslist, *val;
	int             i, j, m, n, *typ;

	/* checks args (args == NULL for an internal call) */
	if ((args != NULL) && (!PyArg_ParseTuple(args, "")))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method getresult() takes no parameters.");
		return NULL;
	}

	/* stores result in tuple */
	reslist = PyList_New(0);
	m = PQntuples(self->last_result);
	n = PQnfields(self->last_result);

	if ((typ = malloc(sizeof(int) * n)) == NULL)
	{
		PyErr_SetString(PyExc_SyntaxError, "memory error in getresult().");
		return NULL;
	}

	for (j = 0; j < n; j++)
	{
		switch (PQftype(self->last_result, j))
		{
			case INT2OID:
			case INT4OID:
			case OIDOID:
				typ[j] = 1;
				break;

			case FLOAT4OID:
			case FLOAT8OID:
				typ[j] = 2;
				break;

			case CASHOID:
				typ[j] = 3;
				break;

			default:
				typ[j] = 4;
				break;
		}
	}

	for (i = 0; i < m; i++)
	{
		rowtuple = PyTuple_New(n);
		for (j = 0; j < n; j++)
		{
			char	*s = PQgetvalue(self->last_result, i, j);
			char	cashbuf[64];

			switch (typ[j])
			{
				case 1:
					val = PyInt_FromLong(strtol(s, NULL, 10));
					break;

				case 2:
					val = PyFloat_FromDouble(strtod(s, NULL));
					break;

				case 3:		/* get rid of the '$' and commas */
					if (*s == '$')	/* there's talk of getting rid of it */
						s++;

					for (i = 0; *s; s++)
						if (*s != ',')
							cashbuf[i++] = *s;

					cashbuf[i] = 0;
					val = PyFloat_FromDouble(strtod(cashbuf, NULL));
					break;

				default:
					val = PyString_FromString(s);
					break;
			}

			PyTuple_SetItem(rowtuple, j, val);
		}

		PyList_Append(reslist, rowtuple);
		Py_XDECREF(rowtuple);
	}

	free(typ);

	/* returns list */
	return reslist;
}

/* retrieves last result as a list of dictionaries*/
static PyObject *
pg_dictresult(pgqueryobject * self, PyObject * args)
{
	PyObject       *dict, *reslist, *val;
	int             i, j, m, n, *typ;

	/* checks args (args == NULL for an internal call) */
	if ((args != NULL) && (!PyArg_ParseTuple(args, "")))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method getresult() takes no parameters.");
		return NULL;
	}

	/* stores result in list */
	reslist = PyList_New(0);
	m = PQntuples(self->last_result);
	n = PQnfields(self->last_result);

	if ((typ = malloc(sizeof(int) * n)) == NULL)
	{
		PyErr_SetString(PyExc_SyntaxError, "memory error in getresult().");
		return NULL;
	}

	for (j = 0; j < n; j++)
	{
		switch (PQftype(self->last_result, j))
		{
			case INT2OID:
			case INT4OID:
			case OIDOID:
				typ[j] = 1;
				break;

			case FLOAT4OID:
			case FLOAT8OID:
				typ[j] = 2;
				break;

			case CASHOID:
				typ[j] = 3;
				break;

			default:
				typ[j] = 4;
				break;
		}
	}

	for (i = 0; i < m; i++)
	{
		dict = PyDict_New();
		for (j = 0; j < n; j++)
		{
			int		k;
			char	*s = PQgetvalue(self->last_result, i, j);
			char	cashbuf[64];

			switch (typ[j])
			{
				case 1:
					val = PyInt_FromLong(strtol(s, NULL, 10));
					break;

				case 2:
					val = PyFloat_FromDouble(strtod(s, NULL));
					break;

				case 3:		/* get rid of the '$' and commas */
					if (*s == '$')	/* there's talk of getting rid of it */
						s++;

					for (k = 0; *s; s++)
						if (*s != ',')
							cashbuf[k++] = *s;

					cashbuf[k] = 0;
					val = PyFloat_FromDouble(strtod(cashbuf, NULL));
					break;

				default:
					val = PyString_FromString(s);
					break;
			}

			PyDict_SetItemString(dict, PQfname(self->last_result, j), val);
			Py_XDECREF(val);
		}

		PyList_Append(reslist, dict);
		Py_XDECREF(dict);
	}

	free(typ);

	/* returns list */
	return reslist;
}

/* getq asynchronous notify */
static PyObject *
pg_getnotify(pgobject * self, PyObject * args)
{
	PGnotify       *notify;
	PGresult       *result;
	PyObject       *notify_result, *temp;

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method getnotify() takes no parameters.");
		return NULL;
	}

	/* gets notify and builds result */
	/* notifies only come back as result of a query, so I send an empty query */
	result = PQexec(self->cnx, " ");

	if ((notify = PQnotifies(self->cnx)) != NULL)
	{
		notify_result = PyTuple_New(2);
		temp = PyString_FromString(notify->relname);
		PyTuple_SetItem(notify_result, 0, temp);
		temp = PyInt_FromLong(notify->be_pid);
		PyTuple_SetItem(notify_result, 1, temp);
		free(notify);
	}
	else
	{
		Py_INCREF(Py_None);
		notify_result = Py_None;
	}

	PQclear(result);

	/* returns result */
	return notify_result;
}

/* database query */
static PyObject *
pg_query(pgobject * self, PyObject * args)
{
	char           *query;
	PGresult       *result;
	pgqueryobject  *npgobj;
	int             status;

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* get query args */
	if (!PyArg_ParseTuple(args, "s", &query))
	{
		PyErr_SetString(PyExc_TypeError, "query(sql), with sql (string).");
		return NULL;
	}

	/* gets result */
	result = PQexec(self->cnx, query);

	/* checks result validity */
	if (!result)
	{
		PyErr_SetString(PyExc_ValueError, PQerrorMessage(self->cnx));
		return NULL;
	}

	/* checks result status */
	if ((status = PQresultStatus(result)) != PGRES_TUPLES_OK)
	{
		const char *str;

		PQclear(result);

		switch (status)
		{
			case PGRES_EMPTY_QUERY:
				PyErr_SetString(PyExc_ValueError, "empty query.");
				break;
			case PGRES_BAD_RESPONSE:
			case PGRES_FATAL_ERROR:
			case PGRES_NONFATAL_ERROR:
				PyErr_SetString(PGError, PQerrorMessage(self->cnx));
				break;
			case PGRES_COMMAND_OK:	/* could be an INSERT */
				if (*(str = PQoidStatus(result)) == 0)	/* nope */
				{
					Py_INCREF(Py_None);
					return Py_None;
				}

				/* otherwise, return the oid */
				return PyInt_FromLong(strtol(str, NULL, 10));

			case PGRES_COPY_OUT:	/* no data will be received */
			case PGRES_COPY_IN:
				Py_INCREF(Py_None);
				return Py_None;
			default:
				PyErr_SetString(PGError, "internal error: "
											"unknown result status.");
				break;
		}

		return NULL;			/* error detected on query */
	}

	if ((npgobj = PyObject_NEW(pgqueryobject, &PgQueryType)) == NULL)
		return NULL;

	/* stores result and returns object */
	npgobj->last_result = result;
	return (PyObject *) npgobj;
}

#ifdef DIRECT_ACCESS
/* direct acces function : putline */
static PyObject *
pg_putline(pgobject * self, PyObject * args)
{
	char           *line;

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* reads args */
	if (!PyArg_ParseTuple(args, "s", &line))
	{
		PyErr_SetString(PyExc_TypeError, "putline(line), with line (string).");
		return NULL;
	}

	/* sends line to backend */
	PQputline(self->cnx, line);
	Py_INCREF(Py_None);
	return Py_None;
}

/* direct access function : getline */
static PyObject *
pg_getline(pgobject * self, PyObject * args)
{
	char            line[MAX_BUFFER_SIZE];
	PyObject       *str = NULL; /* GCC */
	int             ret;

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method getline() takes no parameters.");
		return NULL;
	}

	/* gets line */
	switch (PQgetline(self->cnx, line, MAX_BUFFER_SIZE))
	{
		case 0:
			str = PyString_FromString(line);
			break;
		case 1:
			PyErr_SetString(PyExc_MemoryError, "buffer overflow");
			str = NULL;
			break;
		case EOF:
			Py_INCREF(Py_None);
			str = Py_None;
			break;
	}

	return str;
}

/* direct access function : end copy */
static PyObject *
pg_endcopy(pgobject * self, PyObject * args)
{
	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method endcopy() takes no parameters.");
		return NULL;
	}

	/* ends direct copy */
	PQendcopy(self->cnx);
	Py_INCREF(Py_None);
	return Py_None;
}
#endif							/* DIRECT_ACCESS */


static PyObject *
pg_print(pgqueryobject *self, FILE *fp, int flags)
{
	PQprintOpt		op;

	memset(&op, 0, sizeof(op));
	op.align = 1; 
	op.header = 1;
	op.fieldSep = "|";
	op.pager = 1;
	PQprint(fp, self->last_result, &op);
	return 0;
}

/* insert table */
static PyObject *
pg_inserttable(pgobject * self, PyObject * args)
{
	PGresult       *result;
	char           *table, *buffer, *temp;
	char            temp_buffer[256];
	PyObject       *list, *sublist, *item;
	PyObject       *(*getitem) (PyObject *, int);
	PyObject       *(*getsubitem) (PyObject *, int);
	int             i, j;

	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "sO:filter", &table, &list))
	{
		PyErr_SetString(PyExc_TypeError,
				"tableinsert(table, content), with table (string) "
					"and content (list).");
		return NULL;
	}

	/* checks list type */
	if (PyTuple_Check(list))
		getitem = PyTuple_GetItem;
	else if (PyList_Check(list))
		getitem = PyList_GetItem;
	else
	{
		PyErr_SetString(PyExc_TypeError,
				"second arg must be some kind of array.");
		return NULL;
	}

	/* checks sublists type */
	for (i = 0; (sublist = getitem(list, i)) != NULL; i++)
	{
		if (!PyTuple_Check(sublist) && !PyList_Check(sublist))
		{
			PyErr_SetString(PyExc_TypeError,
					"second arg must contain some kind of arrays.");
			return NULL;
		}
	}

	/* allocate buffer */
	if (!(buffer = malloc(MAX_BUFFER_SIZE)))
	{
		PyErr_SetString(PyExc_MemoryError, "can't allocate insert buffer.");
		return NULL;
	}

	/* starts query */
	sprintf(buffer, "copy %s from stdin", table);

	if (!(result = PQexec(self->cnx, buffer)))
	{
		free(buffer);
		PyErr_SetString(PyExc_ValueError, PQerrorMessage(self->cnx));
		return NULL;
	}

	PQclear(result);

	/* feeds table */
	for (i = 0; (sublist = getitem(list, i)) != NULL; i++)
	{
		if (PyTuple_Check(sublist))
			getsubitem = PyTuple_GetItem;
		else
			getsubitem = PyList_GetItem;

		/* builds insert line */
		buffer[0] = 0;

		for (j = 0; (item = getsubitem(sublist, j)) != NULL; j++)
		{
			/* converts item to string */
			if (PyString_Check(item))
				PyArg_ParseTuple(item, "s", &temp);
			else if (PyInt_Check(item))
			{
				int             k;

				PyArg_ParseTuple(item, "i", &k);
				sprintf(temp_buffer, "%d", k);
				temp = temp_buffer;
			}
			else if (PyLong_Check(item))
			{
				long            k;

				PyArg_ParseTuple(item, "l", &k);
				sprintf(temp_buffer, "%ld", k);
				temp = temp_buffer;
			}
			else if (PyFloat_Check(item))
			{
				double          k;

				PyArg_ParseTuple(item, "d", &k);
				sprintf(temp_buffer, "%g", k);
				temp = temp_buffer;
			}
			else
			{
				free(buffer);
				PyErr_SetString(PyExc_ValueError,
						"items must be strings, integers, "
							"longs or double (real).");
				return NULL;
			}

			/* concats buffer */
			if (strlen(buffer))
				strncat(buffer, "\t", MAX_BUFFER_SIZE - strlen(buffer));

			fprintf(stderr, "Buffer: '%s', Temp: '%s'\n", buffer, temp);
			strncat(buffer, temp, MAX_BUFFER_SIZE - strlen(buffer));
		}

		strncat(buffer, "\n", MAX_BUFFER_SIZE - strlen(buffer));

		/* sends data */
		PQputline(self->cnx, buffer);
	}

	/* ends query */
	PQputline(self->cnx, ".\n");
	PQendcopy(self->cnx);
	free(buffer);

	/* no error : returns nothing */
	Py_INCREF(Py_None);
	return Py_None;
}

/* connection object methods */
static struct PyMethodDef pgobj_methods[] = {
	{"query",		(PyCFunction) pg_query, 1},			/* query method */
	{"reset",		(PyCFunction) pg_reset, 1},			/* connection reset */
	{"close",		(PyCFunction) pg_close, 1},			/* connection close */
	{"fileno",		(PyCFunction) pg_fileno,1},			/* connection socket */
	{"getnotify",	(PyCFunction) pg_getnotify, 1},		/* checks for notify */
	{"inserttable",	(PyCFunction) pg_inserttable, 1},	/* table insert */

#ifdef DIRECT_ACCESS
	{"putline",		(PyCFunction) pg_putline, 1},	/* direct access: putline */
	{"getline",		(PyCFunction) pg_getline, 1},	/* direct access: getline */
	{"endcopy",		(PyCFunction) pg_endcopy, 1},	/* direct access: endcopy */
#endif							/* DIRECT_ACCESS */

#ifdef LARGE_OBJECTS
	{"locreate",	(PyCFunction) pg_locreate, 1},	/* creates large object */
	{"getlo",		(PyCFunction) pg_getlo, 1},		/* get lo from oid */
	{"loimport",	(PyCFunction) pg_loimport, 1},	/* imports lo from file */
#endif							/* LARGE_OBJECTS */

	{NULL, NULL}				/* sentinel */
};

/* get attribute */
static PyObject *
pg_getattr(pgobject * self, char *name)
{
	/* Although we could check individually, there are only a few
	   attributes that don't require a live connection and unless
	   someone has an urgent need, this will have to do */
	if (!self->cnx)
	{
		PyErr_SetString(PyExc_TypeError, "Connection is not valid");
		return NULL;
	}

	/* list postgreSQL connection fields */

	/* postmaster host */
	if (!strcmp(name, "host"))
	{
		char	*r = PQhost(self->cnx);
		return r ? PyString_FromString(r) : PyString_FromString("localhost");
	}

	/* postmaster port */
	if (!strcmp(name, "port"))
		return PyInt_FromLong(atol(PQport(self->cnx)));

	/* selected database */
	if (!strcmp(name, "db"))
		return PyString_FromString(PQdb(self->cnx));

	/* selected options */
	if (!strcmp(name, "options"))
		return PyString_FromString(PQoptions(self->cnx));

	/* selected postgres tty */
	if (!strcmp(name, "tty"))
		return PyString_FromString(PQtty(self->cnx));

	/* error (status) message */
	if (!strcmp(name, "error"))
		return PyString_FromString(PQerrorMessage(self->cnx));

	/* connection status : 1 - OK, 0 - BAD */
	if (!strcmp(name, "status"))
		return PyInt_FromLong(PQstatus(self->cnx) == CONNECTION_OK ? 1 : 0);

	/* provided user name */
	if (!strcmp(name, "user"))
		return PyString_FromString("Deprecated facility");
		/* return PyString_FromString(fe_getauthname("<unknown user>")); */

	/* attributes list */
	if (!strcmp(name, "__members__"))
	{
		PyObject       *list = PyList_New(8);

		if (list)
		{
			PyList_SetItem(list, 0, PyString_FromString("host"));
			PyList_SetItem(list, 1, PyString_FromString("port"));
			PyList_SetItem(list, 2, PyString_FromString("db"));
			PyList_SetItem(list, 3, PyString_FromString("options"));
			PyList_SetItem(list, 4, PyString_FromString("tty"));
			PyList_SetItem(list, 5, PyString_FromString("error"));
			PyList_SetItem(list, 6, PyString_FromString("status"));
			PyList_SetItem(list, 7, PyString_FromString("user"));
		}

		return list;
	}

	return Py_FindMethod(pgobj_methods, (PyObject *) self, name);
}

/* object type definition */
staticforward PyTypeObject PgType = {
	PyObject_HEAD_INIT(NULL)
	0,							/* ob_size */
	"pgobject",					/* tp_name */
	sizeof(pgobject),			/* tp_basicsize */
	0,							/* tp_itemsize */
	/* methods */
	(destructor) pg_dealloc,	/* tp_dealloc */
	0,							/* tp_print */
	(getattrfunc) pg_getattr,	/* tp_getattr */
	0,							/* tp_setattr */
	0,							/* tp_compare */
	0,							/* tp_repr */
	0,							/* tp_as_number */
	0,							/* tp_as_sequence */
	0,							/* tp_as_mapping */
	0,							/* tp_hash */
};


/* query object methods */
static struct PyMethodDef pgquery_methods[] = {
	{"getresult",	(PyCFunction) pg_getresult, 1},		/* get last result */
	{"dictresult",	(PyCFunction) pg_dictresult, 1},	/* get result as dict*/
	{"fieldname",	(PyCFunction) pg_fieldname, 1},		/* get field name */
	{"fieldnum",	(PyCFunction) pg_fieldnum, 1},		/* get field number */
	{"listfields",	(PyCFunction) pg_listfields, 1},	/* list fields names */
	{NULL, NULL}				/* sentinel */
};

static PyObject *
pg_querygetattr(pgqueryobject * self, char *name)
{
	/* list postgreSQL connection fields */
	return Py_FindMethod(pgquery_methods, (PyObject *) self, name);
}

/* query type definition */
staticforward PyTypeObject PgQueryType = {
	PyObject_HEAD_INIT(NULL)
	0,							/* ob_size */
	"pgqueryobject",			/* tp_name */
	sizeof(pgqueryobject),		/* tp_basicsize */
	0,							/* tp_itemsize */
	/* methods */
	(destructor) pg_querydealloc,/* tp_dealloc */
	(printfunc) pg_print,		/* tp_print */
	(getattrfunc) pg_querygetattr,/* tp_getattr */
	0,							/* tp_setattr */
	0,							/* tp_compare */
	0,							/* tp_repr */
	0,							/* tp_as_number */
	0,							/* tp_as_sequence */
	0,							/* tp_as_mapping */
	0,							/* tp_hash */
};



/* --------------------------------------------------------------------- */

/* MODULE FUNCTIONS */

#ifdef DEFAULT_VARS

/* gets default host */
static PyObject       *
pggetdefhost(PyObject *self, PyObject *args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method get_defhost() takes no parameter.");
		return NULL;
	}

	Py_XINCREF(pg_default_host);
	return pg_default_host;
}

/* sets default host */
static PyObject       *
pgsetdefhost(PyObject * self, PyObject *args)
{
	char           *temp = NULL;
	PyObject       *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
				"set_defhost(name), with name (string/None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_host;

	if (temp)
		pg_default_host = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_host = Py_None;
	}

	return old;
}

/* gets default base */
static PyObject       *
pggetdefbase(PyObject * self, PyObject *args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method get_defbase() takes no parameter.");
		return NULL;
	}

	Py_XINCREF(pg_default_base);
	return pg_default_base;
}

/* sets default base */
static PyObject       *
pgsetdefbase(PyObject * self, PyObject *args)
{
	char           *temp = NULL;
	PyObject       *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
				"set_defbase(name), with name (string/None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_base;

	if (temp)
		pg_default_base = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_base = Py_None;
	}

	return old;
}

/* gets default options */
static PyObject       *
pggetdefopt(PyObject * self, PyObject *args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method get_defopt() takes no parameter.");
		return NULL;
	}

	Py_XINCREF(pg_default_opt);
	return pg_default_opt;
}

/* sets default opt */
static PyObject       *
pgsetdefopt(PyObject * self, PyObject *args)
{
	char           *temp = NULL;
	PyObject       *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
				"set_defopt(name), with name (string/None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_opt;

	if (temp)
		pg_default_opt = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_opt = Py_None;
	}

	return old;
}

/* gets default tty */
static PyObject       *
pggetdeftty(PyObject * self, PyObject *args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method get_deftty() takes no parameter.");
		return NULL;
	}

	Py_XINCREF(pg_default_tty);
	return pg_default_tty;
}

/* sets default tty */
static PyObject       *
pgsetdeftty(PyObject * self, PyObject *args)
{
	char           *temp = NULL;
	PyObject       *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
				"set_deftty(name), with name (string/None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_tty;

	if (temp)
		pg_default_tty = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_tty = Py_None;
	}

	return old;
}

/* gets default username */
static PyObject       *
pggetdefuser(PyObject * self, PyObject *args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
			"method get_defuser() takes no parameter.");

		return NULL;
	}

	Py_XINCREF(pg_default_user);
	return pg_default_user;
}

/* sets default username */
static PyObject       *
pgsetdefuser(PyObject * self, PyObject *args)
{
	char           *temp = NULL;
	PyObject       *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
				"set_defuser(name), with name (string/None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_user;

	if (temp)
		pg_default_user = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_user = Py_None;
	}

	return old;
}

/* sets default password */
static PyObject       *
pgsetdefpasswd(PyObject * self, PyObject *args)
{
	char           *temp = NULL;
	PyObject       *old;

	/* gets arguments */
	if (!PyArg_ParseTuple(args, "z", &temp))
	{
		PyErr_SetString(PyExc_TypeError,
				"set_defpasswd(password), with password (string/
None).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_passwd;

	if (temp)
		pg_default_passwd = PyString_FromString(temp);
	else
	{
		Py_INCREF(Py_None);
		pg_default_passwd = Py_None;
	}

	return old;
}

/* gets default port */
static PyObject       *
pggetdefport(PyObject * self, PyObject *args)
{
	/* checks args */
	if (!PyArg_ParseTuple(args, ""))
	{
		PyErr_SetString(PyExc_SyntaxError,
				"method get_defport() takes no parameter.");
		return NULL;
	}

	Py_XINCREF(pg_default_port);
	return pg_default_port;
}

/* sets default port */
static PyObject       *
pgsetdefport(PyObject * self, PyObject *args)
{
	long int        port = -2;
	PyObject       *old;

	/* gets arguments */
	if ((!PyArg_ParseTuple(args, "l", &port)) || (port < -1))
	{
		PyErr_SetString(PyExc_TypeError, "set_defport(port), with port "
					"(positive integer/-1).");
		return NULL;
	}

	/* adjusts value */
	old = pg_default_port;

	if (port != -1)
		pg_default_port = PyLong_FromLong(port);
	else
	{
		Py_INCREF(Py_None);
		pg_default_port = Py_None;
	}

	return old;
}

#endif							/* DEFAULT_VARS */

/* List of functions defined in the module */

static struct PyMethodDef pg_methods[] = {
	{"connect", (PyCFunction) pgconnect, 3},	/* connect to a database */

#ifdef DEFAULT_VARS
	{"get_defhost",		pggetdefhost, 1},		/* gets default host */
	{"set_defhost",		pgsetdefhost, 1},		/* sets default host */
	{"get_defbase",		pggetdefbase, 1},		/* gets default base */
	{"set_defbase",		pgsetdefbase, 1},		/* sets default base */
	{"get_defopt",		pggetdefopt, 1},		/* gets default options */
	{"set_defopt",		pgsetdefopt, 1},		/* sets default options */
	{"get_deftty",		pggetdeftty, 1},		/* gets default debug tty */
	{"set_deftty",		pgsetdeftty, 1},		/* sets default debug tty */
	{"get_defport",		pggetdefport, 1},		/* gets default port */
	{"set_defport",		pgsetdefport, 1},		/* sets default port */
	{"get_defuser",		pggetdefuser, 1},		/* gets default user */
	{"set_defuser",		pgsetdefuser, 1},		/* sets default user */
	{"set_defpasswd",	pgsetdefpasswd, 1},		/* sets default passwd */
#endif							/* DEFAULT_VARS */
	{NULL, NULL}				/* sentinel */
};

static char pg__doc__[] = "Python interface to PostgreSQL DB"; 

/* Initialization function for the module */
void init_pg(void);		/* Python doesn't prototype this */
void
init_pg(void)
{
	PyObject       *mod, *dict, *v;

	/* Initialize here because some WIN platforms get confused otherwise */
	PglargeType.ob_type = PgType.ob_type = PgQueryType.ob_type = &PyType_Type;

	/* Create the module and add the functions */
	mod = Py_InitModule4("_pg", pg_methods, pg__doc__, NULL, PYTHON_API_VERSION);
	dict = PyModule_GetDict(mod);

	/* Add some symbolic constants to the module */
	PGError = PyString_FromString("pg.error");
	PyDict_SetItemString(dict, "error", PGError);

	/* Make the version available */
	v = PyString_FromString(PyPgVersion);
	PyDict_SetItemString(dict, "version", v);
	PyDict_SetItemString(dict, "__version__", v);
	Py_DECREF(v);

#ifdef LARGE_OBJECTS
	/* create mode for large objects */
	PyDict_SetItemString(dict, "INV_READ", PyInt_FromLong(INV_READ));
	PyDict_SetItemString(dict, "INV_WRITE", PyInt_FromLong(INV_WRITE));

	/* position flags for lo_lseek */
	PyDict_SetItemString(dict, "SEEK_SET", PyInt_FromLong(SEEK_SET));
	PyDict_SetItemString(dict, "SEEK_CUR", PyInt_FromLong(SEEK_CUR));
	PyDict_SetItemString(dict, "SEEK_END", PyInt_FromLong(SEEK_END));
#endif							/* LARGE_OBJECTS */

#ifdef DEFAULT_VARS
	/* prepares default values */
	Py_INCREF(Py_None); pg_default_host = Py_None;
	Py_INCREF(Py_None); pg_default_base = Py_None;
	Py_INCREF(Py_None); pg_default_opt = Py_None;
	Py_INCREF(Py_None); pg_default_port = Py_None;
	Py_INCREF(Py_None); pg_default_tty = Py_None;
	Py_INCREF(Py_None); pg_default_user = Py_None;
	Py_INCREF(Py_None); pg_default_passwd = Py_None;
#endif							/* DEFAULT_VARS */

	/* Check for errors */
	if (PyErr_Occurred())
		Py_FatalError("can't initialize module _pg");
}