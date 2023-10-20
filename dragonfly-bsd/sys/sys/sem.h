/* $FreeBSD: src/sys/sys/sem.h,v 1.20.2.2 2000/08/04 22:31:10 peter Exp $ */
/*	$NetBSD: sem.h,v 1.5 1994/06/29 06:45:15 cgd Exp $	*/

/*
 * SVID compatible sem.h file
 *
 * Author:  Daniel Boulet
 */

#ifndef _SYS_SEM_H_
#define	_SYS_SEM_H_

#include <sys/cdefs.h>
#include <sys/ipc.h>
#include <machine/stdint.h>

#ifndef _PID_T_DECLARED
typedef	__pid_t		pid_t;
#define	_PID_T_DECLARED
#endif

#ifndef _SIZE_T_DECLARED
typedef	__size_t	size_t;
#define	_SIZE_T_DECLARED
#endif

#ifndef _TIME_T_DECLARED
typedef	__time_t	time_t;
#define	_TIME_T_DECLARED
#endif

struct sem;

struct semid_ds {
	struct	ipc_perm sem_perm;	/* operation permission struct */
	struct	sem *sem_base;	/* pointer to first semaphore in set */
	unsigned short sem_nsems;	/* number of sems in set */
	time_t	sem_otime;	/* last operation time */
	long	sem_pad1;	/* SVABI/386 says I need this here */
	time_t	sem_ctime;	/* last change time */
    				/* Times measured in secs since */
    				/* 00:00:00 GMT, Jan. 1, 1970 */
	long	sem_pad2;	/* SVABI/386 says I need this here */
	long	sem_pad3[4];	/* SVABI/386 says I need this here */
};

#if defined(_KERNEL) || defined(_KERNEL_STRUCTURES)

#include <sys/lock.h>

struct semid_pool {
	struct lock lk;
	struct semid_ds ds;
	long gen;
};

#endif

/*
 * semop's sops parameter structure
 */
struct sembuf {
	unsigned short sem_num;	/* semaphore # */
	short	sem_op;		/* semaphore operation */
	short	sem_flg;	/* operation flags */
};
#define	SEM_UNDO	010000

#if __BSD_VISIBLE
#define	MAX_SOPS	5	/* maximum # of sembuf's per semop call */

/*
 * semctl's arg parameter structure
 */
union semun {
	int	val;		/* value for SETVAL */
	struct	semid_ds *buf;	/* buffer for IPC_STAT & IPC_SET */
	unsigned short	*array;	/* array for GETALL & SETALL */
};
#endif /* __BSD_VISIBLE */

/*
 * commands for semctl
 */
#define	GETNCNT	3	/* Return the value of semncnt {READ} */
#define	GETPID	4	/* Return the value of sempid {READ} */
#define	GETVAL	5	/* Return the value of semval {READ} */
#define	GETALL	6	/* Return semvals into arg.array {READ} */
#define	GETZCNT	7	/* Return the value of semzcnt {READ} */
#define	SETVAL	8	/* Set the value of semval to arg.val {ALTER} */
#define	SETALL	9	/* Set semvals from arg.array {ALTER} */
#if __BSD_VISIBLE
#define	SEM_STAT 10	/* Like IPC_STAT but treats semid as sema-index */

/*
 * Permissions
 */
#define	SEM_A		0200	/* alter permission */
#define	SEM_R		0400	/* read permission */
#endif /* __BSD_VISIBLE */

#if defined(_KERNEL) || defined(_KERNEL_STRUCTURES)

/*
 * semaphore info struct
 */
struct seminfo {
	int	semmap,		/* # of entries in semaphore map */
		semmni,		/* # of semaphore identifiers */
		semmns,		/* # of semaphores in system */
		semmnu,		/* # of undo structures in system */
		semmsl,		/* max # of semaphores per id */
		semopm,		/* max # of operations per semop call */
		semume,		/* max # of undo entries per process */
		semusz,		/* size in bytes of undo structure */
		semvmx,		/* semaphore maximum value */
		semaem;		/* adjust on exit max value */
};

/* internal "mode" bits */
#define	SEM_ALLOC	01000	/* semaphore is allocated */
#define	SEM_DEST	02000	/* semaphore will be destroyed on last detach */

#endif /* _KERNEL || _KERNEL_STRUCTURES */

#ifdef _KERNEL
/*
 * Process sem_undo vectors at proc exit.
 */
void	semexit(struct proc *p);
extern struct seminfo	seminfo;
#else
__BEGIN_DECLS
int	semctl(int, int, int, ...);
int	semget(key_t, int, int);
int	semop(int, struct sembuf *, unsigned);
__END_DECLS
#endif /* !_KERNEL */

#endif /* !_SEM_H_ */
