#ifndef _BACKGROUD_CMD_H
#define _BACKGROUD_CMD_H

struct background_cmd;
struct background_cmd {
    char cmd[320];
    char send_buf[320];
    int send_len;
    char goipname[64];
    int goip_id;
    int prov_id;
    struct sockaddr_in addr;
    int addr_len;
    int sock;
    int send_id;
    int send_time;
    int send_count;
    int delay;
    struct background_cmd* next;
};

void background_msg_check(char* buf);
int background_cmd_calloc(int goip_id, char* cmd, char* value, int delay);
void background_cmd_check(int now_time);
#endif
