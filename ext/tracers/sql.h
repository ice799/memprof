#if !defined(_sql_h_)
#define _sql_h_

enum memprof_sql_type {
  sql_SELECT,
  sql_UPDATE,
  sql_INSERT,
  sql_DELETE,
  sql_UNKNOWN // last
};

enum memprof_sql_type
memprof_sql_query_type(const char *stmt, unsigned long length);

const char *
memprof_sql_type_str(enum memprof_sql_type);

#endif
