goipcron:goipcron.c srvd.c mysql.c auto_ussd.c send_mail.c report.c background_cmd.c debug.c send_http.c re.c
	gcc -o goipcron goipcron.c srvd.c mysql.c auto_ussd.c send_mail.c send_http.c report.c background_cmd.c debug.c re.c ./lib/libmysqlclient.so.14 -Wl,-rpath,./lib/ -Wall
