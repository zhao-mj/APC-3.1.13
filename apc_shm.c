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
  |          Rasmus Lerdorf <rasmus@php.net>                             |
  +----------------------------------------------------------------------+

   This software was contributed to PHP by Community Connect Inc. in 2002
   and revised in 2005 by Yahoo! Inc. to add support for PHP 5.1.
   Future revisions and derivatives of this source code must acknowledge
   Community Connect Inc. as the original contributor of this module by
   leaving this note intact in the source code.

   All other licensing and usage conditions are those of the PHP Group.

 */

/* $Id: apc_shm.c 307259 2011-01-08 12:05:24Z gopalv $ */

#include "apc_shm.h"
#include "apc.h"
#ifdef PHP_WIN32
/* shm functions are available in TSRM */
#include <tsrm/tsrm_win32.h>
#define key_t long
#else
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#endif

#ifndef SHM_R
# define SHM_R 0444 /* read permission */
#endif
#ifndef SHM_A
# define SHM_A 0222 /* write permission */
#endif
//创建共享内存
int apc_shm_create(int proj, size_t size TSRMLS_DC)
{
    int shmid;			/* shared memory id */
    int oflag;			/* permissions on shm */
    key_t key = IPC_PRIVATE;	/* shm key */

    oflag = IPC_CREAT | SHM_R | SHM_A;
    /*
    shmget: 
    函数说明：  得到一个共享内存标识符或创建一个共享内存对象并返回共享内存标识符
    函数原型：  int shmget(key_t key, size_t size, int shmflg)
    函数传入值：key ：(IPC_PRIVATE)：会建立新共享内存对象; 大于0的32位整数：视参数shmflg来确定操作。通常要求此值来源于ftok返回的IPC键值
                size: 大于0的整数：新建的共享内存大小，以字节为单位; 0：只获取共享内存时指定为0
                shmflg： 
                  0：取共享内存标识符，若不存在则函数会报错
                  IPC_CREAT：当shmflg&IPC_CREAT为真时，如果内核中不存在键值与key相等的共享内存，则新建一个共享内存；如果存在这样的共享内存，返回此共享内存的标识符
                  IPC_CREAT|IPC_EXCL：如果内核中不存在键值 与key相等的共享内存，则新建一个共享内存；如果存在这样的共享内存则报错
    函数返回值:
                成功：返回共享内存的标识符
                出错：-1，错误原因存于error中
    */
    if ((shmid = shmget(key, size, oflag)) < 0) {
        apc_error("apc_shm_create: shmget(%d, %d, %d) failed: %s. It is possible that the chosen SHM segment size is higher than the operation system allows. Linux has usually a default limit of 32MB per segment." TSRMLS_CC, key, size, oflag, strerror(errno));
    }

    return shmid;
}
//移除内存
void apc_shm_destroy(int shmid)
{
    /* we expect this call to fail often, so we do not check */
    /**
    shmctl:
    函数说明: 完成对共享内存的控制
    函数原型: int shmctl(int shmid, int cmd, struct shmid_ds *buf)
    函数传入值: shmid :共享内存标识符
                cmd: 
                    IPC_STAT：得到共享内存的状态，把共享内存的shmid_ds结构复制到buf中
                    IPC_SET：改变共享内存的状态，把buf所指的shmid_ds结构中的uid、gid、mode复制到共享内存的shmid_ds结构内
                    IPC_RMID：删除这片共享内存
                buf: 共享内存管理结构体。具体说明参见共享内存内核结构定义部分
    函数返回值: 
                成功：0
                出错：-1，错误原因存于error中

     */
    shmctl(shmid, IPC_RMID, 0);
}
//将共享内存地址保存在segment对象
apc_segment_t apc_shm_attach(int shmid, size_t size TSRMLS_DC)
{
    apc_segment_t segment; /* shm segment */
    /**
    shmat：
    函数说明：连接共享内存标识符为shmid的共享内存，连接成功后把共享内存区对象映射到调用进程的地址空间，随后可像本地空间一样访问
    函数原型：void *shmat(int shmid, const void *shmaddr, int shmflg)
    函数传入值：
              shmid：共享内存标识符
              shmaddr：指定共享内存出现在进程内存地址的什么位置，直接指定为NULL让内核自己决定一个合适的地址位置
              shmflg：SHM_RDONLY：为只读模式，其他为读写模式
    函数返回值
              成功：附加好的共享内存地址
              出错：-1，错误原因存于error中
    */
    if ((long)(segment.shmaddr = shmat(shmid, 0, 0)) == -1) {
        apc_error("apc_shm_attach: shmat failed:" TSRMLS_CC);
    }

#ifdef APC_MEMPROTECT
    
    if ((long)(segment.roaddr = shmat(shmid, 0, SHM_RDONLY)) == -1) {
        segment.roaddr = NULL;
    }

#endif

    segment.size = size;

    /*
     * We set the shmid for removal immediately after attaching to it. The
     * segment won't disappear until all processes have detached from it.
     */
    apc_shm_destroy(shmid);
    return segment;
}

void apc_shm_detach(apc_segment_t* segment TSRMLS_DC)
{
    //断开共享连接
    /**
    shmdt：
    函数说明：与shmat函数相反，是用来断开与共享内存附加点的地址，禁止本进程访问此片共享内存
    函数原型：int shmdt(const void *shmaddr)
    函数传入值：shmaddr：连接的共享内存的起始地址
    函数返回值：成功：0  出错：-1，错误原因存于error中
     */
    if (shmdt(segment->shmaddr) < 0) {
        apc_error("apc_shm_detach: shmdt failed:" TSRMLS_CC);
    }

#ifdef APC_MEMPROTECT
    if (segment->roaddr && shmdt(segment->roaddr) < 0) {
        apc_error("apc_shm_detach: shmdt failed:" TSRMLS_CC);
    }
#endif
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: expandtab sw=4 ts=4 sts=4 fdm=marker
 * vim<600: expandtab sw=4 ts=4 sts=4
 */
