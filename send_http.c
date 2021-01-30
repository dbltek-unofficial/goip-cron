#include <stdio.h>
#include <stdlib.h>

int send_http(char* addr, char* name, char* number, char* content)
{
    if (name && number && content) {
        char cmdbuf[1024];
        sprintf(cmdbuf,
            "php dopost.php \"%s\" \"%s\" \"%s\" \"%s\" &",
            addr,
            name,
            number,
            content);
        fprintf(stderr, "%s\n", cmdbuf);
        system(cmdbuf);
    }
    fprintf(stderr, "send http system ok\n");
    return 0;
}
