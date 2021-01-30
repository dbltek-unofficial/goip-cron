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
#include "debug.h"
#include "goipcron.h"
#include "mysql.h"
#include "re.h"
#include "send_mail.h"

#define __RE_DEL_TIMEOUT
extern struct goipkeepalive* kahead;
extern int phpsock;
void add_sms0(char* msg, char* tel, int goip_id, int prov_id, int delay, int resend);
extern int
WriteToLog(char* str);
void check_re(int group_id,
    int goip_id,
    char* goip_name,
    int prov_id); //充值完查一下一个
void check_re_all(); //定时检查

void do_re(char* num, int goip_id, char* goip_name, int prov_id, int group_id);

void save_error(char* id, char* num, char* goip, char* disable_flag)
{
    // DB_ROW row;
    // DB_RES *res;
    db_query("update recharge_record set result=400 where id in (%s)", id);
    if (goip[0])
        db_query("update goip set re_ing=0,re_remain_count=0 where id in (%s)",
            goip);
    /* not disable be sim
res=db_query_store_result("select id from goip where num in (%s)", num);
while((row=db_fetch_row(res))!=NULL){ //disable be
        background_cmd_calloc(atoi(row[0]), "disable_sim2", disable_flag, 0);
}

db_free_result(res);
*/
}

void check_re_timeout()
{
    char id_buf[1024] = { 0 }, num_buf[1024] = { 0 }, goip_buf[1024] = { 0 };
    DB_ROW row;
    DB_RES* res = NULL;
    static int last_time = 0;
    int now_time = time(NULL), i = 0;
    if (last_time == 0) {
        last_time = time(NULL);
        return;
    }
    if (last_time + 5 > now_time)
        return;
    last_time = now_time;

    res = db_query_store_result(
        "select recharge_record.id,goip_id,provider,fixed_num from "
        "recharge_record left join recharge_group on "
        "recharge_group.id=recharge_record.re_group_id left join goip on "
        "goip.id=recharge_record.goip_id where fixed_num!='' and result=100 and "
        "`date`<(now()-INTERVAL 15 SECOND) limit 80");
    while ((row = db_fetch_row(res)) != NULL) {
        add_sms0("yes", row[3], atoi(row[1]), atoi(row[2]), 0, 2);
        db_query("update recharge_record set result=101 where id='%s'", row[0]);
    }
    db_free_result(res);
    /*
res=db_query_store_result("select
recharge_record.id,goip.id,provider,dynamic_num,send_num,result,goip.name,recharge_group.id,recharge_record.num
from recharge_record left join recharge_group on
recharge_group.id=recharge_record.re_group_id left join goip on
goip.id=recharge_record.goip_id where (result=100 or result=101) and
`date`<(now()-INTERVAL 30 SECOND) and resend_count<2 limit 80");
while((row=db_fetch_row(res))!=NULL){
        if(atoi(row[5])==100){
                do_re(row[8], atoi(row[1]), row[6], atoi(row[2]),
atoi(row[7]), 1);
        }
        else {
                add_sms("yes", row[4], atoi(row[1]), atoi(row[2]));
                db_query("update recharge_record set
resend_count=resend_count+1 where id='%s'", row[0]);
        }
        return;
}
db_free_result(res);
*/
    res = db_query_store_result(
        "select "
        "recharge_record.id,goip_id,recharge_record.num,recharge_record.re_group_"
        "id,re_remain_count from recharge_record left join goip on "
        "goip.id=recharge_record.goip_id where (result=100 or result=101) and "
        "`date`<(now()-INTERVAL 360 SECOND) limit 80");
    while ((row = db_fetch_row(res)) != NULL) {
        char value_buf[20] = { 0 }; //="load fail:";
        if (id_buf[0] && row[0][0])
            strcat(id_buf, ",");
        strcat(id_buf, row[0]);
        if (num_buf[0] && row[2][0])
            strcat(num_buf, ",");
        strcat(num_buf, row[2]);
        if (goip_buf[0] && row[1][0])
            strcat(goip_buf, ",");
        strcat(goip_buf, row[1]);
        if (row[4])
            snprintf(value_buf + strlen(value_buf), 9, "%d", 10 + atoi(row[4]));
        background_cmd_calloc(atoi(row[1]),
            "disable_sim2",
            value_buf,
            0); // disable re
        i++;
    }
    if (i >= 1)
        save_error(id_buf, num_buf, goip_buf, "4");
    db_free_result(res);
#ifndef __RE_DEL_TIMEOUT
    res = db_query_store_result(
        "select id,goip_id,num,re_group_id from recharge_record where result=0 "
        "and `date`<(now()-INTERVAL 300 SECOND) limit 80");
    i = 0;
    id_buf[0] = 0;
    num_buf[0] = 0;
    goip_buf[0] = 0;
    while ((row = db_fetch_row(res)) != NULL) {
        if (id_buf[0] && row[0][0])
            strcat(id_buf, ",");
        strcat(id_buf, row[0]);
        if (num_buf[0] && row[2][0])
            strcat(num_buf, ",");
        strcat(num_buf, row[2]);
        if (goip_buf[0] && row[1][0])
            strcat(goip_buf, ",");
        strcat(goip_buf, row[1]);
        // background_cmd_calloc(atoi(row[1]), "disable_sim2", "1", 0); //disable re
        i++;
    }
    if (i >= 1)
        save_error(id_buf, num_buf, goip_buf, "1");
    db_free_result(res);
#endif
    // if(i>=80) check_re_timeout();

    check_re_all();
}

char* gen_num(char* num, char* num_bre, char* num_aft, int source_flag)
{
    char buf[64] = { 0 }, *p;
    if (!num || !num_bre)
        return num;
    if ((p = strstr(num, num_bre)) != NULL) {
        sprintf(buf, "%s%s", num_aft, p + (strlen(num_bre) - 1));
    } else
        strcpy(buf, num);
    p = buf;
    if (source_flag)
        return p + strlen(num_aft);
    return p;
}

int check_str(char* content, char* p, char* num, char* num_bre, char* num_aft)
{
    char need_find[256] = { 0 }, *find_p;
    find_p = strchr(p, '$');
    if (find_p) {
        *find_p = 0;
        sprintf(
            need_find, "%s%s%s", p, gen_num(num, num_bre, num_aft, 1), find_p + 1);
    } else
        strcpy(need_find, p);
    printf(need_find);
    if (strstr(content, need_find))
        return 1;
    else
        return 0;
}

int find_str(char* content, char* str, char* num, char* num_bre, char* num_aft)
{
    int get_flag = 0;
    char buf[256] = { 0 }, *p = buf, *pp;
    strcpy(buf, str);
    while ((pp = strchr(p, '|')) != NULL) {
        *pp = 0;
        if (p != pp) {
            if (check_str(content, p, num, num_bre, num_aft))
                return 1;
        }
        p = pp + 1;
    }
    if (!get_flag && check_str(content, p, num, num_bre, num_aft))
        return 1;
    return 0;
}

void check_re_return(int goip_id,
    char* goip_name,
    int prov_id,
    char* num,
    char* content)
{
    char sql[512];
    DB_ROW row;
    DB_RES* res;
    res = db_query_store_result(
        "select recharge_record.id,goip.id,provider,dynamic_num from "
        "recharge_record left join recharge_group on "
        "recharge_group.id=recharge_record.re_group_id left join goip on "
        "goip.id=recharge_record.goip_id where goip_id='%d' and result=100 and "
        "dynamic_num!='' order by `date` desc limit 1",
        goip_id);
    if ((row = db_fetch_row(res)) != NULL) {
        if (!strncmp(num, row[3], strlen(row[3]))) {
            add_sms0("yes", num, atoi(row[1]), atoi(row[2]), 0, 2);
            db_query("update recharge_record set result=101 where id='%s'", row[0]);
        }
        db_free_result(res);
        return;
    }
    sprintf(sql,
        "select return_num, ok_content, fail_content, recharge_record.id, "
        "recharge_group.id as group_id,goip.id as "
        "be_goip_id,num_bre,num_aft,recharge_record.num from recharge_record "
        "left join recharge_group on "
        "recharge_group.id=recharge_record.re_group_id left join goip on "
        "goip.num=recharge_record.num where goip_id='%d' and (result=101 or "
        "result=100) order by `date` desc limit 1",
        goip_id);
    res = db_query_store_result(sql);
    if ((row = db_fetch_row(res)) != NULL) {
        if (strcmp(num, row[0])) { // not find
            DLOG("num error %s %s", num, row[0]);
            db_free_result(res);
            return;
        } else if (find_str(content, row[1], row[8], row[6], row[7])) { // find ok
            int group_id = atoi(row[4]);
            DLOG("OK");
            sprintf(
                sql, "update recharge_record set result=200 where id='%s'", row[3]);
            db_query(sql);
            sprintf(sql,
                "update goip set re_remain_count=re_remain_count-1,re_ing=0 "
                "where id='%d'",
                goip_id);
            db_query(sql);
            db_free_result(res);
            res = db_query_store_result("select re_remain_count from goip where "
                                        "id='%d' and re_type=1 and re_remain_count<=0",
                goip_id);
            if ((row = db_fetch_row(res)) != NULL) {
                background_cmd_calloc(goip_id,
                    "disable_sim2",
                    "2",
                    0); // disable re, done
                db_free_result(res);
                return;
            }
            check_re(group_id, goip_id, goip_name, prov_id);
            return;
        } else if (find_str(content, row[2], row[8], row[6],
                       row[7])) { // find fail
            DLOG("FAIL");
            background_cmd_calloc(atoi(row[5]), "disable_sim2", "4",
                0); // disable be
            sprintf(
                sql, "update recharge_record set result=400 where id='%s'", row[3]);
            db_free_result(res);
            db_query(sql);
            res = db_query_store_result(
                "select re_remain_count from goip where id='%d' and re_type=1",
                goip_id);
            if ((row = db_fetch_row(res)) != NULL) {
                char value_buf[20];
                snprintf(value_buf, 19, "%d", 10 + atoi(row[0]));
                background_cmd_calloc(goip_id,
                    "disable_sim2",
                    value_buf,
                    0); // disable re
                db_free_result(res);
            }
            sprintf(
                sql, "update goip set re_ing=0,re_remain_count=0 where id=%d", goip_id);
            db_query(sql);
            return;
        } else {
            DLOG("ELSE");
            db_free_result(res);
        }
    }
}

void do_re(char* num, int goip_id, char* goip_name, int prov_id, int group_id)
{
    DB_ROW row;
    DB_RES* res;
    char sql[512], num0[64] = { 0 }, *num_aft, content[160] = { 0 }, *p;
    int i;
    sprintf(
        sql,
        "select type,num_bre,num_aft,num,content from recharge_group where id=%d",
        group_id);
    res = db_query_store_result(sql);
    if ((row = db_fetch_row(res)) != NULL) {
        if (!row[3][0] || !row[4][0])
            goto DO_RE_FAIL;
        num_aft = gen_num(num, row[1], row[2], 0);
        p = row[3];
        i = 0;
        while (*p && i < 64) {
            if (*p == '$') {
                strcpy(num0 + strlen(num0), num_aft);
                i += strlen(num_aft);
                p++;
            } else
                num0[i++] = *p++;
        }
        p = row[4];
        i = 0;
        while (*p && i < 64) {
            if (*p == '$') {
                strcpy(content + strlen(content), num_aft);
                i += strlen(num_aft);
                p++;
            } else
                content[i++] = *p++;
        }
        printf("send num:%s, content:%s, goip_id:%d, prov_id:%d",
            num0,
            content,
            goip_id,
            prov_id);
        add_sms0(content, num0, goip_id, prov_id, 0, 2);
        db_free_result(res);
        sprintf(sql, "update goip set re_ing=1 where id=%d", goip_id);
        db_query(sql);
        sprintf(sql,
            "update recharge_record set result=100,goip_id=%d,re_name='%s' "
            "where num='%s' order by id desc",
            goip_id,
            goip_name,
            num);
        db_query(sql);
        return;
    }
DO_RE_FAIL:
    db_free_result(res);
}

void check_re(int group_id,
    int goip_id,
    char* goip_name,
    int prov_id) //充值完查一下一个
{
    char sql[512];
    DB_ROW row;
    DB_RES* res;
    sprintf(sql,
        "select num,id from recharge_record where re_group_id='%d' and "
        "result=0 order by `date` limit 1",
        group_id);
    res = db_query_store_result(sql);
    if ((row = db_fetch_row(res)) != NULL) {
        do_re(row[0], goip_id, goip_name, prov_id, group_id);
        db_free_result(res);
    }
}

void check_re_be(int group_id, char* num) //找充值卡
{
    char sql[512];
    DB_ROW row;
    DB_RES* res;
    sprintf(sql,
        "select id, provider,name from goip where re_group_id='%d' and "
        "gsm_status='LOGIN' and re_remain_count>0 and re_ing=0 and re_type=1 "
        "order by re_remain_count limit 1",
        group_id);
    res = db_query_store_result(sql);
    if ((row = db_fetch_row(res)) != NULL) {
        do_re(num, atoi(row[0]), row[2], atoi(row[1]), group_id);
        db_free_result(res);
    }
}

void check_re_all() //定时检查
{
    DB_ROW row;
    DB_RES* res;
    res = db_query_store_result(
        "select num,re_group_id from recharge_record where result=0");
    while ((row = db_fetch_row(res)) != NULL) {
        check_re_be(atoi(row[1]), row[0]);
    }
    db_free_result(res);
}

void do_num_iccid(char* buf, struct sockaddr_in* cliaddr, int addrlen)
{
    int recvid;
    DB_ROW row;
    DB_RES* res;
    struct goipkeepalive* ka = kahead;
    char id[64] = { 0 }, gpassword[64], sendbuf[1024] = { 0 }, num[64] = { 0 },
         iccid[64] = { 0 };
    sscanf(buf,
        "NUM_ICCID:%d;id:%64[^;];password:%64[^;];num:%64[^;];iccid:%64[0-9]",
        &recvid,
        id,
        gpassword,
        num,
        iccid);
    while (ka) {
        if (!strcmp(ka->id, id) && !strcmp(ka->password, gpassword)) {
            break;
        }
        ka = ka->next;
    }
    if (!ka) {
        printf("recvive error\n");
        sprintf(sendbuf,
            "recharge_ok %d ERROR not find this id:%s, or password error",
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
    sprintf(sendbuf, "recharge_ok %d OK", recvid);
    if (sendto(phpsock,
            sendbuf,
            strlen(sendbuf),
            0,
            (struct sockaddr*)cliaddr,
            addrlen)
        < 0)
        WriteToLog("sendto err");
    // if(iccid[strlen(iccid)-1]=='\n') iccid[strlen(iccid)-1]=0;
    db_query(
        "update goip set num='%s' where iccid='%s' and alive='1'", num, iccid);
    res = db_query_store_result(
        "select id from goip where iccid='%s' and alive='1'", iccid);
    if ((row = db_fetch_row(res)) != NULL) {
        background_cmd_calloc(atoi(row[0]), "set_num", num, 0);
        db_free_result(res);
    }
    printf("num over\n");
}

void do_re_ok(char* buf, struct sockaddr_in* cliaddr, int addrlen)
{
    int recvid;
    DB_ROW row;
    DB_RES* res;
    struct goipkeepalive* ka = kahead;
    char id[64] = { 0 }, gpassword[64], sendbuf[1024] = { 0 }, num[64] = { 0 };
    sscanf(buf,
        "recharge_ok:%d;id:%64[^;];password:%64[^;];num:%64[^;]",
        &recvid,
        id,
        gpassword,
        num);
    while (ka) {
        if (!strcmp(ka->id, id) && !strcmp(ka->password, gpassword)) {
            break;
        }
        ka = ka->next;
    }
    if (!ka) {
        printf("recvive error\n");
        sprintf(sendbuf,
            "recharge_ok %d ERROR not find this id:%s, or password error",
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
    sprintf(sendbuf, "recharge_ok %d OK", recvid);
    if (sendto(phpsock,
            sendbuf,
            strlen(sendbuf),
            0,
            (struct sockaddr*)cliaddr,
            addrlen)
        < 0)
        WriteToLog("sendto err");
    res = db_query_store_result(
        "select id,goip_id,result from recharge_record where num='%s' and "
        "(result=100 or result=101 or result=400)",
        num);
    if ((row = db_fetch_row(res)) != NULL) {
        int goip_id = atoi(row[1]);
        int result = atoi(row[2]);
        db_query("update recharge_record set result=600 where id='%s'", row[0]);
        db_query("update goip set re_remain_count=re_remain_count-1,re_ing=%s "
                 "where id='%d'",
            (result == 400) ? "re_ing" : "0",
            goip_id);
        db_free_result(res);
        res = db_query_store_result(
            "select re_remain_count from goip where id='%d' and re_type=1 and "
            "re_remain_count<=0 and re_ing=0",
            goip_id);
        if ((row = db_fetch_row(res)) != NULL) {
            background_cmd_calloc(goip_id, "disable_sim2", "2", 0); // disable re,
                // done
            db_free_result(res);
            return;
        }
    }
}

int do_need_re(char* buf, struct sockaddr_in* cliaddr, int addrlen)
{
    int re_group_id, recvid;
    DB_ROW row;
    DB_RES* res;
    struct goipkeepalive* ka = kahead;
    char id[64] = { 0 }, gpassword[64], sendbuf[1024] = { 0 }, num[64] = { 0 },
         goip_name[64] = { 0 };
    sscanf(buf,
        "need_recharge:%d;id:%64[^;];password:%64[^;];num:%64[^;]",
        &recvid,
        id,
        gpassword,
        num);
    while (ka) {
        if (!strcmp(ka->id, id) && !strcmp(ka->password, gpassword)) {
            break;
        }
        ka = ka->next;
    }
    if (!ka) {
        printf("recvive error\n");
        sprintf(sendbuf,
            "need_recharge %d ERROR not find this id:%s, or password error",
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
        return -1;
    }
    sprintf(sendbuf, "need_recharge %d OK", recvid);
    if (sendto(phpsock,
            sendbuf,
            strlen(sendbuf),
            0,
            (struct sockaddr*)cliaddr,
            addrlen)
        < 0)
        WriteToLog("sendto err");
    res = db_query_store_result("select re_group_id,num,name from goip where "
                                "id='%d' and re_group_id!=0 and re_type=0",
        ka->sqlid);
    if ((row = db_fetch_row(res)) != NULL) {
        re_group_id = atoi(row[0]);
        // strcpy(num, row[1]);
        strcpy(goip_name, row[2]);
        db_free_result(res);
    } else {
        db_free_result(res);
        return -1;
    }
    res = db_query_store_result("select id from recharge_record where num='%s'", num);
    if ((row = db_fetch_row(res)) != NULL) {
        db_free_result(res);
        return -1;
    } else {
        db_free_result(res);
        db_query("insert into recharge_record set num='%s', be_name='%s', "
                 "re_group_id='%d'",
            num,
            goip_name,
            re_group_id);
        check_re_be(re_group_id, num);
        return 0;
    }
}

void re_resend(int message_id, int send_id)
{
    db_query("update message set resend=resend-1,crontime=%ld,over=0 where "
             "resend>0 and id=%d",
        time(NULL) + 15,
        message_id);
}
