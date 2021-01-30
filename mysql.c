#include "mysql.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "debug.h"

extern char* logfile;
extern char* mysqlhost;
extern char* user;
extern char* password;
extern char* dbname;
extern int dbport;

DB conn;

inline int
db_num_rows(DB_RES* res)
{
    if (res)
        return mysql_num_rows(res);
    else
        return 0;
}
inline DB_ROW
db_fetch_row(DB_RES* res)
{
    if (res)
        return mysql_fetch_row(res);
    else
        return NULL;
}
inline void
db_free_result(DB_RES* res)
{
    mysql_free_result(res);
}
inline void
db_close(DB* conn)
{
    mysql_close(conn);
}

int db_query(const char* fmt, ...)
{
    int t;
    struct timeval t_start, t_end;
    int t_cost;
    char query[2048000] = { 0 };
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(query, 2048000, fmt, ap);
    va_end(ap);

    gettimeofday(&t_start, NULL);
    t = mysql_real_query(&conn, query, (unsigned int)strlen(query));
    if (t) {
        DLOG("mysql error [%s] %s", query, mysql_error(&conn));
        // mysql_close(&conn);
        return -1;
    } else {
        gettimeofday(&t_end, NULL);
        t_cost = t_end.tv_usec - t_start.tv_usec;
        if (t_cost < 0)
            t_cost += 1000000;
        if (t_cost > 10000)
            DLOG("large query:%d", t_cost);
        DLOG("cost: %ldus, ok:[%s]", t_cost, query);
        // mysql_close(&conn);
        return 0;
    }
}

DB_RES*
db_query_store_result(const char* fmt, ...)
{
    int t;
    DB_RES* res;
    struct timeval t_start, t_end;
    int t_cost;
    char query[2048000];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(query, 2048000, fmt, ap);
    va_end(ap);
    gettimeofday(&t_start, NULL);
    t = mysql_query(&conn, query);
    if (t) {
        DLOG("mysql error [%s] %s", query, mysql_error(&conn));
        // mysql_close(&conn);
        return NULL;
    } else {
        res = mysql_store_result(&conn);
        gettimeofday(&t_end, NULL);
        t_cost = t_end.tv_usec - t_start.tv_usec;
        if (t_cost < 0)
            t_cost += 1000000;
        if (t_cost > 10000)
            DLOG("large query:%d", t_cost);
        DLOG("cost: %ldus, ok:[%s]", t_cost, query);
        // mysql_close(&conn);
        return res;
    }
}

int db_init()
{
    char sqlbuf[4096] = { 0 };
    const char reconnect = 1;

    // DLOG("11111111");
    while (1) {
        mysql_init(&conn);

        // set re-conn to true. could use ping to reconn
        if (!mysql_options(&conn, MYSQL_OPT_RECONNECT, &reconnect)) {
            DLOG("Error %u: %s", mysql_errno(&conn), mysql_error(&conn));
            exit(1);
        }

        if (!mysql_real_connect(
                &conn, mysqlhost, user, password, dbname, dbport, NULL, 0)) {
            DLOG("Error connecting to database:%s", mysql_error(&conn));
            mysql_close(&conn);
            sleep(5);
            continue;
            // return -1;
        }
        break;
    }
    memset(sqlbuf, 0, sizeof(sqlbuf));
    strcpy(sqlbuf, "SET NAMES 'utf8'");
    int t = mysql_real_query(&conn, sqlbuf, (unsigned int)strlen(sqlbuf));
    if (t) {
        DLOG("set code error:%s[%s]", sqlbuf, mysql_error(&conn));
    }
    return 0;
}
