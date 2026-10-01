#ifndef OPENC6_SYSCALL_H
#define OPENC6_SYSCALL_H

#include <stdint.h>

#define SYS_EXIT                 0UL
#define SYS_PRINT                1UL
#define SYS_DELAY_MS             2UL
#define SYS_SYS_RESET            3UL
#define SYS_SET_LED_COLOR        4UL
#define SYS_GET_RANDOM           5UL
#define SYS_SHA256               6UL
#define SYS_MATH_ISQRT           7UL
#define SYS_MATH_SIN_DEG         8UL
#define SYS_MATH_COS_DEG         9UL

/* Network Sockets API */
#define SYS_NET_GET_IP           10UL
#define SYS_WIFI_IS_CONNECTED    12UL
#define SYS_TCP_LISTEN           20UL
#define SYS_TCP_ACCEPT           21UL
#define SYS_TCP_READ             22UL
#define SYS_TCP_WRITE            23UL
#define SYS_TCP_CLOSE            24UL

#define SYS_GET_FREE_RAM         13UL
#define SYS_GET_TOTAL_RAM        14UL
#define SYS_GET_TOTAL_FLASH      15UL
#define SYS_FS_WRITE_FILE        16UL
#define SYS_FS_READ_FILE         17UL
#define SYS_FS_DELETE            18UL
#define SYS_SBRK                 19UL

#define SYS_MALLOC               30UL
#define SYS_FREE                 31UL

/* Hardware GPIO API */
#define SYS_GPIO_SET_DIR         32UL
#define SYS_GPIO_WRITE           33UL
#define SYS_GPIO_READ            34UL

#endif /* OPENC6_SYSCALL_H */
