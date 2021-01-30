#ifndef _M_MYSQL_H
#define _M_MYSQL_H
#include <mysql/mysql.h>
typedef MYSQL DB;
typedef MYSQL_ROW DB_ROW;
typedef MYSQL_RES DB_RES;

// int db_init();
int db_query(const char* fmt, ...);

DB_RES*
db_query_store_result(const char* fmt, ...);

inline int
db_num_rows(DB_RES* res);
inline DB_ROW
db_fetch_row(DB_RES* res);
inline void
db_free_result(DB_RES* res);
inline void
db_close(DB* conn);
int db_init();

// DB conn;
#endif
