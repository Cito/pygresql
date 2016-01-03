#! /usr/bin/python
#
# pgdb.py
#
# Written by D'Arcy J.M. Cain
#
# $Id$
#

"""pgdb - DB-API 2.0 compliant module for PygreSQL.

(c) 1999, Pascal Andre <andre@via.ecp.fr>.
See package documentation for further information on copyright.

Inline documentation is sparse.
See DB-API 2.0 specification for usage information:
http://www.python.org/peps/pep-0249.html

Basic usage:

    pgdb.connect(connect_string) # open a connection
    # connect_string = 'host:database:user:password:opt'
    # All parts are optional. You may also pass host through
    # password as keyword arguments. To pass a port,
    # pass it in the host keyword parameter:
    connection = pgdb.connect(host='localhost:5432')

    cursor = connection.cursor() # open a cursor

    cursor.execute(query[, params])
    # Execute a query, binding params (a dictionary) if they are
    # passed. The binding syntax is the same as the % operator
    # for dictionaries, and no quoting is done.

    cursor.executemany(query, list of params)
    # Execute a query many times, binding each param dictionary
    # from the list.

    cursor.fetchone() # fetch one row, [value, value, ...]

    cursor.fetchall() # fetch all rows, [[value, value, ...], ...]

    cursor.fetchmany([size])
    # returns size or cursor.arraysize number of rows,
    # [[value, value, ...], ...] from result set.
    # Default cursor.arraysize is 1.

    cursor.description # returns information about the columns
    #	[(column_name, type_name, display_size,
    #		internal_size, precision, scale, null_ok), ...]
    # Note that display_size, precision, scale and null_ok
    # are not implemented.

    cursor.rowcount # number of rows available in the result set
    # Available after a call to execute.

    connection.commit() # commit transaction

    connection.rollback() # or rollback transaction

    cursor.close() # close the cursor

    connection.close() # close the connection

"""

from __future__ import print_function

from _pg import *

from datetime import date, time, datetime, timedelta
from time import localtime
from decimal import Decimal
from math import isnan, isinf
from collections import namedtuple

try:
    long
except NameError:  # Python >= 3.0
    long = int

try:
    unicode
except NameError:  # Python >= 3.0
    unicode = str

try:
    basestring
except NameError:  # Python >= 3.0
    basestring = (str, bytes)

try:
    from collections import OrderedDict
except ImportError:  # Python 2.6 or 3.0
    try:
        from ordereddict import OrderedDict
    except Exception:
        def OrderedDict(*args):
            raise NotSupportedError('OrderedDict is not supported')

set_decimal(Decimal)


### Module Constants

# compliant with DB API 2.0
apilevel = '2.0'

# module may be shared, but not connections
threadsafety = 1

# this module use extended python format codes
paramstyle = 'pyformat'

# shortcut methods are not supported by default
# since they have been excluded from DB API 2
# and are not recommended by the DB SIG.

shortcutmethods = 0


### Internal Types Handling

def decimal_type(decimal_type=None):
    """Get or set global type to be used for decimal values."""
    global Decimal
    if decimal_type is not None:
        _cast['numeric'] = Decimal = decimal_type
        set_decimal(Decimal)
    return Decimal


def _cast_bool(value):
    return value[:1] in ('t', 'T')


def _cast_money(value):
    return Decimal(''.join(filter(
        lambda v: v in '0123456789.-', value)))


def _cast_bytea(value):
    return unescape_bytea(value)


def _cast_float(value):
    try:
        return float(value)
    except ValueError:
        if value == 'NaN':
            return nan
        elif value == 'Infinity':
            return inf
        elif value == '-Infinity':
            return -inf
        raise


_cast = {'bool': _cast_bool, 'bytea': _cast_bytea,
    'int2': int, 'int4': int, 'serial': int,
    'int8': long, 'oid': long, 'oid8': long,
    'float4': _cast_float, 'float8': _cast_float,
    'numeric': Decimal, 'money': _cast_money}


def _db_error(msg, cls=DatabaseError):
    """Returns DatabaseError with empty sqlstate attribute."""
    error = cls(msg)
    error.sqlstate = None
    return error


def _op_error(msg):
    """Returns OperationalError."""
    return _db_error(msg, OperationalError)


class TypeCache(dict):
    """Cache for database types."""

    def __init__(self, cnx):
        """Initialize type cache for connection."""
        super(TypeCache, self).__init__()
        self._src = cnx.source()

    @staticmethod
    def typecast(typ, value):
        """Cast value to database type."""
        if value is None:
            # for NULL values, no typecast is necessary
            return None
        cast = _cast.get(typ)
        if cast is None:
            # no typecast available or necessary
            return value
        else:
            return cast(value)

    def getdescr(self, oid):
        """Get name of database type with given oid."""
        try:
            return self[oid]
        except KeyError:
            self._src.execute(
                "SELECT typname, typlen "
                "FROM pg_type WHERE oid=%s" % oid)
            res = self._src.fetch(1)[0]
            # The column name is omitted from the return value.
            # It will have to be prepended by the caller.
            res = (res[0], None, int(res[1]), None, None, None)
            self[oid] = res
            return res


class _quotedict(dict):
    """Dictionary with auto quoting of its items.

    The quote attribute must be set to the desired quote function.

    """

    def __getitem__(self, key):
        return self.quote(super(_quotedict, self).__getitem__(key))


### Cursor Object

class Cursor(object):
    """Cursor object."""

    def __init__(self, dbcnx):
        """Create a cursor object for the database connection."""
        self.connection = self._dbcnx = dbcnx
        self._cnx = dbcnx._cnx
        self._type_cache = dbcnx._type_cache
        self._src = self._cnx.source()
        # the official attribute for describing the result columns
        self.description = None
        # unofficial attributes for convenience and performance
        self.colnames = self.coltypes = None
        if self.row_factory is Cursor.row_factory:
            # the row factory needs to be determined dynamically
            self.row_factory = None
        else:
            self.build_row_factory = None
        self.rowcount = -1
        self.arraysize = 1
        self.lastrowid = None

    def __iter__(self):
        """Make cursors compatible to the iteration protocol."""
        return self

    def __enter__(self):
        """Enter the runtime context for the cursor object."""
        return self

    def __exit__(self, et, ev, tb):
        """Exit the runtime context for the cursor object."""
        self.close()

    def _quote(self, val):
        """Quote value depending on its type."""
        if isinstance(val, (datetime, date, time, timedelta)):
            val = str(val)
        if isinstance(val, basestring):
            if isinstance(val, Binary):
                val = self._cnx.escape_bytea(val)
                if bytes is not str:  # Python >= 3.0
                    val = val.decode('ascii')
            else:
                val = self._cnx.escape_string(val)
            val = "'%s'" % val
        elif isinstance(val, (int, long)):
            pass
        elif isinstance(val, float):
            if isinf(val):
                return "'-Infinity'" if val < 0 else "'Infinity'"
            elif isnan(val):
                return "'NaN'"
        elif val is None:
            val = 'NULL'
        elif isinstance(val, (list, tuple)):
            val = '(%s)' % ','.join(map(lambda v: str(self._quote(v)), val))
        elif Decimal is not float and isinstance(val, Decimal):
            pass
        elif hasattr(val, '__pg_repr__'):
            val = val.__pg_repr__()
        else:
            raise InterfaceError(
                'do not know how to handle type %s' % type(val))
        return val

    def _quoteparams(self, string, parameters):
        """Quote parameters.

        This function works for both mappings and sequences.

        """
        if isinstance(parameters, dict):
            parameters = _quotedict(parameters)
            parameters.quote = self._quote
        else:
            parameters = tuple(map(self._quote, parameters))
        return string % parameters

    def close(self):
        """Close the cursor object."""
        self._src.close()
        self.description = None
        self.colnames = self.coltypes = None
        self.rowcount = -1
        self.lastrowid = None

    def execute(self, operation, parameters=None):
        """Prepare and execute a database operation (query or command)."""

        # The parameters may also be specified as list of
        # tuples to e.g. insert multiple rows in a single
        # operation, but this kind of usage is deprecated:
        if (parameters and isinstance(parameters, list) and
                isinstance(parameters[0], tuple)):
            return self.executemany(operation, parameters)
        else:
            # not a list of tuples
            return self.executemany(operation, [parameters])

    def executemany(self, operation, seq_of_parameters):
        """Prepare operation and execute it against a parameter sequence."""
        if not seq_of_parameters:
            # don't do anything without parameters
            return
        self.description = None
        self.colnames = self.coltypes = None
        self.rowcount = -1
        # first try to execute all queries
        rowcount = 0
        sql = "BEGIN"
        try:
            if not self._dbcnx._tnx:
                try:
                    self._cnx.source().execute(sql)
                except DatabaseError:
                    raise
                except Exception:
                    raise _op_error("can't start transaction")
                self._dbcnx._tnx = True
            for parameters in seq_of_parameters:
                if parameters:
                    sql = self._quoteparams(operation, parameters)
                else:
                    sql = operation
                rows = self._src.execute(sql)
                if rows:  # true if not DML
                    rowcount += rows
                else:
                    self.rowcount = -1
        except DatabaseError:
            raise
        except Error as err:
            raise _db_error("error in '%s': '%s' " % (sql, err))
        except Exception as err:
            raise _op_error("internal error in '%s': %s" % (sql, err))
        # then initialize result raw count and description
        if self._src.resulttype == RESULT_DQL:
            self.rowcount = self._src.ntuples
            getdescr = self._type_cache.getdescr
            description = [CursorDescription(
                info[1], *getdescr(info[2])) for info in self._src.listinfo()]
            self.colnames = [info[0] for info in description]
            self.coltypes = [info[1] for info in description]
            self.description = description
            self.lastrowid = None
            if self.build_row_factory:
                self.row_factory = self.build_row_factory()
        else:
            self.rowcount = rowcount
            self.lastrowid = self._src.oidstatus()
        # return the cursor object, so you can write statements such as
        # "cursor.execute(...).fetchall()" or "for row in cursor.execute(...)"
        return self

    def fetchone(self):
        """Fetch the next row of a query result set."""
        res = self.fetchmany(1, False)
        try:
            return res[0]
        except IndexError:
            return None

    def fetchall(self):
        """Fetch all (remaining) rows of a query result."""
        return self.fetchmany(-1, False)

    def fetchmany(self, size=None, keep=False):
        """Fetch the next set of rows of a query result.

        The number of rows to fetch per call is specified by the
        size parameter. If it is not given, the cursor's arraysize
        determines the number of rows to be fetched. If you set
        the keep parameter to true, this is kept as new arraysize.

        """
        if size is None:
            size = self.arraysize
        if keep:
            self.arraysize = size
        try:
            result = self._src.fetch(size)
        except DatabaseError:
            raise
        except Error as err:
            raise _db_error(str(err))
        typecast = self._type_cache.typecast
        return [self.row_factory([typecast(typ, value)
            for typ, value in zip(self.coltypes, row)]) for row in result]

    def callproc(self, procname, parameters=None):
        """Call a stored database procedure with the given name.

        The sequence of parameters must contain one entry for each input
        argument that the procedure expects. The result of the call is the
        same as this input sequence; replacement of output and input/output
        parameters in the return value is currently not supported.

        The procedure may also provide a result set as output. These can be
        requested through the standard fetch methods of the cursor.

        """
        n = parameters and len(parameters) or 0
        query = 'select * from "%s"(%s)' % (procname, ','.join(n * ['%s']))
        self.execute(query, parameters)
        return parameters

    def __next__(self):
        """Return the next row (support for the iteration protocol)."""
        res = self.fetchone()
        if res is None:
            raise StopIteration
        return res

    # Note that since Python 2.6 the iterator protocol uses __next()__
    # instead of next(), we keep it only for backward compatibility of pgdb.
    next = __next__

    @staticmethod
    def nextset():
        """Not supported."""
        raise NotSupportedError("nextset() is not supported")

    @staticmethod
    def setinputsizes(sizes):
        """Not supported."""
        pass  # unsupported, but silently passed

    @staticmethod
    def setoutputsize(size, column=0):
        """Not supported."""
        pass  # unsupported, but silently passed

    @staticmethod
    def row_factory(row):
        """Process rows before they are returned.

        You can overwrite this statically with a custom row factory, or
        you can build a row factory dynamically with build_row_factory().

        For example, you can create a Cursor class that returns rows as
        Python dictionaries like this:

            class DictCursor(pgdb.Cursor):

                def row_factory(self, row):
                    return {desc[0]: value
                        for desc, value in zip(self.description, row)}

            cur = DictCursor(con)  # get one DictCursor instance or
            con.cursor_type = DictCursor  # always use DictCursor instances

        """
        raise NotImplementedError

    def build_row_factory(self):
        """Build a row factory based on the current description.

        This implementation builds a row factory for creating named tuples.
        You can overwrite this method if you want to dynamically create
        different row factories whenever the column description changes.

        """
        colnames = self.colnames
        if colnames:
            try:
                try:
                    return namedtuple('Row', colnames, rename=True)._make
                except TypeError:  # Python 2.6 and 3.0 do not support rename
                    colnames = [v if v.isalnum() else 'column_%d' % n
                             for n, v in enumerate(colnames)]
                    return namedtuple('Row', colnames)._make
            except ValueError:  # there is still a problem with the field names
                colnames = ['column_%d' % n for n in range(len(colnames))]
                return namedtuple('Row', colnames)._make


CursorDescription = namedtuple('CursorDescription',
    ['name', 'type_code', 'display_size', 'internal_size',
     'precision', 'scale', 'null_ok'])


### Connection Objects

class Connection(object):
    """Connection object."""

    # expose the exceptions as attributes on the connection object
    Error = Error
    Warning = Warning
    InterfaceError = InterfaceError
    DatabaseError = DatabaseError
    InternalError = InternalError
    OperationalError = OperationalError
    ProgrammingError = ProgrammingError
    IntegrityError = IntegrityError
    DataError = DataError
    NotSupportedError = NotSupportedError

    def __init__(self, cnx):
        """Create a database connection object."""
        self._cnx = cnx  # connection
        self._tnx = False  # transaction state
        self._type_cache = TypeCache(cnx)
        self.cursor_type = Cursor
        try:
            self._cnx.source()
        except Exception:
            raise _op_error("invalid connection")

    def __enter__(self):
        """Enter the runtime context for the connection object.

        The runtime context can be used for running transactions.

        """
        return self

    def __exit__(self, et, ev, tb):
        """Exit the runtime context for the connection object.

        This does not close the connection, but it ends a transaction.

        """
        if et is None and ev is None and tb is None:
            self.commit()
        else:
            self.rollback()

    def close(self):
        """Close the connection object."""
        if self._cnx:
            if self._tnx:
                try:
                    self.rollback()
                except DatabaseError:
                    pass
            self._cnx.close()
            self._cnx = None
        else:
            raise _op_error("connection has been closed")

    def commit(self):
        """Commit any pending transaction to the database."""
        if self._cnx:
            if self._tnx:
                self._tnx = False
                try:
                    self._cnx.source().execute("COMMIT")
                except DatabaseError:
                    raise
                except Exception:
                    raise _op_error("can't commit")
        else:
            raise _op_error("connection has been closed")

    def rollback(self):
        """Roll back to the start of any pending transaction."""
        if self._cnx:
            if self._tnx:
                self._tnx = False
                try:
                    self._cnx.source().execute("ROLLBACK")
                except DatabaseError:
                    raise
                except Exception:
                    raise _op_error("can't rollback")
        else:
            raise _op_error("connection has been closed")

    def cursor(self):
        """Return a new cursor object using the connection."""
        if self._cnx:
            try:
                return self.cursor_type(self)
            except Exception:
                raise _op_error("invalid connection")
        else:
            raise _op_error("connection has been closed")

    if shortcutmethods:  # otherwise do not implement and document this

        def execute(self, operation, params=None):
            """Shortcut method to run an operation on an implicit cursor."""
            cursor = self.cursor()
            cursor.execute(operation, params)
            return cursor

        def executemany(self, operation, param_seq):
            """Shortcut method to run an operation against a sequence."""
            cursor = self.cursor()
            cursor.executemany(operation, param_seq)
            return cursor


### Module Interface

_connect_ = connect

def connect(dsn=None,
        user=None, password=None,
        host=None, database=None):
    """Connects to a database."""
    # first get params from DSN
    dbport = -1
    dbhost = ""
    dbbase = ""
    dbuser = ""
    dbpasswd = ""
    dbopt = ""
    try:
        params = dsn.split(":")
        dbhost = params[0]
        dbbase = params[1]
        dbuser = params[2]
        dbpasswd = params[3]
        dbopt = params[4]
    except (AttributeError, IndexError, TypeError):
        pass

    # override if necessary
    if user is not None:
        dbuser = user
    if password is not None:
        dbpasswd = password
    if database is not None:
        dbbase = database
    if host is not None:
        try:
            params = host.split(":")
            dbhost = params[0]
            dbport = int(params[1])
        except (AttributeError, IndexError, TypeError, ValueError):
            pass

    # empty host is localhost
    if dbhost == "":
        dbhost = None
    if dbuser == "":
        dbuser = None

    # open the connection
    cnx = _connect_(dbbase, dbhost, dbport, dbopt, dbuser, dbpasswd)
    return Connection(cnx)


### Types Handling

class Type(frozenset):
    """Type class for a couple of PostgreSQL data types.

    PostgreSQL is object-oriented: types are dynamic.
    We must thus use type names as internal type codes.

    """

    def __new__(cls, values):
        if isinstance(values, basestring):
            values = values.split()
        return super(Type, cls).__new__(cls, values)

    def __eq__(self, other):
        if isinstance(other, basestring):
            return other in self
        else:
            return super(Type, self).__eq__(other)

    def __ne__(self, other):
        if isinstance(other, basestring):
            return other not in self
        else:
            return super(Type, self).__ne__(other)


# Mandatory type objects defined by DB-API 2 specs:

STRING = Type('char bpchar name text varchar')
BINARY = Type('bytea')
NUMBER = Type('int2 int4 serial int8 float4 float8 numeric money')
DATETIME = Type('date time timetz timestamp timestamptz datetime abstime'
    ' interval tinterval timespan reltime')
ROWID = Type('oid oid8')


# Additional type objects (more specific):

BOOL = Type('bool')
SMALLINT = Type('int2')
INTEGER = Type('int2 int4 int8 serial')
LONG = Type('int8')
FLOAT = Type('float4 float8')
NUMERIC = Type('numeric')
MONEY = Type('money')
DATE = Type('date')
TIME = Type('time timetz')
TIMESTAMP = Type('timestamp timestamptz datetime abstime')
INTERVAL = Type('interval tinterval timespan reltime')


# Mandatory type helpers defined by DB-API 2 specs:

def Date(year, month, day):
    """Construct an object holding a date value."""
    return date(year, month, day)


def Time(hour, minute=0, second=0, microsecond=0):
    """Construct an object holding a time value."""
    return time(hour, minute, second, microsecond)


def Timestamp(year, month, day, hour=0, minute=0, second=0, microsecond=0):
    """construct an object holding a time stamp value."""
    return datetime(year, month, day, hour, minute, second, microsecond)


def DateFromTicks(ticks):
    """Construct an object holding a date value from the given ticks value."""
    return Date(*localtime(ticks)[:3])


def TimeFromTicks(ticks):
    """construct an object holding a time value from the given ticks value."""
    return Time(*localtime(ticks)[3:6])


def TimestampFromTicks(ticks):
    """construct an object holding a time stamp from the given ticks value."""
    return Timestamp(*localtime(ticks)[:6])


class Binary(bytes):
    """construct an object capable of holding a binary (long) string value."""


# If run as script, print some information:

if __name__ == '__main__':
    print('PyGreSQL version', version)
    print('')
    print(__doc__)
