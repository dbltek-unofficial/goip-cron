#ifndef _MY_DEBUG_H
#define _MY_DEBUG_H
int dlog(const char* func, const char* file, int line, const char* fmt, ...);
#define DLOG(x...) dlog(__FUNCTION__, __FILE__, __LINE__, x);
#endif
