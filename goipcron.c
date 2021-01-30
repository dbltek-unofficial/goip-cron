#ifdef _WIN32
#include <errno.h>
#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <winsock.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <mysql/mysql.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#endif
#include <stdarg.h>

#include "auto_ussd.h"
#include "background_cmd.h"
#include "goipcron.h"
#include "mysql.h"
#include "report.h"
//#define __Q_CODE
//#define _AUTO_SMS_RE
//#define _FU
//#define _GOIPCRONEXIT

#ifdef _AUTO_SMS_RE
#include "re.h"
#endif
//#define __DISABLE_CELL
//#define MAXTEL 14
//#define MAXRECVID 1     //接收短信的缓存数量

char* logfile = "goipsms.log";
// int WriteToLog(char* str);
#ifdef _WIN32
char* cfgfile = "config.inc.php";
#else
char* cfgfile = "inc/config.inc.php";
#endif
char* mysqlhost = "localhost";
char* user = "goip";
char* password = "goip";
char* dbname = "goip";
int dbport = 3306;
int phpport = 44444;
char* charset = "utf8";
int sendid;
int userid;
char sendsmsg[3500];
int msglen;
int sms_count;
int phpsock; //接收php的sock
MYSQL mysql; // mysql连接
int disable_status = 0;
// int *goipkatime;
// int goipcount;

// char *inter[10], *local[10];
struct telstr **telhead, *teltmp;
unsigned telstrcount = 0;

struct goip;
struct errorgoip {
    struct errorgoip* next;
    struct goip* goip;
};
struct telstr {
    struct telstr* next;
    struct errorgoip* errorgoip;
    char* telnum;
    int telid;
    int recvlev;
    int recvid;
};

struct goip_state_entry {
    char* event;
    int (*action)(struct goip*);
};

struct goip {
    struct goip* next;
    int id;
    char name[64];
    //	int provider;
    int messageid;
    // int sendid;
    char password[64];
    struct sockaddr_in addr;
    int sock;
    char* telnum;
    struct telstr* telstr;
    int telid;
    int sms_count;
    char* send; //发送状态
    int timer; //剩余超时次数
    time_t lasttime; //上次发送时间
    int proid;
    char* recvdata;
    // int recvid[MAXRECVID];//接收短信的缓存数量
    // int recvidnow;//当前接收短信在缓存中的位置
    int m_stateEntryCount;
    struct goip_state_entry* m_stateEntry;
};

struct goiprecv {
    int recvid[MAXRECVID]; //接收短信的缓存数量
    int recvidnow; //当前接收短信在缓存中的位置
    char name[64];
    struct goiprecv* next; // name排序链表
};

// struct goip *goiphead;
int goipnum;
/*
struct goipkeepalive{
        char *id;
        int sqlid;
        int prov;
        char *password;
        time_t lasttime;
        time_t lastrecvtime;
        int recvid[MAXRECVID];//接收短信的缓存
        int recvidnow;//当前接收短信在缓存中的位置
        time_t gsm_login_time;
        int report_gsm_logout;
        int report_reg_logout;
        struct goipkeepalive *next;
};
*/
struct goipkeepalive* kahead = NULL;

struct phplist {
    struct sockaddr_in phpaddr;
    struct sockaddr_in cliaddr;
    int messageid;
    time_t timeout;
    struct phplist* next;
};
struct phplist* phphead;

struct auto_send {
    int auto_reply;
    char reply_num_except[512];
    char reply_msg[512];
    int auto_send;
    char auto_send_num[32];
    char auto_send_msg[512];
    int auto_send_timeout;
    int time_limit;
    char all_send_num[32];
    char all_send_msg[512];
};
struct auto_send* auto_send = NULL;

int addrlen = sizeof(struct sockaddr_in);
int dosend(struct goip* goip);
int dopass(struct goip* goip);
int dotimeout(struct goip* goip);
int dosendtimeout(struct goip* goip);
int dormsg(struct goip* goip);
int dooksend(struct goip* goip);
int dosenderror(struct goip* goip);
int dosendwait(struct goip* goip);

int goipka_init();
void goipka_timeout();

void change_auto(struct auto_send* auto_send);
int auto_send_init();

int WriteToLog(char* str)
{
    FILE* log;
    time_t mytime = time(0);
    log = fopen(logfile, "a+");
    if (log == NULL) {
        // OutputDebugString("Log file open failed.");
        fprintf(stderr, "%s %s", str, ctime(&mytime));
        return -1;
    }
    fprintf(log, "%s %s", str, ctime(&mytime));
    fclose(log);
    return 0;
}

void free_telstr(struct telstr* telstr)
{
    struct errorgoip* errorgoip = telstr->errorgoip;
    struct errorgoip* errortmp;
    while (errorgoip) {
        errortmp = errorgoip->next;
        free(errorgoip);
        errorgoip = errortmp;
    }
    free(telstr->telnum);
    free(telstr);
}

inline int
check_disable_status(int goip_id)
{
    return 0;
    if (disable_status) {
        background_cmd_calloc(goip_id, "status_flag", "0", 0);
        return 1;
    } else
        return 0;
}

void set_disable_status()
{
    DB_ROW row;
    DB_RES* res;
    // struct goipkeepalive *ka=kahead;
    res = db_query_store_result("select disable_status from system limit 1");
    if ((row = db_fetch_row(res)) != NULL) {
        disable_status = atoi(row[0]);
    }
}

void do_state(char* buf, struct sockaddr_in* cliaddr, int addrlen)
{
    struct goipkeepalive* ka = kahead;
    int recvid;
    char id[64] = { 0 }, gpassword[64] = { 0 }, state[64] = { 0 }, nbuf[512];
    sscanf(buf,
        "STATE:%d;id:%64[^;];password:%64[^;];gsm_remain_state:%64s",
        &recvid,
        id,
        gpassword,
        state);
    while (ka) {
        if (!strcmp(ka->id, id) && !strcmp(ka->password, gpassword)) {
            break;
        }
        ka = ka->next;
    }
    if (!ka) {
        printf("state error\n");
        // sprintf(sendbuf , "RECEIVE %d ERROR not find this id:%s, or password
        // error", recvid, id); if(sendto(phpsock,sendbuf,strlen(sendbuf),0,(struct
        // sockaddr *)cliaddr,addrlen)<0) WriteToLog("sendto err");
        return;
    }
    if (check_disable_status(ka->sqlid)) {
        sprintf(nbuf, "STATE %d DISABLE", recvid);
        if (sendto(
                phpsock, nbuf, strlen(nbuf), 0, (struct sockaddr*)cliaddr, addrlen)
            < 0)
            WriteToLog("sendto err");
        return;
    }
    db_query("update goip set voip_state='%s' where name='%s'", state, id);
}

void do_deliver(char* buf, struct sockaddr_in* cliaddr, int addrlen)
{
    struct goipkeepalive* ka = kahead;
    int recvid, sms_no, state = 1;

    char id[64] = { 0 }, gpassword[64] = { 0 }, nbuf[512];
    sscanf(buf,
        "DELIVER:%d;id:%64[^;];password:%64[^;];sms_no:%d;state:%d",
        &recvid,
        id,
        gpassword,
        &sms_no,
        &state);
    while (ka) {
        if (!strcmp(ka->id, id) && !strcmp(ka->password, gpassword)) {
            break;
        }
        ka = ka->next;
    }
    if (!ka) {
        printf("do_deliver error\n");
    } else if (state == 0) {
        db_query("update sends set received='1' where goipid='%d' and sms_no=%d "
                 "and now()<TIMESTAMP(time, '01:00:00') order by id desc limit 1",
            ka->sqlid,
            sms_no);
    }
    sprintf(nbuf, "DELIVER %d OK", recvid);
    printf("deliver ok\n");
    if (sendto(
            phpsock, nbuf, strlen(nbuf), 0, (struct sockaddr*)cliaddr, addrlen)
        < 0)
        WriteToLog("sendto error");
}

void do_recv(char* buf, struct sockaddr_in* cliaddr, int addrlen)
{
    struct goipkeepalive* ka = kahead;
    char sendbuf[1024] = { 0 };
    int i, j = 0;
    int recvid;
    char id[64] = { 0 };
    char recvnum[64] = { 0 };
    char gpassword[64] = { 0 };
    char recvmsg[3000] = { 0 };
    char recvmsg0[3000] = { 0 };
    char sqlbuf[1024] = { 0 };
    char recvname[64] = { 0 };
    int recvnameid = 0;
    char recvnamelevel = 0;
    char* inter[10];
    int interlen[10];
    int tmppid = 0; // prov id
    int provid = 0;
    // char *p=recvmsg0;
    char* pp;
    MYSQL_ROW tmprow;
    MYSQL_RES* provres;
    char* p = buf;
    char smscnum[64] = { 0 };

    if ((pp = strchr(p, ';')) == NULL)
        return;
    sscanf(p, "RECEIVE:%d;", &recvid);
    p = ++pp;
    if ((pp = strchr(p, ';')) == NULL)
        return;
    sscanf(p, "id:%64[^;];", id);
    p = ++pp;
    if ((pp = strchr(p, ';')) == NULL)
        return;
    sscanf(p, "password:%64[^;];", gpassword);
    p = ++pp;
    if ((pp = strchr(p, ';')) == NULL)
        return;
    sscanf(p, "srcnum:%64[^;];", recvnum);
    p = ++pp;
    if (!strncmp(p, "smscnum", 7)) {
        if ((pp = strchr(p, ';')) == NULL)
            return;
        sscanf(p, "smscnum:%64[^;];", smscnum);
        p = ++pp;
    }
    sscanf(p, "msg:%3000c", recvmsg0);

    // sscanf(buf,"RECEIVE:%d;id:%64[^;];password:%64[^;];srcnum:%64[^;];msg:%3000c",
    // &recvid, id, gpassword, recvnum, recvmsg0);
    p = recvmsg0;
    while (*p) {
        if (*p == '\'') {
            recvmsg[j++] = '\\';
        }
        recvmsg[j++] = *p++;
    }
    while (ka) {
        if (!strcmp(ka->id, id) && !strcmp(ka->password, gpassword)) {
            break;
        }
        ka = ka->next;
    }
    if (!ka) {
        printf("recvive error\n");
        sprintf(sendbuf,
            "RECEIVE %d ERROR not find this id:%s, or password error",
            recvid,
            id);
        if (sendto(phpsock,
                sendbuf,
                strlen(sendbuf),
                0,
                (struct sockaddr*)cliaddr,
                addrlen)
            < 0)
            WriteToLog("sendto err");
        return;
    }
    printf("recv:num:%s;msg:%s\n", recvnum, recvmsg);

    for (i = 0; i < MAXRECVID; i++) {
        if (ka->recvid[i] == recvid) { //已经发过
            fprintf(stderr, "a old recv msg!!!!!!\n\n\n");
            break;
        }
    }

    if (i == MAXRECVID) { //新的
        fprintf(stderr, "a new recv msg\n");
        if (ka->recvidnow >= MAXRECVID - 1) //满了
            ka->recvidnow = 0;
        else
            ka->recvidnow++;
        ka->recvid[ka->recvidnow] = recvid;
        /*找到goip*/

        /*找到接收人*/

        if (*recvnum == '+') { //国际代码
            memset(sqlbuf, 0, 1024);
            provres = db_query_store_result("select id, inter from prov order by id");
            int j = 0;
            while ((tmprow = db_fetch_row(provres)) != NULL && j < 10) {
                fprintf(stderr, "inter:%s\n", tmprow[1]);
                if (tmprow[1] != NULL) {
                    inter[j] = strdup(tmprow[1]);
                    interlen[j] = strlen(tmprow[1]);
                } else {
                    interlen[j] = 0;
                    inter[j] = NULL;
                }
                j++;
            }
            db_free_result(provres);

            memset(sqlbuf, 0, 1024);
            char* p = recvnum + 5;
            provres = db_query_store_result("(SELECT provider,tel,id,name,0 as level "
                                            "FROM receiver where tel like '%%%s') "
                                            "union (SELECT provider1,tel1,id,name1,1 "
                                            "FROM receiver where tel1 like '%%%s' ) "
                                            "union (SELECT provider2,tel2,id,name2,2 "
                                            "FROM receiver where tel2 like '%%%s' ) ",
                p,
                p,
                p);
            j = 0;
            while ((tmprow = db_fetch_row(provres)) != NULL && j < 10) {
                tmppid = atoi(tmprow[0]);
                printf("get dianhua ben :%d", tmppid);
                if (!strncmp(recvnum, inter[tmppid - 1], interlen[tmppid - 1]) && !strcmp(recvnum + interlen[tmppid - 1],
                        tmprow[1])) { //匹配号码成功
                    strncpy(recvname, tmprow[3], 64);
                    recvnameid = atoi(tmprow[2]);
                    recvnamelevel = atoi(tmprow[4]);
                    provid = tmppid;
                    break;
                }

                j++;
            }
            db_free_result(provres);
        } else {
            char* p = recvnum;
            provres = db_query_store_result("(SELECT provider,tel,id,name,0 as level "
                                            "FROM receiver where tel='%s') "
                                            "union (SELECT provider1,tel1,id,name1,1 "
                                            "FROM receiver where tel1='%s' ) "
                                            "union (SELECT provider2,tel2,id,name2,2 "
                                            "FROM receiver where tel2='%s' ) limit 1",
                p,
                p,
                p);
            if ((tmprow = db_fetch_row(provres)) != NULL) {
                provid = atoi(tmprow[0]);
                printf("get dianhua ben :%d", tmppid);
                strncpy(recvname, tmprow[3], 64);
                recvnameid = atoi(tmprow[2]);
                recvnamelevel = atoi(tmprow[4]);
            }
            db_free_result(provres);
        }
        memset(sqlbuf, 0, 1024);
#ifdef _FU

        char key[33] = { 0 }, out_username[33] = { 0 };
        int t;
        MYSQL conn;
        int s_s_id = 0;
        provres = db_query_store_result(
            "select `key`,id,user_name from out_num where num='%s' and `key`!='' "
            "and used_time=0 and over=0",
            ka->num);
        while ((tmprow = db_fetch_row(provres)) != NULL) {
            if (strstr(recvmsg, tmprow[0])) {
                db_query("update out_num set used_time=now() where id='%s'", tmprow[1]);
                db_query("update goip set out_user=0 where id='%d'", ka->sqlid);
                background_cmd_calloc(ka->sqlid, "disable_sim", NULL, 0);
                strncpy(key, tmprow[0], 32);
                strncpy(out_username, tmprow[2], 32);
            }
        }
        db_free_result(provres);

        mysql_init(&conn);

        if (!mysql_real_connect(&conn,
                mysqlhost,
                "scheduler",
                "scheduler",
                "scheduler",
                dbport,
                NULL,
                0)) {
            printf("Error connecting to database:%s\n", mysql_error(&conn));
            mysql_close(&conn);
            return;
        }
        memset(sqlbuf, 0, 1024);
        sprintf(sqlbuf,
            "select sim_name,line_name from sim where line_name='%d'",
            ka->s_l_id);
        t = mysql_real_query(&conn, sqlbuf, (unsigned int)strlen(sqlbuf));
        if (t) {
            printf("set code error:%s[%s]\n", sqlbuf, mysql_error(&conn));
        } else
            printf("ok:%s\n", sqlbuf);
        provres = mysql_store_result(&conn);
        if ((tmprow = mysql_fetch_row(provres)) != NULL) {
            s_s_id = atoi(tmprow[0]);
        }
        mysql_free_result(provres);
        mysql_close(&conn);
        db_query(
            "insert into receive (srcnum,provid,msg,goipid,goipname,srcid,srcname,srclevel,`time`,smscnum, goipnum,s_s_id,`key`,out_username) \
				values ('%s','%d','%s','%d','%s','%d','%s','%d', now(),'%s', '%s','%d','%s','%s')",
            recvnum,
            provid,
            recvmsg,
            ka->sqlid,
            ka->id,
            recvnameid,
            recvname,
            recvnamelevel,
            smscnum,
            ka->num,
            s_s_id,
            key,
            out_username);

#else
        db_query(
            sqlbuf,
            "insert into receive (srcnum,provid,msg,goipid,goipname,srcid,srcname,srclevel,`time`,smscnum) \
				values ('%s','%d','%s','%d','%s','%d','%s','%d', now(),'%s')",
            recvnum,
            provid,
            recvmsg,
            ka->sqlid,
            ka->id,
            recvnameid,
            recvname,
            recvnamelevel,
            smscnum);
#endif

        check_return_msg(recvmsg,
            TYPE_SMS,
            ka->sqlid,
            ka->prov,
            ka->id,
            ka->group_id,
            ka->is_bal2);
        ka->is_bal2 = 0;
        check_email_forward_sms(ka->sqlid, ka->id, recvnum, recvmsg);
#ifdef __Q_CODE
        provres = db_query_store_result("SELECT num_prefix FROM system WHERE 1 ");
        if ((tmprow = mysql_fetch_row(provres)) != NULL) {
            char *p = tmprow[0], *pp;
            while (p) {
                pp = strchr(p, '|');
                if (pp)
                    *pp++ = 0;
                if (!strncmp(p, recvnum, strlen(p)))
                    db_query("update code set code='%s', recv_num='%s', code_time=now() "
                             "where goipid='%d'",
                        recvmsg,
                        recvnum,
                        ka->sqlid);
                p = pp;
            }
        }
        if (provres)
            mysql_free_result(provres);
        provres = NULL;
#endif

#ifdef _AUTO_SMS_RE
        check_re_return(ka->sqlid, ka->id, ka->prov, recvnum, recvmsg);
#endif
    }

    /* 更新数据库，发送消息给php*/
    sprintf(sendbuf, "RECEIVE %d OK", recvid);
    printf("recvive ok\n");
    if (sendto(phpsock,
            sendbuf,
            strlen(sendbuf),
            0,
            (struct sockaddr*)cliaddr,
            addrlen)
        < 0)
        WriteToLog("sendto err");
}

void do_ussn(char* buf, struct sockaddr_in* cliaddr, int addrlen)
{
    struct goipkeepalive* ka = kahead;
    char sendbuf[1024] = { 0 };
    int i, j = 0;
    int recvid;
    char id[64] = { 0 };
    char msg0[1500] = { 0 };
    char msg[1500] = { 0 };
    char gpassword[64] = { 0 };
    char sqlbuf[2000] = { 0 };
    char* p = msg0;
    // char *p=buf;
    sscanf(buf,
        "USSN:%d;id:%64[^;];password:%64[^;];msg:%1500c",
        &recvid,
        id,
        gpassword,
        msg0);
    while (*p) {
        if (*p == '\'') {
            msg[j++] = '\\';
        }
        msg[j++] = *p++;
    }
    while (ka) {
        if (!strcmp(ka->id, id) && !strcmp(ka->password, gpassword)) {
            break;
        }
        ka = ka->next;
    }
    if (!ka) {
        printf("recvive error\n");
        sprintf(sendbuf,
            "USSN %d ERROR not find this id:%s, or password error",
            recvid,
            id);
        if (sendto(phpsock,
                sendbuf,
                strlen(sendbuf),
                0,
                (struct sockaddr*)cliaddr,
                addrlen)
            < 0)
            WriteToLog("sendto err");
        return;
    }
    printf("recv ussd:%s\n", msg);
    for (i = 0; i < MAXRECVID; i++) {
        if (ka->recvid[i] == recvid) { //已经发过
            fprintf(stderr, "a old recv msg!!!!!!\n\n\n");
            break;
        }
    }
    if (i == MAXRECVID) { //新的
        fprintf(stderr, "a new recv msg\n");
        if (ka->recvidnow == i - 1) //满了
            ka->recvidnow = 0;
        else
            ka->recvidnow++;
        ka->recvid[ka->recvidnow] = recvid;
        memset(sqlbuf, 0, 1024);
        sprintf(sqlbuf,
            "insert into USSD (TERMID, USSD_RETURN , `INSERTTIME`) \
				values ('%s','%s', now())",
            id,
            msg);
        db_query("SET NAMES 'utf8'");
        db_query(sqlbuf);
    }
    check_return_msg(
        msg, TYPE_USSD, ka->sqlid, ka->prov, ka->id, ka->group_id, 0);
    /* 更新数据库，发送消息给php*/
    sprintf(sendbuf, "USSN %d OK", recvid);
    printf("recvive ok\n");
    if (sendto(phpsock,
            sendbuf,
            strlen(sendbuf),
            0,
            (struct sockaddr*)cliaddr,
            addrlen)
        < 0)
        WriteToLog("sendto error");
}

void do_record(char* buf, struct sockaddr_in* cliaddr, int addrlen)
{
    struct goipkeepalive* ka = kahead;
    char sendbuf[1024] = { 0 };
    int i;
    int recvid;
    char id[64] = { 0 };
    char recvnum[65] = { 0 };
    char gpassword[64] = { 0 };
    int dir, exp;
    char sqlbuf[1024] = { 0 };
    char cmd[64];
    char status[256];
    DB_RES* res;
    DB_ROW row;
    int call_record_id = 0;
    // MYSQL conn;
    if (!strncmp(buf, "RECORD", 6))
        sscanf(buf,
            "%[^:]:%d;id:%64[^;];password:%64[^;];dir:%d;num:%s",
            cmd,
            &recvid,
            id,
            gpassword,
            &dir,
            recvnum);
    else if (!strncmp(buf, "EXPIRY", 6))
        sscanf(buf,
            "%[^:]:%d;id:%64[^;];password:%64[^;];exp:%d",
            cmd,
            &recvid,
            id,
            gpassword,
            &exp);
    else if (!strncmp(buf, "REMAIN", 6))
        sscanf(buf,
            "%[^:]:%d;id:%64[^;];password:%64[^;];gsm_remain_time:%d",
            cmd,
            &recvid,
            id,
            gpassword,
            &exp);
    else if (!strncmp(buf, "CELLINFO", 8)) {
        sscanf(buf,
            "%[^:]:%d;id:%64[^;];password:%64[^;];%*[^:]:%64s",
            cmd,
            &recvid,
            id,
            gpassword,
            status);
    } else if (!strncmp(buf, "CGATT", 5)) {
        sscanf(buf,
            "%[^:]:%d;id:%64[^;];password:%64[^;];%*[^:]:%64s",
            cmd,
            &recvid,
            id,
            gpassword,
            status);
    } else if (!strncmp(buf, "BCCH", 4)) {
        sscanf(buf,
            "%[^:]:%d;id:%64[^;];password:%64[^;];%*[^:]:%64s",
            cmd,
            &recvid,
            id,
            gpassword,
            status);
    }
    /*
   else if(!strncmp(buf, "HANGUP", 5)){
   sscanf(buf,"%[^:]:%d;id:%64[^;];password:%64[^;];%*[^:]:%64s", cmd,&recvid,
   id, ststus);
   }
 */
    else {
        sscanf(buf, "%[^:]:%d;", cmd, &recvid);
        sprintf(sendbuf, "%s %d OK", cmd, recvid);
        printf("recvive ok\n");
        if (sendto(phpsock,
                sendbuf,
                strlen(sendbuf),
                0,
                (struct sockaddr*)cliaddr,
                addrlen)
            < 0)
            WriteToLog("sendto err");
        return;
    }
    while (ka) {
        if (!strcmp(ka->id, id) && !strcmp(ka->password, gpassword)) {
            break;
        }
        ka = ka->next;
    }
    if (!ka) {
        printf("recvive error\n");
        sprintf(sendbuf,
            "%s %d ERROR not find this id:%s, or password error",
            cmd,
            recvid,
            id);
        if (sendto(phpsock,
                sendbuf,
                strlen(sendbuf),
                0,
                (struct sockaddr*)cliaddr,
                addrlen)
            < 0)
            WriteToLog("sendto err");
        return;
    }
    if (check_disable_status(ka->sqlid)) {
        sprintf(sendbuf, "%s %d DISABLE", cmd, recvid);
        if (sendto(phpsock,
                sendbuf,
                strlen(sendbuf),
                0,
                (struct sockaddr*)cliaddr,
                addrlen)
            < 0)
            WriteToLog("sendto err");
        return;
    }
    printf("recv:num:%s;dir:%d;exp:%d;\n", recvnum, dir, exp);
    for (i = 0; i < MAXRECVID; i++) {
        if (ka->recvid[i] == recvid) { //已经发过
            fprintf(stderr, "a old recv msg!!!!!!\n\n\n");
            break;
        }
    }
    if (1 || i == MAXRECVID) { //新的
        fprintf(stderr, "a new recv msg\n");
        if (ka->recvidnow >= MAXRECVID - 1) //满了
            ka->recvidnow = 0;
        else
            ka->recvidnow++;
        ka->recvid[ka->recvidnow] = recvid;
        memset(sqlbuf, 0, 1024);
        fprintf(stderr, "222222\n");
        if (!strncmp(buf, "RECORD", 6)) {
            sprintf(sqlbuf,
                "insert into record (goipid,dir,num,`time`) \
                                values ('%d','%d','%s', now())",
                ka->sqlid,
                dir,
                recvnum);
            db_query(sqlbuf);
            res = db_query_store_result("SELECT LAST_INSERT_ID()");
            if ((row = db_fetch_row(res)) != NULL) {
                call_record_id = atoi(row[0]);
            }
            if (res)
                db_free_result(res);
            res = NULL;
            sprintf(sqlbuf,
                "update goip set last_call_record_id='%d' where id=%d",
                call_record_id,
                ka->sqlid);
        } else if (!strncmp(buf, "EXPIRY", 6)) {
            // sprintf(sqlbuf, "update record set expiry=\ncase expiry\nwhen '-1' then
            // '%d'\nelse expiry\nend\n where goipid='%d' order by `time` desc limit
            // 1",exp, ka->sqlid);
            sprintf(sqlbuf,
                "update record set expiry='%d' where expiry=-1 and goipid=%d and "
                "id=(select last_call_record_id from goip where id=%d limit 1) "
                "limit 1",
                exp,
                ka->sqlid,
                ka->sqlid);
        } else if (!strncmp(buf, "REMAIN", 6)) {
            sprintf(
                sqlbuf, "update goip set remain_time='%d' where id=%d", exp, ka->sqlid);
            ka->remain_time = exp;
            if (exp > 0 && exp == -1)
                ka->report_remain_timeout = 0;
            else
                report_check(1);
        }
#ifndef __DISABLE_CELL
        else if (!strncmp(buf, "CELLINFO", 8)) {
            sprintf(sqlbuf,
                "update goip set CELLINFO='%s' where id='%d'",
                status,
                ka->sqlid);
        } else if (!strncmp(buf, "CGATT", 5)) {
            sprintf(
                sqlbuf, "update goip set CGATT='%s' where id='%d'", status, ka->sqlid);
        } else if (!strncmp(buf, "BCCH", 4)) {
            sprintf(
                sqlbuf, "update goip set BCCH='%s' where id='%d'", status, ka->sqlid);
        }
#endif

        if (sqlbuf[0])
            db_query(sqlbuf);
    }
    /* 更新数据库，发送消息给php*/
    sprintf(sendbuf, "%s %d OK", cmd, recvid);
    printf("recvive ok\n");
    if (sendto(phpsock,
            sendbuf,
            strlen(sendbuf),
            0,
            (struct sockaddr*)cliaddr,
            addrlen)
        < 0)
        WriteToLog("sendto err");
}

void keepalive_sendto(char* buf, struct sockaddr_in* cliaddr, int addrlen)
{
    int reg, signal = 0, remain_time = -2, disable_status_receive = -1,
             s_l_id = 0;
    char id[64] = { 0 }, pass[64] = { 0 }, num[32] = { 0 },
         gsm_status[64] = { 0 }, voip_status[64] = { 0 }, voip_state[64] = { 0 };
    char imei[128] = { 0 }, imsi[128] = { 0 }, iccid[128] = { 0 },
         pro[128] = { 0 };
    char *p = buf, *pp;
    struct goipkeepalive* ka = kahead;
    time_t now_time = time(NULL);
    // MYSQL conn;

    while (1) {
        // sscanf(buf,"req:%d;id:%[^;];pass:%[^;];num:%[^;];", &reg,id,pass, num);
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "req:%d;", &reg);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "id:%[^;];", id);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "pass:%[^;];", pass);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "num:%[^;];", num);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "signal:%d;", &signal);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "gsm_status:%[^;];", gsm_status);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "voip_status:%[^;];", voip_status);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "voip_state:%[^;];", voip_state);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "remain_time:%d;", &remain_time);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "imei:%[^;];", imei);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "imsi:%[^;];", imsi);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "iccid:%[^;];", iccid);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "pro:%[^;];", pro);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        // sscanf(p, "idle:%[^;];", pro);
        p = ++pp;
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "disable_status:%d;", &disable_status_receive);
        p = ++pp;
#ifdef _FU
        if ((pp = strchr(p, ';')) == NULL)
            break;
        sscanf(p, "s_l_id:%d;", &s_l_id);
        p = ++pp;
#endif
        break;
    }
    printf("ka buf:%s\n id:%s pass:%s, signal:%d, gsm:%s, voip:%s, state:%s, "
           "remain_time:%d,imei:%s,imsi:%s,iccid:%s,pro:%s,disable_status:%d\n",
        buf,
        id,
        pass,
        signal,
        gsm_status,
        voip_status,
        voip_state,
        remain_time,
        imei,
        imsi,
        iccid,
        pro,
        disable_status_receive);
    char insertmsgbuf[3500] = { 0 };
    while (ka) {
        if (!strcmp(ka->id, id) && !strcmp(ka->password, pass)) {
            ka->lasttime = now_time;
            break;
        }
        ka = ka->next;
    }
    // sprintf(insertmsgbuf, "select id form goip where where name='%s' and
    // password='%s'",id,pass); if(voip_state[0]){
    if (ka) {
        // if(num[0]){
        DB_ROW row;
        DB_RES* res = db_query_store_result(
            "select imsi,goip.id,auto_num_ussd from goip left join prov on "
            "goip.provider=prov.id where name='%s' limit 1",
            id);
        if ((row = mysql_fetch_row(res)) != NULL) {
            /* now check imsi in goip */

            // if(!row || !row[0] || strcmp(imsi, row[0])){
            if (!row[0] || strcmp(imsi, row[0])) {
                printf("imsi different\n");
                db_query("update goip set auto_num_c=0 where name='%s'", id);
                // memset(num, 0, sizeof(num));
                // background_cmd_calloc(atoi(row[1]), "reset_num", 0, 0);
            }

            db_free_result(res);
        } else {
            memset(insertmsgbuf, 0, 3500);
            sprintf(insertmsgbuf, "reg:%d;status:%d;", reg, 304);
            if (sendto(phpsock,
                    insertmsgbuf,
                    strlen(insertmsgbuf),
                    0,
                    (struct sockaddr*)cliaddr,
                    addrlen)
                < 0)
                WriteToLog("sendto err");
            return;
        }
        //}

#ifdef __Q_CODE
        if (num[0])
            db_query("INSERT INTO code (goipid, num, goipname) values (%d, '%s', "
                     "'%s') ON DUPLICATE KEY UPDATE num='%s'",
                ka->sqlid,
                num,
                ka->id,
                num);
#endif
        if (disable_status_receive != -1 && disable_status_receive != disable_status)
            background_cmd_calloc(
                ka->sqlid, "status_flag", disable_status ? "0" : "1", 0);
        strncpy(ka->num, num, sizeof(ka->num) - 1);
#ifdef _AUTO_SMS_RE
        sprintf(insertmsgbuf,
            "update goip set "
            "host='%s',port=%d,alive=1,num='%s',`signal`='%d',gsm_status='%s',"
            "voip_status='%s',voip_state='%s',keepalive_time=now(),keepalive_"
            "time_t='%d',imei='%s',imsi='%s',carrier='%s'",
            inet_ntoa(cliaddr->sin_addr),
            ntohs(cliaddr->sin_port),
            num,
            signal,
            gsm_status,
            voip_status,
            voip_state,
            (int)now_time,
            imei,
            imsi,
            pro);
        if (iccid && *iccid) {
            sprintf(insertmsgbuf + strlen(insertmsgbuf),
                ",re_remain_count=if(iccid!='%s', re_limit_count "
                ",re_remain_count),iccid='%s'",
                iccid,
                iccid);
        }
#else

        sprintf(insertmsgbuf,
            "update goip set "
            "host='%s',port=%d,alive=1,num='%s',`signal`='%d',gsm_status='%s',"
            "voip_status='%s',voip_state='%s',keepalive_time=now(),keepalive_"
            "time_t='%d',imei='%s',imsi='%s',iccid='%s',carrier='%s'",
            inet_ntoa(cliaddr->sin_addr),
            ntohs(cliaddr->sin_port),
            (!strcmp(gsm_status, "LOGOUT")) ? "" : num,
            signal,
            gsm_status,
            voip_status,
            voip_state,
            (int)now_time,
            imei,
            imsi,
            iccid,
            pro);
#endif

        if (remain_time != -2) {
            sprintf(
                insertmsgbuf + strlen(insertmsgbuf), ",remain_time='%d'", remain_time);
            ka->remain_time = remain_time;
            if (remain_time > 0 && remain_time == -1)
                ka->report_remain_timeout = 0;
        }
        if (!strcmp(gsm_status, "LOGIN")) {
            ka->gsm_login_time = now_time;
            ka->report_gsm_logout = 0;
            sprintf(insertmsgbuf + strlen(insertmsgbuf),
                ",gsm_login_time=now(),gsm_login_time_t='%d'",
                (int)now_time);
        }
#ifdef _FU
        sprintf(insertmsgbuf + strlen(insertmsgbuf), ",s_l_id='%d'", s_l_id);
        ka->s_l_id = s_l_id;
#endif
        sprintf(insertmsgbuf + strlen(insertmsgbuf), " where name='%s'", id);
        ka->report_reg_logout = 0;
        /*
   mysql_init(&conn);
   if(!mysql_real_connect(&conn,mysqlhost, user, password,
   dbname,dbport,NULL,0)){ printf( "Error connecting to
   database:%s\n",mysql_error(&conn)); memset(insertmsgbuf,0,3500);
   sprintf(insertmsgbuf,"reg:%d;status:%d;",reg,304);
   if(sendto(phpsock,insertmsgbuf,strlen(insertmsgbuf),0,(struct sockaddr
   *)cliaddr,addrlen)<0) WriteToLog("sendto err"); mysql_close(&conn);
   return;
   }
   int t=mysql_real_query(&conn,insertmsgbuf,(unsigned
   int)strlen(insertmsgbuf)); if(t)
   {
   printf("执行查询时出现异常: [%s] %s",insertmsgbuf,mysql_error(&conn));
   }else
   printf("[%s] 构建成功 \n",insertmsgbuf);
 */
        db_query(insertmsgbuf);
        memset(insertmsgbuf, 0, 3500);

        sprintf(insertmsgbuf, "reg:%d;status:%d;", reg, 200);
        strncpy(ka->host, inet_ntoa(cliaddr->sin_addr), 63);
        ka->port = ntohs(cliaddr->sin_port);
        memcpy(&ka->addr, cliaddr, addrlen);
        ka->addr_len = addrlen;
        if (sendto(phpsock,
                insertmsgbuf,
                strlen(insertmsgbuf),
                0,
                (struct sockaddr*)cliaddr,
                addrlen)
            < 0)
            WriteToLog("sendto err");
        // mysql_close(&conn);
        if (!num[0] && !strcmp(gsm_status, "LOGIN"))
            auto_get_num_start_check(id);
    } else {
        sprintf(insertmsgbuf, "reg:%d;status:%d;", reg, 404);
        if (sendto(phpsock,
                insertmsgbuf,
                strlen(insertmsgbuf),
                0,
                (struct sockaddr*)cliaddr,
                addrlen)
            < 0)
            WriteToLog("sendto err");
    }
}

void telpush(struct goip* goip)
{
    // teltmp=goip->telstr;
    goip->telstr->next = telhead[goip->proid];
    telhead[goip->proid] = goip->telstr;
    // goip->send="RMSG";
    /*删除数据库*/
    // char insertmsgbuf[3500]={0};
    // sprintf(insertmsgbuf, "delete from sends where id=%s", goip->telid);
    // mysql_real_query(&mysql,insertmsgbuf,(unsigned int)strlen(insertmsgbuf));

    goip->telnum = 0;
    goip->telid = 0;
    // memset(goip->telid, 0,sizeof(goip->telid));
}

int check_sms_remain_count(struct goip* goip)
{
    int flag = 1;
    DB_ROW row;
    DB_RES* res;
    char sql[1024];
    int remain_count, remain_count_d;
    sprintf(
        sql, "select remain_count,remain_count_d from goip where id=%d", goip->id);
    res = db_query_store_result(sql);
    if ((row = db_fetch_row(res)) != NULL) {
        remain_count = atoi(row[0]);
        remain_count_d = atoi(row[1]);
        if (remain_count != -1) {
            remain_count = remain_count - goip->sms_count;

            if (remain_count < 0)
                remain_count = 0;
            if (goip->sms_count > 0) {
                sprintf(sql,
                    "update goip set remain_count=%d where id=%d",
                    remain_count,
                    goip->id);
                db_query(sql);
            }
            if (remain_count == 0) {
                printf("GoIP Line(%s) remain count is down", goip->name);
                flag = 0;
            }
        }
        if (remain_count_d != -1) {
            remain_count_d = remain_count_d - goip->sms_count;
            if (remain_count_d <= 0)
                remain_count_d = 0;
            if (goip->sms_count > 0) {
                sprintf(sql,
                    "update goip set remain_count_d=%d where id=%d",
                    remain_count_d,
                    goip->id);
                db_query(sql);
            }
            if (remain_count_d == 0) {
                printf("GoIP Line(%s) remain count of day is down", goip->name);
                flag = 0;
            }
        }
    }
    db_free_result(res);
    return flag;
}

int telpop(struct goip* goip, int sms_count)
{
    goip->sms_count = sms_count;
    if (!check_sms_remain_count(goip))
        return -1;
    if (telhead[goip->proid] == NULL)
        return -1;
    struct errorgoip* errorgoip;
    struct telstr* teltmp = telhead[goip->proid];
    struct telstr* prev = 0;
    while (teltmp) {
        errorgoip = teltmp->errorgoip;
        while (errorgoip) {
            if (goip == errorgoip->goip) {
                break;
            }
            errorgoip = errorgoip->next;
        }
        if (!errorgoip) //不在出错goip行列
            break;
        prev = teltmp;
        teltmp = teltmp->next;
    }
    if (!teltmp)
        return -1;

    goip->telnum = teltmp->telnum;
    goip->telstr = teltmp;
    if (!prev)
        telhead[goip->proid] = telhead[goip->proid]->next;
    else
        prev->next = teltmp->next;
    /*写入数据库得到telid*/
    /*
   char insertmsgbuf[3500]={0};
   sprintf(insertmsgbuf, "INSERT INTO sends
(messageid,userid,telnum,goipid,recvid,recvlev,provider) VALUES
(%d,%d,'%s',%d,%d,%d,%d)",
sendid,userid,goip->telnum,goip->id,goip->telstr->recvid,goip->telstr->recvlev,goip->proid+1);
   int t=mysql_real_query(&mysql,insertmsgbuf,(unsigned
int)strlen(insertmsgbuf)); if(t)
   {
   printf("执行查询时出现异常: %s",mysql_error(&mysql));
   }else
   printf("[%s] 构建成功 \n",insertmsgbuf);
//mesmet(insertmsgbuf,0,3500);
mysql_real_query(&mysql,"SELECT LAST_INSERT_ID()",(unsigned int)strlen("SELECT
LAST_INSERT_ID()")); MYSQL_RES *inserttmpres=mysql_store_result(&mysql);
MYSQL_ROW inserttmprow=mysql_fetch_row(inserttmpres);
 */
    // strncpy(goip->telid,inserttmprow[0],12);
    // strncpy(goip->telid,teltmp->telid,12);
    // mysql_free_result(inserttmpres);
    goip->telid = teltmp->telid;
    return 0;
}

struct goip_state_entry ismsg[] = {
    { "SEND", dosend },
    { "PASSWORD", dopass },
    { "ERROR", dotimeout },
    { "TIMEOUT", dotimeout },
};

struct goip_state_entry issend[] = {
    //{"SNED", },
    //{"PASSWORD", },
    { "OK", dooksend },
    { "ERROR", dosenderror },
    { "TIMEOUT", dosendtimeout },
    { "WAIT", dosendwait },
};

struct goip_state_entry ispass[] = {
    { "SNED", dosend },
    { "PASSWORD", dotimeout },
    { "ERROR", dotimeout },
    { "TIMEOUT", dotimeout },
};

struct goip_state_entry isrmsg[] = {
    //{"SNED", },
    //{"PASSWORD", },
    //{"ERROR", },
    { "TIMEOUT", dormsg },
};
struct goip_state_entry isok[] = {
    //{"SNED", },
    //{"PASSWORD", },
    //{"ERROR", },
    //{"TIMEOUT", dormsg},
};

void dolastsend(struct goip* goip)
{
    char buf[1200];
    memset(buf, 0, 1200);
    if (!strcmp(goip->send, "SEND")) {
        sprintf(buf,
            "%s %d %d %s\n",
            goip->send,
            goip->messageid,
            goip->telid,
            goip->telnum);
    } else if (!strcmp(goip->send, "MSG")) {
        sprintf(
            buf, "%s %d %d %s\n", goip->send, goip->messageid, msglen, sendsmsg);
    } else if (!strcmp(goip->send, "PASSWORD")) {
        sprintf(buf, "%s %d %s\n", goip->send, goip->messageid, goip->password);
    }
    printf("****\n%s****\n", buf);
    if (sendto(goip->sock,
            buf,
            strlen(buf),
            0,
            (struct sockaddr*)&goip->addr,
            addrlen)
        < 0) {
        printf("sendto error\n");
        return;
    }
}

int dosend(struct goip* goip)
{
    printf("dosend\n");

    if (!telpop(goip, 0)) {
        goip->timer = 3;
        goip->send = "SEND";
        goip->m_stateEntry = issend;
        goip->m_stateEntryCount = sizeof(issend) / sizeof(struct goip_state_entry);
        dolastsend(goip);
    } else {
        goip->send = "OK";
        goip->m_stateEntry = isok;
        goip->m_stateEntryCount = sizeof(isok) / sizeof(struct goip_state_entry);
    }
    return 0;
}
int dooksend(struct goip* goip)
{
    /*old %d %d \n
  new %d %d %d \n
*/
    int sms_no = -1;
    printf("dooksend\n");
    /*写入数据库*/
    int recvtelid;
    char* p = goip->recvdata;
    while (*p++ != ' ')
        ;
    recvtelid = atoi(p);
    while (*p++ != ' ')
        ;
    if (*p != '\n')
        sms_no = atoi(p);
    db_query("update sends set over=1,goipid=%d,sms_no=%d where id=%d and "
             "messageid=%d",
        goip->id,
        sms_no,
        recvtelid,
        sendid);
    /*check recharge sms*/
    sms_recharge_return_check(sendid, 1);
    if (recvtelid != goip->telid)
        // if(strncmp(p, goip->telid,strlen(goip->telid))) //不是所要的telid
        return 0;
    free_telstr(goip->telstr);
    if (!telpop(goip, sms_count)) {
        goip->timer = 4;
        dolastsend(goip);
    } else { //结束
        goip->send = "OK";
        goip->m_stateEntry = isok;
        goip->m_stateEntryCount = sizeof(isok) / sizeof(struct goip_state_entry);
    }
    return 0;
}
int dosendwait(struct goip* goip)
{
    char* p = goip->recvdata;
    while (*p++ != ' ')
        ;
    if (goip->telid != atoi(p))
        // if(strncmp(p, goip->telid,strlen(goip->telid)))
        return 0;
    goip->timer = 3;
    return 0;
}

int dosenderror(struct goip* goip)
{
    int errorno;
    char* p = goip->recvdata;
    while (*p++ != ' ')
        ;
    printf("dosenderror:%d:%s\n", goip->telid, p);
    if (goip->telid != atoi(p))
        // if(strncmp(p, goip->telid,strlen(goip->telid)))
        return 0;
    struct errorgoip* errorgoip = calloc(1, sizeof(struct errorgoip));
    errorgoip->goip = goip;
    errorgoip->next = goip->telstr->errorgoip;
    goip->telstr->errorgoip = errorgoip;
    // printf("insert errorgoip:%s,%p\n", goip->telnum,goip);

    goip->timer = 0;
    goip->send = "ERRORSEND";

    while (*p != ':' && *p != 0)
        p++;
    if (*p == ':') {
        p++;
        errorno = atoi(p);
        db_query(
            "update sends set goipid=%d,error_no=%d where id=%d and messageid=%d",
            goip->id,
            errorno,
            goip->telid,
            sendid);
        sms_recharge_return_check(sendid, 0);
#ifdef _AUTO_SMS_RE
        re_resend(sendid, goip->telid);
#endif
    }
    dosendtimeout(goip);
    return 0;
}

int dosendtimeout(struct goip* goip)
{
    printf("dosendtimeout\n");
    if (goip->timer-- > 0)
        dolastsend(goip);
    else { //找到OK的GOIP
        struct goip* goiptmp = goiphead;
        struct errorgoip* errorgoip;
        int i;
        int flag;
        for (i = 0; i < goipnum; i++) {
            goiptmp = &goiphead[i];
            // printf("search goip\n");
            if (goiptmp->sock != -1 && !strcmp(goiptmp->send, "OK") && goiptmp->proid == goip->proid) {
                // printf("get goip:%d\n", goiptmp->id);
                flag = 0;
                errorgoip = goip->telstr->errorgoip;
                while (errorgoip) {
                    // printf("errorgoip:%p\n", errorgoip->goip);
                    if (goiptmp == errorgoip->goip) {
                        flag = 1; //不可用的goip
                        // printf("************\n不可用的goip\n**********\n");
                        break;
                    }
                    errorgoip = errorgoip->next;
                }
                if (flag)
                    continue;
                goiptmp->telstr = goip->telstr;
                goiptmp->telnum = goip->telnum;
                goiptmp->telid = goip->telid;
                // memset(goiptmp->telid,0,12);
                // strncpy(goiptmp->telid,goip->telid,12);
                goiptmp->timer = 3;
                goiptmp->send = "SEND";
                goiptmp->m_stateEntry = issend;
                goiptmp->m_stateEntryCount = sizeof(issend) / sizeof(struct goip_state_entry);
                dotimeout(goiptmp);
                break;
                // goip->send="OK";
                // goip->m_stateEntry=isok;
                // goip->m_stateEntryCount=sizeof(isok)/sizeof(struct goip_state_entry);
            }
            // goiptmp=goiptmp->next;
        }
        /*没找到*/
        if (i >= goipnum)
            // printf("not find\n");
            telpush(goip);
        if (!strcmp(goip->send, "ERRORSEND")) {
            if (!telpop(goip, 0)) {
                goip->send = "SEND";
                goip->timer = 4;
                dolastsend(goip);
            } else { //结束
                goip->send = "OK";
                goip->m_stateEntry = isok;
                goip->m_stateEntryCount = sizeof(isok) / sizeof(struct goip_state_entry);
            }
        } else
            dotimeout(goip);
    }
    return 0;
}

int dotimeout(struct goip* goip)
{
    printf("dotimeout:%d\n", goip->timer);
    if (goip->timer-- > 0)
        dolastsend(goip);
    else {
        printf("do rmsg\n");
        goip->timer = 20;
        goip->send = "RMSG";
        goip->m_stateEntry = isrmsg;
        goip->m_stateEntryCount = sizeof(isrmsg) / sizeof(struct goip_state_entry);
    }
    return 0;
}

int dormsg(struct goip* goip)
{
    if (goip->timer-- > 0)
        ;
    else {
        goip->timer = 6;
        goip->send = "MSG";
        goip->m_stateEntry = ismsg;
        goip->m_stateEntryCount = sizeof(ismsg) / sizeof(struct goip_state_entry);
        dotimeout(goip);
    }
    return 0;
}
int dopass(struct goip* goip)
{
    goip->timer = 3;
    goip->send = "PASSWORD";
    dolastsend(goip);
    return 0;
}
void goip_handleEvent(struct goip* apst_goip, char* ai_event, void* data)
{
    int i;
    printf("handle:%s,%s,%d\n", ai_event, apst_goip->send, apst_goip->id);
    for (i = 0; i < apst_goip->m_stateEntryCount; i++) {
        if (!strcmp(apst_goip->m_stateEntry[i].event, ai_event)) {
            // printf("get handle\n");
            apst_goip->m_stateEntry[i].action(apst_goip);
            break;
        }
        // else printf("%s\n", apst_goip->m_stateEntry[i].event);
    }
}
void release_all(struct goip* goip, int goipcount)
{
    int i;
    struct telstr *tmp, *ttmp;
    /*
   for(i=0;i<goipcount;i++){
   if(goip[i].sock!=-1){
   goip[i].sock==-1;
   }
   }
 */
    free(goip);
    goip = NULL;
    for (i = 0; i < telstrcount; i++) {
        tmp = telhead[i];
        while (tmp) {
            ttmp = tmp->next;
            free_telstr(tmp);
            tmp = ttmp;
            telhead[i] = 0;
        }
    }
}

void send_done(struct goip* goip, int goipcount)
{
    char buf[120];
    WriteToLog("DONE");
    int i;
    for (i = 0; i < goipcount; i++) {
        if (goip[i].sock != -1) {
            memset(buf, 0, 120);
            sprintf(buf, "DONE %d\n", goip[i].messageid);
            sendto(goip[i].sock,
                buf,
                strlen(buf),
                0,
                (struct sockaddr*)&goip[i].addr,
                addrlen);
        }
    }
}

struct phplist*
del_phplist(struct phplist* now)
{
    struct phplist* tmp = phphead;
    if (now == phphead) {
        phphead = now->next;
        tmp = phphead;
    } else {
        while (tmp->next) {
            if (now->messageid == tmp->next->messageid) {
                tmp->next = now->next;
                tmp = tmp->next;
                break;
            }
            tmp = tmp->next;
        }
    }
    printf("free phplist messageid:%d\n", now->messageid);
    free(now);
    return tmp;
}
int do_phplist(struct sockaddr_in* addr, int messageid, char* buf, int buflen)
{
    printf("do_phplist\n");
    struct phplist* tmp = phphead;
    while (tmp) {
        if (messageid == tmp->messageid)
            break;
        tmp = tmp->next;
    }
    if (!tmp)
        return -1;
    tmp->timeout = time(NULL);
    printf("get phplist\n");
    if (tmp->phpaddr.sin_addr.s_addr == addr->sin_addr.s_addr) {
        printf("send,%u\n", addr->sin_addr.s_addr);
        sendto(phpsock, buf, buflen, 0, (struct sockaddr*)&(tmp->cliaddr), addrlen);
    } else {
        printf(
            "send1.=,%u,%u\n", addr->sin_addr.s_addr, tmp->phpaddr.sin_addr.s_addr);
        sendto(phpsock, buf, buflen, 0, (struct sockaddr*)&(tmp->phpaddr), addrlen);
    }
    if (!strncmp(buf, "DONE", 4))
        del_phplist(tmp);
    return 0;
}
int start_phplist(struct sockaddr_in* phpaddr,
    int messageid,
    char* cliip,
    short cliport)
{
    struct phplist* tmp = phphead;
    while (tmp) {
        if (messageid == tmp->messageid)
            return 0;
        tmp = tmp->next;
    }
    tmp = malloc(sizeof(struct phplist));
    tmp->timeout = time(NULL);
    tmp->messageid = messageid;
    // memcpy(&(tmp->phpaddr), phpaddr,sizeof(phpaddr));

    tmp->phpaddr.sin_family = AF_INET;
    tmp->phpaddr.sin_addr.s_addr = phpaddr->sin_addr.s_addr;
    tmp->phpaddr.sin_port = phpaddr->sin_port;
    tmp->cliaddr.sin_family = AF_INET;
    if ((tmp->cliaddr.sin_addr.s_addr = inet_addr(cliip)) == 0) {
        printf("转化地址失败\n");
        return -1;
    }
    printf("start.=,%u,%u\n",
        tmp->cliaddr.sin_addr.s_addr,
        tmp->phpaddr.sin_addr.s_addr);
    tmp->cliaddr.sin_port = htons(cliport);
    tmp->next = phphead;
    phphead = tmp;
    return 0;
}

int do_select(struct goip* goip,
    int goipcount,
    struct telstr* telhead,
    char* msg,
    int msglen)
{
    fd_set reset, allset;
    int maxfde, i, nready, n, nowtime;
    struct timeval timeout = { 5, 0 }; // 5s
    char buf[3500];
    char* cmd;
    int recvid;
    struct sockaddr_in cliaddr;
    unsigned addrlen;
    int flag = 1;

    goiphead = goip;
    goipnum = goipcount;
    FD_ZERO(&allset);

    maxfde = -1;
    FD_SET(phpsock, &allset);
    if (maxfde < phpsock) {
        maxfde = phpsock;
    }

    maxfde++;
    while (1) {
        flag = 1;
        for (i = 0; i < goipcount; i++) {
            // printf("sock:%d %s",goip[i].sock,goip[i].send);
            if (goip[i].sock >= 0 && strcmp(goip[i].send, "OK") && strcmp(goip[i].send, "RMSG")) {
                flag = 0;
                break;
            }
        }
        if (flag) {
            // printf("all down");
            send_done(goip, goipcount);
            release_all(goip, goipcount);
            return 0;
        }
        reset = allset;
        timeout.tv_usec = 0;
        timeout.tv_sec = 5;
        nready = select(maxfde, &reset, NULL, NULL, &timeout);
        printf("nready:%d,sec:%u,usec:%u\n",
            nready,
            (int)timeout.tv_sec,
            (int)timeout.tv_usec);
        nowtime = time(NULL);
        if (nready == -1) {
            send_done(goip, goipcount);
            perror("select error");
            release_all(goip, goipcount);
            return -1;
        } else if (nready == 0) {
            for (i = 0; i < goipcount; i++) {
                if (goip[i].sock != -1) {
                    goip_handleEvent(&goip[i], "TIMEOUT", msg);
                    goip[i].lasttime = nowtime;
                }
            }
        } else { //可读
            if (FD_ISSET(phpsock, &reset)) {
                memset(buf, 0, 3500);
                addrlen = sizeof(cliaddr);
                memset(&cliaddr, 0, addrlen);
                n = recvfrom(phpsock, buf, 3500, 0, (struct sockaddr*)&cliaddr, &addrlen);
                if (n <= 0)
                    continue;
                cmd = buf;
                printf("**** %s ****\n", buf);
                if (!strncmp(buf, "RECEIVE", 7)) { //接收短信
                    /* 检查缓存，是新的就更新数据库,发送php页面更新命令*/
                    do_recv(buf, &cliaddr, addrlen);

                    /* 应答*/
                } else if (!strncmp(buf, "DELIVER", 7)) {
                    do_deliver(buf, &cliaddr, addrlen);
                } else if (!strncmp(buf, "STATE", 5)) { //状态
                    char sbuf[200] = { 0 };
                    char sname[64] = { 0 };
                    char sid[64] = { 0 };
                    sscanf(buf, "%[^:]:%[^;]", sname, sid);
                    snprintf(sbuf, 200, "%s %s OK", sname, sid);
                    sendto(phpsock,
                        sbuf,
                        strlen(sbuf),
                        0,
                        (struct sockaddr*)&cliaddr,
                        addrlen);
                    do_state(buf, &cliaddr, addrlen);
                } else if (!strncmp(buf, "REMAIN", 6)) { //剩余时间
                    char sbuf[200] = { 0 };
                    char sname[64] = { 0 };
                    char sid[64] = { 0 };
                    sscanf(buf, "%[^:]:%[^;]", sname, sid);
                    snprintf(sbuf, 200, "%s %s OK", sname, sid);
                    sendto(phpsock,
                        sbuf,
                        strlen(sbuf),
                        0,
                        (struct sockaddr*)&cliaddr,
                        addrlen);
                } else if (!strncmp(buf, "RECORD", 6)) { //通话记录
                    do_record(buf, &cliaddr, addrlen);
                } else if (!strncmp(buf, "USSN", 4)) {
                    do_ussn(buf, &cliaddr, addrlen);
                } else if (!strncmp(buf, "NUMRECORD", 9)) {
                    char sbuf[200] = { 0 };
                    char sname[64] = { 0 };
                    char sid[64] = { 0 };
                    sscanf(buf, "%[^:]:%[^;]", sname, sid);
                    fprintf(stderr, "%s:%s", sname, sid);
                    snprintf(sbuf, 200, "%s %s OK", sname, sid);
                    sendto(phpsock,
                        sbuf,
                        strlen(sbuf),
                        0,
                        (struct sockaddr*)&cliaddr,
                        addrlen);
                } else if (!strncmp(buf, "CRON", 4))
                    sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
                else if (!strncmp(buf, "AUTO_SEND", 9)) {
                    auto_send_init();
                    sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
                } else if (!strncmp(buf, "req", 3))
                    keepalive_sendto(buf, &cliaddr, addrlen);
                else if (!strncmp(buf, "goip", 4)) {
                    goipka_init();
                    sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
                } else if (!strncmp(buf, "report_init", 11)) {
                    report_init();
                    sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
                } else if (!strncmp(buf, "CELLINFO", 8)) {
                    do_record(buf, &cliaddr, addrlen);
                } else if (!strncmp(buf, "CGATT", 5)) {
                    do_record(buf, &cliaddr, addrlen);
                } else if (!strncmp(buf, "BCCH", 4)) {
                    do_record(buf, &cliaddr, addrlen);
                } else if (!strncmp(buf, "HANGUP", 5)) {
                    do_record(buf, &cliaddr, addrlen);
                }
#ifdef _AUTO_SMS_RE
                else if (!strncmp(buf, "need_recharge", 13)) {
                    do_need_re(buf, &cliaddr, addrlen);
                } else if (!strncmp(buf, "recharge_ok", 11)) {
                    do_re_ok(buf, &cliaddr, addrlen);
                } else if (!strncmp(buf, "NUM_ICCID", 9)) {
                    do_num_iccid(buf, &cliaddr, addrlen);
                }
#endif
                else if (!strncmp(buf, "recharge", 8)) {
                    printf("recharge!\n");
                    auto_ussd_update();
                    sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
                }

                else if (!strncmp(buf, "START", 5)) {
                    int cliport;
                    char* p;
                    sscanf(buf, "START %d %*[^ ] %d", &recvid, &cliport);

                    while (*(++cmd) != ' ')
                        ; //第一个空格
                    *cmd++ = 0;
                    while (*(++cmd) != ' ')
                        ; //第一个空格
                    *cmd++ = 0;
                    p = cmd;
                    while (*(++cmd) != ' ')
                        ; //第个空格
                    *cmd++ = 0;

                    start_phplist(&cliaddr, recvid, p, cliport);
                    sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
                } else if (!strncmp(buf, "SYSTEM_SAVE", 11)) {
                    set_disable_status();
                    sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
                }

                else {
                    printf("goip:%d,prov:%d\n", i, goip[i].proid);
                    // if(!strncmp(buf, "USSD", 4) && strncmp(buf, "USSDEXIT", 8) ){
                    if (!strncmp(buf, "USSD", 4)) {
                        ussd_return_check(buf);
                    } else
                        background_msg_check(buf);
                    while (*cmd != ' ' && *cmd != ':')
                        cmd++; //第一个空格或冒号
                    cmd++;
                    sscanf(cmd, "%d", &recvid);
                    for (i = 0; i < goipcount; i++) {
                        if (goip[i].sock != -1 && goip[i].messageid == recvid)
                            break;
                    }
                    if (i >= goipcount) { //不是正通讯的id,忽略
                        struct phplist* tmp = phphead;
                        while (tmp) {
                            if (tmp->messageid == recvid) {
                                do_phplist(&cliaddr, recvid, buf, n);
                                break;
                            }
                            tmp = tmp->next;
                        }
                        // continue;
                    } else {
                        cmd--;
                        *cmd++ = 0;
                        goip[i].recvdata = cmd;
                        printf("recv:%s\n", goip[i].recvdata);
                        goip_handleEvent(&goip[i], buf, cmd);
                        goip[i].lasttime = nowtime;
                    }
                }
            }
            for (i = 0; i < goipcount; i++) {
                if (goip[i].sock != -1 && goip[i].lasttime + 5 < nowtime) { //超时
                    goip_handleEvent(&goip[i], "TIMEOUT", msg);
                    goip[i].lasttime = nowtime;
                }
            }
            auto_ussd_check();
            report_check(0);
#ifdef _AUTO_SMS_RE
            check_re_timeout();
#endif
        }
        goipka_timeout();
    }
    return 0;
}

int calloc_tel(MYSQL_RES* telres,
    int* interlen,
    int* locallen,
    char** inter,
    char** local,
    int* goip_fixed_id,
    int sms_count)
{
    char query[3500], *telnum, *p, *pp;
    MYSQL_ROW tmprow;
    int i, totalid = 0, len, tlen;
    printf("recv1:num:%d\n", (int)db_num_rows(telres));
    memset(query, 0, 3500);
    while ((tmprow = db_fetch_row(telres)) != NULL) {
        if (!tmprow[1] || !strlen(tmprow[1])) //空号码
            continue;
        if (tmprow[4] && goip_fixed_id)
            *goip_fixed_id = atoi(tmprow[4]);
        i = atoi(tmprow[0]) - 1;
        p = tmprow[1];
        len = 0;
        tlen = strlen(p);
        while (len < tlen) {
            pp = strchr(p, ',');
            if (pp) {
                *pp = 0;
                len += 1;
            }
            if (!(locallen[i] && !strncmp(p, local[i], locallen[i])) && *p != '+') {
                telnum = calloc(1, interlen[i] + strlen(p) + 1);
                sprintf(telnum, "%s%s", inter[i], p);
                printf("intertel:%s\n", telnum);
            } else {
                printf("localtel:%d,%s\n", locallen[i], local[i]);
                telnum = strdup(p);
            }
            len += strlen(p);
            p = tmprow[1] + len;
            // printf("tel %d %p\n", i,tels[i]);
            if (telhead[i] && !strcmp(telhead[i]->telnum, telnum)) { //重复的号码
                free(telnum);
                continue;
            }
            totalid++;
            teltmp = calloc(1, sizeof(struct telstr));
            teltmp->telnum = telnum;
            teltmp->recvid = atoi(tmprow[2]);
            teltmp->recvlev = atoi(tmprow[3]);
            // if(!query[0]){
            db_query("INSERT INTO sends "
                     "(messageid,userid,telnum,recvid,recvlev,provider,total) VALUES "
                     "(%d,%d,'%s',%s,%s,%s,%d)",
                sendid,
                userid,
                telnum,
                tmprow[2],
                tmprow[3],
                tmprow[0],
                sms_count);
            //}
            // else {
            //	sprintf(query+strlen(query), ",(%d,%d,'%s',%s,%s,%s)",
            // sendid,userid,telnum,tmprow[2],tmprow[3],tmprow[0]);
            //}
            // if(totalid++ % 100 == 0){
            //}
            MYSQL_RES* inserttmpres = db_query_store_result("SELECT LAST_INSERT_ID()");
            MYSQL_ROW inserttmprow = db_fetch_row(inserttmpres);
            // strncpy(goip->telid,inserttmprow[0],12);
            teltmp->telid = atoi(inserttmprow[0]);
            db_free_result(inserttmpres);
            totalid++;
            if (!telhead[i]) //加入链表
                telhead[i] = teltmp;
            else {
                teltmp->next = telhead[i];
                telhead[i] = teltmp;
            }
        }
        // telnow[i]=teltmp;
    }
    return totalid;
}

int calloc_tel2(MYSQL_RES* telres,
    char* num,
    int i,
    int recvid,
    int goipid,
    int provid,
    int sms_count)
{
    char* telnum;
    // MYSQL_ROW tmprow;
    int totalid = 0;
    if (!num) //空号码
        return -1;
    // i=atoi(tmprow[0])-1;
    telnum = strdup(num);
    if (telhead[i] && !strcmp(telhead[i]->telnum, telnum)) { //重复的号码
        free(telnum);
        return -1;
    }
    // totalid++;
    teltmp = calloc(1, sizeof(struct telstr));
    teltmp->telnum = telnum;
    teltmp->recvid = recvid;
    teltmp->recvlev = 0;
    db_query("INSERT INTO sends "
             "(messageid,userid,telnum,recvid,recvlev,provider,goipid,total) "
             "VALUES (%d,%d,'%s',%d,%d,%d,%d,%d)",
        sendid,
        userid,
        telnum,
        recvid,
        0,
        provid,
        goipid,
        sms_count);
    MYSQL_RES* inserttmpres = db_query_store_result("SELECT LAST_INSERT_ID()");
    MYSQL_ROW inserttmprow = db_fetch_row(inserttmpres);
    teltmp->telid = atoi(inserttmprow[0]);
    db_free_result(inserttmpres);
    // totalid++;
    // printf("444444\n");
    if (!telhead[i]) //加入链表
        telhead[i] = teltmp;
    else {
        teltmp->next = telhead[i];
        telhead[i] = teltmp;
    }
    // printf("444444\n");
    return totalid;
}

int checkcron()
{
    int totalid = 0; //总共的发送数量
    MYSQL_RES *res, *provres; //这个结构代表返回行的一个查询结果集
    MYSQL_ROW row, tmprow; //一个行数据的类型安全(type-safe)的表示
    char query[3500]; //查询语句
    int num, i, j, type, goip_fixed_id = 0;
    time_t nowtime;
    my_ulonglong prov_count;
    int *interlen, *locallen;
    char **inter, **local;
    struct goip* goip = 0;

    time(&nowtime);
    printf("%u\n", (unsigned int)nowtime);
    db_query(
        "update message set over=2 where stoptime>0 and over=0 and stoptime<%lu",
        nowtime); //去除超时的计划
    res = db_query_store_result(
        "select "
        "id,userid,receiverid,receiverid1,receiverid2,msg,crontime,groupid,"
        "groupid1,groupid2,type,recv,recv1,recv2,tel,prov,total from message "
        "where stoptime>0 and over=0 and crontime<%lu order by stoptime limit 1",
        nowtime); //
    if (db_num_rows(res) <= 0) {
        db_free_result(res);
        res = db_query_store_result(
            "select "
            "id,userid,receiverid,receiverid1,receiverid2,msg,crontime,groupid,"
            "groupid1,groupid2,type,recv,recv1,recv2,tel,prov,total from message "
            "where over=0 and crontime>0 order BY crontime LIMIT 1");
        // printf("%d\n", nowtime);
        if ((num = db_num_rows(res)) <= 0) { //没有需要执行的计划
            db_free_result(res);
            return 3600;
        }
        printf("num:%d\n", num);
        /*get id*/
    }
    row = db_fetch_row(res); //开始执行

    // char *p;

    // char insertmsgbuf[3500]={0};
    int crontime = atoi(row[6]);
    if (crontime > nowtime) { //没有到时的计划
        printf("%u, %u", (unsigned)crontime, (unsigned)nowtime);
        db_free_result(res);
        return crontime - nowtime;
    }
    memset(sendsmsg, 0, 3500);
    sprintf(sendsmsg,
        "<do cron>id:%s userid:%s receiverid:%s receiverid1:%s "
        "receiverid2:%s message:%s",
        row[0],
        row[1],
        row[2],
        row[3],
        row[4],
        row[5]);
    WriteToLog(sendsmsg);

    sendid = atoi(row[0]);
    printf("id:%s userid:%s receiverid:%s receiverid1:%s receiverid2:%s "
           "message:%s\n",
        row[0],
        row[1],
        row[2],
        row[3],
        row[4],
        row[5]);
    userid = atoi(row[1]);
    memset(sendsmsg, 0, 3500);
    msglen = strlen(row[5]);
    strncpy(sendsmsg, row[5], 3500);
    sms_count = atoi(row[16]);

    provres = db_query_store_result("select id,inter,local from prov order by id");
    prov_count = db_num_rows(provres);
    interlen = calloc(sizeof(int*), prov_count);
    locallen = calloc(sizeof(int*), prov_count);
    inter = calloc(sizeof(char*), prov_count);
    local = calloc(sizeof(char*), prov_count);
    i = 0;
    while ((tmprow = db_fetch_row(provres)) != NULL) {
        inter[i] = strdup(tmprow[1]);
        // printf("********\ninter:%s\n********\n", inter[i]);
        interlen[i] = strlen(tmprow[1]);
        local[i] = strdup(tmprow[2]);
        locallen[i] = strlen(tmprow[2]);
        telstrcount = atoi(tmprow[0]);
        printf("********\ninter:%s,local:%s\n********\n", inter[i], local[i]);
        i++;
    }
    telhead = calloc(sizeof(struct telstr*), telstrcount);
    db_free_result(provres);
    type = atoi(row[10]);
    if (!strcmp("6", row[10]) || !strcmp("7", row[10])) {
        printf("do 6,7");
    } else if (!strcmp("4", row[10])) { // number list
        printf("do 4\n");
        res = db_query_store_result(
            "select prov.id,message.tel,0,0,goipid,card_id from message,prov where "
            "prov.id=message.prov and message.id=%s",
            row[0]);
        // if(row[4]) goip_fixed_id=atoi(row[4]);
        // printf("do4 %s %s %s %s %s\n", row[0],row[1],row[2],row[3],row[4]);
        totalid = calloc_tel(
            res, interlen, locallen, inter, local, &goip_fixed_id, sms_count);
        if (row[5] && row[5][0])
            recharge_sms_start(atoi(row[5]));
        db_free_result(res);
    } else if (!strcmp("3", row[10])) {
        printf("do 3\n");
        res = db_query_store_result(
            "select prov.id,message.tel,0,0 from message,prov where "
            "prov.prov=message.prov and message.id=%s",
            row[0]);
        printf("%s\n", query);
        totalid = calloc_tel(res, interlen, locallen, inter, local, 0, sms_count);
        db_free_result(res);
    } else if (!strcmp("0", row[10])) { // receiver id list
        printf("do 0\n");

        // printf("rwo[%d]:%s\n",j,row[j]);
        res = db_query_store_result(
            "(SELECT provider,tel,id,0 as level FROM receiver where id in (%s)) "
            "union (SELECT provider1,tel1,id,1 FROM receiver where id in (%s) ) "
            "union (SELECT provider2,tel2,id,2 FROM receiver where id in (%s) ) "
            "ORDER BY tel",
            strlen(row[2]) ? row[2] : "0",
            strlen(row[3]) ? row[3] : "0",
            strlen(row[4]) ? row[4] : "0");
        totalid = calloc_tel(res, interlen, locallen, inter, local, 0, sms_count);
        db_free_result(res);
    } else if (!strcmp("1", row[10])) { //组发送
        // printf("rwo[%d]:%s\n",j,row[j]);
        res = db_query_store_result(
            "(SELECT provider,tel,receiver.id,0 as level ROM receiver inner join "
            "recvgroup on (receiver.id=recvgroup.recvid ) where groupsid in (%s)) "
            "union ( SELECT provider1,tel1,receiver.id,1 FROM receiver inner join "
            "recvgroup on (receiver.id=recvgroup.recvid ) where groupsid in (%s)) "
            "union ( SELECT provider2,tel2,receiver.id,2 FROM receiver inner join "
            "recvgroup on (receiver.id=recvgroup.recvid ) where groupsid in (%s)) "
            "order by tel",
            strlen(row[7]) ? row[7] : "0",
            strlen(row[8]) ? row[8] : "0",
            strlen(row[9]) ? row[9] : "0");
        totalid = calloc_tel(res, interlen, locallen, inter, local, 0, sms_count);
        db_free_result(res);
    } else { //全体发送  type=2
        res = db_query_store_result(
            "(SELECT provider,tel,id,0 as level FROM receiver where %s) "
            "union (SELECT provider1,tel1,id,1 FROM receiver where %s) "
            "union (SELECT provider2,tel2,id,2 FROM receiver where %s) ORDER BY "
            "tel",
            row[11],
            row[12],
            row[13]);
        totalid = calloc_tel(res, interlen, locallen, inter, local, 0, sms_count);
        db_free_result(res);
    }
    mysql_free_result(res);
    /*
   for(i=0;i<6;i++){
   printf("tel%d:\n", i);
   teltmp=telhead[i];
   while(teltmp){
   printf("%s\n", teltmp->telnum);
   teltmp=teltmp->next;
   }
   }
 */
    for (i = 0; i < prov_count; i++) {
        // if(inter[i])
        free(inter[i]);
        // if(local[i])
        free(local[i]);
    }
    free(interlen);
    free(locallen);

    MYSQL_RES* goipres = db_query_store_result(
        "SELECT name,provider,host,port,id,password FROM goip where alive=1 and "
        "gsm_status!='LOGOUT' ORDER BY id ");
    num = mysql_num_rows(goipres);
    printf("start to goip info:%d\n", num);
    if (num) {
        // struct goip *goiptmp=0;
        if (type == 7 || type == 6) {
            printf("000000\n");
            release_all(0, 0);
            telhead = calloc(sizeof(struct telstr*), num);
            telstrcount = num;
        }
        goip = calloc(num, sizeof(struct goip));
        for (i = 0; (row = mysql_fetch_row(goipres)) != NULL; i++) {
            goip[i].id = atoi(row[4]);
            // goip[i].proid=atoi(row[1]);
            if (type == 7 || type == 6) {
                goip[i].proid = i;
                goip[i].proid = atoi(row[1]);
                if (type == 7)
                    calloc_tel2(NULL,
                        auto_send->auto_send_num,
                        i,
                        0,
                        goip[i].id,
                        goip[i].proid,
                        sms_count);
                if (type == 6)
                    calloc_tel2(NULL,
                        auto_send->all_send_num,
                        i,
                        0,
                        goip[i].id,
                        goip[i].proid,
                        sms_count);
                goip[i].proid = i;
            } else {
                j = atoi(row[1]) - 1;
                printf(
                    "***************%d %d *************\n", goip_fixed_id, goip[i].id);
                if (telhead[j] == NULL || (goip_fixed_id && goip_fixed_id != goip[i].id)) { //没要发给改服务商的号码
                    goip[i].sock = -1;
                    continue;
                }
                goip[i].proid = j;
            }
            /*do connect*/
            // if(goiptmp)
            // goiptmp->next=&goip[i];
            // goiptmp=&goip[i];
            goip[i].id = atoi(row[4]);
            strncpy(goip[i].name, row[0], 64);
            // strncpy(goip[i].provider,row[1], 20);
            // goip[i].provider=j+1;
            strncpy(goip[i].password, row[5], 64);

            goip[i].addr.sin_family = AF_INET;
            if ((goip[i].addr.sin_addr.s_addr = inet_addr(row[2])) == 0) {
                printf("转化地址失败\n");
                goip[i].sock = -1;
                continue;
                release_all(goip, num);
                mysql_free_result(goipres);
                mysql_close(&mysql);
                return -1;
            }
            goip[i].addr.sin_port = htons(atoi(row[3]));
            goip[i].sock = phpsock;
            goip[i].messageid = sendid + (i << 16);
            /*
   goip[i].sock=socket(AF_INET,SOCK_DGRAM,0);
   if(goip[i].sock<0)
   {printf("socket error\n"); release_all(goip, num);
   mysql_free_result(goipres);mysql_close(&mysql);return -1;}
 */
            memset(query, 0, 3500);
            sprintf(query, "MSG %d %d %s\n", goip[i].messageid, msglen, sendsmsg);
            if (sendto(goip[i].sock,
                    query,
                    strlen(query),
                    0,
                    (struct sockaddr*)&goip[i].addr,
                    addrlen)
                < 0) {
                printf("sendto error\n");
                goip[i].sock = -1;
                continue;
                release_all(goip, num);
                mysql_free_result(goipres);
                mysql_close(&mysql);
                return -1;
            }
            printf("%s\n", query);
            goip[i].send = "MSG";
            goip[i].timer = 3;
            goip[i].m_stateEntry = ismsg;
            goip[i].m_stateEntryCount = sizeof(ismsg) / sizeof(struct goip_state_entry);
        }
    } else {
        printf("not find goip\n");
        WriteToLog("not find goip");
        memset(query, 0, 3500);
        sprintf(query, "delete from sends where messageid=%d", sendid);
        mysql_real_query(&mysql, query, (unsigned int)strlen(query));
        mysql_free_result(goipres);
        mysql_close(&mysql);

        return 60;
    } // if(num)
    mysql_free_result(goipres);
    db_query("update message set over=1,time=NOW() where id=%d", sendid);
    do_select(goip, num, *telhead, sendsmsg, msglen);
    db_query("update message set over=2 where id=%d and crontime<%ld",
        sendid,
        time(NULL));
    printf("over 1 type:%d\n", type);
    if (type == 7) {
        change_auto(auto_send);
    } else if (auto_send && auto_send->auto_send == 1) {
        MYSQL_RES* auto_res = db_query_store_result("select id form message where over=0 and type=7");
        if (db_num_rows(auto_res) != 1) {
            change_auto(auto_send);
        }
        db_free_result(auto_res);
    }
    printf("over\n");
    return 0;
}

void goipka_timeout()
{
    int nowtime = time(NULL);
    // struct goipkeepalive *ka=kahead;
    struct phplist* list = phphead;
    // int flag=0;
    printf("check timeout\n");
    db_query("update goip set "
             "alive=0,`signal`='',gsm_status='',voip_status='',voip_state='',num="
             "'' where keepalive_time_t<%d-92",
        nowtime);

    while (list) {
        if (nowtime - list->timeout > 1200)
            list = del_phplist(list);
        else
            list = list->next;
    }
}

int goipka_init()
{
    MYSQL_RES* res;
    MYSQL_ROW row;
    struct goipkeepalive* ka;
    time_t lasttime;

    db_query("update goip set "
             "alive=0,`signal`='',gsm_status='',voip_status='',voip_state='',num="
             "'' where 1");

    db_query("update message set over=2 where over=1");
    res = db_query_store_result(
        "select id,name,password,provider,remain_time,group_id,report_mail from "
        "goip order by id");
    while (kahead != NULL) {
        ka = kahead->next;
        free(kahead->id);
        free(kahead->password);
        free(kahead);
        kahead = ka;
    }
    lasttime = time(NULL);
    while ((row = db_fetch_row(res)) != NULL) {
        int i;
        // printf("goip\n");
        ka = calloc(sizeof(struct goipkeepalive), 1);
        ka->sqlid = atoi(row[0]);
        ka->id = strdup(row[1]);
        ka->password = strdup(row[2]);
        ka->prov = atoi(row[3]);
        ka->group_id = atoi(row[5]);
        strcpy(ka->report_mail, row[6]);
        // printf("report_mail:%s\n",ka->report_mail);
        ka->lasttime = 0;
        ka->gsm_login_time = 0;
        // ka->remain_time=atoi(row[4]);
        ka->remain_time = -1;
        // if(kahead==NULL) kahead=ka;
        // else {
        for (i = 0; i < MAXRECVID; i++)
            ka->recvid[i] = 1000; // default
        ka->next = kahead;
        kahead = ka;
        //}
    }
    db_free_result(res);
    return 0;
}

void add_auto_reply(struct auto_send* auto_send, char* num, int goipid)
{
    db_query("insert into message (type,msg,crontime,tel,goipid) VALUES (%d, "
             "'%s',%ld,'%s','%d')",
        5,
        auto_send->auto_send_msg,
        time(NULL),
        num,
        goipid);
}

void change_auto(struct auto_send* auto_send)
{
    printf("change_auto %d\n", auto_send->auto_send);
    db_query("delete from message where type=7 and over=0");
    if (auto_send->auto_send) {
        db_query("insert into message (type,msg,crontime,tel) VALUES (%d, "
                 "'%s',%ld,'%s')",
            7,
            auto_send->auto_send_msg,
            time(NULL) + auto_send->auto_send_timeout,
            auto_send->auto_send_num);
    }
}

int auto_send_init()
{
    MYSQL_RES* res;
    MYSQL_ROW row;
    time_t lasttime;
    res = db_query_store_result(
        "select auto_reply,reply_num_except,reply_msg, "
        "auto_send,auto_send_num,auto_send_msg,auto_send_timeout, "
        "all_send_num,all_send_msg from auto limit 1");
    db_query("delete from message where type=7 and over=0");
    lasttime = time(NULL);
    while ((row = db_fetch_row(res)) != NULL) {
        if (!auto_send)
            auto_send = calloc(sizeof(struct auto_send), 1);
        auto_send->auto_reply = atoi(row[0]);
        strncpy(
            auto_send->reply_num_except, row[1], sizeof(auto_send->reply_num_except));
        strncpy(auto_send->reply_msg, row[2], sizeof(auto_send->reply_msg));
        strncpy(auto_send->auto_send_num, row[4], sizeof(auto_send->auto_send_num));
        strncpy(auto_send->auto_send_msg, row[5], sizeof(auto_send->auto_send_msg));
        int t = auto_send->auto_send_timeout;
        auto_send->auto_send_timeout = atoi(row[6]) * 60;
        if (auto_send->auto_send != atoi(row[3]) || t > atoi(row[6]) * 60) { //启用变化或者时间变小
            auto_send->auto_send = atoi(row[3]);
            change_auto(auto_send);
        }
        auto_send->auto_send = atoi(row[3]);
        strncpy(auto_send->all_send_num, row[7], sizeof(auto_send->all_send_num));
        strncpy(auto_send->all_send_msg, row[8], sizeof(auto_send->all_send_msg));
    }
    db_free_result(res);
    return 0;
}

void getcfg(char*);
int real_main(int argc, char** argv)
{
    struct sockaddr_in phpaddr, cliaddr;
    fd_set reset, allset;
    int maxfde, n, nready;
    unsigned addrlen;
    struct timeval timeout = { 0 };
    char buf[3500];
    int recvid;
    char* cmd;
    time_t starttime, endtime, checktime = 1;

    if (argc > 1)
        logfile = argv[1];
    if (argc > 0)
        getcfg(argv[0]);
    else
        getcfg(NULL);
    db_init();
    goipka_init();
    auto_send_init();
    auto_ussd_init();
    report_init();
    set_disable_status();
    phpaddr.sin_family = AF_INET;
    if ((phpaddr.sin_addr.s_addr = htonl(INADDR_ANY)) < 0) {
        perror("phpsock转化地址失败");
        return -1;
    }
    phpaddr.sin_port = htons(phpport);
    phpsock = socket(AF_INET, SOCK_DGRAM, 0);
    if (phpsock < 0) {
        perror("socket error:");
        return -1;
    }
    if (bind(phpsock, (struct sockaddr*)&phpaddr, sizeof(phpaddr)) < 0) {
        perror("bind error:");
        return -1;
    }

    FD_ZERO(&allset);
    maxfde = -1;
    FD_SET(phpsock, &allset);
    maxfde = phpsock + 1;

    starttime = time(NULL);
    srand(starttime);
    timeout.tv_sec = 2;
    while (1) {
        timeout.tv_usec = 0;

        // WriteToLog("sleep");
        printf("sleep:%lds, %ldus\n", timeout.tv_sec, timeout.tv_usec);
        reset = allset;
        nready = select(maxfde, &reset, NULL, NULL, &timeout);
        printf("only phpsock nready:%d\n", nready);
        // WriteToLog("only phpsock nready");
        if (nready == -1) {
            perror("select error");
            return -1;
        } else if (nready > 0) {
            memset(buf, 0, 3500);
            addrlen = sizeof(cliaddr);
            memset(&cliaddr, 0, addrlen);
            n = recvfrom(phpsock, buf, 3500, 0, (struct sockaddr*)&cliaddr, &addrlen);
            if (n <= 0)
                continue;
            printf("**** %s ****\n", buf);
            if (!strncmp(buf, "RECEIVE", 7)) { //接收短信
                /* 检查缓存，是新的就更新数据库,发送php页面更新命令*/
                do_recv(buf, &cliaddr, addrlen);

                /* 应答*/
            } else if (!strncmp(buf, "DELIVER", 7)) {
                do_deliver(buf, &cliaddr, addrlen);
            } else if (!strncmp(buf, "STATE", 5)) { //状态
                char sbuf[200] = { 0 };
                char sname[64] = { 0 };
                char sid[64] = { 0 };
                sscanf(buf, "%[^:]:%[^;]", sname, sid);
                snprintf(sbuf, 200, "%s %s OK", sname, sid);
                sendto(
                    phpsock, sbuf, strlen(sbuf), 0, (struct sockaddr*)&cliaddr, addrlen);
                do_state(buf, &cliaddr, addrlen);
            } else if (!strncmp(buf, "REMAIN", 6)) { //剩余时间
                char sbuf[200] = { 0 };
                char sname[64] = { 0 };
                char sid[64] = { 0 };
                sscanf(buf, "%[^:]:%[^;]", sname, sid);
                snprintf(sbuf, 200, "%s %s OK", sname, sid);
                sendto(
                    phpsock, sbuf, strlen(sbuf), 0, (struct sockaddr*)&cliaddr, addrlen);
            } else if (!strncmp(buf, "RECORD", 6)) { //通话记录
                do_record(buf, &cliaddr, addrlen);
            } else if (!strncmp(buf, "USSN", 4)) {
                do_ussn(buf, &cliaddr, addrlen);
            } else if (!strncmp(buf, "EXPIRY", 6)) { //通话时间
                do_record(buf, &cliaddr, addrlen);
            } else if (!strncmp(buf, "NUMRECORD", 9)) {
                char sbuf[200] = { 0 };
                char sname[64] = { 0 };
                char sid[64] = { 0 };
                sscanf(buf, "%[^:]:%[^;]", sname, sid);
                fprintf(stderr, "%s:%s", sname, sid);
                snprintf(sbuf, 200, "%s %s OK", sname, sid);
                sendto(
                    phpsock, sbuf, strlen(sbuf), 0, (struct sockaddr*)&cliaddr, addrlen);
            } else if (!strncmp(buf, "CRON", 4)) {
                sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
            } else if (!strncmp(buf, "AUTO_SEND", 9)) {
                auto_send_init();
                sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
            } else if (!strncmp(buf, "req", 3)) {
                keepalive_sendto(buf, &cliaddr, addrlen);
            } else if (!strncmp(buf, "goip", 4)) {
                goipka_init();
                sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
            } else if (!strncmp(buf, "goipcronexit", 12)) {
                sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
#ifdef _GOIPCRONEXIT
                exit(0);
#endif
            } else if (!strncmp(buf, "report_init", 11)) {
                report_init();
                sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
            } else if (!strncmp(buf, "CELLINFO", 8)) {
                do_record(buf, &cliaddr, addrlen);
            } else if (!strncmp(buf, "CGATT", 5)) {
                do_record(buf, &cliaddr, addrlen);
            } else if (!strncmp(buf, "BCCH", 4)) {
                do_record(buf, &cliaddr, addrlen);
            } else if (!strncmp(buf, "HANGUP", 5)) {
                do_record(buf, &cliaddr, addrlen);
            }
#ifdef _AUTO_SMS_RE
            else if (!strncmp(buf, "need_recharge", 13)) {
                do_need_re(buf, &cliaddr, addrlen);
            } else if (!strncmp(buf, "recharge_ok", 11)) {
                do_re_ok(buf, &cliaddr, addrlen);
            } else if (!strncmp(buf, "NUM_ICCID", 9)) {
                printf("test1111\n");
                do_num_iccid(buf, &cliaddr, addrlen);
            }
#endif
            else if (!strncmp(buf, "recharge", 8)) {
                printf("recharge!\n");
                auto_ussd_update();
                sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
            }

            else if (!strncmp(buf, "START", 5)) {
                int cliport;
                char* p;
                printf("ssss:%s", buf);
                cmd = buf;
                sscanf(buf, "START %d %*[^ ] %d", &recvid, &cliport);
                if (recvid == 0 || cliport == 0)
                    continue;

                while (*(++cmd) != ' ')
                    ; //第一个空格
                *cmd++ = 0;
                while (*(++cmd) != ' ')
                    ; //第一个空格
                *cmd++ = 0;
                p = cmd;
                while (*(++cmd) != ' ')
                    ; //第个空格
                *cmd++ = 0;

                printf("lala:%s\n", p);
                start_phplist(&cliaddr, recvid, p, cliport);
                sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
            } else if (!strncmp(buf, "SYSTEM_SAVE", 11)) {
                set_disable_status();
                sendto(phpsock, "OK", 2, 0, (struct sockaddr*)&cliaddr, addrlen);
            } else {
                cmd = buf;
                // printf("buf:%s", buf);
                if (!strncmp(buf, "USSD", 4)) {
                    // if(!strncmp(buf, "USSD", 4) && strncmp(buf, "USSDEXIT", 8)){
                    ussd_return_check(buf);
                } else {
                    background_msg_check(buf);
                }
                while (*cmd != ' ' && *cmd != ':')
                    cmd++; //第一个空格
                cmd++;
                sscanf(cmd, "%d", &recvid);
                if (recvid == 0)
                    continue;
                struct phplist* tmp = phphead;
                while (tmp) {
                    if (tmp->messageid == recvid) {
                        do_phplist(&cliaddr, recvid, buf, n);
                        break;
                    }
                    tmp = tmp->next;
                }
            }
        }

        endtime = time(NULL);
        auto_ussd_check();
        report_check(0);
#ifdef _AUTO_SMS_RE
        check_re_timeout();
#endif
        if (endtime - checktime >= starttime || !strncmp(buf, "CRON", 4) || !strncmp(buf, "RECEIVE", 7)) {
            starttime = endtime;
            while ((timeout.tv_sec = checkcron()) <= 0)
                ;
            if (timeout.tv_sec > 5)
                timeout.tv_sec = 5;
        } else
            timeout.tv_sec = 5;
        // checktime=timeout.tv_sec;
        checktime = 30;
        goipka_timeout();
    }
    return 0;
}

void getcfg(char* file)
{
    FILE* fp;
    char buf[3500];
    char tmpname[3500];
    char tmpvalue[3500];
    if (file)
        cfgfile = file;
    fp = fopen(cfgfile, "r");
    if (fp == NULL) {
        // OutputDebugString("Log file open failed.");
        return;
    }
    while (!feof(fp)) {
        memset(buf, 0, 3500);
        memset(tmpname, 0, 3500);
        memset(tmpvalue, 0, 3500);
        if (fgets(buf, 3500, fp) < 0)
            continue;
        fprintf(stderr, "read:%s", buf);
        sscanf(buf, "%[^=]='%[^']'", tmpname, tmpvalue);
        if (strcmp(tmpname, "$dbhost") == 0) {
            mysqlhost = strdup(tmpvalue);
        } else if (strcmp(tmpname, "$dbuser") == 0) {
            user = strdup(tmpvalue);
        } else if (strcmp(tmpname, "$dbpw") == 0) {
            password = strdup(tmpvalue);
        } else if (strcmp(tmpname, "$dbname") == 0) {
            dbname = strdup(tmpvalue);
        } else if (strcmp(tmpname, "$goipcronport") == 0) {
            phpport = atoi(tmpvalue);
        } else if (strcmp(tmpname, "$charset") == 0) {
            charset = strdup(tmpvalue);
        }
    }
    fclose(fp);
    return;
}
