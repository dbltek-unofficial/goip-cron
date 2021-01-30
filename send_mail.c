#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int send_mail(char* dest_mail, char* title, char* content)
{
    if (title && content && dest_mail) {
        char cmdbuf[1024] = { 0 };
        int i = 0;
        sprintf(cmdbuf, "php send_mail.php \"%s\" \"%s\" \"", dest_mail, title);
        while (*(content + i)) {
            if (*(content + i) == '"' || *(content + i) == '\\')
                cmdbuf[strlen(cmdbuf)] = '\\';
            cmdbuf[strlen(cmdbuf)] = *(content + i);
            i++;
        }
        sprintf(cmdbuf + strlen(cmdbuf), "\" &");
        fprintf(stderr, "%s\n", cmdbuf);
        system(cmdbuf);
    }
    fprintf(stderr, "send mail ok\n");
    return 0;
}
