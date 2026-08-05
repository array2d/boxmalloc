#ifndef LOGUTIL_H
#define LOGUTIL_H

#include <stdio.h>

// 如果 ENABLE_LOG 被定义，则启用日志打印，否则禁用
#ifdef ENABLE_LOG
    #define LOG(fmt, ...) printf("[slotsboxmalloc]" fmt "\n", ##__VA_ARGS__)
#else
    #define LOG(fmt, ...) // 禁用日志打印
#endif

#endif // LOGUTIL_H