#ifndef _GOIPCRON_H
#define _GOIPCRON_H

#ifdef _WIN32
#include <winsock.h>
#else
#include <arpa/inet.h>
#endif

#define MAXTEL 14
#define MAXRECVID 5 //接收短信的缓存数量

struct goip* goiphead;
struct goipkeepalive {
    char* id;
    int sqlid;
    int prov;
    int group_id;
    char* password;
    time_t lasttime;
    time_t lastrecvtime;
    int recvid[MAXRECVID]; //接收短信的缓存
    int recvidnow; //当前接收短信在缓存中的位置
    time_t gsm_login_time;
    int report_gsm_logout;
    int report_reg_logout;
    int report_remain_timeout;
    int remain_time;
    struct sockaddr_in addr;
    int addr_len;
    char host[64];
    int port;
    char report_mail[64];
    char num[64];
    struct goipkeepalive* next;
    int is_bal2;
    int s_l_id;
};
#endif
