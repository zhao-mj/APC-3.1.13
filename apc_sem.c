/*
  +----------------------------------------------------------------------+
  | APC                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 2006-2011 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Daniel Cowgill <dcowgill@communityconnect.com>              |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

/* $Id: apc_sem.c 307048 2011-01-03 23:53:17Z kalle $ */

#include "apc.h"

#ifdef APC_SEM_LOCKS

#include "apc_sem.h"
#include "php.h"
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <unistd.h>

#if HAVE_SEMUN
/* we have semun, no need to define */
#else
#undef HAVE_SEMUN
union semun {
    int val;                  /* value for SETVAL */
    struct semid_ds *buf;     /* buffer for IPC_STAT, IPC_SET */
    unsigned short *array;    /* array for GETALL, SETALL */
                              /* Linux specific part: */
    struct seminfo *__buf;    /* buffer for IPC_INFO */
};
#define HAVE_SEMUN 1
#endif

#ifndef SEM_R
# define SEM_R 0444
#endif
#ifndef SEM_A
# define SEM_A 0222
#endif

/* always use SEM_UNDO, otherwise we risk deadlock */
#define USE_SEM_UNDO

#ifdef USE_SEM_UNDO
# define UNDO SEM_UNDO
#else
# define UNDO 0
#endif

//信号量
  /*
  semget：获取与某个键关联的信号量集标识。

  函数原型：int semget(key_t key,int nsems,int semflg);

  功能描述
      信号量集被建立的情况有两种：
      1.如果键的值是IPC_PRIVATE。
      2.或者键的值不是IPC_PRIVATE，并且键所对应的信号量集不存在，同时标志中指定IPC_CREAT。
      当调用semget创建一个信号量时，他的相应的semid_ds结构被初始化。ipc_perm中各个量被设置为相应
  值：
      sem_nsems被设置为nsems所示的值；
      sem_otime被设置为0；
      sem_ctime被设置为当前时间


  semop说明：
  功能描述
    操作一个或一组信号。
  函数原型：int semop（int semid，struct sembuf *sops，size_t nsops）；

  用法
    #include <sys/types.h>
    #include <sys/ipc.h>
    #include <sys/sem.h>
    int semop(int semid, struct sembuf *sops, unsigned nsops);
    int semtimedop(int semid, struct sembuf *sops, unsigned nsops, struct timespec *timeout);
    参数
    semid：信号集的识别码，可通过semget获取。
    sops：指向存储信号操作结构的数组指针，信号操作结构的原型如下
    struct sembuf
    {
    unsigned short sem_num;
    short sem_op; 
    short sem_flg;
    };
    这三个字段的意义分别为：
    sem_num：操作信号在信号集中的编号，第一个信号的编号是0。
    sem_op：如果其值为正数，该值会加到现有的信号内含值中。通常用于释放所控资源的使用权；
            如果sem_op的值为负数，而其绝对值又大于信号的现值，操作将会阻塞，直到信号值大于或等于sem_op的绝对值。通常用于获取资源的使用权；
            如果sem_op的值为0，如果没有设置IPC_NOWAIT，则调用该操作的进程或者线程将暂时睡眠，直到信号量的值为0；否则，进程或者线程不会睡眠，函数返回错误EAGAIN。

    sem_flg：信号操作标志，可能的选择有两种
            IPC_NOWAIT //对信号的操作不能满足时，semop()不会阻塞，并立即返回，同时设定错误信息。
            SEM_UNDO //程序结束时(不论正常或不正常)，保证信号值会被重设为semop()调用前的值。这样做的目的在于避免程序在异常情况下结束时未将锁定的资源解锁，造成该资源永远锁定。
    nsops：信号操作结构的数量，恒大于或等于1。
    timeout：当semtimedop()调用致使进程进入睡眠时，睡眠时间不能超过本参数指定的值。如果睡眠超时，semtimedop()将失败返回，并设定错误值为EAGAIN。如果本参数的值为NULL，semtimedop()将永远睡眠等待。
    
  返回说明:
    成功执行时，两个系统调用都返回0。失败返回-1，errno被设为以下的某个值
    E2BIG：一次对信号的操作数超出系统的限制
    EACCES：调用进程没有权能执行请求的操作，并且不具有CAP_IPC_OWNER权能
    EAGAIN：信号操作暂时不能满足，需要重试
    EFAULT：sops或timeout指针指向的空间不可访问
    EFBIG：sem_num指定的值无效
    EIDRM：信号集已被移除
    EINTR：系统调用阻塞时，被信号中断
    EINVAL：参数无效
    ENOMEM：内存不足
    ERANGE：信号所允许的值越界
  */

int apc_sem_create(int proj, int initval TSRMLS_DC)
{
    int semid;
    int perms = 0777;
    union semun arg;
    key_t key = IPC_PRIVATE;

    if ((semid = semget(key, 1, IPC_CREAT | IPC_EXCL | perms)) >= 0) {
        /* sempahore created for the first time, initialize now */
        arg.val = initval;
        if (semctl(semid, 0, SETVAL, arg) < 0) {
            apc_error("apc_sem_create: semctl(%d,...) failed:" TSRMLS_CC, semid);
        }
    }
    else if (errno == EEXIST) {
        /* sempahore already exists, don't initialize */
        if ((semid = semget(key, 1, perms)) < 0) {
            apc_error("apc_sem_create: semget(%u,...) failed:" TSRMLS_CC, key);
        }
        /* insert <sleazy way to avoid race condition> here */
    }
    else {
        apc_error("apc_sem_create: semget(%u,...) failed:" TSRMLS_CC, key);
    }

    return semid;
}

void apc_sem_destroy(int semid)
{
    /* we expect this call to fail often, so we do not check */
    union semun arg;
    semctl(semid, 0, IPC_RMID, arg);
}
//获取信号锁
void apc_sem_lock(int semid TSRMLS_DC)
{
    struct sembuf op;

    op.sem_num = 0;
    op.sem_op  = -1;
    op.sem_flg = UNDO;
    if (semop(semid, &op, 1) < 0) {
        if (errno != EINTR) {
            apc_error("apc_sem_lock: semop(%d) failed:" TSRMLS_CC, semid);
        }
    }
}
//非阻塞
int apc_sem_nonblocking_lock(int semid TSRMLS_DC) 
{
    struct sembuf op;

    op.sem_num = 0;
    op.sem_op  = -1;
    op.sem_flg = UNDO | IPC_NOWAIT;

    if (semop(semid, &op, 1) < 0) {
      if (errno == EAGAIN) {
        return 0;  /* Lock is already held */
      } else if (errno != EINTR) {
        apc_error("apc_sem_lock: semop(%d) failed:" TSRMLS_CC, semid);
      }
    }

    return 1;  /* Lock obtained */
}
//释放信号锁
void apc_sem_unlock(int semid TSRMLS_DC)
{
    struct sembuf op;

    op.sem_num = 0;
    op.sem_op  = 1;
    op.sem_flg = UNDO;

    if (semop(semid, &op, 1) < 0) {
        if (errno != EINTR) {
            apc_error("apc_sem_unlock: semop(%d) failed:" TSRMLS_CC, semid);
        }
    }
}

void apc_sem_wait_for_zero(int semid TSRMLS_DC)
{
    struct sembuf op;

    op.sem_num = 0;
    op.sem_op  = 0;
    op.sem_flg = UNDO;

    if (semop(semid, &op, 1) < 0) {
        if (errno != EINTR) {
            apc_error("apc_sem_waitforzero: semop(%d) failed:" TSRMLS_CC, semid);
        }
    }
}

int apc_sem_get_value(int semid TSRMLS_DC)
{
    union semun arg;
    unsigned short val[1];

    arg.array = val;
    if (semctl(semid, 0, GETALL, arg) < 0) {
        apc_error("apc_sem_getvalue: semctl(%d,...) failed:" TSRMLS_CC, semid);
    }
    return val[0];
}

#endif /* APC_SEM_LOCKS */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
