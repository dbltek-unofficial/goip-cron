#include "report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "goipcron.h"
#include "mysql.h"
#include "send_http.h"
#include "send_mail.h"
struct report {
    time_t last_check_time;
    int gsm_logout_enable;
    int gsm_logout_time_limit;
    int reg_logout_enable;
    int reg_logout_time_limit;
    int remain_timeout_enable;
    int email_forward_sms_enable;
    char report_mail[64];
} report;

extern struct goipkeepalive* kahead;

int check_email_forward_sms(int id, char* name, char* recvnum, char* recvmsg)
{
    char title[128], content[1024], sqlbuf[1024];

    DB_ROW row;
    DB_RES* res;

    sprintf(title, "GoIP(%s) received a SMS.", name);
    // if(!report.email_forward_sms_enable)	return -2;
    sprintf(sqlbuf,
        "select report_mail,fwd_mail_enable,report_http,fwd_http_enable,num "
        "from goip where id=%d",
        id);
    res = db_query_store_result(sqlbuf);
    if (res != NULL && (row = db_fetch_row(res)) != NULL && atoi(row[1]) == 1) {
        sprintf(content,
            "GoIP(Id:%s,Number:%s) received a SMS.\nNumber:%s\nContent:%s",
            name,
            row[4],
            recvnum,
            recvmsg);
        if (row[0] && row[0][0])
            send_mail(row[0], title, content);
        else
            send_mail(report.report_mail, title, content);
    }
    if (res != NULL && row != NULL && atoi(row[3]) == 1) {
        if (row[2] && row[2][0])
            send_http(row[2], name, recvnum, recvmsg);
    }
    /*
if(res != NULL && (row=db_fetch_row(res))!=NULL && row[0] && row[0][0]){
        send_mail(row[0], title, content);
}else if(report.report_mail[0]){
        send_mail(report.report_mail, title, content);
}
*/
    db_free_result(res);
    return 0;
}

int report_check(int check_soon)
{
    struct goipkeepalive* ka = kahead;
    char title[128], content[4028];
    int count = 0;
    time_t now_time = time(NULL);

    printf("check1\n");

    if (!check_soon && report.last_check_time + 30 > now_time)
        return -1;
    printf("check2 :%d %d %d\n",
        check_soon,
        (int)report.last_check_time,
        (int)now_time);
    ;
    report.last_check_time = now_time;

    if (report.reg_logout_enable) {
        sprintf(title, "Register of GoIP LOGOUT in SMS Server");
        sprintf(
            content,
            "Register of GoIP LOGOUT in SMS Server longer than %d minutes.GoIP ID:",
            report.reg_logout_time_limit / 60);
        while (ka) {
            if (!ka->report_reg_logout && ka->lasttime && ka->lasttime + report.reg_logout_time_limit < now_time) {
                if (count > 0)
                    sprintf(content + strlen(content), ",");
                sprintf(content + strlen(content), "%s", ka->id);
                ka->report_reg_logout = 1;
                if (++count >= 100) {
                    count = 0;
                    sprintf(content + strlen(content), ".");
                    send_mail(report.report_mail, title, content);
                    sprintf(content,
                        "Register of GoIP LOGOUT in SMS Server longer than %d "
                        "minutes.GoIP ID:",
                        report.reg_logout_time_limit / 60);
                }
            }
            ka = ka->next;
        }
    }
    printf("check3\n");
    if (count) {
        sprintf(content + strlen(content), ".");
        send_mail(report.report_mail, title, content);
    }
    count = 0;
    ka = kahead;
    if (report.gsm_logout_enable) {
        sprintf(title, "GSM of GoIP LOGOUT in SMS Server");
        sprintf(content,
            "GSM of GoIP LOGOUT in SMS Server longer than %d minutes.GoIP ID:",
            report.gsm_logout_time_limit / 60);
        while (ka) {
            // printf("last time :%d, now_time:%d\n", (int)ka->gsm_login_time,
            // (int)now_time);
            if (!ka->report_gsm_logout && !ka->report_gsm_logout && ka->gsm_login_time && ka->gsm_login_time + report.gsm_logout_time_limit < now_time) {
                if (count > 0)
                    sprintf(content + strlen(content), ",");
                sprintf(content + strlen(content), "%s", ka->id);
                ka->report_gsm_logout = 1;
                if (++count >= 100) {
                    count = 0;
                    sprintf(content + strlen(content), ".");
                    send_mail(report.report_mail, title, content);
                    sprintf(content,
                        "GSM of GoIP LOGOUT in SMS Server longer than %d "
                        "minutes.GoIP ID:",
                        report.gsm_logout_time_limit / 60);
                }
            }
            ka = ka->next;
        }
    }
    if (count) {
        sprintf(content + strlen(content), ".");
        send_mail(report.report_mail, title, content);
    }
    count = 0;
    ka = kahead;
    if (report.remain_timeout_enable) {
        sprintf(title, "Remain Time Out From SMS Server");
        sprintf(content, "Remain Time Out From SMS Server.GoIP ID:");
        while (ka) {
            if (!ka->report_remain_timeout && ka->remain_time == 0) {
                if (count > 0)
                    sprintf(content + strlen(content), ",");
                sprintf(content + strlen(content), "%s", ka->id);
                ka->report_remain_timeout = 1;
                if (++count >= 100) {
                    count = 0;
                    sprintf(content + strlen(content), ".");
                    send_mail(report.report_mail, title, content);
                    sprintf(content, "Remain Time Out From SMS Server.GoIP ID:");
                }
            }
            ka = ka->next;
        }
    }
    if (count) {
        sprintf(content + strlen(content), ".");
        send_mail(report.report_mail, title, content);
    }
    return 0;
}

int report_init()
{
    unsigned now_time = time(NULL);
    // struct goipkeepalive *ka=kahead;
    DB_ROW row;
    DB_RES* res = db_query_store_result(
        "select "
        "report_mail,email_report_gsm_logout_enable,email_report_gsm_logout_time_"
        "limit,email_report_reg_logout_enable,email_report_reg_logout_time_limit,"
        "email_report_remain_timeout_enable,email_forward_sms_enable from "
        "system");
    if (res == NULL) {
        printf(" init error !");
        return -1;
    }
    if ((row = db_fetch_row(res)) != NULL) {
        strncpy(report.report_mail, row[0], sizeof(report.report_mail) - 1);
        report.last_check_time = now_time;
        report.gsm_logout_enable = atoi(row[1]);
        report.gsm_logout_time_limit = atoi(row[2]) * 60;
        report.reg_logout_enable = atoi(row[3]);
        report.reg_logout_time_limit = atoi(row[4]) * 60;
        report.remain_timeout_enable = atoi(row[5]);
        report.email_forward_sms_enable = atoi(row[6]);
        printf("report:%d %d %d %d %d %d\n",
            report.gsm_logout_enable,
            report.gsm_logout_time_limit,
            report.reg_logout_enable,
            report.reg_logout_time_limit,
            report.remain_timeout_enable,
            report.email_forward_sms_enable);
        // while(ka){
        // ka->lasttime=now_time;
        // ka->gsm_login_time=now_time;
        // ka->remain_time=-1;
        // ka=ka->next;
        //}
    }
    db_free_result(res);
    return 0;
}
