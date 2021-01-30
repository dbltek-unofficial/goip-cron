#ifndef _AUTO_USSD_H
#define _AUTO_USSD_H

#define TYPE_USSD 0
#define TYPE_SMS 1
#define TYPE_FIXED_TIME 2
#define TYPE_REMAIN_LIMIT 3

#define USSD_OK 0
#define USSD_ERROR 1

#define TYPE_RECHARGE_SELF 0
#define TYPE_RECHARGE_OTHER 1
#define TYPE_RECHARGR_SMS 2

#define TYPE_NOR 0
#define TYPE_BALANCE 1
#define TYPE_RECHARGE 2
#define TYPE_2RECHARGE 3
#define TYPE_2USSD 4
#define TYPE_22USSD 5
#define TYPE_2BALANCE 6 // need check ussd2
#define TYPE_S2BALANCE 7 // ussd balance step 2
#define TYPE_GETNUM 8
#define TYPE_RE_STEP2 9
#define TYPE_RE_OTHE_STEP2 10
#define TYPE_USSD_ZD 11
#define TYPE_S3BALANCE 12 //
#define TYPE_S4BALANCE 13 //

//#define _FU
struct auto_ussd;
struct auto_ussd {
    int auto_id;
    int auto_type;
    char sms_num;
    char cmd[320];
    char send_buf[320];
    int send_len;
    char goipname[64];
    int goip_id;
    int be_goip_id;
    int prov_id;
    int be_prov_id;
    int group_id;
    int card_id;
    int ussd_type; // normal or balance or recharge or get num
    char card[64]; // recharge card number
    struct sockaddr_in addr;
    int sock;
    int send_id;
    int send_time;
    int send_count;
    int delay;
    char recharge_ok_r[64];
    char recharge_ok_r2[64];
    char re_step2_ok_r[64];
    char re_step2_cmd[64];
    int auto_disconnect_after_bal;
    struct auto_ussd* next;
    int disable_callout_when_bal;
    char ussd2[64];
    char ussd2_ok_match[64];
    char ussd22[64];
    char ussd22_ok_match[64];
    char send_mail2[64];
    int disable_if_ussd2_undone;
    int recharge_limit;
    char send_email[64];
    char send_sms2[64];
    char auto_ussd_step2[64];
    char auto_ussd_step2_start_r[64];
    int auto_reset_remain_enable;
    int need_ussd2;
};

struct auto_recharge;
struct auto_recharge {
    int type;
    char bal_msg[161];
    char bal_zero_msg[161];
    int msg_len;
    float bal_limit;
    char recharge_cmd_b[161];
    char recharge_cmd_p[161];
    int prov_id;
    int group_id;
    char send_sms[128];
    int recharge_type;
    int send_goip_id;
    char recharge_ok_r[64];
    char recharge_ok_r2[64];
    char re_step2_ok_r[64];
    char re_step2_cmd[64];
    int disable_line;
    int auto_disconnect_after_bal;
    struct auto_recharge* next;
    int disable_callout_when_bal;
    char ussd2[64];
    char ussd2_ok_match[64];
    char ussd22[64];
    char ussd22_ok_match[64];
    char send_mail2[64];
    int disable_if_ussd2_undone;
    int recharge_limit;
    char send_email[64];
    char send_sms2[64];
    int send_sms_goip;
    char recharge_sms_num[64];
    char recharge_sms_msg[161];
    int try_count;
    int auto_reset_remain_enable;
    char ussd_zd[64];
    int id;
    char fixed_time[10];
    int remain_limit;
    int remain_set;
};

struct auto_recharge_sms;
struct auto_recharge_sms {
    int send_time;
    int card_id;
    int goip_id;
    int auto_reset_remain_enable;
    int step;
    char step2_num[32];
    char step2_msg[32];
    char step2_ok_r[32];
    struct auto_recharge_sms* next;
};

int check_return_msg(char* recv,
    int type,
    int goip_id,
    int prov_id,
    char* goipname,
    int group_id,
    int need_ussd2);
void ussd_return_check(char* buf);
void auto_recharge_init();
void auto_ussd_init();
int auto_ussd_check();
void auto_ussd_update();

void sms_recharge_return_check(int message_id, int isok);
void recharge_sms_start(int card_id);
int auto_get_num_start_check(char* goip_name);

#endif
