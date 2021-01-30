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
#include "background_cmd.h"
#include "debug.h"
#include "goipcron.h"

extern int phpsock; //接收php的sock
extern struct goipkeepalive* kahead;
struct background_cmd* background_cmd_head = NULL;

void background_cmd_free(struct background_cmd* tmp)
{
    struct background_cmd* now = background_cmd_head;
    if (!tmp)
        return;

    if (tmp == now) {
        background_cmd_head = tmp->next;
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

void background_msg_check(char* buf)
{
    struct background_cmd* tmp = background_cmd_head;
    int send_id;
    sscanf(buf, "%*[^ ] %d", &send_id);
    while (tmp) {
        if (tmp->send_id == send_id) {
            DLOG("free %d", send_id);
            background_cmd_free(tmp);
            return;
        }
        tmp = tmp->next;
    }
}

int background_cmd_send(struct background_cmd* tmp)
{
    DLOG("$$$$$$$\n%s ussd send:%s\n$$$$$$", tmp->goipname, tmp->send_buf);
    if (tmp->send_count-- <= 0) {
        background_cmd_free(tmp);
        return -2;
    }
    if (sendto(tmp->sock,
            tmp->send_buf,
            tmp->send_len,
            0,
            (struct sockaddr*)&tmp->addr,
            tmp->addr_len)
        < 0) {
        printf("sendto error\n");
        background_cmd_free(tmp);
        return -1;
    }
    tmp->send_time = time(NULL);
    tmp->delay = 5;
    return 0;
}

void background_cmd_check(now_time)
{
    struct background_cmd *tmp = background_cmd_head, *tmp2;
    while (tmp) {
        if (tmp->send_time + tmp->delay < now_time) { // time out
            tmp->send_time = now_time;
            tmp2 = tmp->next;
            background_cmd_send(tmp); // send again
            tmp = tmp2;
        } else
            tmp = tmp->next;
    }
}

int background_cmd_calloc(int goip_id, char* cmd, char* value, int delay)
{
    struct background_cmd* tmp = background_cmd_head;
    struct goipkeepalive* ka = kahead;
    while (ka) {
        if (goip_id == ka->sqlid) {
            tmp = calloc(sizeof(struct background_cmd), 1);
            tmp->goip_id = goip_id;
            tmp->prov_id = ka->prov;
            tmp->send_id = rand();
            snprintf(tmp->goipname, 64, "%s", ka->id);
            tmp->sock = phpsock;
            memcpy(&tmp->addr, &ka->addr, ka->addr_len);
            tmp->addr_len = ka->addr_len;
            tmp->send_count = 3;
            if (value)
                snprintf(tmp->send_buf,
                    sizeof(tmp->send_buf),
                    "%s %d %s %s",
                    cmd,
                    tmp->send_id,
                    value,
                    ka->password);
            else
                snprintf(tmp->send_buf,
                    sizeof(tmp->send_buf),
                    "%s %d %s",
                    cmd,
                    tmp->send_id,
                    ka->password);
            tmp->send_len = strlen(tmp->send_buf);
            tmp->next = background_cmd_head;
            background_cmd_head = tmp;
            if (delay <= 0)
                background_cmd_send(tmp);
            else {
                tmp->send_time = time(NULL);
                tmp->delay = delay;
            }
            DLOG("add a background cmd, cmd:%s,sendid:%d,delay:%d",
                cmd,
                tmp->send_id,
                delay);
            return 0;
        }
        ka = ka->next;
    }
    if (!ka)
        return -1;
    return 0;
}
