#ifndef __RE_H
#define __RE_H
void check_re_return(int goip_id,
    char* goip_name,
    int prov_id,
    char* num,
    char* content);
int do_need_re(char* buf, struct sockaddr_in* cliaddr, int addrlen);
void do_re_ok(char* buf, struct sockaddr_in* cliaddr, int addrlen);
void check_re_timeout();
void re_resend(int message_id, int send_id);
void do_num_iccid(char* buf, struct sockaddr_in* cliaddr, int addrlen);
#endif
