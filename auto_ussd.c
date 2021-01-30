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
#include "send_mail.h"

//#define _FRANCE

struct auto_ussd* auto_ussd_head = NULL;
struct auto_recharge* auto_recharge_head = NULL;
struct auto_recharge_sms* auto_recharge_sms_head = NULL;
int auto_check_time;

extern int phpsock; //接收php的sock
extern int addrlen; //接收php的sock
extern struct goipkeepalive* kahead;

int ussd_send(struct auto_ussd* tmp);
struct auto_ussd*
auto_ussd_calloc(char* cmd,
    char* goipname,
    char* password,
    int goip_id,
    int prov_id,
    char* host,
    int port,
    int type,
    char* card,
    int card_id,
    char* recharge_ok_r,
    char* recharge_ok_r2,
    int group_id,
    int auto_disconnect_after_bal,
    char*,
    char*,
    int delay,
    int auto_reset_remain_enable,
    int auto_id);
void add_bal_cmd(int prov_id, int goip_id);
void add_recharge_sms(char* msg,
    char* tel,
    int goip_id,
    int prov_id,
    int card_id,
    char* card,
    int auto_reset_remain_enable,
    int auto_id);
static void
get_num(int goip_id, int prov_id, char* recv);

#define SET_CARD_UNUSED(card_id, usedflag)                    \
    {                                                         \
        char sqlbuf[512];                                     \
        sprintf(sqlbuf,                                       \
            "update recharge_card set used='%d' where id=%d", \
            usedflag,                                         \
            card_id);                                         \
        db_query(sqlbuf);                                     \
    }

void ussd_save(int ok,
    int type,
    char* cmd,
    char* msg,
    char* goipname,
    char* card,
    int recharge_ok)
{
    char recvmsg[512] = { 0 };
    int j = 0;
    char* p = msg;
    char sql[512];

    while (*p) {
        if (*p == '\'' || *p == '\\') {
            recvmsg[j++] = '\\';
        }
        recvmsg[j++] = *p++;
    }
    snprintf(sql,
        512,
        "insert into USSD set TERMID='%s', USSD_MSG='%s', %s='%s', "
        "type='%d', INSERTTIME=now(), card='%s', recharge_ok='%d'",
        goipname,
        cmd,
        ok ? "USSD_RETURN" : "ERROR_MSG",
        recvmsg,
        type,
        card,
        recharge_ok);
    db_query(sql);
}

void auto_ussd_free(struct auto_ussd* tmp)
{
    struct auto_ussd* now = auto_ussd_head;
    if (!tmp)
        return;

    if (tmp == now) {
        auto_ussd_head = tmp->next;
        free(tmp);
        return;
    }
    while (now->next) {
        if (tmp == now->next) {
            now->next = now->next->next;
            break;
        }
        now = now->next;
    }

    free(tmp);
}

int ussd_send(struct auto_ussd* tmp)
{
    DLOG("$$$$$$$\n%s ussd send:%s\n$$$$$$", tmp->goipname, tmp->send_buf);
    if (tmp->send_count-- <= 0) {
        if (tmp->ussd_type == TYPE_RECHARGE)
            SET_CARD_UNUSED(tmp->card_id, 0);
        ussd_save(0,
            tmp->ussd_type,
            tmp->cmd,
            "ERROR term no response",
            tmp->goipname,
            tmp->card,
            0);
        auto_ussd_free(tmp);
        return -2;
    }
    if (sendto(tmp->sock,
            tmp->send_buf,
            tmp->send_len,
            0,
            (struct sockaddr*)&tmp->addr,
            addrlen)
        < 0) {
        if (tmp->ussd_type == TYPE_RECHARGE)
            SET_CARD_UNUSED(tmp->card_id, 0);
        printf("sendto error\n");
        ussd_save(
            0, tmp->ussd_type, tmp->cmd, "sendto error", tmp->goipname, tmp->card, 0);
        auto_ussd_free(tmp);
        return -1;
    }
    return 0;
}

int do_recharge(int recharge_type,
    int goip_id,
    int prov_id,
    int send_goip_id,
    char* cmd_b,
    char* cmd_p,
    char* recharge_ok_r,
    char* recharge_ok_r2,
    char* recharge_sms_num,
    int auto_reset_remain_enable,
    int auto_id)
{
    DB_ROW row;
    DB_RES* res;
    char sql[512];
    char cmd[160];
    // char *p=cmd_b;
    if (cmd_b == NULL) {
        DLOG("no cmd");
        return -1;
    }
    if (recharge_type == TYPE_RECHARGE_OTHER && send_goip_id == 0) {
        DLOG("no send goip id");
        return -1;
    }
    if (recharge_type == TYPE_RECHARGE_SELF || recharge_type == TYPE_RECHARGR_SMS) {
        if (recharge_type == TYPE_RECHARGE_SELF) {
            /*leo*/
            res = db_query_store_result(
                "select id from goip where voip_state='IDLE' and id='%d'", goip_id);
            // res=db_query_store_result("select id from goip where id='%d'",
            // goip_id);
            if (db_num_rows(res) < 1) {
                db_free_result(res);
                DLOG("not find goip:%d or busy!", goip_id);
                return -1;
            }
            db_free_result(res);
            res = 0;
        }
        sprintf(sql,
            "select "
            "recharge_card.card,prov_id,goip.name,host,port,password,recharge_"
            "card.id as cid,group_id from recharge_card,goip where "
            "goip.provider=recharge_card.prov_id and used=0 and goip.id='%d' "
            "order by use_time, recharge_card.id limit 1",
            goip_id);
        res = db_query_store_result(sql);
        if (db_num_rows(res) < 1) {
            db_free_result(res);
            DLOG("ERROR, can not find recharge card for goip_id:%d", goip_id);
            return -1;
        }
        if ((row = db_fetch_row(res)) != NULL) {
            // db_free_result(res);
            sprintf(sql,
                "update recharge_card set used=2, goipid='%d',use_time=now() "
                "where id=%s",
                goip_id,
                row[6]);
            db_query(sql);
            /**/
            snprintf(cmd, 160, "%s%s%s", cmd_b, row[0], cmd_p);
            // add_recharge_sms(char *msg, char *tel, int goip_id, int prov_id, int
            // card_id, char *card)
            if (recharge_type == TYPE_RECHARGR_SMS)
                add_recharge_sms(cmd,
                    recharge_sms_num,
                    goip_id,
                    prov_id,
                    atoi(row[6]),
                    row[0],
                    atoi(row[8]),
                    0);
            else
                auto_ussd_calloc(cmd,
                    row[2],
                    row[5],
                    goip_id,
                    atoi(row[1]),
                    row[3],
                    atoi(row[4]),
                    TYPE_RECHARGE,
                    row[0],
                    atoi(row[6]),
                    recharge_ok_r,
                    recharge_ok_r2,
                    atoi(row[7]),
                    0,
                    NULL,
                    NULL,
                    0,
                    auto_reset_remain_enable,
                    0);
            // return ;
        }
        db_free_result(res);
    } else if (recharge_type == TYPE_RECHARGE_OTHER) {
        if (send_goip_id == 0) {
            DLOG("no send goip id");
            return -1;
        }
        sprintf(sql,
            "select provider,name,host,port,password,group_id from goip where "
            "id='%d' limit 1",
            send_goip_id);
        res = db_query_store_result(sql);
        if (db_num_rows(res) < 1) {
            db_free_result(res);
            DLOG("ERROR, can not find the recharge goip:%d", send_goip_id);
            return -1;
        }
        if ((row = db_fetch_row(res)) != NULL) {
            DB_ROW row1;
            DB_RES* res1;
            sprintf(
                sql, "select num,group_id from goip where id='%d' limit 1", goip_id);
            res1 = db_query_store_result(sql);
            if (db_num_rows(res) < 1) {
                db_free_result(res);
                db_free_result(res1);
                DLOG("ERROR, can not find goip_id:%d", goip_id);
                return -1;
            }
            if ((row1 = db_fetch_row(res1)) != NULL) {
                struct auto_ussd* tmp = auto_ussd_head;
                int find_flag = 0;
                while (tmp) {
                    if (tmp->ussd_type == TYPE_2RECHARGE && tmp->be_goip_id == goip_id) {
                        find_flag = 1;
                        break;
                    }
                    tmp = tmp->next;
                }
                if (!find_flag) {
                    snprintf(cmd, 160, "%s%s%s", cmd_b, row1[0], cmd_p);
                    tmp = auto_ussd_calloc(cmd,
                        row[1],
                        row[4],
                        send_goip_id,
                        prov_id,
                        row[2],
                        atoi(row[3]),
                        TYPE_2RECHARGE,
                        "",
                        0,
                        recharge_ok_r,
                        recharge_ok_r2,
                        atoi(row1[1]),
                        0,
                        NULL,
                        NULL,
                        0,
                        0,
                        0);
                    if (tmp)
                        tmp->be_goip_id = goip_id, tmp->be_prov_id = prov_id;
                }
            }
            db_free_result(res1);
            // return ;
        }
        db_free_result(res);
    }

    return 0;
}

void add_sms0(char* msg, char* tel, int goip_id, int prov_id, int delay, int resend)
{
    char sql[512];
    if (!msg || !tel || goip_id == 0 || !msg[0] || !tel[0])
        return;
    sprintf(sql,
        "insert into message (type,msg,crontime,tel,goipid,prov, resend) "
        "VALUES (%d, '%s',%ld,'%s','%d','%d','%d')",
        4,
        msg,
        time(NULL) + delay,
        tel,
        goip_id,
        prov_id,
        resend);
    db_query(sql);
}

void add_sms(char* msg, char* tel, int goip_id, int prov_id, int delay)
{
    add_sms0(msg, tel, goip_id, prov_id, delay, 0);
}
void add_recharge_sms(char* msg,
    char* tel,
    int goip_id,
    int prov_id,
    int card_id,
    char* card,
    int auto_reset_remain_enable,
    int auto_id)
{
    char sql[512];
    struct auto_recharge_sms* tmp;
    if (!msg || !tel || goip_id == 0 || !msg[0] || !tel[0])
        return;
    sprintf(
        sql,
        "insert into message (type,msg,crontime,tel,goipid,prov,card_id,card) "
        "VALUES (%d, '%s',%ld,'%s','%d','%d','%d','%s')",
        4,
        msg,
        time(NULL),
        tel,
        goip_id,
        prov_id,
        card_id,
        card);
    db_query(sql);
    tmp = calloc(sizeof(struct auto_recharge_sms), 1);
    tmp->card_id = card_id;
    tmp->goip_id = goip_id;
    tmp->auto_reset_remain_enable = auto_reset_remain_enable;
    tmp->next = auto_recharge_sms_head;
}

void recharge_sms_start(int card_id)
{
    struct auto_recharge_sms* tmp = auto_recharge_sms_head;
    while (tmp) {
        if (tmp->card_id == card_id) {
            tmp->send_time = time(NULL);
        }
        tmp = tmp->next;
    }
}

float get_bal(char* bal_str)
{
#ifdef _FRANCE
    char bal_buf[1024] = { 0 };
    char *p = bal_str, *bufp = bal_buf;
    float bal = 0;
    while ((*p >= '0' && *p <= '9') || *p == ',' || *p == '.') {
        printf("a:%c\n", *p);
        if (*p >= '0' && *p <= '9') {
            *bufp++ = *p;
        } else if (*p == ',') {
            *bufp++ = '.';
        } else if (*p == '.') {
        } else
            break;
        p++;
    }
    sscanf(bal_buf, "%f", &bal);
#else
    float bal = 0;
    sscanf(bal_str, "%f", &bal);
#endif
    return bal;
}

int check_bal_and_recharge(struct auto_recharge* now,
    char* bal_msg,
    char* p,
    int goip_id,
    char* goipname,
    int prov_id,
    int need_ussd2)
{
    char *p2, sql[512];
    float bal = 0;
    float limit;
    struct goipkeepalive* ka = kahead;
    while (ka) {
        if (ka->sqlid == goip_id)
            break;
        ka = ka->next;
    }

    DLOG("need:%d", need_ussd2);
    if (need_ussd2)
        limit = now->recharge_limit;
    else
        limit = now->bal_limit;
    if ((*p >= '0' && *p <= '9' && !strcmp(bal_msg, "%")) || ((p2 = strstr(p, bal_msg)) != NULL) || (*bal_msg == '%' && *(bal_msg + 1) != 0 && (p2 = strstr(p, bal_msg + 1)) != NULL)) {
        DLOG("bal_msg:%s", bal_msg);
        if (*p >= '0' && *p <= '9' && !strcmp(bal_msg, "%"))
            sscanf(p, "%f", &bal);
        else if ((p2 = strstr(p, bal_msg)) != NULL) {
            p2 += strlen(bal_msg);
            while (*p2 == ' ')
                p2++;
            bal = get_bal(p2);
        } else {
            p2 = strstr(p, bal_msg + 1);
            p2--;
            while (*p == ' ')
                p2--;
            while ((*p2 >= '0' && *p2 <= '9') || *p2 == '.')
                p2--;
            p2++;
            if (p2 > p)
                bal = get_bal(p2);
        }
        sprintf(sql,
            "update goip set bal='%f', bal_time=now() where id=%d",
            bal,
            goip_id);
        db_query(sql);
        DLOG("Balance of SIM is %f, the limit %d", bal, limit);
        if (bal <= limit) {
            /* do recharge and send sms */
            char msg[160], num[128] = { 0 }, *p = num, *pp;
            sprintf(msg,
                "ID:%s, balance of SIM(%s) is %f, less than the limit %f.(From "
                "SMS Server)",
                goipname,
                ka ? ka->num : "",
                bal,
                limit);
            strncpy(num, now->send_sms, sizeof(num));
            while ((pp = strchr(p, ',')) != NULL) {
                *pp = 0;
                if (p != pp)
                    add_sms(msg,
                        p,
                        now->send_sms_goip ? now->send_sms_goip : goip_id,
                        prov_id,
                        0); // not ,,
                p = pp + 1;
            }
            if (strlen(p))
                add_sms(msg,
                    p,
                    now->send_sms_goip ? now->send_sms_goip : goip_id,
                    prov_id,
                    0);
            send_mail(now->send_email, "Low Balance", msg);
            if (now->disable_line) {
                background_cmd_calloc(goip_id, "disable_sim", NULL, 10);
                return 0;
            }
            now->try_count = 2;
            if (!do_recharge(now->recharge_type,
                    goip_id,
                    prov_id,
                    now->send_goip_id,
                    now->recharge_cmd_b,
                    now->recharge_cmd_p,
                    now->recharge_ok_r,
                    now->recharge_ok_r2,
                    now->recharge_sms_num,
                    now->auto_reset_remain_enable,
                    0)) {
                if (now->disable_callout_when_bal)
                    background_cmd_calloc(goip_id, "callout_ctl", "0", 10);
            }
            return 0;
        } else {
            if (need_ussd2 && now->ussd2 && now->ussd2[0] && ka && ka->host[0] && ka->port) {
                auto_ussd_calloc(now->ussd2,
                    goipname,
                    ka->password,
                    goip_id,
                    now->prov_id,
                    ka->host,
                    ka->port,
                    TYPE_2USSD,
                    "",
                    0,
                    "",
                    "",
                    now->group_id,
                    0,
                    NULL,
                    NULL,
                    0,
                    0,
                    0);
            } else if (now->disable_callout_when_bal)
                background_cmd_calloc(goip_id, "callout_ctl", "1", 0);
            return 0;
        }
    }
    return -1;
}

int check_return_msg(char* recv,
    int type,
    int goip_id,
    int prov_id,
    char* goipname,
    int group_id,
    int need_ussd2)
/* type={TYPE_USSD=0, TYPE_SMS=1}*/
{
    char *p = recv, *p2;
    struct auto_recharge* now = auto_recharge_head;
    char sql[512];
    while (now) {
        DLOG("%s %s, type=%d type=%d now->group_id:%d group_id:%d",
            recv,
            now->bal_msg,
            type,
            now->type,
            now->group_id,
            group_id);
        if (type == now->type && prov_id == now->prov_id && (now->group_id == 0 || group_id == now->group_id)) {
            p = recv;
            // while(*p) {
            DLOG("%s %s %d %s",
                p,
                now->bal_msg,
                (*p >= '0' && *p <= '9' && !strcmp(now->bal_msg, "%")),
                now->bal_zero_msg);
            if (now->bal_zero_msg[0] && (p2 = strstr(p, now->bal_zero_msg)) != NULL) {
                /* do recharge and send sms */
                char msg[160], num[128] = { 0 }, *p = num, *pp;
                struct goipkeepalive* ka = kahead;
                while (ka) {
                    if (ka->sqlid == goip_id)
                        break;
                    ka = ka->next;
                }

                DLOG("Balance is 0");
                sprintf(
                    sql, "update goip set bal='0', bal_time=now() where id=%d", goip_id);
                db_query(sql);
                sprintf(msg,
                    "ID:%s, balance of SIM(%s) is %d, less than the limit %f.(From "
                    "SMS Server)",
                    goipname,
                    ka ? ka->num : "",
                    0,
                    now->bal_limit);
                strncpy(num, now->send_sms, sizeof(num));
                while ((pp = strchr(p, ',')) != NULL) {
                    *pp = 0;
                    if (p != pp)
                        add_sms(msg,
                            p,
                            now->send_sms_goip ? now->send_sms_goip : goip_id,
                            prov_id,
                            0); // not ,,
                    p = pp + 1;
                }
                if (strlen(p))
                    add_sms(msg,
                        p,
                        now->send_sms_goip ? now->send_sms_goip : goip_id,
                        prov_id,
                        0);
                send_mail(now->send_email, "Low Balance", msg);
                if (now->disable_line) {
                    background_cmd_calloc(goip_id, "disable_sim", NULL, 10);
                    return 0;
                }
                now->try_count = 2;
                do_recharge(now->recharge_type,
                    goip_id,
                    prov_id,
                    now->send_goip_id,
                    now->recharge_cmd_b,
                    now->recharge_cmd_p,
                    now->recharge_ok_r,
                    now->recharge_ok_r2,
                    now->recharge_sms_num,
                    now->auto_reset_remain_enable,
                    0);
                if (now->disable_callout_when_bal)
                    background_cmd_calloc(goip_id, "callout_ctl", "0", 10);
                return 0;
            } else {
                char bal_msg[161];
                char *bal_p1 = now->bal_msg, *bal_p2 = bal_msg;
                while (*bal_p1 != 0) {
                    while (*bal_p1 != '|' && *bal_p1 != 0)
                        *bal_p2++ = *bal_p1++;
                    *bal_p2 = 0;
                    if (bal_p2 == bal_msg) {
                        bal_p1++;
                        continue;
                    }
                    if (!check_bal_and_recharge(
                            now, bal_msg, p, goip_id, goipname, prov_id, need_ussd2)) {
                        /*
        if(type==TYPE_USSD && now->auto_disconnect_after_bal) {
                struct goipkeepalive *ka=kahead;
                while(ka){
                        if(ka->sqlid==goip_id) break;
                        ka=ka->next;
                }
                if(ka && ka->host[0] && ka->port)
                        printf("qqqqqqqqqq\n");
                        auto_ussd_calloc("USSDEXIT", goipname,
   ka->password, goip_id, prov_id, ka->host, ka->port, TYPE_NOR, "",
   0, "", "", group_id);
        }
*/
                        return 0;
                    }
                    if (bal_p1 == 0)
                        break;
                    else
                        bal_p1++;
                    bal_p2 = bal_msg;
                }
            }
            // p++;
            //}
        }
        now = now->next;
    }
    DLOG("not find!\n");
    return -1;
}

void del_recharge_sms(int card_id)
{
    struct auto_recharge_sms* tmp = auto_recharge_sms_head;
    if (!tmp)
        return;
    if (tmp->card_id == card_id) {
        auto_recharge_sms_head = tmp->next;
        free(tmp);
    } else {
        while (tmp->next) {
            if (tmp->next->card_id == card_id) {
                struct auto_recharge_sms* tmp2 = tmp->next;
                tmp->next = tmp->next->next;
                free(tmp2);
                return;
            }
            tmp = tmp->next;
        }
    }
}

void sms_recharge_return_check(int message_id, int isok)
{
    DB_ROW row;
    char buf[512];
    sprintf(
        buf,
        "select card_id,goipid,prov from message where id='%d' and card_id!=''",
        message_id);
    DB_RES* res = db_query_store_result(buf);
    if ((row = db_fetch_row(res)) != NULL) {
        int card_id = atoi(row[0]);
        struct auto_recharge_sms* tmp = auto_recharge_sms_head;
        if (isok) {
            SET_CARD_UNUSED(card_id, 1);
            add_bal_cmd(atoi(row[2]), atoi(row[1]));
            // background_cmd_calloc(atoi(row[1]), "reset_remain_time", NULL, 0);
        } else {
            SET_CARD_UNUSED(card_id, 0);
        }
        while (tmp) {
            if (tmp->card_id == card_id) {
            }
            tmp = tmp->next;
        }
    }
}

void update_next_fixed_time(int id, char* fixed_time)
{
    // select ADDTIME(ADDDATE(TIMESTAMP(now()),1),"240000");
    DLOG("");
    if (strlen(fixed_time) == 8) {
        time_t now_time = time(NULL), next_time = 0;
        struct tm* tmp = localtime(&now_time);
        int d, h, m, s;
        sscanf(fixed_time, "%2d%2d%2d%2d", &d, &h, &m, &s);
        DLOG("%d,%d,%d,%d", d, h, m, s);
        tmp->tm_mday = d, tmp->tm_hour = h, tmp->tm_min = m, tmp->tm_sec = s;
        next_time = mktime(tmp);
        if (next_time <= now_time) {
            tmp->tm_mon++;
            next_time = mktime(tmp);
        }

        db_query(
            "update auto_ussd set fixed_next_time='%d' where id='%d'", next_time, id);
    } else {
        db_query(
            "update auto_ussd set fixed_next_time=if(ADDTIME(TIMESTAMP(CURDATE()), "
            "\"%s\")>now(), UNIX_TIMESTAMP(ADDTIME(TIMESTAMP(CURDATE()), \"%s\")), "
            "UNIX_TIMESTAMP(ADDTIME(TIMESTAMP(CURDATE(), 240000), \"%s\"))) where "
            "id='%d'",
            fixed_time,
            fixed_time,
            fixed_time,
            id);
    }
}

void ussd_return_check(char* buf)
{
    int send_id, ussd_ok = 1, goip_id;
    char recv[320] = { 0 }, *p = recv;
    struct auto_ussd *now = auto_ussd_head, *tmp = NULL;
    sscanf(buf, "%*[^ ] %d %320c", &send_id, recv);
    // printf("recv ussd:%d %s", send_id, recv);
    p = p + strlen(p) - 1;
    while (*p == '@')
        *p-- = 0;
    if (!strncmp(recv, "USSD send failed!", strlen("USSD send failed!")) || !strncmp(recv, "USSDERROR", strlen("USSDERROR")))
        ussd_ok = 0;

    DLOG("11111111");
    while (now) {
        if (send_id == now->send_id) {
            goip_id = now->goip_id;
            struct goipkeepalive* ka = kahead;
            while (ka) {
                if (ka->sqlid == goip_id)
                    break;
                ka = ka->next;
            }
            struct auto_recharge* now_recharge = auto_recharge_head;
            DLOG("22222222222");
            while (now_recharge) {
                if (now_recharge->prov_id == now->prov_id && now_recharge->group_id == now->group_id)
                    break;
                now_recharge = now_recharge->next;
            }
            DLOG("3333333333");
            // if(!now_recharge) {
            // ussd_save(ussd_ok, now->ussd_type, now->cmd, recv, now->goipname,
            // now->card, 0); return;
            //}
            // DLOG("get sendid:%d, type:%d, re:%s, step re2:%s, step2 cmd:%s",
            // send_id, now->ussd_type, now_recharge->recharge_ok_r,
            // now_recharge->re_step2_ok_r, now_recharge->re_step2_cmd);
            /*save sql */
            if ((now->ussd_type == TYPE_RECHARGE || now->ussd_type == TYPE_2RECHARGE) && (ussd_ok == 0 || (now_recharge->re_step2_cmd[0] && !strstr(recv, now_recharge->re_step2_ok_r)) || (!now_recharge->re_step2_cmd[0] && !strstr(recv, now_recharge->recharge_ok_r) && (now_recharge->recharge_ok_r2[0] == 0 || !strstr(recv, now_recharge->recharge_ok_r2))))) {
                if (now->ussd_type == TYPE_RECHARGE)
                    SET_CARD_UNUSED(now->card_id, 0);
                ussd_save(
                    ussd_ok, now->ussd_type, now->cmd, recv, now->goipname, now->card, 0);
                DLOG("%p %d %s %s %p",
                    now_recharge,
                    now_recharge->try_count,
                    now_recharge->re_step2_cmd,
                    now_recharge->re_step2_ok_r,
                    strstr(recv, now_recharge->re_step2_ok_r));
                if (now_recharge && --now_recharge->try_count > 0 && now->ussd_type == TYPE_RECHARGE)
                    do_recharge(now_recharge->recharge_type,
                        goip_id,
                        now_recharge->prov_id,
                        now_recharge->send_goip_id,
                        now_recharge->recharge_cmd_b,
                        now_recharge->recharge_cmd_p,
                        now_recharge->recharge_ok_r,
                        now->recharge_ok_r2,
                        now_recharge->recharge_sms_num,
                        now_recharge->auto_reset_remain_enable,
                        0);
                // do_recharge(int recharge_type, int goip_id, int prov_id, int
                // send_goip_id, char *cmd_b, char *cmd_p, char *recharge_ok_r, char
                // *recharge_ok_r2, char *recharge_sms_num);
            } else if ((now->ussd_type == TYPE_RECHARGE || now->ussd_type == TYPE_2RECHARGE) && now_recharge->re_step2_cmd[0] && strstr(recv, now_recharge->re_step2_ok_r)) { // need step2
                if (ka && ka->host[0] && ka->port) {
                    // now->ussd_type = TYPE_RE_STEP2;
                    auto_ussd_calloc(now_recharge->re_step2_cmd,
                        now->goipname,
                        ka->password,
                        goip_id,
                        now->prov_id,
                        ka->host,
                        ka->port,
                        (now->ussd_type == TYPE_RECHARGE)
                            ? TYPE_RE_STEP2
                            : TYPE_RE_OTHE_STEP2,
                        now->card,
                        now->card_id,
                        "",
                        "",
                        now->group_id,
                        0,
                        NULL,
                        NULL,
                        0,
                        now->auto_reset_remain_enable,
                        0);
                }
                ussd_save(
                    ussd_ok, now->ussd_type, now->cmd, recv, now->goipname, now->card, 1);
                DLOG("recharge step 1 ok, send step 2");
            } else if ((now->ussd_type == TYPE_RECHARGE || now->ussd_type == TYPE_2RECHARGE)) { // recharge ok!  need
                // check balance again
                int goip_id, prov_id;
                if (now->ussd_type == TYPE_RECHARGE)
                    SET_CARD_UNUSED(now->card_id, 1);
                ussd_save(
                    ussd_ok, now->ussd_type, now->cmd, recv, now->goipname, now->card, 1);
                if (now->ussd_type == TYPE_RECHARGE)
                    goip_id = now->goip_id, prov_id = now->prov_id;
                else
                    goip_id = now->be_goip_id, prov_id = now->be_prov_id;
                if (now_recharge->type == TYPE_SMS || now_recharge->type == TYPE_USSD)
                    add_bal_cmd(prov_id, goip_id);
                DLOG("type:%d, reset:%d",
                    now_recharge->type,
                    now->auto_reset_remain_enable);
                if (now_recharge->remain_set) {
                    char value[32] = { 0 };
                    sprintf(value, "%d", now_recharge->remain_set);
                    background_cmd_calloc(goip_id, "set_exp_time", value, 0);
                    background_cmd_calloc(goip_id, "reset_remain_time", NULL, 0);
                } else if (now->auto_reset_remain_enable)
                    background_cmd_calloc(goip_id, "reset_remain_time", NULL, 0);
                // if(now_recharge->type==TYPE_FIXED_TIME){
                // update_next_fixed_time(now_recharge->id, now_recharge->fixed_time);
                //}

                /*
if(now->ussd_type == TYPE_RECHARGE) {
        SET_CARD_UNUSED(now->card_id, 1);
        ussd_save(ussd_ok, now->ussd_type, now->cmd, recv,
now->goipname, now->card, 1); if(now_recharge->type==TYPE_SMS ||
now_recharge->type==TYPE_USSD) add_bal_cmd(now->prov_id, now->goip_id);
        if(now->auto_reset_remain_enable)
background_cmd_calloc(now->goip_id, "reset_remain_time", NULL, 0);
        if(now_recharge->remain_set) {
                char value[32]={0};
                sprintf(value, "%d", now_recharge->remain_set);
                background_cmd_calloc(now->goip_id, "set_exp_time",
value, 0);
        }
        if(now_recharge->type==TYPE_FIXED_TIME){
                update_next_fixed_time(now_recharge->id,
now_recharge->fixed_time);
        }
} else { //other ok
        ussd_save(ussd_ok, now->ussd_type, now->cmd, recv,
now->goipname, now->card, 1); if(now_recharge->type==TYPE_SMS ||
now_recharge->type==TYPE_USSD) add_bal_cmd(now->be_prov_id,
now->be_goip_id); if(now->auto_reset_remain_enable)
background_cmd_calloc(now->be_goip_id, "reset_remain_time", NULL, 0);
        if(now_recharge->remain_set) {
                char value[32]={0};
                sprintf(value, "%d", now_recharge->remain_set);
                background_cmd_calloc(now->goip_id, "set_exp_time",
value, 0);
        }
        if(now_recharge->type==TYPE_FIXED_TIME){
                update_next_fixed_time(now_recharge->id,
now_recharge->fixed_time);
        }

        be_goip_id;
}
*/
            } else if (now->ussd_type == TYPE_RE_STEP2 || now->ussd_type == TYPE_RE_OTHE_STEP2) {
                int goip_id, prov_id;
                if (now->ussd_type == TYPE_RECHARGE)
                    goip_id = now->goip_id, prov_id = now->prov_id;
                else
                    goip_id = now->be_goip_id, prov_id = now->be_prov_id;

                if (strstr(recv, now_recharge->recharge_ok_r) || (now_recharge->recharge_ok_r2[0] && strstr(recv, now_recharge->recharge_ok_r2))) { // recharge ok
                    // DLOG("recharge step 2 ok:%s", now_recharge->re_step2_ok_r);
                    if (now->ussd_type == TYPE_RE_STEP2)
                        SET_CARD_UNUSED(now->card_id, 1);
                    ussd_save(ussd_ok,
                        now->ussd_type,
                        now->cmd,
                        recv,
                        now->goipname,
                        now->card,
                        1);
                    if (now_recharge->type == TYPE_SMS || now_recharge->type == TYPE_USSD)
                        add_bal_cmd(prov_id, goip_id);
                    DLOG("type:%d, reset:%d",
                        now_recharge->type,
                        now->auto_reset_remain_enable);
                    if (now_recharge->remain_set) {
                        char value[32] = { 0 };
                        sprintf(value, "%d", now_recharge->remain_set);
                        background_cmd_calloc(goip_id, "set_exp_time", value, 0);
                        background_cmd_calloc(goip_id, "reset_remain_time", NULL, 0);
                    } else if (now->auto_reset_remain_enable)
                        background_cmd_calloc(goip_id, "reset_remain_time", NULL, 0);
                    // if(now_recharge->type==TYPE_FIXED_TIME){
                    //	update_next_fixed_time(now_recharge->id,
                    // now_recharge->fixed_time);
                    //}
                } else {
                    DLOG("recharge step 2 error");
                    SET_CARD_UNUSED(now->card_id, 0);
                    ussd_save(ussd_ok,
                        now->ussd_type,
                        now->cmd,
                        recv,
                        now->goipname,
                        now->card,
                        0);
                }
            } else if (now->ussd_type == TYPE_2USSD && now_recharge) {
                if (!now_recharge->ussd2_ok_match[0] || strstr(recv, now_recharge->ussd2_ok_match)) { // match
                    // add_ussd2_cmd();
                    if (ka && ka->host[0] && ka->port) {
                        auto_ussd_calloc(now_recharge->ussd22,
                            now->goipname,
                            ka->password,
                            goip_id,
                            now->prov_id,
                            ka->host,
                            ka->port,
                            TYPE_22USSD,
                            "",
                            0,
                            "",
                            "",
                            now->group_id,
                            0,
                            NULL,
                            NULL,
                            0,
                            0,
                            0);
                    }
                } else {
                    // if disable, disable line
                    char msg[160];
                    snprintf(msg, 160, "ID:%s, Secondary USSD Undone!", now->goipname);
                    add_sms(msg, now_recharge->send_sms2, goip_id, now->prov_id, 0);
                    send_mail(now_recharge->send_mail2, "Secondary USSD Undone!", msg);
                    if (now_recharge->disable_if_ussd2_undone)
                        background_cmd_calloc(goip_id, "disable_sim", NULL, 0);
                }
                ussd_save(
                    ussd_ok, now->ussd_type, now->cmd, recv, now->goipname, now->card, 0);
            } else if (now->ussd_type == TYPE_22USSD && now_recharge) {
                if (!now_recharge->ussd22_ok_match[0] || strstr(recv, now_recharge->ussd22_ok_match)) { // match
                    // enable line
                    if (now_recharge->disable_callout_when_bal)
                        background_cmd_calloc(goip_id, "callout_ctl", "1", 0);
                } else {
                    char msg[160];
                    snprintf(msg, 160, "ID:%s, Secondary USSD Undone!", now->goipname);
                    add_sms(msg, now_recharge->send_sms2, goip_id, now->prov_id, 0);
                    send_mail(now_recharge->send_mail2, "Secondary USSD Undone!", msg);
                    // if disable disable line
                    if (now_recharge->disable_if_ussd2_undone)
                        background_cmd_calloc(goip_id, "disable_sim", NULL, 0);
                }
                ussd_save(
                    ussd_ok, now->ussd_type, now->cmd, recv, now->goipname, now->card, 0);
            } else if (now->ussd_type == TYPE_GETNUM) {
                // background_cmd_calloc(goip_id, "set_num", , 0);
                if (ussd_ok)
                    get_num(goip_id, now->prov_id, recv);
                else {
                    db_query("update goip set imsi='' where id='%d'",
                        goip_id); // need check num again
                }
                ussd_save(
                    ussd_ok, now->ussd_type, now->cmd, recv, now->goipname, now->card, 0);
            } else {
                ussd_save(
                    ussd_ok, now->ussd_type, now->cmd, recv, now->goipname, now->card, 0);
            }
            if (now->ussd_type == TYPE_BALANCE || now->ussd_type == TYPE_2BALANCE || now->ussd_type == TYPE_S2BALANCE || now->ussd_type == TYPE_S3BALANCE || now->ussd_type == TYPE_S4BALANCE) {
                int get_final = 0;
                DB_ROW row;
                DB_RES* res = db_query_store_result(
                    "select "
                    "type,auto_ussd_step2_start_r,auto_ussd_step2,auto_ussd_step3_"
                    "start_r,auto_ussd_step3,auto_ussd_step4_start_r,auto_ussd_step4,"
                    "id from auto_ussd where id=%d",
                    now->auto_id);
                row = db_fetch_row(res);
                switch (now->ussd_type) {
                case TYPE_BALANCE:
                case TYPE_2BALANCE:
                    if (row[0][0] <= '1')
                        get_final = 1;
                    else if (row[1][0] && row[2][0] && strstr(recv, row[1])) {
                        auto_ussd_calloc(row[2],
                            now->goipname,
                            ka->password,
                            goip_id,
                            now->prov_id,
                            ka->host,
                            ka->port,
                            TYPE_S2BALANCE,
                            "",
                            0,
                            "",
                            "",
                            now->group_id,
                            now->auto_disconnect_after_bal,
                            NULL,
                            NULL,
                            0,
                            0,
                            atoi(row[7]));
                    }
                    break;
                case TYPE_S2BALANCE:
                    if (row[0][0] <= '2')
                        get_final = 1;
                    else if (row[3][0] && row[4][0] && strstr(recv, row[3])) {
                        auto_ussd_calloc(row[4],
                            now->goipname,
                            ka->password,
                            goip_id,
                            now->prov_id,
                            ka->host,
                            ka->port,
                            TYPE_S3BALANCE,
                            "",
                            0,
                            "",
                            "",
                            now->group_id,
                            now->auto_disconnect_after_bal,
                            NULL,
                            NULL,
                            0,
                            0,
                            atoi(row[7]));
                    }
                    break;

                case TYPE_S3BALANCE:
                    if (row[0][0] <= '3')
                        get_final = 1;
                    else if (row[5][0] && row[6][0] && strstr(recv, row[5])) {
                        auto_ussd_calloc(row[6],
                            now->goipname,
                            ka->password,
                            goip_id,
                            now->prov_id,
                            ka->host,
                            ka->port,
                            TYPE_S4BALANCE,
                            "",
                            0,
                            "",
                            "",
                            now->group_id,
                            now->auto_disconnect_after_bal,
                            NULL,
                            NULL,
                            0,
                            0,
                            atoi(row[7]));
                    }
                    break;

                case TYPE_S4BALANCE:
                    if (row[0][0] <= '4')
                        get_final = 1;
                    break;

                default:
                    break;
                }
                /*
if(now->auto_ussd_step2_start_r[0] && now->auto_ussd_step2[0]){
        if(strstr(recv, now->auto_ussd_step2_start_r)){
                DLOG("ussd step2 :%s %s", now->auto_ussd_step2_start_r,
now->auto_ussd_step2); auto_ussd_calloc(now->auto_ussd_step2,
now->goipname, ka->password, goip_id , now->prov_id, ka->host, ka->port,
now->ussd_type, "", 0, "", "",now->group_id,
now->auto_disconnect_after_bal, NULL, NULL, 0, 0);
        }
}
else  {
*/
                if (get_final == 1) {
                    if (now->auto_disconnect_after_bal && ka && ka->host[0] && ka->port)
                        auto_ussd_calloc("USSDEXIT",
                            now->goipname,
                            ka->password,
                            goip_id,
                            now->prov_id,
                            ka->host,
                            ka->port,
                            TYPE_NOR,
                            "",
                            0,
                            "",
                            "",
                            now->group_id,
                            0,
                            NULL,
                            NULL,
                            0,
                            0,
                            0);

                    check_return_msg(recv,
                        TYPE_USSD,
                        goip_id,
                        now->prov_id,
                        now->goipname,
                        now->group_id,
                        now->ussd_type == TYPE_2BALANCE ? 1 : 0);
                }
            }
            auto_ussd_free(now);
            return;
        }
        tmp = now;
        now = now->next;
    }
}

void auto_recharge_init()
{
    char *p, *recharge_cmd = NULL;
    struct auto_recharge *now = auto_recharge_head, *tmp;
    int re_step2_enable = 0; // re_sms_step2_enable=0;
    DB_ROW row;
    DB_RES* res = db_query_store_result(
        "select "
        "prov_id,bal_sms_r,bal_ussd_r,bal_limit,recharge_ussd,send_sms,recharge_"
        "type,recharge_ussd1_goip,recharge_ok_r,recharge_ok_r2,bal_ussd_zero_"
        "match_char,bal_sms_zero_match_char,disable_if_low_bal,group_id,disable_"
        "callout_when_bal,ussd2,ussd2_ok_match,ussd22,ussd22_ok_match,send_mail2,"
        "disable_if_ussd2_undone,recharge_limit,send_email,send_sms2,recharge_"
        "sms_num,recharge_sms_msg,sms_report_goip,re_step2_enable,re_step2_cmd,"
        "re_step2_ok_r,auto_reset_remain_enable,recharge_con_type,fixed_time,"
        "remain_limit,remain_set,id from auto_ussd ");
    while (now) {
        tmp = now->next;
        free(now);
        now = tmp;
    }
    auto_recharge_head = NULL;
    while ((row = db_fetch_row(res)) != NULL) {
        // if(!row[1] && !row[2]) continue;
        DLOG("1111111");
        if ((row[2] && row[3] && row[2][0] && row[3][0]) || (row[10] && row[10][0]) || (row[1] && row[1][0] && row[3] && row[3][0]) || (row[11] && row[11][0]) || (row[31] && atoi(row[31]))) {
            tmp = calloc(sizeof(struct auto_recharge), 1);
            tmp->id = atoi(row[35]);
            if (atoi(row[31]) > 0) {
                tmp->type = atoi(row[31]); // 0,1,2,3,4
                snprintf(tmp->fixed_time, 10, "%s", row[32]);
                tmp->remain_limit = atoi(row[33]);
            } else if ((row[2] && row[3] && row[2][0] && row[3][0]) || (row[10] && row[10][0])) {
                tmp->type = TYPE_USSD;
                strncpy(tmp->bal_msg, row[2], sizeof(tmp->bal_msg) - 1);
                DLOG("a recharge plan, balance type=USSD, chars:%s, limit:%s, zero "
                     "chars:%s, re:%s, re2:%s",
                    row[2],
                    row[3],
                    row[10],
                    row[8],
                    row[9]);
                strncpy(tmp->bal_zero_msg, row[10], sizeof(tmp->bal_zero_msg) - 1);
            } else if ((row[1] && row[1][0] && row[3] && row[3][0]) || (row[11] && row[11][0])) {
                tmp->type = TYPE_SMS;
                strncpy(tmp->bal_msg, row[1], sizeof(tmp->bal_msg) - 1);
                DLOG("a recharge plan, balance type=SMS, chars:%s, limit:%s, zero "
                     "chars:%s",
                    row[1],
                    row[3],
                    row[11]);
                strncpy(tmp->bal_zero_msg, row[11], sizeof(tmp->bal_zero_msg) - 1);
            } else {
                // int d=0,h=0,m=0,s=0;
                tmp->type = atoi(row[31]); // 0,1,2,3,4
                // strncpy(tmp->fixed_time, row[32], sizeof(tmp->fixed_time)-1);
                // sscanf(row[32], "%2d%2d%2d", &h,&m,&s);
                snprintf(tmp->fixed_time, 10, "%s", row[32]);
                // tmp->fixed_time=3600*24;

                // set_recharge_fixed_time();
                tmp->remain_limit = atoi(row[33]);
            }
            tmp->prov_id = atoi(row[0]);
            tmp->group_id = atoi(row[13]);
            tmp->msg_len = strlen(tmp->bal_msg);
            tmp->bal_limit = atof(row[3]);
            tmp->recharge_type = atoi(row[6]);
            tmp->send_goip_id = atoi(row[7]);
            strncpy(tmp->recharge_ok_r, row[8], sizeof(tmp->recharge_ok_r) - 1);
            strncpy(tmp->recharge_ok_r2, row[9], sizeof(tmp->recharge_ok_r) - 1);
            tmp->disable_line = atoi(row[12]);
            tmp->disable_callout_when_bal = atoi(row[14]);
            strncpy(tmp->ussd2, row[15], sizeof(tmp->ussd2) - 1);
            strncpy(tmp->ussd2_ok_match, row[16], sizeof(tmp->ussd2_ok_match) - 1);
            strncpy(tmp->ussd22, row[17], sizeof(tmp->ussd22) - 1);
            strncpy(tmp->ussd22_ok_match, row[18], sizeof(tmp->ussd22_ok_match) - 1);
            strncpy(tmp->send_mail2, row[19], sizeof(tmp->send_mail2) - 1);
            tmp->disable_if_ussd2_undone = atoi(row[20]);
            tmp->recharge_limit = atoi(row[21]);
            strncpy(tmp->send_email, row[22], sizeof(tmp->send_email) - 1);
            strncpy(tmp->send_sms2, row[23], sizeof(tmp->send_sms2) - 1);
            strncpy(
                tmp->recharge_sms_num, row[24], sizeof(tmp->recharge_sms_num) - 1);
            if (tmp->recharge_type == TYPE_RECHARGR_SMS)
                recharge_cmd = row[25];
            else
                recharge_cmd = row[4];
            tmp->send_sms_goip = atoi(row[26]);
            re_step2_enable = atoi(row[27]);
            if (re_step2_enable && row[28][0]) {
                DLOG("%s %s", row[28], row[29]);
                strncpy(tmp->re_step2_cmd, row[28], sizeof(tmp->re_step2_cmd));
                strncpy(tmp->re_step2_ok_r, row[29], sizeof(tmp->re_step2_ok_r));
            }
            tmp->auto_reset_remain_enable = atoi(row[30]);
            tmp->remain_set = atoi(row[34]);

            if (recharge_cmd[0]) {
                p = recharge_cmd;

                while (*p != '?' && *p != '!' && *p != 0)
                    p++;
                if (*p == '?' || *p == '!') {
                    *p++ = 0;
                    strncpy(
                        tmp->recharge_cmd_b, recharge_cmd, sizeof(tmp->recharge_cmd_b) - 1);
                    strncpy(tmp->recharge_cmd_p, p, sizeof(tmp->recharge_cmd_p) - 1);
                } else {
                    strncpy(
                        tmp->recharge_cmd_b, recharge_cmd, sizeof(tmp->recharge_cmd_b) - 1);
                }
            }
            if (row[5][0]) {
                strncpy(tmp->send_sms, row[5], sizeof(tmp->send_sms) - 1);
            }
            tmp->next = auto_recharge_head;
            auto_recharge_head = tmp;
            DLOG("%s %s %s %s, %d",
                tmp->send_sms,
                tmp->send_sms2,
                tmp->send_email,
                tmp->send_mail2,
                tmp->auto_reset_remain_enable);
        }
        /*
if((row[1] && row[1][0] && row[3] && row[3][0]) || (row[11] && row[11][0])){
        tmp=calloc(sizeof(struct auto_recharge), 1);
        tmp->type=TYPE_SMS;
        tmp->prov_id=atoi(row[0]);
        tmp->group_id=atoi(row[13]);
        strncpy(tmp->bal_msg, row[1], sizeof(tmp->bal_msg)-1);
        tmp->msg_len=strlen(tmp->bal_msg);
        DLOG("a recharge plan, balance type=SMS, chars:%s, limit:%s, zero
chars:%s", row[1], row[3], row[11]); strncpy(tmp->bal_zero_msg, row[11],
sizeof(tmp->bal_zero_msg)-1); tmp->bal_limit=atof(row[3]);
        tmp->recharge_type=atoi(row[6]);
        tmp->send_goip_id=atoi(row[7]);
        strncpy(tmp->recharge_ok_r, row[8], sizeof(tmp->recharge_ok_r)-1);
        strncpy(tmp->recharge_ok_r2, row[9], sizeof(tmp->recharge_ok_r2)-1);
        tmp->disable_line=atoi(row[12]);
        tmp->disable_callout_when_bal=atoi(row[14]);
        strncpy(tmp->ussd2, row[15], sizeof(tmp->ussd2)-1);
        strncpy(tmp->ussd2_ok_match, row[16],
sizeof(tmp->ussd2_ok_match)-1); strncpy(tmp->ussd22, row[17],
sizeof(tmp->ussd22)-1); strncpy(tmp->ussd22_ok_match, row[18],
sizeof(tmp->ussd22_ok_match)-1); strncpy(tmp->send_mail2, row[19],
sizeof(tmp->send_mail2)-1); tmp->disable_if_ussd2_undone=atoi(row[20]);
        tmp->recharge_limit=atoi(row[21]);
        strncpy(tmp->send_email, row[22], sizeof(tmp->send_email)-1);
        strncpy(tmp->send_sms2, row[23], sizeof(tmp->send_sms2)-1);
        strncpy(tmp->recharge_sms_num, row[24],
sizeof(tmp->recharge_sms_num)-1);

        if(tmp->recharge_type == TYPE_RECHARGR_SMS) recharge_cmd=row[25];
        else recharge_cmd=row[4];
        tmp->send_sms_goip=atoi(row[26]);
        re_step2_enable=atoi(row[27]);
        if(re_step2_enable && row[28][0]){
                DLOG("%s %s", row[28], row[29]);
                strncpy(tmp->re_step2_cmd, row[28],
sizeof(tmp->re_step2_cmd)); strncpy(tmp->re_step2_ok_r, row[29],
sizeof(tmp->re_step2_ok_r));
        }
        tmp->auto_reset_remain_enable=atoi(row[30]);

        if(recharge_cmd[0]){
                p=recharge_cmd;

                while(*p!='!' && *p!='?' && *p!=0) p++;
                if(*p=='!' || *p=='?'){
                        *p++=0;
                        strncpy(tmp->recharge_cmd_b, recharge_cmd,
sizeof(tmp->recharge_cmd_b)-1); strncpy(tmp->recharge_cmd_p, p,
sizeof(tmp->recharge_cmd_p)-1);
                }
                else {
                        strncpy(tmp->recharge_cmd_b, recharge_cmd,
sizeof(tmp->recharge_cmd_b)-1);
                }
        }
        if(row[5][0])
                strncpy(tmp->send_sms, row[5], sizeof(tmp->send_sms)-1);
        tmp->next=auto_recharge_head;
        auto_recharge_head=tmp;
        DLOG("%s %s %s %s", tmp->send_sms, tmp->send_sms2, tmp->send_email,
tmp->send_mail2);
}
*/
    }
    db_free_result(res);
}

void auto_ussd_init()
{
    unsigned now_time = time(NULL);
    char sql[128];
    DB_ROW row;
    DB_RES* res = db_query_store_result("select id,crontime,next_time from auto_ussd");
    if (res == NULL) {
        printf("auto ussd init error !");
        return;
    }
    srand(now_time);
    while ((row = db_fetch_row(res)) != NULL) {
        sprintf(sql,
            "update auto_ussd set next_time='%u' where id=%s and "
            "(recharge_con_type=1 or recharge_con_type=0)",
            now_time + 30 + rand() % 300,
            row[0]);
        db_query(sql);
    }
    db_query("update recharge_card set used=0 where used=2");
    db_free_result(res);
    auto_check_time = now_time;
    auto_recharge_init();
}

void auto_ussd_update()
{
    unsigned now_time = time(NULL), next_time, crontime;
    char sql[128];
    DB_ROW row;
    DB_RES* res = db_query_store_result("select id,crontime,next_time from auto_ussd");
    if (res == NULL) {
        printf("auto ussd update error !");
        return;
    }
    while ((row = db_fetch_row(res)) != NULL) {
        next_time = atoi(row[2]);
        crontime = atoi(row[1]) * 60;
        if (next_time < now_time) {
            sprintf(sql,
                "update auto_ussd set next_time='%u' where id=%s",
                now_time,
                row[0]);
            db_query(sql);
        }
    }
    db_free_result(res);

    auto_recharge_init();
}

int ussd_delay_send(struct auto_ussd* now, int delay)
{
    struct auto_ussd* tmp = auto_ussd_head;
    int goip_id = now->goip_id, count = -1;

    while (tmp) {
        if (goip_id == tmp->goip_id)
            count++;
        tmp = tmp->next;
    }

    if (delay == -1) {
        DLOG("send now");
        now->delay = 0;
        now->send_time = time(NULL);
        return ussd_send(now);
    } else {
        if (delay < 10 * count)
            delay = 10 * count;
        DLOG("wait %d seconds", delay);
        now->delay = time(NULL) + delay;
    }
    return 0;
}

struct auto_ussd*
auto_ussd_calloc(char* cmd,
    char* goipname,
    char* password,
    int goip_id,
    int prov_id,
    char* host,
    int port,
    int ussd_type,
    char* card,
    int card_id,
    char* recharge_ok_r,
    char* recharge_ok_r2,
    int group_id,
    int auto_disconnect_after_bal,
    char* auto_ussd_step2_start_r,
    char* auto_ussd_step2,
    int delay,
    int auto_reset_remain_enable,
    int auto_id)
{
    // unsigned now_time=time(NULL);
    struct auto_ussd* tmp = auto_ussd_head;
    if (!cmd || !cmd[0])
        return NULL;
    DLOG("password:%s", password);
    tmp = calloc(sizeof(struct auto_ussd), 1);
    tmp->goip_id = goip_id;
    tmp->prov_id = prov_id;
    tmp->group_id = group_id;
    tmp->card_id = card_id;
    tmp->ussd_type = ussd_type;
    tmp->auto_id = auto_id;
    if (ussd_type == TYPE_2BALANCE)
        tmp->need_ussd2 = 1;
    tmp->send_id = rand();
    ;
    DLOG("auto_id:%d,send_id:%d,reset:%d",
        auto_id,
        tmp->send_id,
        auto_reset_remain_enable);
    tmp->auto_disconnect_after_bal = auto_disconnect_after_bal;
    tmp->auto_reset_remain_enable = auto_reset_remain_enable;
    snprintf(tmp->goipname, 64, "%s", goipname);
    strncpy(tmp->cmd, cmd, sizeof(tmp->cmd) - 1);
    strncpy(tmp->card, card, sizeof(tmp->card) - 1);
    strncpy(tmp->recharge_ok_r, recharge_ok_r, sizeof(tmp->recharge_ok_r) - 1);
    strncpy(tmp->recharge_ok_r2, recharge_ok_r2, sizeof(tmp->recharge_ok_r2) - 1);
    if (auto_ussd_step2_start_r)
        strncpy(tmp->auto_ussd_step2_start_r,
            auto_ussd_step2_start_r,
            sizeof(tmp->auto_ussd_step2_start_r) - 1);
    if (auto_ussd_step2)
        strncpy(
            tmp->auto_ussd_step2, auto_ussd_step2, sizeof(tmp->auto_ussd_step2) - 1);
    if (!strcmp(cmd, "USSDEXIT")) {
        tmp->send_id = rand();
        snprintf(tmp->send_buf,
            sizeof(tmp->send_buf),
            "USSDEXIT %d %s",
            tmp->send_id,
            password);
    } else
        snprintf(tmp->send_buf,
            sizeof(tmp->send_buf),
            "USSD %d %s %s",
            tmp->send_id,
            password,
            tmp->cmd);
    tmp->send_len = strlen(tmp->send_buf);
    tmp->send_count = 3;
    tmp->addr.sin_family = AF_INET;
    if ((tmp->addr.sin_addr.s_addr = inet_addr(host)) == 0) {
        printf("inet_addr error:%m\n");
        free(tmp);
        return NULL;
    }
    tmp->addr.sin_port = htons(port);
    tmp->sock = phpsock;
    tmp->next = auto_ussd_head;
    auto_ussd_head = tmp;
    if (tmp->ussd_type == TYPE_RE_OTHE_STEP2)
        delay = -1;
    ussd_delay_send(tmp, delay);
    /*
if(ussd_send(tmp)<0){
        return -1;
}
*/
    return tmp;
}

static void ussd_delay_check(now_time)
{
    struct auto_ussd* tmp = auto_ussd_head;
    while (tmp) {
        if (tmp->delay && tmp->delay < now_time) {
            tmp->delay = 0;
            tmp->send_time = now_time;
            ussd_send(tmp);
        }
        tmp = tmp->next;
    }
}

static void ussd_timeout_check(now_time)
{
    struct auto_ussd* tmp2;
    struct auto_ussd* tmp = auto_ussd_head;

    while (tmp) {
        if (tmp->send_time && tmp->send_time + 30 < now_time) { // 30 seconds
            if (tmp->ussd_type == TYPE_RECHARGE)
                SET_CARD_UNUSED(tmp->card_id, 0);
            tmp2 = tmp->next;
            auto_ussd_free(tmp);
            tmp = tmp2;
            continue;
        }
        tmp = tmp->next;
    }
}

static void auto_recharge_sms_timeout_check(now_time)
{
    struct auto_recharge_sms* tmp = auto_recharge_sms_head;
    while (tmp) {
        if (tmp->send_time && tmp->send_time + 60 < now_time) {
            SET_CARD_UNUSED(tmp->card_id, 0);
            del_recharge_sms(tmp->card_id);
        }
        tmp = tmp->next;
    }
}

void add_bal_cmd(int prov_id, int goip_id)
{
    char sql[512];
    DB_ROW row, row2;
    DB_RES *res, *res2;
    int auto_type;

    sprintf(sql,
        "SELECT id,name,host,port,password FROM goip where provider='%d' and "
        "id='%d' and alive=1 and gsm_status!='LOGOUT'",
        prov_id,
        goip_id);
    res2 = db_query_store_result(sql);
    if ((row2 = db_fetch_row(res2)) != NULL) {
        sprintf(sql,
            "select "
            "id,crontime,prov_id,auto_ussd,type,auto_sms_num,auto_sms_msg,"
            "group_id,auto_disconnect_after_bal,auto_ussd_step2_start_r,auto_"
            "ussd_step2,bal_delay from auto_ussd where prov_id='%d'",
            prov_id);
        res = db_query_store_result(sql);
        if ((row = db_fetch_row(res)) != NULL) {
            auto_type = atoi(row[4]);
            DLOG("%d\n", auto_type);
            if (auto_type >= 2 && row[9][0] && row[10][0]) {
                DLOG("add_ussd, bal delay:%d\n", atoi(row[11]));
                auto_ussd_calloc(row[3],
                    row2[1],
                    row2[4],
                    atoi(row2[0]),
                    atoi(row[2]),
                    row2[2],
                    atoi(row2[3]),
                    TYPE_2BALANCE,
                    "",
                    0,
                    "",
                    "",
                    atoi(row[7]),
                    atoi(row[8]),
                    row[9],
                    row[10],
                    atoi(row[11]),
                    0,
                    atoi(row[0]));
            } else if (auto_type == TYPE_USSD || auto_type >= 2) {
                DLOG("add_ussd, bal delay:%d\n", atoi(row[11]));
                auto_ussd_calloc(row[3],
                    row2[1],
                    row2[4],
                    atoi(row2[0]),
                    atoi(row[2]),
                    row2[2],
                    atoi(row2[3]),
                    TYPE_2BALANCE,
                    "",
                    0,
                    "",
                    "",
                    atoi(row[7]),
                    atoi(row[8]),
                    NULL,
                    NULL,
                    atoi(row[11]),
                    0,
                    atoi(row[0]));
            } else {
                DLOG("add_sms\n");
                add_sms(row[6], row[5], atoi(row2[0]), prov_id, atoi(row[11]));
                struct goipkeepalive* ka = kahead;
                while (ka) {
                    if (ka->sqlid == goip_id)
                        break;
                    ka = ka->next;
                }
                if (ka)
                    ka->is_bal2 = 1;
            }
        }
    }
    db_free_result(res2);
}

void add_auto_ussd_cmd()
{
}

void remain_count_day_check(int now_time)
{
    static int last_day = 0;
    int this_day;
    DB_ROW row;
    DB_RES* res;
    char sql[1024];
    if (last_day == 0) {
        sprintf(sql, "select this_day from system limit 1");
        res = db_query_store_result(sql);
        if ((row = db_fetch_row(res)) != NULL) {
            last_day = atoi(row[0]);
            DLOG("this day is %d", last_day);
            db_free_result(res);
        }
    }
    this_day = now_time / 86400;
    if (last_day != this_day) {
        last_day = this_day;
        DLOG("change day:%d", last_day);
        sprintf(sql, "update goip set remain_count_d=count_limit_d WHERE 1");
        db_query(sql);
        sprintf(sql, "update system set this_day=%d where 1", last_day);
        db_query(sql);
    }
}

int auto_ussd_check()
{
    int prov_id, auto_type, group_id, next_time, fixed_next_time,
        recharge_con_type, remain_limit;
    unsigned now_time = time(NULL);
    char sql[512];
    DB_ROW row, row2;
    DB_RES *res, *res2;
    background_cmd_check(now_time);
    ussd_delay_check(now_time);
    ussd_timeout_check(now_time);
    auto_recharge_sms_timeout_check(now_time);
    remain_count_day_check(now_time);
    // struct auto_ussd *tmp=auto_ussd_head;
    printf("now_time:%u\n", now_time);
    if (auto_check_time + 62 > now_time)
        return 62;
#ifdef _FU
    /*
res=db_query_store_result("select out_user,count(id) from out_num where over=0
and used_time=0 and TIMESTAMPDIFF(SECOND,`time`,now()) >600 GROUP BY
out_user"); while((row=db_fetch_row(res))!=NULL){ db_query("update out_user
set num_used=num_used-%s where id='%s'", row[1], row[0]);
}
db_free_result(res);
//db_query("delete from out_num where used_time=0 and
TIMESTAMPDIFF(SECOND,`time`,now()) >600"); res=db_query_store_result("select
id from goip where alive=1 and num in (select num from out_num where over=0
and used_time=0 and TIMESTAMPDIFF(SECOND,`time`,now()) >600)");
while((row=db_fetch_row(res))!=NULL){
        background_cmd_calloc(atoi(row[0]), "disable_sim", NULL, 0);
}
db_free_result(res);
*/
    db_query("update goip set out_user=0,out_time=0 where out_time>0 and "
             "TIMESTAMPDIFF(SECOND,`out_time`,now()) >600");
    res = db_query_store_result(
        "select out_user,num,id from out_num where over=0 and used_time=0 and "
        "TIMESTAMPDIFF(SECOND,`time`,now()) >600 order by out_user limit 50");
    char num_buf[102400] = { 0 };
    char id_buf[102400] = { 0 };
    while ((row = db_fetch_row(res)) != NULL) {
        db_query("update out_user set num_used=num_used-1 where id='%s'", row[0]);
        if (*num_buf == 0) {
            sprintf(num_buf + strlen(num_buf), "('%s'", row[1]);
            sprintf(id_buf + strlen(id_buf), "(%s", row[2]);
        } else {
            sprintf(num_buf + strlen(num_buf), ",'%s'", row[1]);
            sprintf(id_buf + strlen(id_buf), ",%s", row[2]);
        }
    }
    db_free_result(res);
    if (*num_buf) {
        *(num_buf + strlen(num_buf)) = ')';
        *(id_buf + strlen(id_buf)) = ')';

        res = db_query_store_result(
            "select id from goip where alive=1 and num in (select num from out_num "
            "where num in %s group by num having count(num)>1)",
            num_buf);
        while ((row = db_fetch_row(res)) != NULL) {
            background_cmd_calloc(atoi(row[0]), "disable_sim", NULL, 0);
        }
        db_free_result(res);
        db_query("update out_num set over=1 where id in %s", id_buf);
    }
#endif
    sprintf(sql,
        "select "
        "id,crontime,prov_id,auto_ussd,type,auto_sms_num,auto_sms_msg,"
        "group_id,auto_disconnect_after_bal,auto_ussd_step2_start_r,"
        "auto_ussd_step2,recharge_con_type,next_time,fixed_next_time,"
        "remain_limit,fixed_time from auto_ussd ");
    res = db_query_store_result(sql);
    if (db_num_rows(res) < 1) {
        auto_check_time = now_time;
        db_free_result(res);
        return 62;
    }
    while ((row = db_fetch_row(res)) != NULL) {
        next_time = atoi(row[12]);
        fixed_next_time = atoi(row[13]);
        recharge_con_type = atoi(row[11]);
        remain_limit = atoi(row[14]);

        if (next_time > now_time && (recharge_con_type == 0 || recharge_con_type == 1))
            continue;
        if (fixed_next_time > now_time && recharge_con_type == 2)
            continue;
        prov_id = atoi(row[2]);
        group_id = atoi(row[7]);
        auto_type = atoi(row[4]);
        DLOG("G:%d,P:%d, %d, %s, %s", group_id, prov_id, auto_type, row[5], row[6]);
        sprintf(sql,
            "update auto_ussd set last_time=now(), next_time='%u' where id=%s",
            now_time + atoi(row[1]) * 60,
            row[0]); // next send time
        db_query(sql);
        if (group_id == 0)
            sprintf(sql,
                "SELECT id,name,host,port,password,remain_time FROM goip where "
                "provider='%d' and alive=1 and gsm_status!='LOGOUT'",
                prov_id);
        else
            sprintf(sql,
                "SELECT id,name,host,port,password,remain_time FROM goip where "
                "provider='%d' and group_id='%d' and alive=1 and "
                "gsm_status!='LOGOUT'",
                prov_id,
                group_id);
        res2 = db_query_store_result(sql);
        while ((row2 = db_fetch_row(res2)) != NULL) {
            DLOG("%d\n", auto_type);
            if (atoi(row[11]) == TYPE_REMAIN_LIMIT) {
                struct auto_recharge* now = auto_recharge_head;
                DLOG("");
                while (now) {
                    if (now->id == atoi(row[0]) && (atoi(row2[5]) >= 0 && atoi(row2[5]) < remain_limit)) {
                        do_recharge(now->recharge_type,
                            atoi(row2[0]),
                            prov_id,
                            now->send_goip_id,
                            now->recharge_cmd_b,
                            now->recharge_cmd_p,
                            now->recharge_ok_r,
                            now->recharge_ok_r2,
                            now->recharge_sms_num,
                            now->auto_reset_remain_enable,
                            0);
                        break;
                    }
                    now = now->next;
                }
            } else if (atoi(row[11]) == TYPE_FIXED_TIME) {
                struct auto_recharge* now = auto_recharge_head;
                DLOG("");
                while (now) {
                    if (now->id == atoi(row[0])) {
                        do_recharge(now->recharge_type,
                            atoi(row2[0]),
                            prov_id,
                            now->send_goip_id,
                            now->recharge_cmd_b,
                            now->recharge_cmd_p,
                            now->recharge_ok_r,
                            now->recharge_ok_r2,
                            now->recharge_sms_num,
                            now->auto_reset_remain_enable,
                            0);
                        update_next_fixed_time(atoi(row[0]), row[15]);
                        break;
                    }
                    now = now->next;
                }
            }

            else if (auto_type >= 2 && row[9][0] && row[10][0]) { // USSD or USSD2 USSD3 USSD4
                DLOG("add_ussd2\n");
                auto_ussd_calloc(row[3],
                    row2[1],
                    row2[4],
                    atoi(row2[0]),
                    atoi(row[2]),
                    row2[2],
                    atoi(row2[3]),
                    TYPE_BALANCE,
                    "",
                    0,
                    "",
                    "",
                    group_id,
                    atoi(row[8]),
                    row[9],
                    row[10],
                    0,
                    0,
                    atoi(row[0]));
            } else if (auto_type == TYPE_USSD || auto_type >= 2) {
                DLOG("add_ussd\n");
                auto_ussd_calloc(row[3],
                    row2[1],
                    row2[4],
                    atoi(row2[0]),
                    atoi(row[2]),
                    row2[2],
                    atoi(row2[3]),
                    TYPE_BALANCE,
                    "",
                    0,
                    "",
                    "",
                    group_id,
                    atoi(row[8]),
                    NULL,
                    NULL,
                    0,
                    0,
                    atoi(row[0]));
            } else {
                DLOG("add_sms\n");
                add_sms(row[6], row[5], atoi(row2[0]), prov_id, 0);
            }
        }
        db_free_result(res2);
    }
    db_free_result(res);
    auto_check_time = now_time;
    return 62;
}

static void
get_num(int goip_id, int prov_id, char* recv)
{
    char sql[512], gsm_num[64] = { 0 };
    char *p, *num = recv;
    DB_ROW row;
    DB_RES* res;
    sprintf(
        sql, "select num_prefix,num_postfix from prov where id='%d'", prov_id);
    res = db_query_store_result(sql);
    if ((row = db_fetch_row(res)) != NULL) {
        if (row[0][0]) {
            if ((p = strstr(recv, row[0])) == NULL)
                return; // not find prefix
            num = p + strlen(row[0]); // num start
        }
        if (row[1][0] && ((p = strstr(num + 1, row[1])) != NULL)) { // find postfix
        } else
            p = num + strlen(num);
        strncpy(gsm_num, num, p - num);
        // sprintf(sql, "update goip set num='%s' where id='%d'",gsm_num, goip_id);
        // db_query(sql);
        background_cmd_calloc(goip_id, "set_num", gsm_num, 0);
    }
    db_free_result(res);
}

int auto_get_num_start_check(char* goip_name)
{
    char sql[512];
    DB_ROW row;
    DB_RES* res;
    sprintf(
        sql,
        "SELECT "
        "goip.id,name,host,port,password,provider,auto_num_ussd,auto_num_c FROM "
        "goip left join prov on goip.provider=prov.id where name='%s' and "
        "alive=1 and gsm_status!='LOGOUT' and num='' and auto_num_ussd!=''",
        goip_name);
    res = db_query_store_result(sql);
    if ((row = db_fetch_row(res)) != NULL) {
        if (atoi(row[7]) < 5) {
            auto_ussd_calloc(row[6],
                goip_name,
                row[4],
                atoi(row[0]),
                atoi(row[5]),
                row[2],
                atoi(row[3]),
                TYPE_GETNUM,
                "",
                0,
                "",
                "",
                0,
                0,
                NULL,
                NULL,
                0,
                0,
                0);
            db_query("update goip set auto_num_c=auto_num_c+1 where name='%s'",
                goip_name);
        }
        db_free_result(res);
        return 0;
    } else
        return -1;
}
