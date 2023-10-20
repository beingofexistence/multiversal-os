/*
 * Copyright (c) 2016 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/*
 * Administration thread
 *
 * - Handles resetting, features, iteration of namespaces, and disk
 *   attachments.  Most admin operations are serialized by the admin thread.
 *
 * - Ioctls as well as any BIOs which require more sophisticated processing
 *   are handed to this thread as well.
 *
 * - Can freeze/resume other queues for various purposes.
 */

#include "nvme.h"

static void nvme_admin_thread(void *arg);
static int nvme_admin_state_identify_ctlr(nvme_softc_t *sc);
static int nvme_admin_state_make_queues(nvme_softc_t *sc);
static int nvme_admin_state_identify_ns(nvme_softc_t *sc);
static int nvme_admin_state_operating(nvme_softc_t *sc);
static int nvme_admin_state_failed(nvme_softc_t *sc);

/*
 * Start the admin thread and block until it says it is running.
 */
int
nvme_start_admin_thread(nvme_softc_t *sc)
{
	int error, intr_flags;

	lockinit(&sc->admin_lk, "admlk", 0, 0);
	lockinit(&sc->ioctl_lk, "nvioc", 0, 0);
	sc->admin_signal = 0;

	intr_flags = INTR_MPSAFE;
	if (sc->nirqs == 1) {
		/* This interrupt processes data CQs too */
		intr_flags |= INTR_HIFREQ;
	}

	error = bus_setup_intr(sc->dev, sc->irq[0], intr_flags,
			       nvme_intr, &sc->comqueues[0],
			       &sc->irq_handle[0], NULL);
	if (error) {
		device_printf(sc->dev, "unable to install interrupt\n");
		return error;
	}
	lockmgr(&sc->admin_lk, LK_EXCLUSIVE);
	kthread_create(nvme_admin_thread, sc, &sc->admintd, "nvme_admin");
	while ((sc->admin_signal & ADMIN_SIG_RUNNING) == 0)
		lksleep(&sc->admin_signal, &sc->admin_lk, 0, "nvwbeg", 0);
	lockmgr(&sc->admin_lk, LK_RELEASE);

	return 0;
}

/*
 * Stop the admin thread and block until it says it is done.
 */
void
nvme_stop_admin_thread(nvme_softc_t *sc)
{
	uint32_t i;

	atomic_set_int(&sc->admin_signal, ADMIN_SIG_STOP);

	/*
	 * We have to wait for the admin thread to finish its probe
	 * before shutting it down.  Break out if the admin thread
	 * never managed to even start.
	 */
	lockmgr(&sc->admin_lk, LK_EXCLUSIVE);
	while ((sc->admin_signal & ADMIN_SIG_PROBED) == 0) {
		if ((sc->admin_signal & ADMIN_SIG_RUNNING) == 0)
			break;
		lksleep(&sc->admin_signal, &sc->admin_lk, 0, "nvwend", 0);
	}
	lockmgr(&sc->admin_lk, LK_RELEASE);

	/*
	 * Disconnect our disks while the admin thread is still running,
	 * ensuring that the poll works even if interrupts are broken.
	 * Otherwise we could deadlock in the devfs core.
	 */
	for (i = 0; i < NVME_MAX_NAMESPACES; ++i) {
		nvme_softns_t *nsc;

		if ((nsc = sc->nscary[i]) != NULL) {
			nvme_disk_detach(nsc);

			kfree(nsc, M_NVME);
			sc->nscary[i] = NULL;
		}
	}

	/*
	 * Ask the admin thread to shut-down.
	 */
	lockmgr(&sc->admin_lk, LK_EXCLUSIVE);
	wakeup(&sc->admin_signal);
	while (sc->admin_signal & ADMIN_SIG_RUNNING)
		lksleep(&sc->admin_signal, &sc->admin_lk, 0, "nvwend", 0);
	lockmgr(&sc->admin_lk, LK_RELEASE);
	if (sc->irq_handle[0]) {
		bus_teardown_intr(sc->dev, sc->irq[0], sc->irq_handle[0]);
		sc->irq_handle[0] = NULL;
	}
	lockuninit(&sc->ioctl_lk);
	lockuninit(&sc->admin_lk);

	/*
	 * Thread might be running on another cpu, give it time to actually
	 * exit before returning in case the caller is about to unload the
	 * module.  Otherwise we don't need this.
	 */
	nvme_os_sleep(1);
}

static
void
nvme_admin_thread(void *arg)
{
	nvme_softc_t *sc = arg;
	uint32_t i;

	lockmgr(&sc->admin_lk, LK_EXCLUSIVE);
	atomic_set_int(&sc->admin_signal, ADMIN_SIG_RUNNING);
	wakeup(&sc->admin_signal);

	sc->admin_func = nvme_admin_state_identify_ctlr;

	while ((sc->admin_signal & ADMIN_SIG_STOP) == 0) {
		for (i = 0; i <= sc->niocomqs; ++i) {
			nvme_comqueue_t *comq = &sc->comqueues[i];

			if (comq->nqe == 0)	/* not configured */
				continue;

			lockmgr(&comq->lk, LK_EXCLUSIVE);
			nvme_poll_completions(comq, &comq->lk);
			lockmgr(&comq->lk, LK_RELEASE);
		}
		if (sc->admin_signal & ADMIN_SIG_REQUEUE) {
			atomic_clear_int(&sc->admin_signal, ADMIN_SIG_REQUEUE);
			nvme_disk_requeues(sc);
		}
		if (sc->admin_func(sc) == 0 &&
		    (sc->admin_signal & ADMIN_SIG_RUN_MASK) == 0) {
			lksleep(&sc->admin_signal, &sc->admin_lk, 0,
				"nvidle", hz);
		}
	}

	/*
	 * Cleanup state.
	 *
	 * Note that we actually issue delete queue commands here.  The NVME
	 * spec says that for a normal shutdown the I/O queues should be
	 * deleted prior to issuing the shutdown in the CONFIG register.
	 */
	for (i = 1; i <= sc->niosubqs; ++i) {
		nvme_delete_subqueue(sc, i);
		nvme_free_subqueue(sc, i);
	}
	for (i = 1; i <= sc->niocomqs; ++i) {
		nvme_delete_comqueue(sc, i);
		nvme_free_comqueue(sc, i);
	}

	/*
	 * Signal that we are done.
	 */
	atomic_clear_int(&sc->admin_signal, ADMIN_SIG_RUNNING);
	wakeup(&sc->admin_signal);
	lockmgr(&sc->admin_lk, LK_RELEASE);
}

/*
 * Identify the controller
 */
static
int
nvme_admin_state_identify_ctlr(nvme_softc_t *sc)
{
	nvme_request_t *req;
	nvme_ident_ctlr_data_t *rp;
	int status;
	uint64_t mempgsize;
	char serial[20+16];
	char model[40+16];

	/*
	 * Identify Controller
	 */
	mempgsize = NVME_CAP_MEMPG_MIN_GET(sc->cap);

	req = nvme_get_admin_request(sc, NVME_OP_IDENTIFY);
	req->cmd.identify.cns = NVME_CNS_CTLR;
	req->cmd.identify.cntid = 0;
	bzero(req->info, sizeof(*req->info));
	nvme_submit_request(req);
	status = nvme_wait_request(req);
	/* XXX handle status */

	sc->idctlr = req->info->idctlr;
	nvme_put_request(req);

	rp = &sc->idctlr;

	KKASSERT(sizeof(sc->idctlr.serialno) == 20);
	KKASSERT(sizeof(sc->idctlr.modelno) == 40);
	bzero(serial, sizeof(serial));
	bzero(model, sizeof(model));
	bcopy(rp->serialno, serial, sizeof(rp->serialno));
	bcopy(rp->modelno, model, sizeof(rp->modelno));
	string_cleanup(serial, 0);
	string_cleanup(model, 0);

	device_printf(sc->dev, "Model %s BaseSerial %s nscount=%d\n",
		      model, serial, rp->ns_count);

	sc->admin_func = nvme_admin_state_make_queues;

	return 1;
}

#define COMQFIXUP(msix, ncomqs)	((((msix) - 1) % ncomqs) + 1)

/*
 * Request and create the I/O queues.  Figure out CPU mapping optimizations.
 */
static
int
nvme_admin_state_make_queues(nvme_softc_t *sc)
{
	nvme_request_t *req;
	uint16_t niosubqs, subq_err_idx;
	uint16_t niocomqs, comq_err_idx;
	uint32_t i;
	uint16_t qno;
	int status;
	int error;

	/*
	 * Calculate how many I/O queues (non-inclusive of admin queue)
	 * we want to have, up to 65535.  dw0 in the response returns the
	 * number of queues the controller gives us.  Submission and
	 * Completion queues are specified separately.
	 *
	 * This driver runs optimally with 4 submission queues and one
	 * completion queue per cpu (rdhipri, rdlopri, wrhipri, wrlopri),
	 *
	 * +1 for dumps			XXX future
	 * +1 for async events		XXX future
	 *
	 * NOTE: Set one less than the #define because we use 1...N for I/O
	 *	 queues (queue 0 is used for the admin queue).  Easier this
	 *	 way.
	 */
	req = nvme_get_admin_request(sc, NVME_OP_SET_FEATURES);

	niosubqs = ncpus * 2 + 0;
	niocomqs = ncpus + 0;
	if (niosubqs >= NVME_MAX_QUEUES)
		niosubqs = NVME_MAX_QUEUES - 1;
	if (niocomqs >= NVME_MAX_QUEUES)
		niocomqs = NVME_MAX_QUEUES - 1;

	/*
	 * If there are insufficient MSI-X vectors or we use a normal
	 * interrupt, the completion queues are going to wind up being
	 * polled by a single admin interrupt.  Limit the number of
	 * completion queues in this case to something reasonable.
	 */
	if (sc->nirqs == 1 && niocomqs > 4) {
		niocomqs = 4;
		device_printf(sc->dev, "no MSI-X support, limit comqs to %d\n",
			      niocomqs);
	}

	device_printf(sc->dev, "Request %u/%u queues, ", niosubqs, niocomqs);

	req->cmd.setfeat.flags = NVME_FID_NUMQUEUES;
	req->cmd.setfeat.numqs.nsqr = niosubqs - 1;	/* 0's based 0=1 */
	req->cmd.setfeat.numqs.ncqr = niocomqs - 1;	/* 0's based 0=1 */

	nvme_submit_request(req);

	/*
	 * Get response and set our operations mode.  Limit the returned
	 * queue counts to no more than we requested (some chipsets may
	 * return more than the requested number of queues while others
	 * will not).
	 */
	status = nvme_wait_request(req);
	/* XXX handle status */

	if (status == 0) {
		sc->niosubqs = 1 + (req->res.setfeat.dw0 & 0xFFFFU);
		sc->niocomqs = 1 + ((req->res.setfeat.dw0 >> 16) & 0xFFFFU);
		if (sc->niosubqs > niosubqs)
			sc->niosubqs = niosubqs;
		if (sc->niocomqs > niocomqs)
			sc->niocomqs = niocomqs;
	} else {
		sc->niosubqs = 0;
		sc->niocomqs = 0;
	}
	kprintf("Returns %u/%u queues, ", sc->niosubqs, sc->niocomqs);

	nvme_put_request(req);

tryagain:
	sc->dumpqno = 0;
	sc->eventqno = 0;

	if (sc->niosubqs >= ncpus * 2 + 0 && sc->niocomqs >= ncpus + 0) {
		/*
		 * If we got all the queues we wanted do a full-bore setup of
		 * qmap[cpu][type].
		 *
		 * Remember that subq 0 / comq 0 is the admin queue.
		 */
		kprintf("optimal map\n");
		qno = 1;
		for (i = 0; i < ncpus; ++i) {
			int cpuqno = COMQFIXUP(sc->cputovect[i], ncpus);

			KKASSERT(cpuqno != 0);
			sc->qmap[i][0] = qno + 0;
			sc->qmap[i][1] = qno + 1;
			sc->subqueues[qno + 0].comqid = cpuqno;
			sc->subqueues[qno + 1].comqid = cpuqno;
			qno += 2;
		}
		sc->niosubqs = ncpus * 2 + 0;
		sc->niocomqs = ncpus + 0;
	} else if (sc->niosubqs >= ncpus && sc->niocomqs >= ncpus) {
		/*
		 * We have enough to give each cpu its own submission
		 * and completion queue.
		 *
		 * leave dumpqno and eventqno set to the admin queue.
		 */
		kprintf("nominal map 1:1 cpu\n");
		for (i = 0; i < ncpus; ++i) {
			qno = sc->cputovect[i];
			KKASSERT(qno != 0);
			sc->qmap[i][0] = qno;
			sc->qmap[i][1] = qno;
			sc->subqueues[qno].comqid = COMQFIXUP(qno, ncpus);
		}
		sc->niosubqs = ncpus;
		sc->niocomqs = ncpus;
	} else if (sc->niosubqs >= 2 && sc->niocomqs >= 2) {
		/*
		 * prioritize trying to distribute available queues to
		 * cpus, don't separate read and write.
		 *
		 * leave dumpqno and eventqno set to the admin queue.
		 */
		kprintf("rw-sep map (%d, %d)\n", sc->niosubqs, sc->niocomqs);
		for (i = 0; i < ncpus; ++i) {
			int cpuqno = COMQFIXUP(sc->cputovect[i], sc->niocomqs);
			int qno = COMQFIXUP((i + 1), sc->niosubqs);

			KKASSERT(qno != 0);
			sc->qmap[i][0] = qno;		/* read */
			sc->qmap[i][1] = qno;		/* write */
			sc->subqueues[qno].comqid = cpuqno;
			/* do not increment qno */
		}
#if 0
		sc->niosubqs = 2;
		sc->niocomqs = 2;
#endif
	} else if (sc->niosubqs >= 2) {
		/*
		 * We have enough to have separate read and write queues.
		 */
		kprintf("basic map\n");
		qno = 1;
		for (i = 0; i < ncpus; ++i) {
			int cpuqno = COMQFIXUP(sc->cputovect[i], 1);

			KKASSERT(qno != 0);
			sc->qmap[i][0] = qno + 0;	/* read */
			sc->qmap[i][1] = qno + 1;	/* write */
			if (i <= 0)
				sc->subqueues[qno + 0].comqid = cpuqno;
			if (i <= 1)
				sc->subqueues[qno + 1].comqid = cpuqno;
		}
		sc->niosubqs = 2;
		sc->niocomqs = 1;
	} else {
		/*
		 * Minimal configuration, all cpus and I/O types use the
		 * same queue.  Sad day.
		 */
		kprintf("minimal map\n");
		sc->dumpqno = 0;
		sc->eventqno = 0;
		for (i = 0; i < ncpus; ++i) {
			sc->qmap[i][0] = 1;
			sc->qmap[i][1] = 1;
		}
		sc->subqueues[1].comqid = 1;
		sc->niosubqs = 1;
		sc->niocomqs = 1;
	}

	/*
	 * Create all I/O submission and completion queues.  The I/O
	 * queues start at 1 and are inclusive of niosubqs and niocomqs.
	 *
	 * NOTE: Completion queues must be created before submission queues.
	 *	 That is, the completion queue specified when creating a
	 *	 submission queue must already exist.
	 */
	error = 0;
	for (i = 1; i <= sc->niocomqs; ++i) {
		error += nvme_alloc_comqueue(sc, i);
		if (error) {
			device_printf(sc->dev, "Unable to alloc comq %d/%d\n",
				      i, sc->niocomqs);
			break;
		}
		error += nvme_create_comqueue(sc, i);
		if (error) {
			device_printf(sc->dev, "Unable to create comq %d/%d\n",
				      i, sc->niocomqs);
			++i;	/* also delete this one below */
			break;
		}
	}
	comq_err_idx = i;

	for (i = 1; i <= sc->niosubqs; ++i) {
		error += nvme_alloc_subqueue(sc, i);
		if (error) {
			device_printf(sc->dev, "Unable to alloc subq %d/%d\n",
				      i, sc->niosubqs);
			break;
		}
		error += nvme_create_subqueue(sc, i);
		if (error) {
			device_printf(sc->dev, "Unable to create subq %d/%d\n",
				      i, sc->niosubqs);
			++i;	/* also delete this one below */
			break;
		}
	}
	subq_err_idx = i;

	/*
	 * If we are unable to allocate and create the number of queues
	 * the device told us it could handle.
	 */
	if (error) {
		device_printf(sc->dev, "Failed to initialize device!\n");
		for (i = subq_err_idx - 1; i >= 1; --i) {
			nvme_delete_subqueue(sc, i);
			nvme_free_subqueue(sc, i);
		}
		for (i = comq_err_idx - 1; i >= 1; --i) {
			nvme_delete_comqueue(sc, i);
			nvme_free_comqueue(sc, i);
		}
		sc->admin_func = nvme_admin_state_failed;
		if (sc->niosubqs > 1 || sc->niocomqs > 1) {
			int trywith = 1;

			device_printf(sc->dev,
				      "Retrying with fewer queues (%d/%d) "
				      "just in case the device lied to us\n",
				      trywith, trywith);
			if (sc->niosubqs > trywith)
				sc->niosubqs = trywith;
			if (sc->niocomqs > trywith)
				sc->niocomqs = trywith;
			goto tryagain;
		}
	} else {
		sc->admin_func = nvme_admin_state_identify_ns;
	}

	/*
	 * Disable interrupt coalescing.  It is basically worthless because
	 * setting the threshold has no effect when time is set to 0, and the
	 * smallest time that can be set is 1 (== 100uS), which is too long.
	 * Sequential performance is destroyed (on e.g. the Intel 750).
	 * So kill it.
	 */
	req = nvme_get_admin_request(sc, NVME_OP_SET_FEATURES);
	device_printf(sc->dev, "Interrupt Coalesce: 100uS / 4 qentries\n");

	req->cmd.setfeat.flags = NVME_FID_INTCOALESCE;
	req->cmd.setfeat.intcoal.thr = 0;
	req->cmd.setfeat.intcoal.time = 0;

	nvme_submit_request(req);
	status = nvme_wait_request(req);
	if (status) {
		device_printf(sc->dev,
			      "Interrupt coalesce failed status=%d\n",
			      status);
	}
	nvme_put_request(req);

	return 1;
}

/*
 * Identify available namespaces, iterate, and attach to disks.
 */
static
int
nvme_admin_state_identify_ns(nvme_softc_t *sc)
{
	nvme_request_t *req;
	nvme_ident_ns_list_t *rp;
	int status;
	int i;
	int j;

	if (bootverbose) {
		if (sc->idctlr.admin_cap & NVME_ADMIN_NSMANAGE)
			device_printf(sc->dev,
				      "Namespace management supported\n");
		else
			device_printf(sc->dev,
				      "Namespace management not supported\n");
	}
#if 0
	/*
	 * Identify Controllers		TODO TODO TODO
	 */
	if (sc->idctlr.admin_cap & NVME_ADMIN_NSMANAGE) {
		req = nvme_get_admin_request(sc, NVME_OP_IDENTIFY);
		req->cmd.identify.cns = NVME_CNS_ANY_CTLR_LIST;
		req->cmd.identify.cntid = 0;
		bzero(req->info, sizeof(*req->info));
		nvme_submit_request(req);
		status = nvme_wait_request(req);
		kprintf("nsquery status %08x\n", status);

#if 0
		for (i = 0; i < req->info->ctlrlist.idcount; ++i) {
			kprintf("CTLR %04x\n", req->info->ctlrlist.ctlrids[i]);
		}
#endif
		nvme_put_request(req);
	}
#endif

	rp = kmalloc(sizeof(*rp), M_NVME, M_WAITOK | M_ZERO);
	if (sc->idctlr.admin_cap & NVME_ADMIN_NSMANAGE) {
		/*
		 * Namespace management supported, query active namespaces.
		 */
		req = nvme_get_admin_request(sc, NVME_OP_IDENTIFY);
		req->cmd.identify.cns = NVME_CNS_ACT_NSLIST;
		req->cmd.identify.cntid = 0;
		bzero(req->info, sizeof(*req->info));
		nvme_submit_request(req);
		status = nvme_wait_request(req);
		kprintf("nsquery status %08x\n", status);
		/* XXX handle status */

		cpu_lfence();
		*rp = req->info->nslist;
		nvme_put_request(req);
	} else {
		/*
		 * Namespace management not supported, assume nsids 1..N.
		 * (note: (i) limited to 1024).
		 */
		for (i = 1; i <= (int)sc->idctlr.ns_count && i <= 1024; ++i)
			rp->nsids[i-1] = i;
	}

	/*
	 * Identify each Namespace
	 */
	for (i = 0; i < 1024; ++i) {
		nvme_softns_t *nsc;
		nvme_lba_fmt_data_t *lbafmt;

		if (rp->nsids[i] == 0)
			continue;
		req = nvme_get_admin_request(sc, NVME_OP_IDENTIFY);
		req->cmd.identify.cns = NVME_CNS_ACT_NS;
		req->cmd.identify.cntid = 0;
		req->cmd.identify.head.nsid = rp->nsids[i];
		bzero(req->info, sizeof(*req->info));
		nvme_submit_request(req);
		status = nvme_wait_request(req);
		if (status != 0) {
			kprintf("NS FAILED %08x\n", status);
			continue;
		}

		for (j = 0; j < NVME_MAX_NAMESPACES; ++j) {
			if (sc->nscary[j] &&
			    sc->nscary[j]->nsid == rp->nsids[i])
				break;
		}
		if (j == NVME_MAX_NAMESPACES) {
			j = i;
			if (sc->nscary[j] != NULL) {
				for (j = NVME_MAX_NAMESPACES - 1; j >= 0; --j) {
					if (sc->nscary[j] == NULL)
						break;
				}
			}
		}
		if (j < 0) {
			device_printf(sc->dev, "not enough room in nscary for "
					       "namespace %08x\n", rp->nsids[i]);
			nvme_put_request(req);
			continue;
		}
		nsc = sc->nscary[j];
		if (nsc == NULL) {
			nsc = kmalloc(sizeof(*nsc), M_NVME, M_WAITOK | M_ZERO);
			nsc->unit = nvme_alloc_disk_unit();
			sc->nscary[j] = nsc;
		}
		if (sc->nscmax <= j)
			sc->nscmax = j + 1;
		nsc->sc = sc;
		nsc->nsid = rp->nsids[i];
		nsc->state = NVME_NSC_STATE_UNATTACHED;
		nsc->idns = req->info->idns;
		bioq_init(&nsc->bioq);
		lockinit(&nsc->lk, "nvnsc", 0, 0);

		nvme_put_request(req);

		j = NVME_FLBAS_SEL_GET(nsc->idns.flbas);
		lbafmt = &nsc->idns.lba_fmt[j];
		nsc->blksize = 1 << lbafmt->sect_size;

		/*
		 * Attach the namespace
		 */
		nvme_disk_attach(nsc);
	}
	kfree(rp, M_NVME);

	sc->admin_func = nvme_admin_state_operating;
	return 1;
}

static
int
nvme_admin_state_operating(nvme_softc_t *sc)
{
	if ((sc->admin_signal & ADMIN_SIG_PROBED) == 0) {
		atomic_set_int(&sc->admin_signal, ADMIN_SIG_PROBED);
		wakeup(&sc->admin_signal);
	}

	return 0;
}

static
int
nvme_admin_state_failed(nvme_softc_t *sc)
{
	if ((sc->admin_signal & ADMIN_SIG_PROBED) == 0) {
		atomic_set_int(&sc->admin_signal, ADMIN_SIG_PROBED);
		wakeup(&sc->admin_signal);
	}

	return 0;
}
