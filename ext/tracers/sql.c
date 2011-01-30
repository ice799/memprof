#include <tracers/sql.h>

enum memprof_sql_type
memprof_sql_query_type(const char *stmt, unsigned long length)
{
  int i;

  for (i=0; i<length && i<10; i++) {
    switch (stmt[i]) {
      case ' ':
      case '\n':
      case '\r':
        continue;
        break;

      case 'S':
      case 's':
        return sql_SELECT;

      case 'I':
      case 'i':
        return sql_INSERT;

      case 'U':
      case 'u':
        return sql_UPDATE;

      case 'D':
      case 'd':
        return sql_DELETE;

      default:
        return sql_UNKNOWN;
    }
  }

  return sql_UNKNOWN;
}

const char *
memprof_sql_type_str(enum memprof_sql_type type)
{
  switch (type) {
    case sql_SELECT:
      return "select";
    case sql_UPDATE:
      return "update";
    case sql_INSERT:
      return "insert";
    case sql_DELETE:
      return "delete";
    default:
    case sql_UNKNOWN:
      return "unknown";
  }
}
