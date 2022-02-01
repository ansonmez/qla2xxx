// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell Fibre Channel HBA Driver
 * Copyright (c)  2018-     Marvell
 */
#include "qla_def.h"
#include "qla_edif.h"

#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <scsi/scsi_tcq.h>


static edif_sa_index_entry_t *qla_edif_sadb_find_sa_index_entry(uint16_t nport_handle,
    struct list_head *sa_list);
static uint16_t qla_edif_sadb_get_sa_index(fc_port_t *fcport,
    struct qla_sa_update_frame *sa_frame);
int qla_edif_sadb_delete_sa_index(fc_port_t *fcport, uint16_t nport_handle,
    uint16_t sa_index);

static int qla_pur_get_pending(scsi_qla_host_t *, fc_port_t *, bsg_job_t *);
extern void qla24xx_process_purex_auth_rjt_iocb(struct scsi_qla_host *, void *);

typedef struct edb_node {
	struct  list_head	list;
	uint32_t		ntype;
	uint32_t		lstate;
	union {
		port_id_t 	plogi_did;
		uint32_t	async;
		port_id_t 	els_sid;
		edif_sa_update_aen_t	sa_aen;
	} u;
} edb_node_t;

static struct els_sub_cmd {
	uint16_t cmd;
	const char *str;
} sc_str[] = {
	{SEND_ELS, "send ELS"},
	{SEND_ELS_REPLY, "send ELS Reply"},
	{PULL_ELS, "retrieve ELS"},
};
const char *sc_to_str(uint16_t cmd)
{
	int i;
	struct els_sub_cmd *e;

	for (i = 0; i < ARRAY_SIZE(sc_str); i++) {
		e = sc_str + i;
		if (cmd == e->cmd)
			return e->str;
	}
	return "unknown";
}

//
// find an edif list entry for an nport_handle
//
edif_list_entry_t *qla_edif_list_find_sa_index(fc_port_t *fcport,
    uint16_t handle)
{

	edif_list_entry_t *entry;
	edif_list_entry_t *tentry;
	struct list_head *indx_list = &fcport->edif.edif_indx_list;

	list_for_each_entry_safe(entry, tentry, indx_list, next) {
		if (entry->handle == handle) {
			return entry;
		}
	}
	return NULL;
}


//
// timeout called when no traffic and delayed rx sa_index delete
//
void qla2x00_sa_replace_iocb_timeout(qla_timer_arg_t t)
{
	edif_list_entry_t *edif_entry = qla_from_timer(edif_entry, t, timer);
	fc_port_t *fcport = edif_entry->fcport;

	struct scsi_qla_host *vha = fcport->vha;
	struct  edif_sa_ctl *sa_ctl;
	uint16_t nport_handle;
	unsigned long flags = 0;

	ql_dbg(ql_dbg_edif, vha, 0x3069,
	    "%s:  nport_handle 0x%x,  SA REPL Delay Timeout, %8phC portid=%06x\n",
	    __func__, edif_entry->handle, fcport->port_name, fcport->d_id.b24);

	//
	// if delete_sa_index is valid then no one has serviced this
	// delayed delete
	//
	spin_lock_irqsave(&fcport->edif.indx_list_lock, flags);

	//
	// delete_sa_index is invalidated when we find the new sa_index in
	// the incomming data stream.  If it is not invalidated then we are
	// still looking for the new sa_index because there is no I/O and we
	// need to just force the rx delete and move on.  Otherwise
	// we could get another rekey which will result in an error 66.
	//
	if (edif_entry->delete_sa_index != INVALID_EDIF_SA_INDEX) {

		uint16_t delete_sa_index = edif_entry->delete_sa_index;
		edif_entry->delete_sa_index = INVALID_EDIF_SA_INDEX;
		nport_handle = edif_entry->handle;
		spin_unlock_irqrestore(&fcport->edif.indx_list_lock, flags);

		sa_ctl = qla_edif_find_sa_ctl_by_index(fcport,
		    delete_sa_index, 0);

		if (sa_ctl) {
			ql_dbg(ql_dbg_edif, vha, 0x3063,
			    "%s: POST SA DELETE TIMEOUT  sa_ctl: %px, delete "
			    "index %d, update index: %d, nport_handle: 0x%x\n",
			    __func__, sa_ctl, delete_sa_index,
			    edif_entry->update_sa_index, nport_handle);

			sa_ctl->flags = EDIF_SA_CTL_FLG_DEL;
			set_bit(EDIF_SA_CTL_REPL, &sa_ctl->state);
			qla_post_sa_replace_work(fcport->vha, fcport,
			    nport_handle, sa_ctl);

		} else {
			ql_dbg(ql_dbg_edif, vha, 0x3063,
			    "%s: POST SA DELETE TIMEOUT  sa_ctl not found "
			    "for delete_sa_index: %d\n",
			    __func__, edif_entry->delete_sa_index);
		}
	} else {
		spin_unlock_irqrestore(&fcport->edif.indx_list_lock, flags);
	}
}


//
// create a new list entry for this nport handle and
// add an sa_update index to the list - called for sa_update
//
static int qla_edif_list_add_sa_update_index(fc_port_t *fcport,
    uint16_t sa_index, uint16_t handle)
{
	edif_list_entry_t *entry;
	unsigned long flags = 0;

	//
	// if the entry exists, then just update the sa_index
	//
	entry = qla_edif_list_find_sa_index(fcport,handle);
	if (entry) {
		entry->update_sa_index = sa_index;
		entry->count = 0;
		return 0;
	}

	//
	// This is the normal path - there should be no existing entry
	// when update is called.  The exception is at startup
	// when update is called for the first two sa_indexes
	// followed by a delete of the first sa_index
	//
	entry = kzalloc((sizeof(edif_list_entry_t)),GFP_ATOMIC);
	if (!entry) {
		return -1;
	}

	INIT_LIST_HEAD(&entry->next);
	entry->handle = handle;
	entry->update_sa_index = sa_index;
	entry->delete_sa_index = INVALID_EDIF_SA_INDEX;
	entry->count = 0;
	entry->flags = 0;
	qla_timer_setup(&entry->timer, qla2x00_sa_replace_iocb_timeout,
	    			0, entry);
	spin_lock_irqsave(&fcport->edif.indx_list_lock, flags);
	list_add_tail(&entry->next, &fcport->edif.edif_indx_list);
	spin_unlock_irqrestore(&fcport->edif.indx_list_lock, flags);
	return 0;
}

//
// remove an entry from the list
//
static void qla_edif_list_delete_sa_index(fc_port_t *fcport, edif_list_entry_t *entry)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&fcport->edif.indx_list_lock, flags);
	list_del(&entry->next);
	spin_unlock_irqrestore(&fcport->edif.indx_list_lock, flags);
}



int qla_post_sa_replace_work(struct scsi_qla_host *vha,
	 fc_port_t *fcport, uint16_t nport_handle, struct edif_sa_ctl *sa_ctl)
{
	struct qla_work_evt *e;

	e = qla2x00_alloc_work(vha, QLA_EVT_SA_REPLACE);
	if (!e)
		return QLA_FUNCTION_FAILED;

	e->u.sa_update.fcport = fcport;
	e->u.sa_update.sa_ctl = sa_ctl;
	e->u.sa_update.nport_handle = nport_handle;
	fcport->flags |= FCF_ASYNC_ACTIVE;
	return qla2x00_post_work(vha, e);
}

static void
qla_edif_sa_ctl_init(scsi_qla_host_t *vha, struct fc_port  *fcport)
{

	ql_dbg(ql_dbg_edif, vha, 0x2058,
		"Init SA_CTL List for fcport - nn %8phN pn %8phN "
		"portid=%02x%02x%02x.\n",
		fcport->node_name, fcport->port_name,
		fcport->d_id.b.domain, fcport->d_id.b.area,
		fcport->d_id.b.al_pa);

	fcport->edif.tx_rekey_cnt = 0;
	fcport->edif.rx_rekey_cnt = 0;

	fcport->edif.tx_bytes = 0;
	fcport->edif.rx_bytes = 0;
}


int qla_bsg_check(scsi_qla_host_t *vha, bsg_job_t *bsg_job, fc_port_t *fcport)
{
	struct extra_auth_els *p;
	struct qla_bsg_auth_els_request *req =
	    (struct qla_bsg_auth_els_request *)bsg_job->request;

	if (!vha->hw->flags.edif_enabled) {
		/* edif support not enabled */
		ql_dbg(ql_dbg_edif, vha, 0x9105,
		    "%s edif not enabled\n", __func__);
		goto done;
	}
	if (vha->e_dbell.db_flags != EDB_ACTIVE) {
		/* doorbell list not enabled */
		ql_dbg(ql_dbg_edif, vha, 0x09102,
		    "%s doorbell not enabled\n", __func__);
		goto done;
	}

	p = &req->e;

	/* Get response */
	if (p->sub_cmd == PULL_ELS) {
		struct qla_bsg_auth_els_reply *rpl =
			(struct qla_bsg_auth_els_reply *)bsg_job->reply;

		qla_pur_get_pending(vha, fcport, bsg_job);

		ql_dbg(ql_dbg_edif, vha, 0x911d,
			"%s %s %8phN sid=%x. xchg %x, nb=%xh bsg ptr %px\n",
			__func__, sc_to_str(p->sub_cmd), fcport->port_name,
			fcport->d_id.b24, rpl->rx_xchg_address,
			rpl->r.reply_payload_rcv_len, bsg_job);

		goto done;
	}
	return 0;

done:

	bsg_job_done(bsg_job, ((struct fc_bsg_reply *)bsg_job->reply)->result,
		((struct fc_bsg_reply *)bsg_job->reply)->reply_payload_rcv_len);
	return -EIO;
}

fc_port_t *
qla2x00_find_fcport_by_pid(scsi_qla_host_t *vha, port_id_t *id)
{
	fc_port_t *f, *tf;

	f = NULL;
	list_for_each_entry_safe(f, tf, &vha->vp_fcports, list) {
		if ((f->flags & FCF_FCSP_DEVICE)) {
			ql_dbg(ql_dbg_edif+ql_dbg_verbose, vha, 0x2058,
			    "Found secure fcport - nn %8phN pn %8phN "
			    "portid=%02x%02x%02x, 0x%x, 0x%x.\n",
			    f->node_name, f->port_name,
			    f->d_id.b.domain, f->d_id.b.area,
			    f->d_id.b.al_pa, f->d_id.b24, id->b24);
			if (f->d_id.b24 == id->b24)
				return f;
		}
	}
	return NULL;
}

int qla2x00_check_rdp_test( uint32_t cmd, uint32_t port)
{
	if (cmd == ELS_COMMAND_RDP && port == 0xFEFFFF)
		return 1;
	else
		return 0;

}

static int
qla_edif_app_check(scsi_qla_host_t *vha, app_id_t appid)
{
	int rval = 0;	// assume failure

	// check that the app is allow/known to the driver

	// TODO: edif: implement key/cert check for permitted apps...

	if (appid.app_vid == 0x73730001) {
		rval = 1;
		ql_dbg(ql_dbg_edif + ql_dbg_verbose, vha, 0x911d, "%s app id ok\n", __func__);
	} else {
		ql_dbg(ql_dbg_edif, vha, 0x911d, "%s app id not ok (%x)",
		    __func__, appid.app_vid);
	}

	return rval;
}

/*
 * reset the session to auth wait.
 */
static void qla_edif_reset_auth_wait(struct fc_port *fcport, int state,
    int waitonly)
{
	int cnt, max_cnt = 4000;
	bool traced = false;

	fcport->keep_nport_handle=1;
	//if (!waitonly && qla_ini_mode_enabled(fcport->vha)) {
	if (!waitonly) {
		qla2x00_set_fcport_disc_state(fcport, state);
		qlt_schedule_sess_for_deletion(fcport);
	} else {
		qla2x00_set_fcport_disc_state(fcport, state);
	}

	ql_dbg(ql_dbg_edif, fcport->vha, 0xf086,
		"%s: waiting for session, max_cnt=%u\n",
		__func__, max_cnt);

	cnt=0;

	if (waitonly) {
		//Marker wait min 10 msecs.
		msleep(50);
		cnt+=50;
	}
	while (1) {
		if (!traced) {
			ql_dbg(ql_dbg_edif, fcport->vha, 0xf086,
			"%s: session sleep.\n",
			__func__);
			traced = true;
		}
		msleep(1);
		cnt++;
		if (waitonly && (fcport->disc_state == state ||
			fcport->disc_state == DSC_LOGIN_COMPLETE))
			break;
		if (fcport->disc_state == DSC_LOGIN_AUTH_PEND)
			break;
		if (cnt > max_cnt)
			break;
	}

	if (!waitonly) {
		ql_dbg(ql_dbg_edif, fcport->vha, 0xf086,
		    "%s: waited for session - %8phC, loopid=%x portid=%06x "
		    "fcport=%px state=%u, cnt=%u\n",
		    __func__,fcport->port_name, fcport->loop_id,
		    fcport->d_id.b24,fcport, fcport->disc_state, cnt);
	} else {
		ql_dbg(ql_dbg_edif, fcport->vha, 0xf086,
		    "%s: waited ONLY for session - %8phC, loopid=%x portid=%06x "
		    "fcport=%px state=%u, cnt=%u\n",
		    __func__,fcport->port_name, fcport->loop_id,
		    fcport->d_id.b24,fcport, fcport->disc_state, cnt);
	}
}

static void
qla_edif_free_sa_ctl(fc_port_t *fcport, struct edif_sa_ctl *sa_ctl,
	int index)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&fcport->edif.sa_list_lock, flags);
	list_del(&sa_ctl->next);
	spin_unlock_irqrestore(&fcport->edif.sa_list_lock, flags);
	if (index >= 512)
		fcport->edif.tx_rekey_cnt--;
	else
		fcport->edif.rx_rekey_cnt--;
	kfree(sa_ctl);
}

//
// return an index to the freepool
//
static void qla_edif_add_sa_index_to_freepool(fc_port_t *fcport, int dir,
    uint16_t sa_index)
{
	void *sa_id_map;
	struct scsi_qla_host *vha = fcport->vha;
	struct qla_hw_data *ha = vha->hw;
	unsigned long flags = 0;
	u16 lsa_index = sa_index;

	ql_dbg(ql_dbg_edif + ql_dbg_verbose, vha, 0x3063,
	    "%s: entry\n", __func__);

	if (dir) {
		sa_id_map = ha->edif_tx_sa_id_map;
		lsa_index -= EDIF_TX_SA_INDEX_BASE;
	} else {
		sa_id_map = ha->edif_rx_sa_id_map;
	}

	spin_lock_irqsave(&ha->sadb_fp_lock, flags);
	clear_bit(lsa_index, sa_id_map);
	spin_unlock_irqrestore(&ha->sadb_fp_lock, flags);
	ql_dbg(ql_dbg_edif, vha, 0x3063,
	    "%s: index %d added to free pool\n", __func__, sa_index);
}

//
// find an release all outstanding sadb sa_indicies
//
void qla2x00_release_all_sadb(struct scsi_qla_host *vha, struct fc_port *fcport)
{
	edif_sa_index_entry_t *entry, *tmp;
	int rx_key_cnt = 0;
	int tx_key_cnt = 0;
	int i, dir;
	struct qla_hw_data *ha = vha->hw;
	edif_list_entry_t *edif_entry;
	struct  edif_sa_ctl *sa_ctl;
	unsigned long flags;

	ql_dbg(ql_dbg_edif + ql_dbg_verbose, vha, 0x3063,
	    "%s: Starting...\n", __func__);

	spin_lock_irqsave(&ha->sadb_lock, flags);

	list_for_each_entry_safe(entry, tmp, &ha->sadb_rx_index_list, next) {
		if (entry->fcport == fcport) {
			list_del(&entry->next);
			spin_unlock_irqrestore(&ha->sadb_lock, flags);

			for (i=0; i<2; i++) {
				if (entry->sa_pair[i].sa_index != INVALID_EDIF_SA_INDEX) {
					if(fcport->loop_id != entry->handle) {
						ql_dbg(ql_dbg_edif, vha, 0x3063,
						    "%s: ** WARNING %d** entry handle: 0x%x, "
						    "fcport nport_handle: 0x%x, sa_index: %d\n",
						    __func__, i, entry->handle,
						    fcport->loop_id,
						    entry->sa_pair[i].sa_index);
					}

					// release the sa_ctl
					//
					sa_ctl = qla_edif_find_sa_ctl_by_index(fcport,
					    entry->sa_pair[i].sa_index , 0);
					if ((sa_ctl != NULL) &&
					    (qla_edif_find_sa_ctl_by_index(fcport, sa_ctl->index,
						0) != NULL)) {
						ql_dbg(ql_dbg_edif, vha, 0x3063,
						    "%s: freeing sa_ctl for index %d\n",
						    __func__, sa_ctl->index);
						qla_edif_free_sa_ctl(fcport, sa_ctl, sa_ctl->index);
					} else {
						ql_dbg(ql_dbg_edif, vha, 0x3063,
						    "%s: sa_ctl NOT freed, sa_ctl: %px\n",
						    __func__, sa_ctl);

					}

					// Release the index
					//
					ql_dbg(ql_dbg_edif, vha, 0x3063,
					    "%s: freeing sa_index %d, nph: 0x%x\n",
					    __func__, entry->sa_pair[i].sa_index, entry->handle);

					dir = (entry->sa_pair[i].sa_index < EDIF_TX_SA_INDEX_BASE) ? 0 : 1;
					qla_edif_add_sa_index_to_freepool(fcport, dir,
					    entry->sa_pair[i].sa_index);

					//Delete timer on RX

					edif_entry = qla_edif_list_find_sa_index(fcport, entry->handle);
					if (edif_entry) {
						ql_dbg(ql_dbg_edif, vha, 0x5033,
						    "%s: removing edif_entry %px, update_sa_index: 0x%x, delete_sa_index: 0x%x\n",
						    __func__, edif_entry, edif_entry->update_sa_index  , edif_entry->delete_sa_index);
						qla_edif_list_delete_sa_index(fcport, edif_entry);
						// valid delete_sa_index indicates there is a rx delayed delete queued
						if (edif_entry->delete_sa_index != INVALID_EDIF_SA_INDEX) {
							del_timer(&edif_entry->timer);

							// build and send the aen
							fcport->edif.rx_sa_set = 1;
							fcport->edif.rx_sa_pending = 0;
							qla_edb_eventcreate(vha,
							    VND_CMD_AUTH_STATE_SAUPDATE_COMPL,
							    QL_VND_SA_STAT_SUCCESS,
							    QL_VND_RX_SA_KEY, fcport);
						}
						ql_dbg(ql_dbg_edif, vha, 0x5033,
						    "%s: releasing edif_entry %px, update_sa_index: 0x%x, delete_sa_index: 0x%x\n",
						    __func__, edif_entry, edif_entry->update_sa_index  , edif_entry->delete_sa_index);

						kfree(edif_entry);
					}

					rx_key_cnt++;
				}
			}
			kfree(entry);
			spin_lock_irqsave(&ha->sadb_lock, flags);
			break;
		}
	}

	list_for_each_entry_safe(entry, tmp, &ha->sadb_tx_index_list, next) {
		if (entry->fcport == fcport) {
			list_del(&entry->next);
			spin_unlock_irqrestore(&ha->sadb_lock, flags);

			for (i=0; i<2; i++) {
				if (entry->sa_pair[i].sa_index != INVALID_EDIF_SA_INDEX) {

					if(fcport->loop_id != entry->handle) {
						ql_dbg(ql_dbg_edif, vha, 0x3063,
						    "%s: ** WARNING %i** entry handle: 0x%x, "
						    "fcport nport_handle: 0x%x, sa_index: %d\n",
						    __func__,i+2, entry->handle,
						    fcport->loop_id,
						    entry->sa_pair[i].sa_index );
					}

					// release the sa_ctl
					//
					sa_ctl = qla_edif_find_sa_ctl_by_index(fcport,
					    entry->sa_pair[i].sa_index , SAU_FLG_TX);
					if ((sa_ctl != NULL) &&
					    (qla_edif_find_sa_ctl_by_index(fcport, sa_ctl->index,
						SAU_FLG_TX) != NULL)) {
						ql_dbg(ql_dbg_edif, vha, 0x3063,
						    "%s: freeing sa_ctl for index %d\n",
						    __func__, sa_ctl->index);
						qla_edif_free_sa_ctl(fcport, sa_ctl, sa_ctl->index);
					} else {
						ql_dbg(ql_dbg_edif, vha, 0x3063,
						    "%s: sa_ctl NOT freed, sa_ctl: %px\n",
						    __func__, sa_ctl);

					}

					// release the index
					//
					ql_dbg(ql_dbg_edif, vha, 0x3063,
					    "%s: freeing sa_index %d, nph: 0x%x\n",
					    __func__, entry->sa_pair[i].sa_index, entry->handle);

					dir = (entry->sa_pair[i].sa_index < EDIF_TX_SA_INDEX_BASE) ? 0 : 1;
					qla_edif_add_sa_index_to_freepool(fcport, dir,
					    entry->sa_pair[i].sa_index);

					tx_key_cnt++;
				}
			}
			kfree(entry);
			spin_lock_irqsave(&ha->sadb_lock, flags);
			break;
		}
	}
	spin_unlock_irqrestore(&ha->sadb_lock, flags);
	ql_dbg(ql_dbg_edif, vha, 0x3063,
	    "%s: %d rx_keys released, %d tx_keys released\n",
	    __func__, rx_key_cnt, tx_key_cnt);
}


/*
 * event that the app has started.  Clear and start doorbell
 */
static int
qla_edif_app_start(scsi_qla_host_t *vha, bsg_job_t *bsg_job)
{
	int32_t			rval = 0;
	struct fc_bsg_reply	*bsg_reply = bsg_job->reply;
	struct app_start 	appstart;
	struct app_start_reply 	appreply;
	struct fc_port  *fcport, *tf;

	ql_dbg(ql_dbg_edif, vha, 0x911d, "%s app start\n", __func__);

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, &appstart,
	    sizeof(struct app_start));

	ql_dbg(ql_dbg_edif, vha, 0x911d, "%s app_vid=%x app_start_flags %x\n",
	     __func__, appstart.app_info.app_vid,appstart.app_start_flags);

	vha->edif_prli_timeout = (appstart.prli_to > EDIF_TO_MIN ?
	    appstart.prli_to : EDIF_PRLI_TO);
	vha->edif_kshred_timeout = (appstart.key_shred > EDIF_TO_MIN ?
	    appstart.key_shred : EDIF_KSHRED_TO);
	ql_dbg(ql_dbg_edif, vha, 0x911e,
	    "%s: PRLI Timeout %d, KSHRED Timeout %d\n",
	    __func__, vha->edif_prli_timeout, vha->edif_kshred_timeout);

	if (vha->e_dbell.db_flags != EDB_ACTIVE) {
		// mark doorbell as active since an app is now present
		vha->e_dbell.db_flags = EDB_ACTIVE;
	} else {
		ql_dbg(ql_dbg_edif, vha, 0x911e, "%s doorbell already active\n",
		     __func__);
	}

	if (N2N_TOPO(vha->hw)) {
		if (vha->hw->flags.n2n_fw_acc_sec)
			set_bit(N2N_LINK_RESET, &vha->dpc_flags);
		else
			set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
		qla2xxx_wake_dpc(vha);
	} else {
		list_for_each_entry_safe(fcport, tf, &vha->vp_fcports, list) {
			if ((fcport->flags & FCF_FCSP_DEVICE)) {
				ql_dbg(ql_dbg_edif, vha, 0x2058,
					"FCSP - nn %8phN pn %8phN "
					"portid=%02x%02x%02x.\n",
					fcport->node_name, fcport->port_name,
					fcport->d_id.b.domain, fcport->d_id.b.area,
					fcport->d_id.b.al_pa);
				ql_dbg(ql_dbg_edif, vha, 0xf084,
					"%s: se_sess %px / sess %px from port %8phC "
					"loop_id %#04x s_id %02x:%02x:%02x logout %d "
					"keep %d els_logo %d disc state %d auth state %d"
					"stop state %d\n",
					__func__, fcport->se_sess, fcport, fcport->port_name,
					fcport->loop_id, fcport->d_id.b.domain,
					fcport->d_id.b.area, fcport->d_id.b.al_pa,
					fcport->logout_on_delete, fcport->keep_nport_handle,
					fcport->send_els_logo, fcport->disc_state,
					fcport->edif.auth_state, fcport->edif.app_stop);

				if (atomic_read(&vha->loop_state) == LOOP_DOWN) {
					break;
				}
				if (!(fcport->flags & FCF_FCSP_DEVICE))
					continue;

				fcport->edif.app_started = 1;
				if((fcport->edif.app_stop) ||
					(fcport->disc_state != DSC_LOGIN_COMPLETE &&
					 fcport->disc_state != DSC_LOGIN_PEND
					 && fcport->disc_state != DSC_DELETED )) {
					/* no activity */
					fcport->edif.app_stop = 0;

					ql_dbg(ql_dbg_edif, vha, 0x911e,
						"%s wwpn %8phC calling qla_edif_reset_auth_wait"
						"\n", __func__, fcport->port_name);
					fcport->edif.app_sess_online = 1;
					qla_edif_reset_auth_wait(fcport, DSC_LOGIN_PEND, 0);
				}
			}
			qla_edif_sa_ctl_init(vha, fcport);
		}
	}

	if (vha->pur_cinfo.enode_flags != ENODE_ACTIVE) {
		// mark as active since an app is now present
		vha->pur_cinfo.enode_flags = ENODE_ACTIVE;
	} else {
		ql_dbg(ql_dbg_edif, vha, 0x911f, "%s enode already active\n",
		     __func__);
	}

	appreply.host_support_edif = vha->hw->flags.edif_enabled;
	appreply.edif_enode_active = vha->pur_cinfo.enode_flags;
	appreply.edif_edb_active = vha->e_dbell.db_flags;

	bsg_job->reply_len = sizeof(struct fc_bsg_reply) +
	    sizeof(app_start_reply_t);

	SET_DID_STATUS(bsg_reply->result, DID_OK);

	sg_copy_from_buffer(bsg_job->reply_payload.sg_list,
	    bsg_job->reply_payload.sg_cnt, &appreply,
	    sizeof(app_start_reply_t));

	ql_dbg(ql_dbg_edif, vha, 0x911d,
	    "%s app start completed with 0x%x\n",
	    __func__, rval);

	return rval;
}

/*
 * notification from the app that the app is stopping.
 * actions:	stop and doorbell
 *		stop and clear enode
 */
static int
qla_edif_app_stop(scsi_qla_host_t *vha, bsg_job_t *bsg_job)
{
	int32_t                 rval = 0;
	struct app_stop         appstop;
	struct fc_bsg_reply     *bsg_reply = bsg_job->reply;
	struct fc_port  *fcport, *tf;

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, &appstop,
	    sizeof(struct app_stop));

	ql_dbg(ql_dbg_edif, vha, 0x911d, "%s Stopping APP: app_vid=%x\n",
	    __func__, appstop.app_info.app_vid);

	// Call db stop and enode stop functions

	// if we leave this running short waits are operational < 16 secs
	qla_enode_stop(vha);        // stop enode
	qla_edb_stop(vha);          // stop db

	list_for_each_entry_safe(fcport, tf, &vha->vp_fcports, list) {
		if (!(fcport->flags & FCF_FCSP_DEVICE))
			continue;

		if (fcport->flags & FCF_FCSP_DEVICE) {
			ql_dbg(ql_dbg_edif, vha, 0x2058,
			    "FCSP - nn %8phN pn %8phN "
			    "portid=%02x%02x%02x.\n",
			    fcport->node_name, fcport->port_name,
			    fcport->d_id.b.domain, fcport->d_id.b.area,
			    fcport->d_id.b.al_pa);
			ql_dbg(ql_dbg_edif, vha, 0xf084,
			    "%s: se_sess %px / sess %px from port %8phC loop_id %#04x"
			    " s_id %02x:%02x:%02x logout %d keep %d els_logo %d\n",
			    __func__, fcport->se_sess, fcport,
			    fcport->port_name, fcport->loop_id,
			    fcport->d_id.b.domain, fcport->d_id.b.area,
			    fcport->d_id.b.al_pa, fcport->logout_on_delete,
			    fcport->keep_nport_handle, fcport->send_els_logo);


			if (atomic_read(&vha->loop_state) == LOOP_DOWN) {
				break;
			}

			fcport->edif.app_stop = APP_STOPPING;
			ql_dbg(ql_dbg_edif, vha, 0x911e,
				"%s wwpn %8phC calling qla_edif_reset_auth_wait"
				"\n", __func__, fcport->port_name);

			fcport->send_els_logo = 1;
			qlt_schedule_sess_for_deletion(fcport);

			//qla_edif_flush_sa_ctl_lists(fcport);
			fcport->edif.app_started = 0;
		}
	}

	bsg_job->reply_len = sizeof(struct fc_bsg_reply);
	SET_DID_STATUS(bsg_reply->result, DID_OK);

	// no return interface to app - it assumes we cleaned up ok

	return rval;
}

static int
qla_edif_app_chk_sa_update(scsi_qla_host_t *vha, fc_port_t *fcport,
    struct app_plogi_reply *appplogireply)
{
	int	ret = 0;

	fcport->edif.db_sent = 0;
	if (!(fcport->edif.rx_sa_set && fcport->edif.tx_sa_set)) {
		ql_dbg(ql_dbg_edif, vha, 0x911e, "%s: wwpn %8phC"
		    "Both SA indexes has not been SET TX %d, RX %d. \n",
		    __func__, fcport->port_name, fcport->edif.tx_sa_set,
		    fcport->edif.rx_sa_set);
		appplogireply->prli_status = 0;
		ret = 1;
	} else  {
		ql_dbg(ql_dbg_edif, vha, 0x911e,
		    "%s wwpn %8phC Both SA(s) updated. \n", __func__,
		    fcport->port_name);
		fcport->edif.rx_sa_set = fcport->edif.tx_sa_set = 0;
		fcport->edif.rx_sa_pending = fcport->edif.tx_sa_pending = 0;
		appplogireply->prli_status = 1;
	}
	return ret;
}

/*
 * event that the app has approved plogi to complete (e.g., finish
 * up with prli
 */
static int
qla_edif_app_authok(scsi_qla_host_t *vha, bsg_job_t *bsg_job)
{
	int32_t			rval = 0;
	struct auth_complete_cmd_t appplogiok;
	struct app_plogi_reply	appplogireply = {0};
	struct fc_bsg_reply	*bsg_reply = bsg_job->reply;
	fc_port_t		*fcport = NULL;
	port_id_t		portid = {0};
	// port_id_t		portid = {0x10100};
	// int i;

	//ql_dbg(ql_dbg_edif, vha, 0x911d, "%s app auth ok\n", __func__);

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, &appplogiok,
	    sizeof(struct auth_complete_cmd_t));

	switch (appplogiok.type) {
	case PL_TYPE_WWPN:
		fcport = qla2x00_find_fcport_by_wwpn(vha,
		    appplogiok.u.wwpn, 0);
		if (fcport == NULL)
			ql_dbg(ql_dbg_edif, vha, 0x911d,
			    "%s wwpn lookup failed: %8phC\n",
			    __func__, appplogiok.u.wwpn);
		break;
	case PL_TYPE_DID:
		fcport = qla2x00_find_fcport_by_pid(vha, &appplogiok.u.d_id);
		if (fcport == NULL)
			ql_dbg(ql_dbg_edif, vha, 0x911d,
			    "%s d_id lookup failed: %x\n", __func__,
			    portid.b24);
		break;
	default:
		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s undefined type: %x\n", __func__,
		    appplogiok.type);
		break;
	}

	if (fcport == NULL) {
		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		goto errstate_exit;
	}

	// TODO: edif: Kill prli timer...

	/* if port is online then this is a REKEY operation */
	/* Only do sa update checking */
	if (atomic_read(&fcport->state) == FCS_ONLINE){
		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s Skipping PRLI complete based on rekey\n", __func__);
		appplogireply.prli_status = 1;
		SET_DID_STATUS(bsg_reply->result, DID_OK);
		qla_edif_app_chk_sa_update(vha, fcport, &appplogireply);
		goto errstate_exit;
	}

	// make sure in AUTH_PENDING or else reject
	if (fcport->disc_state != DSC_LOGIN_AUTH_PEND) {
		// if (atomic_read(&fcport->state) != FCS_ONLINE) {
			ql_dbg(ql_dbg_edif, vha, 0x911e, "%s wwpn %8phC is not "
		    "in auth pending state (%x)\n", __func__,
		    fcport->port_name, fcport->disc_state);
		// }
		// SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		/* App can't fix us - initaitor will retry */
		SET_DID_STATUS(bsg_reply->result, DID_OK);
		appplogireply.prli_status = 0;
		goto errstate_exit;
	}

	SET_DID_STATUS(bsg_reply->result, DID_OK);
	appplogireply.prli_status = 1;
	fcport->edif.db_sent = 0;
	fcport->edif.authok = 1;
	if (!(fcport->edif.rx_sa_set && fcport->edif.tx_sa_set)) {
		ql_dbg(ql_dbg_edif, vha, 0x911e, "%s: wwpn %8phC"
	    "Both SA indexes has not been SET TX %d, RX %d. \n", __func__,
	    fcport->port_name, fcport->edif.tx_sa_set, fcport->edif.rx_sa_set);
		SET_DID_STATUS(bsg_reply->result, DID_OK);
		appplogireply.prli_status = 0;
		goto errstate_exit;

	} else {
		ql_dbg(ql_dbg_edif, vha, 0x911e, "%s wwpn %8phC"
		    " Both SA(s) updated. \n", __func__,
		    fcport->port_name);
		fcport->edif.rx_sa_set = fcport->edif.tx_sa_set = 0;
		fcport->edif.rx_sa_pending = fcport->edif.tx_sa_pending = 0;
	}
	//qla_edif_app_chk_sa_update(vha, fcport, &appplogireply);
	// TODO: edif: check this - discovery state changed by prli work?
	if (qla_ini_mode_enabled(vha)) {
		ql_dbg(ql_dbg_edif, vha, 0x911e,
		    "%s AUTH complete - RESUME with prli for wwpn %8phC \n",
		    __func__, fcport->port_name);
		qla_edif_reset_auth_wait(fcport, DSC_LOGIN_PEND, 1);
		//qla2x00_set_fcport_disc_state(fcport, DSC_LOGIN_PEND);
		qla24xx_post_prli_work(vha, fcport);
	}

errstate_exit:

	bsg_job->reply_len = sizeof(struct fc_bsg_reply);
	sg_copy_from_buffer(bsg_job->reply_payload.sg_list,
	    bsg_job->reply_payload.sg_cnt, &appplogireply,
	    sizeof(app_plogi_reply_t));

	return rval;
}

/*
 * event that the app has failed the plogi. logout the device (tbd)
 */
static int
qla_edif_app_authfail(scsi_qla_host_t *vha, bsg_job_t *bsg_job)
{
	int32_t			rval = 0;
	struct auth_complete_cmd_t appplogifail;
	struct fc_bsg_reply	*bsg_reply = bsg_job->reply;
	fc_port_t		*fcport = NULL;
	port_id_t		portid = {0};

	ql_dbg(ql_dbg_edif, vha, 0x911d, "%s app auth fail\n", __func__);

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, &appplogifail,
	    sizeof(struct auth_complete_cmd_t));

	// TODO: edif: app has failed this plogi.  Inform driver to take any action
	// (if any).

	switch (appplogifail.type) {
	case PL_TYPE_WWPN:
		fcport = qla2x00_find_fcport_by_wwpn(vha,
		    appplogifail.u.wwpn, 0);
		SET_DID_STATUS(bsg_reply->result, DID_OK);
		break;
	case PL_TYPE_DID:
		fcport = qla2x00_find_fcport_by_pid(vha, &appplogifail.u.d_id);
		if (fcport == NULL)
			ql_dbg(ql_dbg_edif, vha, 0x911d,
			    "%s d_id lookup failed: %x\n", __func__,
			    portid.b24);
		SET_DID_STATUS(bsg_reply->result, DID_OK);
		break;
	default:
		ql_dbg(ql_dbg_edif, vha, 0x911e,
		    "%s undefined type: %x\n", __func__,
		    appplogifail.type);
		bsg_job->reply_len = sizeof(struct fc_bsg_reply);
		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		rval = -1;
		break;
	}

	ql_dbg(ql_dbg_edif, vha, 0x911d,
	    "%s fcport is 0x%px\n", __func__, fcport);

	if (fcport != NULL) {
		/* set/reset edif values and flags */
		ql_dbg(ql_dbg_edif, vha, 0x911e,
			    "%s reset the auth process - %8phC, loopid=%x portid=%06x.\n", __func__, 
			    fcport->port_name, fcport->loop_id, fcport->d_id.b24
			    );

		if (qla_ini_mode_enabled(fcport->vha)) {
			fcport->send_els_logo = 1;
			qla_edif_reset_auth_wait(fcport, DSC_LOGIN_PEND, 0);
		}
		//fcport->explicit_logout = 1;
		//qla_edif_reset_auth_wait(fcport, DSC_LOGIN_PEND, 0);
	}

	return rval;
}

/*
 * event that the app has has sent down new rekey trigger parameters
 */
static int
qla_edif_app_rekey(scsi_qla_host_t *vha, bsg_job_t *bsg_job)
{
	int32_t			rval = 0;
	struct app_rekey_cfg	app_recfg;
	struct fc_bsg_reply	*bsg_reply = bsg_job->reply;
	struct fc_port		*fcport= NULL, *tf;
	port_id_t		did;

	SET_DID_STATUS(bsg_reply->result, DID_OK);

	ql_dbg(ql_dbg_edif, vha, 0x911d, "%s app rekey config\n", __func__);

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, &app_recfg,
	    sizeof(struct app_rekey_cfg));

	did = app_recfg.d_id;

	list_for_each_entry_safe(fcport, tf, &vha->vp_fcports, list) {
		if (fcport->edif.enable) {
			// this device has edif support
			if ((did.b24 == 0) ||
			    (did.b24 == fcport->d_id.b24)) {

				if (app_recfg.rekey_mode == RECFG_TIME) {
					fcport->edif.rekey_mode = 1;
					fcport->edif.reload_value =
					    fcport->edif.rekey =
					    app_recfg.rky_units.time;
				} else if (app_recfg.rekey_mode == RECFG_BYTES) {
					fcport->edif.rekey_mode = 0;
					fcport->edif.reload_value =
					    fcport->edif.rekey =
					    app_recfg.rky_units.bytes;
				} else {
					// invalid rekey mode passed
					ql_dbg(ql_dbg_edif, vha, 0x911d,
					    "%s inavlid rekey passed (%x)\n",
					     __func__, app_recfg.rekey_mode);
					SET_DID_STATUS(bsg_reply->result, DID_ERROR);
					break;
				}

				// dpc to check and generate db event to app
				// if (app_recfg.force == 1)
					// fcport->edif.new_sa = 1;

				if (did.b24 != 0)
					break;
			}
		}
	}

	return rval;
}

/*
 * event that the app needs fc port info (either all or individual d_id)
 */
static int
qla_edif_app_getfcinfo(scsi_qla_host_t *vha, bsg_job_t *bsg_job)
{
	int32_t			rval = 0;
	int32_t			num_cnt = 1;
	struct fc_bsg_reply	*bsg_reply = bsg_job->reply;
	app_pinfo_req_t		app_req;
	app_pinfo_reply_t	*app_reply;
	port_id_t		tdid;

	ql_dbg(ql_dbg_edif, vha, 0x911d, "%s app get fcinfo\n", __func__);

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, &app_req,
	    sizeof(struct app_pinfo_req));

	num_cnt =  app_req.num_ports;	// num of ports alloc'd by app

	if ((app_reply =
	    (app_pinfo_reply_t *)kzalloc((sizeof(app_pinfo_reply_t) +
	    sizeof(app_pinfo_t)*num_cnt), GFP_KERNEL)) == NULL) {
		ql_dbg(ql_dbg_edif, vha, 0x911d, "%s unable to alloc space\n",
		    __func__);
		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		rval = -1;
	} else {
		struct fc_port	*fcport = NULL, *tf;
		uint32_t	pcnt = 0;

		list_for_each_entry_safe(fcport, tf, &vha->vp_fcports, list) {

			if (!(fcport->flags & FCF_FCSP_DEVICE))
				continue;

			tdid = app_req.remote_pid;

			ql_dbg(ql_dbg_edif, vha, 0x2058,
			    "APP request entry - portid=%02x%02x%02x.\n",
			    tdid.b.domain, tdid.b.area, tdid.b.al_pa);

			/* Ran out of space */
			if (pcnt > app_req.num_ports)
				break;

			if ((tdid.b24 != 0) && (tdid.b24 != fcport->d_id.b24))
				continue;

			// we are intersted in this one

			app_reply->ports[pcnt].rekey_mode =
				fcport->edif.rekey_mode;
			app_reply->ports[pcnt].rekey_count =
				fcport->edif.rekey_cnt;
			app_reply->ports[pcnt].rekey_config_value =
				fcport->edif.reload_value;
			app_reply->ports[pcnt].rekey_consumed_value =
				fcport->edif.rekey;

			app_reply->ports[pcnt].remote_type =
				VND_CMD_RTYPE_UNKNOWN;
			if (fcport->port_type & (FCT_NVME_TARGET|FCT_TARGET))
				app_reply->ports[pcnt].remote_type |=
					VND_CMD_RTYPE_TARGET;
			if (fcport->port_type & (FCT_NVME_INITIATOR|FCT_INITIATOR))
				app_reply->ports[pcnt].remote_type |=
					VND_CMD_RTYPE_INITIATOR;

			app_reply->ports[pcnt].remote_pid = fcport->d_id;

			ql_dbg(ql_dbg_edif, vha, 0x2058,
			    "Found FC_SP fcport - nn %8phN pn %8phN "
			    "pcnt %d portid=%06x secure %d.\n",
			    fcport->node_name, fcport->port_name, pcnt,
			    fcport->d_id.b24, fcport->flags & FCF_FCSP_DEVICE);

			switch(fcport->edif.auth_state){
			case VND_CMD_AUTH_STATE_ELS_RCVD:
				if (fcport->disc_state == DSC_LOGIN_AUTH_PEND) {
					fcport->edif.auth_state = VND_CMD_AUTH_STATE_NEEDED;
					app_reply->ports[pcnt].auth_state =
						VND_CMD_AUTH_STATE_NEEDED;
				} else {
					app_reply->ports[pcnt].auth_state =
						VND_CMD_AUTH_STATE_ELS_RCVD;
				}
				break;
			default:
				app_reply->ports[pcnt].auth_state = fcport->edif.auth_state;
				break;
			}

			memcpy(app_reply->ports[pcnt].remote_wwpn,
			    fcport->port_name, 8);

			app_reply->ports[pcnt].remote_state =
				(atomic_read(&fcport->state) ==
				    FCS_ONLINE ? 1 : 0);

			pcnt++;

			if (tdid.b24 != 0)
				break;  // found the one req'd
		}
		app_reply->port_count = pcnt;
		SET_DID_STATUS(bsg_reply->result, DID_OK);
	}

	sg_copy_from_buffer(bsg_job->reply_payload.sg_list,
	    bsg_job->reply_payload.sg_cnt, app_reply,
	    sizeof(app_pinfo_reply_t)+sizeof(app_pinfo_t)*num_cnt);

	kfree(app_reply);

	return rval;
}

/*
 * return edif stats (TBD) to app
 */
static int32_t
qla_edif_app_getstats(scsi_qla_host_t *vha, bsg_job_t *bsg_job)
{
	int32_t			rval = 0;
	struct fc_bsg_reply	*bsg_reply = bsg_job->reply;
	uint32_t ret_size, size;

	app_sinfo_req_t		app_req;
	app_stats_reply_t	*app_reply;

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, &app_req,
	    sizeof(struct app_sinfo_req));
	if (app_req.num_ports == 0) {
		ql_dbg(ql_dbg_async, vha, 0x911d,
		   "%s app did not indicate number of ports to return\n",
		    __func__);
		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		rval = -1;
	}

	size = sizeof(app_stats_reply_t) +
	    (sizeof(app_sinfo_t) * app_req.num_ports);

	if (size > bsg_job->reply_payload.payload_len)
		ret_size = bsg_job->reply_payload.payload_len;
	else
		ret_size = size;

	app_reply = (app_stats_reply_t *)kzalloc(size, GFP_KERNEL);
	if (app_reply == NULL) {
		ql_dbg(ql_dbg_edif, vha, 0x911d, "%s unable to alloc space\n",
		    __func__);
		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		rval = -1;
	} else {
		struct fc_port	*fcport = NULL, *tf;
		uint32_t	pcnt = 0;

		list_for_each_entry_safe(fcport, tf, &vha->vp_fcports, list) {
			if (fcport->edif.enable) {

				if (pcnt > app_req.num_ports)
					break;

				app_reply->elem[pcnt].rekey_mode =
				    fcport->edif.rekey_mode ? RECFG_TIME : RECFG_BYTES;
				app_reply->elem[pcnt].rekey_count =
				    fcport->edif.rekey_cnt;
				app_reply->elem[pcnt].tx_bytes =
				    fcport->edif.tx_bytes;
				app_reply->elem[pcnt].rx_bytes =
				    fcport->edif.rx_bytes;

				memcpy(app_reply->elem[pcnt].remote_wwpn,
				    fcport->port_name, 8);

				pcnt++;

			}
		}
		app_reply->elem_count = pcnt;
		SET_DID_STATUS(bsg_reply->result, DID_OK);
	}

	bsg_reply->reply_payload_rcv_len =
	    sg_copy_from_buffer(bsg_job->reply_payload.sg_list,
	       bsg_job->reply_payload.sg_cnt, app_reply, ret_size);

	kfree(app_reply);

	return rval;
}

int32_t
qla_edif_app_mgmt(bsg_job_t *bsg_job)
{
	struct fc_bsg_request	*bsg_request = bsg_job->request;
	struct fc_bsg_reply	*bsg_reply = bsg_job->reply;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
	scsi_qla_host_t		*vha = shost_priv(host);
	struct app_id		appcheck;
	bool done = true;
	int32_t         rval = 0;
	uint32_t	vnd_sc = bsg_request->rqst_data.h_vendor.vendor_cmd[1];

	ql_dbg(ql_dbg_edif, vha, 0x911d, "%s vnd subcmd=%x\n",
	    __func__, vnd_sc);

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, &appcheck,
	    sizeof(struct app_id));

	if (!vha->hw->flags.edif_enabled ||
		test_bit(VPORT_DELETE, &vha->dpc_flags)) {
		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s edif not enabled or vp delete. bsg ptr done %px\n",
		    __func__, bsg_job);

		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		goto done;
	}

	if (qla_edif_app_check(vha, appcheck) == 0) {
		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s app checked failed.\n",
		    __func__);

		bsg_job->reply_len = sizeof(struct fc_bsg_reply);
		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		goto done;
	}

	switch (vnd_sc) {
	case QL_VND_SC_SA_UPDATE:
		done = false;
		rval = qla24xx_sadb_update(bsg_job);
		break;
	case QL_VND_SC_APP_START:
		rval = qla_edif_app_start(vha, bsg_job);
		break;
	case QL_VND_SC_APP_STOP:
		rval = qla_edif_app_stop(vha, bsg_job);
		break;
	case QL_VND_SC_AUTH_OK:
		rval = qla_edif_app_authok(vha, bsg_job);
		break;
	case QL_VND_SC_AUTH_FAIL:
		rval = qla_edif_app_authfail(vha, bsg_job);
		break;
	case QL_VND_SC_REKEY_CONFIG:
		rval = qla_edif_app_rekey(vha, bsg_job);
		break;
	case QL_VND_SC_GET_FCINFO:
		rval = qla_edif_app_getfcinfo(vha, bsg_job);
		break;
	case QL_VND_SC_GET_STATS:
		rval = qla_edif_app_getstats(vha, bsg_job);
		break;
	default:
		ql_dbg(ql_dbg_edif, vha, 0x911d, "%s unknown cmd=%x\n",
		    __func__,
		    bsg_request->rqst_data.h_vendor.vendor_cmd[1]);
		rval = EXT_STATUS_INVALID_PARAM;
		bsg_job->reply_len = sizeof(struct fc_bsg_reply);
		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		break;
	}

done:
	if (done) {
		ql_dbg(ql_dbg_user, vha, 0x7009,
		    "%s: %d  bsg ptr done %px\n", __func__, __LINE__, bsg_job);
		bsg_job_done(bsg_job, bsg_reply->result,bsg_reply->reply_payload_rcv_len);
	}

	return rval;
}


static struct edif_sa_ctl *
qla_edif_add_sa_ctl(fc_port_t *fcport, struct qla_sa_update_frame *sa_frame,
	int dir)
{
	struct	edif_sa_ctl *sa_ctl;
	struct qla_sa_update_frame *sap;
	int	index = sa_frame->fast_sa_index;
	unsigned long flags = 0;

	if ((sa_ctl = (struct edif_sa_ctl *)
	    kzalloc(sizeof(struct edif_sa_ctl), GFP_KERNEL)) == NULL) {
		/* couldn't get space */
		ql_dbg(ql_dbg_edif, fcport->vha, 0x9100,
		    "unable to allocate SA CTL\n");
		return NULL;
	}

	// need to allocate sa_index here and save it
	// in both sa_ctl->index and sa_frame->fast_sa_index;
	// If alloc fails then delete sa_ctl and return NULL


	INIT_LIST_HEAD(&sa_ctl->next);
	sap = &sa_ctl->sa_frame;
	*sap = *sa_frame;
	sa_ctl->index = index;
	sa_ctl->fcport = fcport;
	sa_ctl->flags = 0;
	sa_ctl->state = 0L;
	ql_dbg(ql_dbg_edif, fcport->vha, 0x9100,
	    "%s: Added sa_ctl %px, index %d, state 0x%lx\n",
	    __func__, sa_ctl, sa_ctl->index, sa_ctl->state);
	spin_lock_irqsave(&fcport->edif.sa_list_lock, flags);
	if (dir == SAU_FLG_TX)
		list_add_tail(&sa_ctl->next, &fcport->edif.tx_sa_list);
	else
		list_add_tail(&sa_ctl->next, &fcport->edif.rx_sa_list);
	spin_unlock_irqrestore(&fcport->edif.sa_list_lock, flags);
	return sa_ctl;
}

void
qla_edif_flush_sa_ctl_lists(fc_port_t *fcport)
{
	struct edif_sa_ctl *sa_ctl, *tsa_ctl;
	unsigned long flags = 0;

	spin_lock_irqsave(&fcport->edif.sa_list_lock, flags);

	list_for_each_entry_safe(sa_ctl, tsa_ctl, &fcport->edif.tx_sa_list,
	    next) {
		list_del(&sa_ctl->next);
		kfree(sa_ctl);
	}

	list_for_each_entry_safe(sa_ctl, tsa_ctl, &fcport->edif.rx_sa_list,
	    next) {
		list_del(&sa_ctl->next);
		kfree(sa_ctl);
	}

	spin_unlock_irqrestore(&fcport->edif.sa_list_lock, flags);
}

struct edif_sa_ctl *
qla_edif_find_sa_ctl_by_index(fc_port_t *fcport, int index, int dir)
{
	struct edif_sa_ctl *sa_ctl, *tsa_ctl;
	struct list_head *sa_list;

	if (dir == SAU_FLG_TX)
		sa_list = &fcport->edif.tx_sa_list;
	else
		sa_list = &fcport->edif.rx_sa_list;
	list_for_each_entry_safe(sa_ctl, tsa_ctl, sa_list, next) {
		if (test_bit(EDIF_SA_CTL_USED, &sa_ctl->state) &&
		    sa_ctl->index == index)
			return sa_ctl;
	}
	return NULL;
}

//
// add the sa to the correct list
//
static int
qla24xx_check_sadb_avail_slot(bsg_job_t *bsg_job, fc_port_t *fcport,
	struct qla_sa_update_frame *sa_frame)
{
	struct 	edif_sa_ctl *sa_ctl = NULL;
	int dir;
	uint16_t sa_index;

	dir = (sa_frame->flags & SAU_FLG_TX);

	//
	// map the spi to an sa_index
	//
	sa_index = qla_edif_sadb_get_sa_index(fcport, sa_frame);
	if (sa_index == RX_DELETE_NO_EDIF_SA_INDEX ) {

		// process rx delete
		ql_dbg(ql_dbg_edif, fcport->vha, 0x3063,
		    "%s: rx delete for nport_handle 0x%x, spi 0x%x, no entry found, "
		    "returning good bsg and aen status\n",
		    __func__,fcport->loop_id, sa_frame->spi );

		// build and send the aen
		fcport->edif.rx_sa_set = 1;
		fcport->edif.rx_sa_pending = 0;
		qla_edb_eventcreate(fcport->vha, VND_CMD_AUTH_STATE_SAUPDATE_COMPL,
		    QL_VND_SA_STAT_SUCCESS,
		    QL_VND_RX_SA_KEY, fcport);

		// force a return of good bsg status;
		return RX_DELETE_NO_EDIF_SA_INDEX ;

	} else if (sa_index == INVALID_EDIF_SA_INDEX) {

		ql_dbg(ql_dbg_edif, fcport->vha, 0x9100,
		    "%s: Failed to get sa_index for spi 0x%x, dir: %d\n",
		    __func__, sa_frame->spi, dir);
		return INVALID_EDIF_SA_INDEX;
	}

	ql_dbg(ql_dbg_edif, fcport->vha, 0x9100,
	    "%s: index %d allocated to spi 0x%x, dir: %d, nport_handle: 0x%x\n",
	    __func__, sa_index, sa_frame->spi, dir, fcport->loop_id);

	//
	// This is a local copy of sa_frame.
	//
	sa_frame->fast_sa_index = sa_index;
	// create the sa_ctl
	sa_ctl = qla_edif_add_sa_ctl(fcport, sa_frame, dir);
	if (!sa_ctl) {
		ql_dbg(ql_dbg_edif, fcport->vha, 0x9100,
		    "%s: Failed to add sa_ctl for spi 0x%x, dir: %d, sa_index: %d\n",
		    __func__, sa_frame->spi, dir, sa_index);
		return -1;
	}

	set_bit(EDIF_SA_CTL_USED, &sa_ctl->state);

	if (dir == SAU_FLG_TX) {
		fcport->edif.tx_rekey_cnt++;
	} else {
		fcport->edif.rx_rekey_cnt++;
	}
	ql_dbg(ql_dbg_edif, fcport->vha, 0x9100,
	    "%s: Found sa_ctl %px, index %d, state 0x%lx, tx_cnt %d, rx_cnt %d, nport_handle: 0x%x\n",
	    __func__, sa_ctl, sa_ctl->index, sa_ctl->state,
	    fcport->edif.tx_rekey_cnt,
	    fcport->edif.rx_rekey_cnt, fcport->loop_id);
	return 0;
}

#define QLA_SA_UPDATE_FLAGS_RX_KEY      0x0
#define QLA_SA_UPDATE_FLAGS_TX_KEY      0x2

int
qla24xx_sadb_update(bsg_job_t *bsg_job)
{
	struct	fc_bsg_reply	*bsg_reply = bsg_job->reply;
	struct Scsi_Host *host = fc_bsg_to_shost(bsg_job);
	scsi_qla_host_t *vha = shost_priv(host);
	fc_port_t		*fcport = NULL;
	srb_t			*sp = NULL;
	edif_list_entry_t *edif_entry = NULL;
	int			found = 0;
	int			rval = 0;
	int result = 0;
	struct qla_sa_update_frame sa_frame;
	struct srb_iocb *iocb_cmd;


	ql_dbg(ql_dbg_edif + ql_dbg_verbose, vha, 0x911d,
	    "%s entered, vha: 0x%px\n", __func__, vha);

	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, &sa_frame,
	    sizeof(struct qla_sa_update_frame));

	/* Check if host is online */
	if (!vha->flags.online) {
		ql_log(ql_log_warn, vha, 0x70a1, "Host is not online\n");
		rval = -EIO;
		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		goto done;
	}

	if (vha->e_dbell.db_flags != EDB_ACTIVE) {
		ql_log(ql_log_warn, vha, 0x70a1, "App not started\n");
		rval = -EIO;
		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		goto done;
	}

	fcport = qla2x00_find_fcport_by_pid(vha, &sa_frame.port_id);
	if (fcport) {
		found = 1;
		if (sa_frame.flags == QLA_SA_UPDATE_FLAGS_TX_KEY)
			fcport->edif.tx_bytes = 0;
		if (sa_frame.flags == QLA_SA_UPDATE_FLAGS_RX_KEY)
			fcport->edif.rx_bytes = 0;
	}

	if (!found) {
		ql_dbg(ql_dbg_edif, vha, 0x70a3, "Failed to find port= %06x\n",
		    sa_frame.port_id.b24);
		rval = -EINVAL;
		SET_DID_STATUS(bsg_reply->result, DID_TARGET_FAILURE);
		goto done;
	}

	/* make sure the nport_handle is valid */
	if (fcport->loop_id == FC_NO_LOOP_ID) {
		ql_dbg(ql_dbg_edif, vha, 0x70e1,
		    "%s: %8phNn lid=FC_NO_LOOP_ID, spi: 0x%x, DS %d, returning NO_CONNECT\n",
		    __func__, fcport->port_name, sa_frame.spi,
		    fcport->disc_state);
		rval = -EINVAL;
		SET_DID_STATUS(bsg_reply->result, DID_NO_CONNECT);
		goto done;
	}

	/* allocate and queue an sa_ctl */
	result = qla24xx_check_sadb_avail_slot(bsg_job, fcport, &sa_frame);

	// failure of bsg
	if (result == INVALID_EDIF_SA_INDEX) {
		ql_dbg(ql_dbg_edif, vha, 0x70e1,
		    "%s: %8phNn, skipping update.\n",
		    __func__, fcport->port_name);
		rval = -EINVAL;
		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		goto done;

	// rx delete failure
	} else if (result == RX_DELETE_NO_EDIF_SA_INDEX) {
		ql_dbg(ql_dbg_edif, vha, 0x70e1,
		    "%s: %8phNn, skipping rx delete.\n",
		    __func__, fcport->port_name);
		SET_DID_STATUS(bsg_reply->result, DID_OK);
		goto done;
	}

	ql_dbg(ql_dbg_edif, vha, 0x70e1,
	    "%s: %8phNn, sa_index in sa_frame: %d flags %xh\n",
	    __func__, fcport->port_name, sa_frame.fast_sa_index,
	    sa_frame.flags);

	//
	// looking for rx index and delete
	//
	if ( ((sa_frame.flags & SAU_FLG_TX) == 0) &&
	    (sa_frame.flags & SAU_FLG_INV) ) {
		uint16_t nport_handle = fcport->loop_id;
		uint16_t sa_index = sa_frame.fast_sa_index;

		//
		// make sure we have an existing rx key, otherwise just process
		// this as a straight delete just like TX
		// This is NOT a normal case, it indicates an error recovery or key cleanup
		// by the ipsec code above us.
		//
		edif_entry = qla_edif_list_find_sa_index(fcport, fcport->loop_id);
		if (!edif_entry) {
			ql_dbg(ql_dbg_edif, vha, 0x911d,
			    "%s: WARNING: no active sa_index for nport_handle 0x%x, "
			    "forcing delete for sa_index 0x%x\n",
			    __func__, fcport->loop_id, sa_index);
			goto force_rx_delete;
		}

		//
		// if we have a forced delete for rx, remove the sa_index from the edif list
		// and proceed with normal delete.  The rx delay timer should not be running
		//
		if ( (sa_frame.flags & SAU_FLG_FORCE_DELETE) == SAU_FLG_FORCE_DELETE) {

			qla_edif_list_delete_sa_index(fcport, edif_entry);
			ql_dbg(ql_dbg_edif, vha, 0x911d,
			    "%s: FORCE DELETE flag found for nport_handle 0x%x, "
			    "sa_index 0x%x, forcing DELETE\n",
			    __func__, fcport->loop_id, sa_index);
			kfree(edif_entry);
			goto force_rx_delete;
		}

		//
		// delayed rx delete
		//
		// if delete_sa_index is not invalid then there is already a delayed
		// index in progress, return bsg bad status
		//
		if (edif_entry->delete_sa_index != INVALID_EDIF_SA_INDEX) {
			struct 	edif_sa_ctl *sa_ctl;

			ql_dbg(ql_dbg_edif, vha, 0x911d,
			    "%s:  delete for npport handle 0x%x, delete_sa_index "
			    "%d is pending, aborting this delete request\n",
			    __func__, edif_entry->handle, edif_entry->delete_sa_index);

			//
			// free up the sa_ctl that was allocated with the sa_index
			//
			sa_ctl = qla_edif_find_sa_ctl_by_index(fcport, sa_index,
			    (sa_frame.flags & SAU_FLG_TX));
			if (sa_ctl != NULL) {
				ql_dbg(ql_dbg_edif, vha, 0x3063,
				    "%s: freeing sa_ctl for index %d\n",
				    __func__, sa_ctl->index);
				qla_edif_free_sa_ctl(fcport, sa_ctl, sa_ctl->index);
			}
			//
			// release the sa_index
			//
			ql_dbg(ql_dbg_edif, vha, 0x3063,
			    "%s: freeing sa_index %d, nph: 0x%x\n",
			    __func__, sa_index, nport_handle);
			qla_edif_sadb_delete_sa_index(fcport, nport_handle, sa_index);

			rval = -EINVAL;
			SET_DID_STATUS(bsg_reply->result, DID_ERROR);
			goto done;
		}

		/* clean up edif flags/state */
		fcport->edif.new_sa = 0;		// NOT USED ???
		fcport->edif.db_sent = 0;		// NOT USED ???
		fcport->edif.rekey = fcport->edif.reload_value;
		fcport->edif.rekey_cnt++;

		//
		// configure and start the rx delay timer
		//
		edif_entry->fcport = fcport;			// needed by qla2x00_sa_replace_iocb_timeout
		edif_entry->timer.expires = jiffies + RX_DELAY_DELETE_TIMEOUT * HZ;

		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s:  adding timer, edif_entry: %px, delete sa_index %d, "
		    "nport_handle 0x%x to edif_list\n",
		    __func__, edif_entry, sa_index, nport_handle);

		//
		// Start the timer when we queue the delayed rx delete.
		// This is an activity timer that goes off if we have not received packets with the
		// new sa_index
		//
		add_timer(&edif_entry->timer);

		//
		// sa_delete for rx key with an active rx key including this one
		// add the delete rx sa index to the hash so we can look for it
		// in the rsp queue.  Do this after making any changes to the
		// edif_entry as part of the rx delete.
		//

		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s:  adding delete sa_index %d, vha: 0x%px, nport_handle 0x%x "
		    "to edif_list. bsg done ptr %px\n",
		    __func__, sa_index, vha, nport_handle, bsg_job);

		edif_entry->delete_sa_index = sa_index;

		bsg_job->reply_len = sizeof(struct fc_bsg_reply);
		bsg_reply->result = DID_OK << 16;

		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s:  SA_DELETE vha: 0x%px  nport_handle: 0x%x  sa_index: %d successfully queued \n",
		    __func__, vha, fcport->loop_id, sa_index);
		goto done;

	//
	// rx index and update
	//   add the index to the list and continue with normal update
	//
	} else if ( ((sa_frame.flags & SAU_FLG_TX) == 0) &&
	    ((sa_frame.flags & SAU_FLG_INV) == 0 )) {
		//
		// sa_update for rx key
		//
		uint32_t nport_handle = fcport->loop_id;
		uint16_t sa_index = sa_frame.fast_sa_index;
		int result;

		//
		// add the update rx sa index to the hash so we can look for it
		// in the rsp queue and continue normally
		//

		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s:  adding update sa_index %d, nport_handle 0x%x to edif_list\n",
		    __func__, sa_index, nport_handle);

		result = qla_edif_list_add_sa_update_index(fcport, sa_index,
		    nport_handle);
		if (result) {
			ql_dbg(ql_dbg_edif, vha, 0x911d,
			    "%s:  SA_UPDATE failed to add new sa index %d to "
			    "list for nport_handle 0x%x\n",
			    __func__, sa_index, nport_handle);
		}
	}
	if (sa_frame.flags & SAU_FLG_GMAC_MODE)
		fcport->edif.aes_gmac = 1;
	else
		fcport->edif.aes_gmac = 0;

force_rx_delete:
	//
	// sa_update for both rx and tx keys, sa_delete for tx key
	// immediately process the request
	//
	sp = qla2x00_get_sp(vha, fcport, GFP_KERNEL);
	if (!sp) {

		ql_log(ql_log_warn, vha, 0x70e1, "qla2x00_get_sp failed.\n");

		rval = -ENOMEM;
		SET_DID_STATUS(bsg_reply->result, DID_IMM_RETRY);
		goto done;
	}


	sp->type = SRB_SA_UPDATE;
	sp->name = "bsg_sa_update";
	sp->u.bsg_job = bsg_job;
//	sp->free = qla2x00_bsg_sp_free;
	sp->free = qla2x00_rel_sp;
	sp->done = qla2x00_bsg_job_done;
	iocb_cmd = &sp->u.iocb_cmd;
	iocb_cmd->u.sa_update.sa_frame  = sa_frame;

	rval = qla2x00_start_sp(sp);
	if (rval != QLA_SUCCESS) {

		ql_log(ql_dbg_edif, vha, 0x70e3,
		    "qla2x00_start_sp failed=%d.\n", rval);

		qla2x00_rel_sp(sp);
		rval = -EIO;
		SET_DID_STATUS(bsg_reply->result, DID_IMM_RETRY);
		goto done;
	}

	/* clean up edif flags/state */
	fcport->edif.new_sa = 0;			// NOT USED ???
	fcport->edif.db_sent = 0;			// NOT USED ???
	fcport->edif.rekey = fcport->edif.reload_value;

	ql_dbg(ql_dbg_edif, vha, 0x911d,
	    "%s:  %s sent, hdl=%x, portid=%06x.\n",
	    __func__, sp->name, sp->handle, fcport->d_id.b24);

	fcport->edif.rekey_cnt++;
	bsg_job->reply_len = sizeof(struct fc_bsg_reply);
	SET_DID_STATUS(bsg_reply->result, DID_OK);

	return 0;

//
// send back error status
//
done:
	bsg_job->reply_len = sizeof(struct fc_bsg_reply);
	ql_dbg(ql_dbg_edif, vha, 0x911d,
	    "%s:status: FAIL, result: 0x%x, bsg ptr done %px\n",
	    __func__, bsg_reply->result, bsg_job);
	bsg_job_done(bsg_job, bsg_reply->result,
	    bsg_reply->reply_payload_rcv_len);
	return 0;
}


static void
qla_enode_free(scsi_qla_host_t *vha, enode_t *node)
{
	/*
	 * releases the space held by this enode entry
	 * this function does _not_ free the enode itself
	 * NB: the pur node entry passed should not be on any list
	 */

	if (node == NULL) {
		ql_dbg(ql_dbg_edif, vha, 0x09122,
		    "%s error - no valid node passed\n", __func__);
		return;
	}

	node->dinfo.lstate = LSTATE_DEST;
	node->ntype = N_UNDEF;
	kfree(node);
}

// function to initialize enode structs & lock
// NB: should only be called when driver attaching

void
qla_enode_init(scsi_qla_host_t *vha)
{
	struct	qla_hw_data *ha = vha->hw;
	char	name[32];

	if (vha->pur_cinfo.enode_flags == ENODE_ACTIVE) {
		/* list still active - error */
		ql_dbg(ql_dbg_edif, vha, 0x09102, "%s enode still active\n",
		    __func__);
		return;
	}

	/* initialize lock which protects pur_core & init list */
	spin_lock_init(&vha->pur_cinfo.pur_lock);
	INIT_LIST_HEAD(&vha->pur_cinfo.head);

	snprintf(name, sizeof(name), "%s_%d_purex", QLA2XXX_DRIVER_NAME,
	    ha->pdev->device);
}

// function to stop and clear and enode data
// called when app notified it is stopping

void
qla_enode_stop(scsi_qla_host_t *vha)
{
	unsigned long flags;
	struct enode *node, *q;

	if (vha->pur_cinfo.enode_flags != ENODE_ACTIVE) {
		/* doorbell list not enabled */
		ql_dbg(ql_dbg_edif, vha, 0x09102,
		    "%s enode not active\n", __func__);
		return;
	}

	/* grab lock so list doesn't move */
	spin_lock_irqsave(&vha->pur_cinfo.pur_lock, flags);

	vha->pur_cinfo.enode_flags &= ~ENODE_ACTIVE; // mark it not active

	/* hopefully this is a null list at this point */
	list_for_each_entry_safe(node, q, &vha->pur_cinfo.head, list) {
		ql_dbg(ql_dbg_edif, vha, 0x910f, "%s freeing enode type="
		    "%x, cnt=%x\n", __func__, node->ntype,
		    node->dinfo.nodecnt);
		list_del_init(&node->list);
		spin_unlock_irqrestore(&vha->pur_cinfo.pur_lock, flags);
		qla_enode_free(vha, node);
		spin_lock_irqsave(&vha->pur_cinfo.pur_lock, flags);
	}
	spin_unlock_irqrestore(&vha->pur_cinfo.pur_lock, flags);
}

/*
 *  allocate enode struct and populate buffer
 *  returns: enode pointer with buffers
 *           NULL on error
 */
static enode_t *
qla_enode_alloc(scsi_qla_host_t *vha, uint32_t ntype)
{
	enode_t		*node;
	purexevent_t	*purex;

	node = (enode_t *)kzalloc(RX_ELS_SIZE, GFP_ATOMIC);
	if (node  == NULL ) {
		ql_dbg(ql_dbg_edif, vha, 0x9100,
		    "pur buf node unable to be allocated\n");
		return NULL;
	}

	purex = &node->u.purexinfo;
	purex->msgp = (u8*) (node + 1);
	purex->msgp_len = ELS_MAX_PAYLOAD;

	node->dinfo.lstate = LSTATE_OFF;

	node->ntype = ntype;
	INIT_LIST_HEAD(&node->list);
	return node;
}

/* adds a already alllocated enode to the linked list */
static bool
qla_enode_add(scsi_qla_host_t *vha, enode_t *ptr)
{
	unsigned long		flags;

	ql_dbg(ql_dbg_edif+ql_dbg_verbose, vha, 0x9109,
	    "%s add enode for type=%x, cnt=%x\n",
	    __func__, ptr->ntype, ptr->dinfo.nodecnt);

	spin_lock_irqsave(&vha->pur_cinfo.pur_lock, flags);
	ptr->dinfo.lstate = LSTATE_ON;
	list_add_tail(&(ptr->list), &(vha->pur_cinfo.head));
	spin_unlock_irqrestore(&vha->pur_cinfo.pur_lock, flags);

	return true;
}

static enode_t *
qla_enode_find(scsi_qla_host_t *vha, uint32_t ntype, uint32_t p1, uint32_t p2)
{
	enode_t			*node_rtn = NULL;
	enode_t			*list_node = NULL;
	unsigned long		flags;
	struct list_head	*pos, *q;

	uint32_t		sid;
	uint32_t		rw_flag;

	purexevent_t		*purex;

	/* secure the list from moving under us */
	spin_lock_irqsave(&vha->pur_cinfo.pur_lock, flags);

	list_for_each_safe(pos, q, &vha->pur_cinfo.head) {

		list_node = list_entry(pos, struct enode, list);

		// node type determines what p1 and p2 are

			purex = &list_node->u.purexinfo;
			sid = p1;
			rw_flag = p2;

			if (purex->pur_info.pur_sid.b24 == sid) {
				if ((purex->pur_info.pur_pend == 1) &&
				    (rw_flag == PUR_GET)) {
					// if the receive is in progress
					// and its a read/get then can't
					// transfer yet
					ql_dbg(ql_dbg_edif, vha, 0x9106,
					    "%s purex xfer in progress "
					    "for sid=%x\n", __func__,
					    sid);
				} else {
						// found it and its complete
					node_rtn = list_node;
				}
			}


		if (node_rtn != NULL) {
			/*
			 * found node that we're looking for so take it
			 * off the list and return it to the caller
			 */
			list_del(pos);
			list_node->dinfo.lstate = LSTATE_OFF;
			break;
		}
	}

	spin_unlock_irqrestore(&vha->pur_cinfo.pur_lock, flags);

	return node_rtn;
}

/*
 * Return number of bytes of purex payload pending for consumption
 */
static int
qla_pur_get_pending(scsi_qla_host_t *vha, fc_port_t *fcport, bsg_job_t *bsg_job)
{
	enode_t		*ptr;
	purexevent_t	*purex;
	struct qla_bsg_auth_els_reply *rpl =
	    (struct qla_bsg_auth_els_reply *)bsg_job->reply;

	bsg_job->reply_len = sizeof(*rpl);

	if ((ptr = qla_enode_find(vha, N_PUREX, fcport->d_id.b24, PUR_GET)) == NULL) {
		ql_dbg(ql_dbg_edif, vha, 0x9111,
		    "%s no enode data found for %8phN sid=%06x\n",
		    __func__, fcport->port_name, fcport->d_id.b24);
		SET_DID_STATUS(rpl->r.result, DID_IMM_RETRY);
		return -EIO;
	}

	/*
	 * enode is now off the linked list and is ours to deal with
	 */
	purex = &ptr->u.purexinfo;

	/* Copy info back to caller */
	rpl->rx_xchg_address = purex->pur_info.pur_rx_xchg_address;

	SET_DID_STATUS(rpl->r.result, DID_OK);
	rpl->r.reply_payload_rcv_len =
	    sg_pcopy_from_buffer(bsg_job->reply_payload.sg_list,
		bsg_job->reply_payload.sg_cnt, purex->msgp,
		purex->pur_info.pur_bytes_rcvd , 0);



	/* data copy / passback completed - destroy enode */
	qla_enode_free(vha, ptr);

	return 0;
}

/* it is assume qpair lock is held */
static int
qla_els_reject_iocb(scsi_qla_host_t *vha, struct qla_qpair *qp,
	struct qla_els_pt_arg *a)
{
	struct els_entry_24xx *els_iocb;

	els_iocb = __qla2x00_alloc_iocbs(qp, NULL);
	if (!els_iocb) {
		ql_log(ql_log_warn, vha, 0x700c,
		    "qla2x00_alloc_iocbs failed.\n");
		return QLA_FUNCTION_FAILED;
	}

	qla_els_pt_iocb(vha, els_iocb, a);

	ql_dbg(ql_dbg_edif, vha, 0x0183,
	    "Sending ELS reject...\n");
	ql_dump_buffer(ql_dbg_edif + ql_dbg_verbose, vha, 0x0185,
	    vha->hw->elsrej.c, sizeof(*vha->hw->elsrej.c));

	wmb();
	qla2x00_start_iocbs(vha, qp->req);
	return 0;
}

void
qla_edb_init(scsi_qla_host_t *vha)
{
	if (vha->e_dbell.db_flags == EDB_ACTIVE) {
		/* list already init'd - error */
		ql_dbg(ql_dbg_edif, vha, 0x09102,
		    "edif db already initialized, cannot reinit\n");
		return;
	}

	/* initialize lock which protects doorbell & init list */
	spin_lock_init(&vha->e_dbell.db_lock);
	INIT_LIST_HEAD(&vha->e_dbell.head);

	/* create and initialize doorbell */
	init_completion(&vha->e_dbell.dbell);
}

static void
qla_edb_node_free(scsi_qla_host_t *vha, edb_node_t *node)
{

	/*
	 * releases the space held by this edb node entry
	 * this function does _not_ free the edb node itself
	 * NB: the edb node entry passed should not be on any list
	 *
	 * currently for doorbell there's no additional cleanup
	 * needed, but here as a placeholder for furture use.
	 */

	if (node == NULL) {
		ql_dbg(ql_dbg_edif, vha, 0x09122,
		    "%s error - no valid node passed\n", __func__);
		return;
	}

	node->lstate = LSTATE_DEST;
	node->ntype = N_UNDEF;
}

// function called when app is stopping

void
qla_edb_stop(scsi_qla_host_t *vha)
{
	unsigned long flags;
	struct edb_node *node, *q;

	if (vha->e_dbell.db_flags != EDB_ACTIVE) {
		/* doorbell list not enabled */
		ql_dbg(ql_dbg_edif, vha, 0x09102,
		    "%s doorbell not enabled\n", __func__);
		return;
	}

	/* grab lock so list doesn't move */
	spin_lock_irqsave(&vha->e_dbell.db_lock, flags);

	vha->e_dbell.db_flags &= ~EDB_ACTIVE; // mark it not active
	/* hopefully this is a null list at this point */
	list_for_each_entry_safe(node, q, &vha->e_dbell.head, list) {
		ql_dbg(ql_dbg_edif, vha, 0x910f, "%s freeing edb_node type="
		    "%x\n", __func__, node->ntype);
		qla_edb_node_free(vha, node);
		list_del(&node->list);

		spin_unlock_irqrestore(&vha->e_dbell.db_lock, flags);
		kfree(node);
		spin_lock_irqsave(&vha->e_dbell.db_lock, flags);
	}
	spin_unlock_irqrestore(&vha->e_dbell.db_lock, flags);

	// wake up doorbell waiters - they'll be dismissed with error code
	complete_all(&vha->e_dbell.dbell);
}

static edb_node_t *
qla_edb_node_alloc(scsi_qla_host_t *vha, uint32_t ntype)
{
	edb_node_t	*node;

	if ((node = (edb_node_t *)
	    kzalloc(sizeof(edb_node_t), GFP_ATOMIC)) == NULL) {
		/* couldn't get space */
		ql_dbg(ql_dbg_edif, vha, 0x9100,
		    "edb node unable to be allocated\n");
		return NULL;
	}

	node->lstate = LSTATE_OFF;
	node->ntype = ntype;
	INIT_LIST_HEAD(&node->list);
	return node;
}


/* adds a already alllocated enode to the linked list */
static bool
qla_edb_node_add(scsi_qla_host_t *vha, edb_node_t *ptr)
{
	unsigned long		flags;


	if (ptr->lstate != LSTATE_OFF) {
		ql_dbg(ql_dbg_edif, vha, 0x911a, "%s error, "
		    "edb node(%px) state=%x\n", __func__, ptr, ptr->lstate);
		return false;
	}

	if (vha->e_dbell.db_flags != EDB_ACTIVE) {
		/* doorbell list not enabled */
		ql_dbg(ql_dbg_edif, vha, 0x09102,
		    "%s doorbell not enabled\n", __func__);
		return false;
	}

	spin_lock_irqsave(&vha->e_dbell.db_lock, flags);
	ptr->lstate = LSTATE_ON;
	list_add_tail(&(ptr->list), &(vha->e_dbell.head));
	spin_unlock_irqrestore(&vha->e_dbell.db_lock, flags);

	// ring doorbell for waiters
	complete(&vha->e_dbell.dbell);

	return true;
}

/* adds event to doorbell list */
void
qla_edb_eventcreate(scsi_qla_host_t *vha, uint32_t dbtype,
	uint32_t data, uint32_t data2, fc_port_t	*sfcport)
{
	edb_node_t	*edbnode;
	fc_port_t *fcport = sfcport;
	port_id_t id;

	if (!vha->hw->flags.edif_enabled) {
		/* edif not enabled */
		return;
	}


	if (vha->e_dbell.db_flags != EDB_ACTIVE) {
		if (fcport)
			fcport->edif.auth_state = dbtype;
		/* doorbell list not enabled */
		ql_dbg(ql_dbg_edif, vha, 0x09102,
		    "%s doorbell not enabled (type=%d\n", __func__, dbtype);
		return;
	}

	if ((edbnode = qla_edb_node_alloc(vha, dbtype)) == NULL) {
		ql_dbg(ql_dbg_edif, vha, 0x09102,
		    "%s unable to alloc db node\n", __func__);
		return;
	}

	if (!fcport){
		id.b.domain = (data >> 16)& 0xff;
		id.b.area = (data >> 8)& 0xff;
		id.b.al_pa = data & 0xff;
		ql_dbg(ql_dbg_edif, vha, 0x09222,
		    "%s: Arrived s_id: %02x%02x%02x \n", __func__,
		    id.b.domain,
		    id.b.area,
		    id.b.al_pa);
		if(!(fcport = qla2x00_find_fcport_by_pid(vha, &id))) {
			ql_dbg(ql_dbg_edif, vha, 0x09102,
			    "%s can't find fcport for sid= 0x%x - ignoring\n",
			__func__, id.b24);
			kfree(edbnode);
			return;
		}
	}

	// populate the edb node
	switch (dbtype) {
	case VND_CMD_AUTH_STATE_NEEDED:
	case VND_CMD_AUTH_STATE_SESSION_SHUTDOWN:
	    edbnode->u.plogi_did.b24 = fcport->d_id.b24;
	    break;
	case VND_CMD_AUTH_STATE_ELS_RCVD:
	    edbnode->u.els_sid.b24 = fcport->d_id.b24;
	    break;
	case VND_CMD_AUTH_STATE_SAUPDATE_COMPL:
	    edbnode->u.sa_aen.port_id = fcport->d_id;
	    edbnode->u.sa_aen.status =  data;
	    edbnode->u.sa_aen.key_type =  data2;
	    break;
	default:
	    ql_dbg(ql_dbg_edif, vha, 0x09102,
		"%s unknown type: %x\n", __func__, dbtype);
	    qla_edb_node_free(vha, edbnode);
	    kfree(edbnode);
	    edbnode = NULL;
	    break;
	}

	if (edbnode && (!qla_edb_node_add(vha, edbnode))) {
		ql_dbg(ql_dbg_edif, vha, 0x09102,
		    "%s unable to add dbnode\n", __func__);
		qla_edb_node_free(vha, edbnode);
		kfree(edbnode);
		return;
	}
	if (edbnode && fcport)
		fcport->edif.auth_state = dbtype;
	ql_dbg(ql_dbg_edif, vha, 0x09102,
	    "%s Doorbell produced : type=%d %p\n", __func__, dbtype, edbnode);
}

static edb_node_t *
qla_edb_getnext(scsi_qla_host_t *vha)
{
	unsigned long	flags;
	edb_node_t	*edbnode = NULL;

	spin_lock_irqsave(&vha->e_dbell.db_lock, flags);

	// db nodes are fifo - no qualifications done
	if (!list_empty(&vha->e_dbell.head)) {
		edbnode = list_first_entry(&vha->e_dbell.head,
		    struct edb_node, list);
		list_del(&edbnode->list);
		edbnode->lstate = LSTATE_OFF;
	}

	spin_unlock_irqrestore(&vha->e_dbell.db_lock, flags);

	return edbnode;
}

void
qla_edif_timer(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;

	if (!vha->vp_idx && N2N_TOPO(ha) && ha->flags.n2n_fw_acc_sec) {
		if (vha->e_dbell.db_flags != EDB_ACTIVE &&
			ha->edif_post_stop_cnt_down) {
			ha->edif_post_stop_cnt_down--;

			/* turn off auto 'Plogi Acc + secure=1' feature
			 * Set Add FW option[3] BIT_15, if.
			 */
			if (ha->edif_post_stop_cnt_down == 0) {
				ql_dbg(ql_dbg_async, vha, 0x911d,
				   "%s chip reset to turn off PLOGI ACC + secure\n",
				   __func__);
				set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
			}
		} else {
			ha->edif_post_stop_cnt_down = 60;
		}
	}
}


/*
 * app uses seperate thread to read this. It'll wait until the doorbell
 * is rung by the driver or the max wait time has expired
 */
ssize_t
qla2x00_edif_doorbellr(struct device *dev, struct device_attribute *attr,
    char *buf)
{
	scsi_qla_host_t *vha = shost_priv(class_to_shost(dev));
	edb_node_t		*dbnode = NULL;
	struct edif_app_dbell	*ap = (struct edif_app_dbell *) buf;
	uint32_t dat_siz, buf_size, sz;
	sz = 256; // app currently hardcode to 256.

	/* stop new threads from waiting if we're not init'd */
	if (vha->e_dbell.db_flags != EDB_ACTIVE) {
		ql_dbg(ql_dbg_edif + ql_dbg_verbose, vha, 0x09122,
		    "%s error - edif db not enabled\n", __func__);
		return 0;
	}
	if (!vha->hw->flags.edif_enabled) {
		/* edif not enabled */
		ql_dbg(ql_dbg_edif + ql_dbg_verbose, vha, 0x09122,
		    "%s error - edif not enabled\n", __func__);
		return -1;
	}

	buf_size = 0;
	while ((sz - buf_size) >= sizeof(edb_node_t)) {
		// remove the next item from the doorbell list
		dat_siz = 0;
		if ((dbnode = qla_edb_getnext(vha)) != NULL) {
			ap->event_code = dbnode->ntype;
			switch (dbnode->ntype) {
			case VND_CMD_AUTH_STATE_SESSION_SHUTDOWN:
			case VND_CMD_AUTH_STATE_NEEDED:
				ap->port_id = dbnode->u.plogi_did;
				dat_siz += sizeof(ap->port_id);
				break;
			case VND_CMD_AUTH_STATE_ELS_RCVD:
				ap->port_id = dbnode->u.els_sid;
				dat_siz += sizeof(ap->port_id);
				break;
			case VND_CMD_AUTH_STATE_SAUPDATE_COMPL:
				ap->port_id = dbnode->u.sa_aen.port_id;
				memcpy(ap->event_data, &dbnode->u,
						sizeof(edif_sa_update_aen_t));
				dat_siz += sizeof(edif_sa_update_aen_t);
				break;
			default:
				// unknown node type, rtn unknown ntype
				ap->event_code = VND_CMD_AUTH_STATE_UNDEF;
				memcpy(ap->event_data, &dbnode->ntype, 4);
				dat_siz += 4;
				break;
			}

			ql_dbg(ql_dbg_edif, vha, 0x09102,
				"%s Doorbell consumed : type=%d %p\n",
				__func__, dbnode->ntype, dbnode);
			// we're done with the db node, so free it up
			qla_edb_node_free(vha, dbnode);
			kfree(dbnode);
		} else {
			break;
		}

		ap->event_data_size = dat_siz;
		// 8bytes = ap->event_code + ap->event_data_size
		buf_size += dat_siz + 8;
		ap = (struct edif_app_dbell *)(buf + buf_size);
	}
	return buf_size;
}


void
ql_print_bsg_sglist(uint level, scsi_qla_host_t *vha, uint id, char *str,
    void *q)
{
	struct bsg_buffer *p = q;
	struct scatterlist *sg;
	uint i;

	ql_dbg(level, vha, id,
	    "%s->(sg_cnt=%#x payload_len=%#x):\n",
	    str, p->sg_cnt, p->payload_len);
	for_each_sg(p->sg_list, sg, p->sg_cnt, i) {
		ql_dbg(level, vha, id,
		    "%x: dma(adr=%#llx len=%#x) off=%#x len=%#x\n",
		    i, sg_dma_address(sg), sg_dma_len(sg), sg->offset,
		    sg->length);
	}
}


static void qla_noop_sp_done(srb_t *sp, int res)
{
	sp->free(sp);
}
//
// Called from work queue
// build and send the sa_update iocb to delete an rx sa_index
//
int
qla24xx_issue_sa_replace_iocb(scsi_qla_host_t *vha, struct qla_work_evt *e)
{
	srb_t *sp;
	fc_port_t	*fcport = NULL;
	struct srb_iocb *iocb_cmd = NULL;
	int rval = QLA_SUCCESS;
	struct	edif_sa_ctl *sa_ctl = e->u.sa_update.sa_ctl;
	uint16_t nport_handle = e->u.sa_update.nport_handle;

	ql_dbg(ql_dbg_edif, vha, 0x70e6,
	    "%s: starting,  sa_ctl: %px\n", __func__, sa_ctl);

	if (!sa_ctl) {
		ql_dbg(ql_dbg_edif, vha, 0x70e6,
		    "sa_ctl allocation failed\n");
		return -ENOMEM;
	}

	fcport = sa_ctl->fcport;

	/* Alloc SRB structure */
	sp = qla2x00_get_sp(vha, fcport, GFP_KERNEL);
	if (!sp) {
		ql_dbg(ql_dbg_edif, vha, 0x70e6,
		 "SRB allocation failed\n");
		return -ENOMEM;
	}

	fcport->flags |= FCF_ASYNC_SENT;
	iocb_cmd = &sp->u.iocb_cmd;
	iocb_cmd->u.sa_update.sa_ctl = sa_ctl;

	ql_dbg(ql_dbg_edif, vha, 0x3073,
	    "Enter: SA REPL portid=%06x, sa_ctl %px, index %x, nport_handle: 0x%x\n",
	    fcport->d_id.b24, sa_ctl, sa_ctl->index, nport_handle);
	//
	// if this is a sadb cleanup delete, mark it so the isr can take the correct action
	//
	if (sa_ctl->flags & EDIF_SA_CTL_FLG_CLEANUP_DEL) {

		// mark this srb as a cleanup delete
		sp->flags |= SRB_EDIF_CLEANUP_DELETE;
		ql_dbg(ql_dbg_edif, vha, 0x70e6,
		    "%s: sp 0x%px flagged as cleanup delete\n", __func__, sp);
	}

	sp->type = SRB_SA_REPLACE;
	sp->name = "SA_REPLACE";
	sp->fcport = fcport;
	sp->free = qla2x00_rel_sp;
	sp->done = qla_noop_sp_done;

	rval = qla2x00_start_sp(sp);

	if (rval != QLA_SUCCESS) {
		rval = QLA_FUNCTION_FAILED;
	}

	return rval;
}


void qla24xx_sa_update_iocb(srb_t *sp, struct sa_update_28xx *sa_update_iocb)
{
	int	itr = 0;
	struct	scsi_qla_host		*vha = sp->vha;
	struct	qla_sa_update_frame	*sa_frame =
		&sp->u.iocb_cmd.u.sa_update.sa_frame;
	u8 flags = 0;

	switch (sa_frame->flags & (SAU_FLG_INV | SAU_FLG_TX)) {
	case 0:
		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s: EDIF SA UPDATE RX IOCB  vha: 0x%px  index: %d\n",
		    __func__, vha, sa_frame->fast_sa_index);
		break;
	case 1:
		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s: EDIF SA DELETE RX IOCB  vha: 0x%px  index: %d\n",
		    __func__, vha, sa_frame->fast_sa_index);
		flags |= SA_FLAG_INVALIDATE;
		break;
	case 2:
		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s: EDIF SA UPDATE TX IOCB  vha: 0x%px  index: %d\n",
		    __func__, vha, sa_frame->fast_sa_index);
		flags |= SA_FLAG_TX;
		break;
	case 3:
		ql_dbg(ql_dbg_edif, vha, 0x911d,
		    "%s: EDIF SA DELETE TX IOCB  vha: 0x%px  index: %d\n",
		    __func__, vha, sa_frame->fast_sa_index);
		flags |= SA_FLAG_TX|SA_FLAG_INVALIDATE;
		break;
	}

	sa_update_iocb->entry_type = SA_UPDATE_IOCB_TYPE;
	sa_update_iocb->entry_count = 1;
	sa_update_iocb->sys_define = 0;
	sa_update_iocb->entry_status = 0;
	sa_update_iocb->handle = sp->handle;
	sa_update_iocb->u.nport_handle = cpu_to_le16(sp->fcport->loop_id);
	sa_update_iocb->vp_index = sp->fcport->vha->vp_idx;
	sa_update_iocb->port_id[0] = sp->fcport->d_id.b.al_pa;
	sa_update_iocb->port_id[1] = sp->fcport->d_id.b.area;
	sa_update_iocb->port_id[2] = sp->fcport->d_id.b.domain;

	sa_update_iocb->flags = flags;
	sa_update_iocb->salt = cpu_to_le32(sa_frame->salt);
	sa_update_iocb->spi = cpu_to_le32(sa_frame->spi);
	sa_update_iocb->sa_index = cpu_to_le16(sa_frame->fast_sa_index);

	sa_update_iocb->sa_control |= SA_CNTL_ENC_FCSP;
	if (sp->fcport->edif.aes_gmac)
		sa_update_iocb->sa_control |= SA_CNTL_AES_GMAC;

	if (sa_frame->flags & SAU_FLG_KEY256) {
		sa_update_iocb->sa_control |= SA_CNTL_KEY256;
		for (itr = 0; itr < 32; itr++)
			sa_update_iocb->sa_key[itr] = sa_frame->sa_key[itr];

		ql_dbg(ql_dbg_edif+ql_dbg_verbose, vha, 0x921f, "%s 256 sa key=%32phN\n",
		    __func__, sa_update_iocb->sa_key);
	} else {
		sa_update_iocb->sa_control |= SA_CNTL_KEY128;
		for (itr = 0; itr < 16; itr++)
			sa_update_iocb->sa_key[itr] = sa_frame->sa_key[itr];

		ql_dbg(ql_dbg_edif+ql_dbg_verbose, vha, 0x921f, "%s 128 sa key=%16phN\n",
		    __func__, sa_update_iocb->sa_key);
	}

	ql_dbg(ql_dbg_edif, vha, 0x921d, "%s SAU Port ID = %02x:%02x:%02x,"
	    " flags=%xh, index=%u, ctl=%xh, SPI 0x%x user flags 0x%x hdl=%x gmac %d\n",
	    __func__, sa_update_iocb->port_id[2],
	    sa_update_iocb->port_id[1], sa_update_iocb->port_id[0],
	    sa_update_iocb->flags, sa_update_iocb->sa_index,
	    sa_update_iocb->sa_control, sa_update_iocb->spi,
	    sa_frame->flags, sp->handle, sp->fcport->edif.aes_gmac);

	if (sa_frame->flags & SAU_FLG_TX) {
		sp->fcport->edif.tx_sa_pending = 1;
	} else {
		sp->fcport->edif.rx_sa_pending = 1;
	}

	sp->fcport->vha->qla_stats.control_requests++;
}

void
qla24xx_sa_replace_iocb(srb_t *sp, struct sa_update_28xx *sa_update_iocb)
{
	struct	scsi_qla_host		*vha = sp->vha;
	struct srb_iocb *srb_iocb = &sp->u.iocb_cmd;
	struct	edif_sa_ctl		*sa_ctl = srb_iocb->u.sa_update.sa_ctl;
	uint16_t nport_handle = sp->fcport->loop_id;

	sa_update_iocb->entry_type = SA_UPDATE_IOCB_TYPE;
	sa_update_iocb->entry_count = 1;
	sa_update_iocb->sys_define = 0;
	sa_update_iocb->entry_status = 0;
	sa_update_iocb->handle = sp->handle;

	sa_update_iocb->u.nport_handle = cpu_to_le16(nport_handle);

	sa_update_iocb->vp_index = sp->fcport->vha->vp_idx;
	sa_update_iocb->port_id[0] = sp->fcport->d_id.b.al_pa;
	sa_update_iocb->port_id[1] = sp->fcport->d_id.b.area;
	sa_update_iocb->port_id[2] = sp->fcport->d_id.b.domain;

	/* Invalidate the index. salt, spi, control & key are ignore */
	sa_update_iocb->flags = SA_FLAG_INVALIDATE;
	sa_update_iocb->salt = 0;
	sa_update_iocb->spi = 0;
	sa_update_iocb->sa_index = cpu_to_le16(sa_ctl->index);
	sa_update_iocb->sa_control = 0;

	ql_dbg(ql_dbg_edif, vha, 0x921d,
	    "%s SAU DELETE RX Port ID = %02x:%02x:%02x, lid %d"
	    " flags=%xh, index=%u, hdl=%x\n",
	    __func__, sa_update_iocb->port_id[2],
	    sa_update_iocb->port_id[1], sa_update_iocb->port_id[0],
	    nport_handle, sa_update_iocb->flags, sa_update_iocb->sa_index,
	    sp->handle);

	sp->fcport->vha->qla_stats.control_requests++;
}

void qla24xx_auth_els(scsi_qla_host_t *vha, void **pkt, struct rsp_que **rsp)
{
	struct purex_entry_24xx *p = *pkt;
	enode_t		*ptr;
	int		sid;
	u16 totlen;
	purexevent_t	*purex;
	struct scsi_qla_host *host = NULL;
	int rc;
	struct fc_port *fcport;
	struct qla_els_pt_arg a;
	be_id_t beid;

	memset(&a, 0, sizeof(a));

	a.els_opcode = ELS_AUTH_ELS;
	a.nport_handle = p->nport_handle;
	a.rx_xchg_address = p->rx_xchg_addr;
	a.did.b.domain = p->s_id[2];
	a.did.b.area   = p->s_id[1];
	a.did.b.al_pa  = p->s_id[0];
	a.tx_byte_count = a.tx_len = sizeof(struct fc_els_ls_rjt);
	a.tx_addr = vha->hw->elsrej.cdma;
	a.vp_idx = vha->vp_idx;
	a.control_flags = EPD_ELS_RJT;

	sid = p->s_id[0] | (p->s_id[1] << 8) | (p->s_id[2] << 16);
	//ql_dbg(ql_dbg_edif, vha, 0x09108, "%s rec'vd sid=0x%x\n", __func__, sid);


	totlen = (le16_to_cpu(p->frame_size) & 0x0fff) - PURX_ELS_HEADER_SIZE;
	if (le16_to_cpu(p->status_flags) & 0x8000) {
		totlen = le16_to_cpu(p->trunc_frame_size);
		qla_els_reject_iocb(vha, (*rsp)->qpair, &a);
		__qla_consume_iocb(vha, pkt, rsp);
		return;
	}

	if (totlen > MAX_PAYLOAD) {
		ql_dbg(ql_dbg_edif, vha, 0x0910d, "%s WARNING: verbose"
		    " ELS frame received (totlen=%x)\n", __func__,
		    totlen);
		qla_els_reject_iocb(vha, (*rsp)->qpair, &a);
		__qla_consume_iocb(vha, pkt, rsp);
		return;
	}
	if (!vha->hw->flags.edif_enabled) {
		/* edif support not enabled */
		ql_dbg(ql_dbg_edif, vha, 0x910e, "%s edif not enabled\n",
		    __func__);
		qla_els_reject_iocb(vha, (*rsp)->qpair, &a);
		__qla_consume_iocb(vha, pkt, rsp);
		return;
	}


	if ((ptr = qla_enode_alloc(vha, N_PUREX)) == NULL) {
		ql_dbg(ql_dbg_edif, vha, 0x09109,
		    "WARNING: enode allloc failed for sid=%x\n",
		    sid);
		qla_els_reject_iocb(vha, (*rsp)->qpair, &a);
		__qla_consume_iocb(vha, pkt, rsp);
		return;
	}

	purex = &ptr->u.purexinfo;
	purex->pur_info.pur_sid = a.did;
	purex->pur_info.pur_pend = 0;
	purex->pur_info.pur_bytes_rcvd = totlen;
	purex->pur_info.pur_rx_xchg_address = le32_to_cpu(p->rx_xchg_addr);
	purex->pur_info.pur_nphdl = le16_to_cpu(p->nport_handle);
	purex->pur_info.pur_did.b.domain =  p->d_id[2];
	purex->pur_info.pur_did.b.area =  p->d_id[1];
	purex->pur_info.pur_did.b.al_pa =  p->d_id[0];
	purex->pur_info.vp_idx = p->vp_idx;

	rc = __qla_copy_purex_to_buffer(vha, pkt, rsp, purex->msgp,
		purex->msgp_len);
	if (rc) {
		qla_els_reject_iocb(vha, (*rsp)->qpair, &a);
		qla_enode_free(vha, ptr);
		return;
	}

	//ql_dump_buffer(ql_dbg_edif, vha, 0x70e0,
	//    purex->msgp, purex->pur_info.pur_bytes_rcvd);
	beid.al_pa = purex->pur_info.pur_did.b.al_pa;
	beid.area   = purex->pur_info.pur_did.b.area;
	beid.domain = purex->pur_info.pur_did.b.domain;
	host = qla_find_host_by_d_id(vha, beid);
	if (!host) {
		ql_log(ql_log_fatal, vha, 0x508b,
		    "%s Drop ELS due to unable to find host %06x\n",
		    __func__, purex->pur_info.pur_did.b24);

		qla_els_reject_iocb(vha, (*rsp)->qpair, &a);
		qla_enode_free(vha, ptr);
		return;
	}

	fcport = qla2x00_find_fcport_by_pid(host, &purex->pur_info.pur_sid);

	if (host->e_dbell.db_flags != EDB_ACTIVE ||
	    (fcport && EDIF_SESSION_DOWN(fcport))) {
		ql_dbg(ql_dbg_edif, host, 0x0910c, "%s e_dbell.db_flags =%x %06x\n",
		    __func__, host->e_dbell.db_flags,
		    fcport ? fcport->d_id.b24 : 0);

		qla_els_reject_iocb(host, (*rsp)->qpair, &a);
		qla_enode_free(host, ptr);
		return;
	}

	/* add the local enode to the list */
	qla_enode_add(host, ptr);

	ql_dbg(ql_dbg_edif, host, 0x0910c,
	    "%s COMPLETE purex->pur_info.pur_bytes_rcvd =%xh "
	    "s:%06x -> d:%06x xchg=%xh\n",
	    __func__, purex->pur_info.pur_bytes_rcvd,
	    purex->pur_info.pur_sid.b24,
	    purex->pur_info.pur_did.b24, p->rx_xchg_addr);

	qla_edb_eventcreate(host, VND_CMD_AUTH_STATE_ELS_RCVD, sid, 0, NULL);
}


static uint16_t  qla_edif_get_sa_index_from_freepool(fc_port_t *fcport, int dir)
{
	struct scsi_qla_host *vha = fcport->vha;
	struct qla_hw_data *ha = vha->hw;
	void *sa_id_map;
	unsigned long flags = 0;
	u16 sa_index;

	ql_dbg(ql_dbg_edif + ql_dbg_verbose, vha, 0x3063,
	    "%s: entry\n", __func__);

	if (dir) {
		sa_id_map = ha->edif_tx_sa_id_map;
	} else {
		sa_id_map = ha->edif_rx_sa_id_map;
	}

	spin_lock_irqsave(&ha->sadb_fp_lock, flags);
	sa_index = find_first_zero_bit(sa_id_map, EDIF_NUM_SA_INDEX);
	if (sa_index >=  EDIF_NUM_SA_INDEX) {
		spin_unlock_irqrestore(&ha->sadb_fp_lock, flags);
		return INVALID_EDIF_SA_INDEX;
	} else {
		set_bit(sa_index, sa_id_map);
	}
	spin_unlock_irqrestore(&ha->sadb_fp_lock, flags);


	if (dir) {
		sa_index += EDIF_TX_SA_INDEX_BASE;
	}

	ql_dbg(ql_dbg_edif, vha, 0x3063,
	    "%s: index retrieved from free pool %d\n", __func__, sa_index);

	return sa_index;
}



//
// find an sadb entry for an nport_handle
//
static edif_sa_index_entry_t *
qla_edif_sadb_find_sa_index_entry(uint16_t nport_handle,
    struct list_head *sa_list)
{
	edif_sa_index_entry_t *entry;
	edif_sa_index_entry_t *tentry;
	struct list_head *indx_list = sa_list;

	list_for_each_entry_safe(entry, tentry, indx_list, next) {
		if (entry->handle == nport_handle) {
			return entry;
		}
	}
	return NULL;
}


// remove an sa_index from the nport_handle and return it to the free pool
int qla_edif_sadb_delete_sa_index(fc_port_t *fcport, uint16_t nport_handle,
    uint16_t sa_index)
{
	edif_sa_index_entry_t *entry;
	struct list_head *sa_list;
	int dir = (sa_index < EDIF_TX_SA_INDEX_BASE) ? 0 : 1;
	int slot = 0;
	int free_slot_count = 0;
	scsi_qla_host_t *vha = fcport->vha;
	struct qla_hw_data *ha = vha->hw;
	unsigned long flags = 0;

	ql_dbg(ql_dbg_edif, vha, 0x3063,
	    "%s: entry\n", __func__);

	if (dir) {
		sa_list = &ha->sadb_tx_index_list;
	} else {
		sa_list = &ha->sadb_rx_index_list;
	}

	entry = qla_edif_sadb_find_sa_index_entry(nport_handle, sa_list);
	if (!entry) {
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: no entry found for nport_handle 0x%x\n",
		    __func__, nport_handle);
		return -1;
	}

	spin_lock_irqsave(&ha->sadb_lock, flags);
	for (slot=0; slot< 2; slot++) {
		if (entry->sa_pair[slot].sa_index == sa_index) {
			entry->sa_pair[slot].sa_index = INVALID_EDIF_SA_INDEX;
			entry->sa_pair[slot].spi = 0;
			free_slot_count++;
			qla_edif_add_sa_index_to_freepool(fcport, dir, sa_index);
		} else if (entry->sa_pair[slot].sa_index == INVALID_EDIF_SA_INDEX) {
			free_slot_count++;
		}
	}

	if (free_slot_count == 2) {
		list_del(&entry->next);
		kfree(entry);
	}
	spin_unlock_irqrestore(&ha->sadb_lock, flags);

	ql_dbg(ql_dbg_edif, vha, 0x3063,
	    "%s: sa_index %d removed, free_slot_count: %d\n",
	    __func__, sa_index, free_slot_count);

	return 0;
}

void
qla28xx_sa_update_iocb_entry(scsi_qla_host_t *v, struct req_que *req,
    struct sa_update_28xx *pkt)
{
	const char func[] = "SA_UPDATE_RESPONSE_IOCB";
	srb_t *sp;
	struct 	edif_sa_ctl *sa_ctl;
	int old_sa_deleted = 1;
	uint16_t nport_handle;
	struct scsi_qla_host *vha;

	sp = qla2x00_get_sp_from_handle(v, func, req, pkt);

	if (!sp) {
		ql_dbg(ql_dbg_edif, v, 0x3063,
			"%s: no sp found for pkt\n", __func__);
		return;
	}
	// use sp->vha due to npiv
	vha = sp->vha;

	switch (pkt->flags & (SA_FLAG_INVALIDATE|SA_FLAG_TX)) {
	case 0:
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: EDIF SA UPDATE RX IOCB  vha: 0x%px  index: %d\n",
		    __func__, vha, pkt->sa_index);
		break;
	case 1:
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: EDIF SA DELETE RX IOCB  vha: 0x%px  index: %d\n",
		    __func__, vha, pkt->sa_index);
		break;
	case 2:
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: EDIF SA UPDATE TX IOCB  vha: 0x%px  index: %d\n",
		    __func__, vha, pkt->sa_index);
		break;
	case 3:
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: EDIF SA DELETE TX IOCB  vha: 0x%px  index: %d\n",
		    __func__, vha, pkt->sa_index);
		break;
	}

	//
	// dig the nport handle out of the iocb, fcport->loop_id can not be trusted
	// to be correct during cleanup sa_update iocbs.
	//
	nport_handle = sp->fcport->loop_id;

	ql_dbg(ql_dbg_edif, vha, 0x3063,
	    "%s: %8phN comp status=%x entry_status=%x old_sa_info=%x new_sa_info=%x nport_handle %d,"
	    "index=0x%x pkt_flags %xh hdl=%x\n",
	    __func__, sp->fcport->port_name,
	    pkt->u.comp_sts, pkt->entry_status,
	    pkt->old_sa_info, pkt->new_sa_info, nport_handle,
	    pkt->sa_index, pkt->flags, sp->handle);


	//
	// if rx delete, remove the timer
	//
	if ( (pkt->flags & (SA_FLAG_INVALIDATE | SA_FLAG_TX)) ==  SA_FLAG_INVALIDATE) {
		edif_list_entry_t *edif_entry;
		sp->fcport->flags &= ~(FCF_ASYNC_SENT|FCF_ASYNC_ACTIVE);

		edif_entry = qla_edif_list_find_sa_index(sp->fcport, nport_handle);
		if (edif_entry) {
			ql_dbg(ql_dbg_edif, vha, 0x5033,
			    "%s: removing edif_entry %px, new sa_index: 0x%x\n",
			    __func__, edif_entry, pkt->sa_index);
			qla_edif_list_delete_sa_index(sp->fcport, edif_entry);
			del_timer(&edif_entry->timer);

			ql_dbg(ql_dbg_edif, vha, 0x5033,
			    "%s: releasing edif_entry %px, new sa_index: 0x%x\n",
			    __func__, edif_entry, pkt->sa_index);

			kfree(edif_entry);
		}
	}

	// if this is a delete for either tx or rx, make sure it succeeded.
	// The new_sa_info field should be 0xffff on success
	if (pkt->flags & SA_FLAG_INVALIDATE) {
		old_sa_deleted = (le16_to_cpu(pkt->new_sa_info) == 0xffff) ? 1: 0;
	}

	//
	// Process update and delete the same way
	//

	// If this is an sadb cleanup delete, bypass sending events to IPSEC
	if (sp->flags & SRB_EDIF_CLEANUP_DELETE ) {
		sp->fcport->flags &= ~(FCF_ASYNC_SENT|FCF_ASYNC_ACTIVE);
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: nph 0x%x, sa_index %d removed from fw\n",
		    __func__, sp->fcport->loop_id, pkt->sa_index );

	} else if ((pkt->entry_status == 0) && (pkt->u.comp_sts == 0) &&
	    old_sa_deleted) {
		// Note: Wa are only keeping track of latest SA,
		// so we know when we can start enableing encryption per I/O.
		// If all SA's get deleted, let FW reject the IOCB.

		// TODO: edif: don't set enabled here I think
		// TODO: edif: prli complete is where it should be set
		ql_dbg(ql_dbg_edif + ql_dbg_verbose, vha, 0x3063,
			"SA(%x)updated for s_id %02x%02x%02x\n",
			pkt->new_sa_info,
			pkt->port_id[2], pkt->port_id[1], pkt->port_id[0]);
		sp->fcport->edif.enable = 1;
		if (pkt->flags & SA_FLAG_TX) {
			sp->fcport->edif.tx_sa_set = 1;
			sp->fcport->edif.tx_sa_pending = 0;
			qla_edb_eventcreate(vha, VND_CMD_AUTH_STATE_SAUPDATE_COMPL,
				QL_VND_SA_STAT_SUCCESS,
				QL_VND_TX_SA_KEY, sp->fcport);

		} else {
			sp->fcport->edif.rx_sa_set = 1;
			sp->fcport->edif.rx_sa_pending = 0;
			qla_edb_eventcreate(vha, VND_CMD_AUTH_STATE_SAUPDATE_COMPL,
				QL_VND_SA_STAT_SUCCESS,
				QL_VND_RX_SA_KEY, sp->fcport);
		}
	} else {

		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: %8phN SA update FAILED: sa_index: %d, new_sa_info %d, "
		    "%02x%02x%02x -- dumping \n",__func__, sp->fcport->port_name,
		    pkt->sa_index, pkt->new_sa_info, pkt->port_id[2],
		    pkt->port_id[1], pkt->port_id[0]);

		if (pkt->flags & SA_FLAG_TX)
			qla_edb_eventcreate(vha, VND_CMD_AUTH_STATE_SAUPDATE_COMPL,
//				QL_VND_SA_STAT_FAILED,
				(le16_to_cpu(pkt->u.comp_sts) << 16) | QL_VND_SA_STAT_FAILED,
				QL_VND_TX_SA_KEY, sp->fcport);
		else
			qla_edb_eventcreate(vha, VND_CMD_AUTH_STATE_SAUPDATE_COMPL,
//				QL_VND_SA_STAT_FAILED,
				(le16_to_cpu(pkt->u.comp_sts) << 16) | QL_VND_SA_STAT_FAILED,
				QL_VND_RX_SA_KEY, sp->fcport);
	}


	// for delete, release sa_ctl, sa_index
	if (pkt->flags & SA_FLAG_INVALIDATE) {
		//
		// release the sa_ctl
		//
		sa_ctl = qla_edif_find_sa_ctl_by_index(sp->fcport,
		    le16_to_cpu(pkt->sa_index), (pkt->flags & SA_FLAG_TX));
		if ((sa_ctl != NULL) &&
		    (qla_edif_find_sa_ctl_by_index(sp->fcport, sa_ctl->index,
			(pkt->flags & SA_FLAG_TX)) != NULL)) {
			ql_dbg(ql_dbg_edif+ql_dbg_verbose, vha, 0x3063,
			    "%s: freeing sa_ctl for index %d\n",
			    __func__, sa_ctl->index);
			qla_edif_free_sa_ctl(sp->fcport, sa_ctl, sa_ctl->index);
		} else {
			ql_dbg(ql_dbg_edif, vha, 0x3063,
			    "%s: sa_ctl NOT freed, sa_ctl: %px\n",
			    __func__, sa_ctl);

		}
		//
		//
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: freeing sa_index %d, nph: 0x%x\n",
		    __func__, le16_to_cpu(pkt->sa_index), nport_handle);
		qla_edif_sadb_delete_sa_index(sp->fcport, nport_handle,
		    le16_to_cpu(pkt->sa_index));

	//
	// check for a failed sa_update and remove
	// the sadb entry.
	//
	} else if (pkt->u.comp_sts) {
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: freeing sa_index %d, nph: 0x%x\n",
		    __func__, pkt->sa_index, nport_handle);
		qla_edif_sadb_delete_sa_index(sp->fcport, nport_handle,
		    le16_to_cpu(pkt->sa_index));
		switch (le16_to_cpu(pkt->u.comp_sts)) {
		case CS_PORT_EDIF_UNAVAIL:
		case CS_PORT_EDIF_LOGOUT:
			qlt_schedule_sess_for_deletion(sp->fcport);
			break;
		default:
			break;
		}
	}

	sp->done(sp, 0);
}

/*
 * qla28xx_start_scsi_edif() - Send a SCSI type 6 command ot the ISP
 * @sp: command to send to the ISP
 * req/rsp queue to use for this request
 * lock to protect submission
 *
 * Returns non-zero if a failure occurred, else zero.
 */
int
qla28xx_start_scsi_edif(srb_t *sp)
{
	int             nseg;
	unsigned long   flags;
	struct scsi_cmnd *cmd;
	uint32_t        *clr_ptr;
	uint32_t        index, i;
	uint32_t        handle;
	uint16_t        cnt;
	int16_t        req_cnt;
	uint16_t        tot_dsds;
	__be32 *fcp_dl;
	uint8_t additional_cdb_len;
	struct ct6_dsd *ctx;
	struct scsi_qla_host *vha = sp->vha;
	struct qla_hw_data *ha = vha->hw;
	struct cmd_type_6 *cmd_pkt;
	struct dsd64	*cur_dsd;
	uint8_t		avail_dsds = 0;
	struct 	scatterlist *sg;
	struct req_que *req = sp->qpair->req;
	spinlock_t *lock = sp->qpair->qp_lock_ptr;

	/* Setup device pointers. */
	cmd = GET_CMD_SP(sp);

	/* So we know we haven't pci_map'ed anything yet */
	tot_dsds = 0;

	/* Send marker if required */
	if (vha->marker_needed != 0) {
		if (qla2x00_marker(vha, sp->qpair, 0, 0, MK_SYNC_ALL) !=
			QLA_SUCCESS) {
			ql_log(ql_log_warn, vha, 0x300c,
			    "qla2x00_marker failed for cmd=%px.\n", cmd);
			return QLA_FUNCTION_FAILED;
		}
		vha->marker_needed = 0;
	}

	/* Acquire ring specific lock */
	spin_lock_irqsave(lock, flags);

	/* Check for room in outstanding command list. */
	handle = req->current_outstanding_cmd;
	for (index = 1; index < req->num_outstanding_cmds; index++) {
		handle++;
		if (handle == req->num_outstanding_cmds)
			handle = 1;
		if (!req->outstanding_cmds[handle])
		break;
	}
	if (index == req->num_outstanding_cmds)
		goto queuing_error;

	/* Map the sg table so we have an accurate count of sg entries needed */
	if (scsi_sg_count(cmd)) {
		nseg = dma_map_sg(&ha->pdev->dev, scsi_sglist(cmd),
		    scsi_sg_count(cmd), cmd->sc_data_direction);
		if (unlikely(!nseg))
			goto queuing_error;
	} else
		nseg = 0;

	tot_dsds = nseg;
	req_cnt = qla24xx_calc_iocbs(vha, tot_dsds);
	if (req->cnt < (req_cnt + 2)) {
		cnt = IS_SHADOW_REG_CAPABLE(ha) ? *req->out_ptr :
		RD_REG_DWORD_RELAXED(req->req_q_out);
		if (req->ring_index < cnt)
			req->cnt = cnt - req->ring_index;
		else
			req->cnt = req->length -
			    (req->ring_index - cnt);
		if (req->cnt < (req_cnt + 2))
			goto queuing_error;
	}

	ctx = sp->u.scmd.ct6_ctx =
	    mempool_alloc(ha->ctx_mempool, GFP_ATOMIC);
	if (!ctx) {
		ql_log(ql_log_fatal, vha, 0x3010,
		    "Failed to allocate ctx for cmd=%px.\n", cmd);
		goto queuing_error;
	}

	memset(ctx, 0, sizeof(struct ct6_dsd));
	ctx->fcp_cmnd = dma_pool_zalloc(ha->fcp_cmnd_dma_pool,
	    GFP_ATOMIC, &ctx->fcp_cmnd_dma);
	if (!ctx->fcp_cmnd) {
		ql_log(ql_log_fatal, vha, 0x3011,
		    "Failed to allocate fcp_cmnd for cmd=%px.\n", cmd);
		goto queuing_error;
	}

	/* Initialize the DSD list and dma handle */
	INIT_LIST_HEAD(&ctx->dsd_list);
	ctx->dsd_use_cnt = 0;

	if (cmd->cmd_len > 16) {
		additional_cdb_len = cmd->cmd_len - 16;
		if ((cmd->cmd_len % 4) != 0) {
			/* SCSI command bigger than 16 bytes must be
			 * multiple of 4
			 */
			ql_log(ql_log_warn, vha, 0x3012,
			    "scsi cmd len %d not multiple of 4 "
			    "for cmd=%px.\n", cmd->cmd_len, cmd);
			goto queuing_error_fcp_cmnd;
		}
			ctx->fcp_cmnd_len = 12 + cmd->cmd_len + 4;
	} else {
		additional_cdb_len = 0;
		ctx->fcp_cmnd_len = 12 + 16 + 4;
	}

	cmd_pkt = (struct cmd_type_6 *)req->ring_ptr;
	cmd_pkt->handle = MAKE_HANDLE(req->id, handle);

	/* Zero out remaining portion of packet. */
	/*    tagged queuing modifier -- default is TSK_SIMPLE (0). */
	clr_ptr = (uint32_t *)cmd_pkt + 2;
	memset(clr_ptr, 0, REQUEST_ENTRY_SIZE - 8);
	cmd_pkt->dseg_count = cpu_to_le16(tot_dsds);

	/* No data transfer */
	if (!scsi_bufflen(cmd) || cmd->sc_data_direction == DMA_NONE) {
		cmd_pkt->byte_count = cpu_to_le32(0);
		goto no_dsds;
	}

	/* Set transfer direction */
	if (cmd->sc_data_direction == DMA_TO_DEVICE) {
		cmd_pkt->control_flags = cpu_to_le16(CF_WRITE_DATA);
		vha->qla_stats.output_bytes += scsi_bufflen(cmd);
		vha->qla_stats.output_requests++;
		sp->fcport->edif.tx_bytes+= scsi_bufflen(cmd);
	} else if (cmd->sc_data_direction == DMA_FROM_DEVICE) {
		cmd_pkt->control_flags = cpu_to_le16(CF_READ_DATA);
		vha->qla_stats.input_bytes += scsi_bufflen(cmd);
		vha->qla_stats.input_requests++;
		sp->fcport->edif.rx_bytes+= scsi_bufflen(cmd);
	}

	cmd_pkt->control_flags |= cpu_to_le16(CF_EN_EDIF);
	cmd_pkt->control_flags &= ~(cpu_to_le16(CF_NEW_SA));

	/* One DSD is available in the Command Type 6 IOCB */
	avail_dsds = 1;
	cur_dsd = &cmd_pkt->fcp_dsd;

	/* Load data segments */
	scsi_for_each_sg(cmd, sg, tot_dsds, i) {
		dma_addr_t      sle_dma;
		cont_a64_entry_t *cont_pkt;

		/* Allocate additional continuation packets? */
		if (avail_dsds == 0) {
			/*
			 * Five DSDs are available in the Continuation
			 * Type 1 IOCB.
			 */
			cont_pkt = qla2x00_prep_cont_type1_iocb(vha, req);
			cur_dsd = cont_pkt->dsd;
			avail_dsds = 5;
		}

		sle_dma = sg_dma_address(sg);
		put_unaligned_le64(sle_dma, &cur_dsd->address);
		cur_dsd->length = cpu_to_le32(sg_dma_len(sg));
		cur_dsd++;
		avail_dsds--;
	}

no_dsds:
	/* Set NPORT-ID and LUN number*/
	cmd_pkt->nport_handle = cpu_to_le16(sp->fcport->loop_id);
	cmd_pkt->port_id[0] = sp->fcport->d_id.b.al_pa;
	cmd_pkt->port_id[1] = sp->fcport->d_id.b.area;
	cmd_pkt->port_id[2] = sp->fcport->d_id.b.domain;
	cmd_pkt->vp_index = sp->vha->vp_idx;

	cmd_pkt->entry_type = COMMAND_TYPE_6;

	/* Set total data segment count. */
	cmd_pkt->entry_count = (uint8_t)req_cnt;

	int_to_scsilun(cmd->device->lun, &cmd_pkt->lun);
	host_to_fcp_swap((uint8_t *)&cmd_pkt->lun, sizeof(cmd_pkt->lun));

	/* build FCP_CMND IU */
	int_to_scsilun(cmd->device->lun, &ctx->fcp_cmnd->lun);
	ctx->fcp_cmnd->additional_cdb_len = additional_cdb_len;

	if (cmd->sc_data_direction == DMA_TO_DEVICE)
		ctx->fcp_cmnd->additional_cdb_len |= 1;
	else if (cmd->sc_data_direction == DMA_FROM_DEVICE)
		ctx->fcp_cmnd->additional_cdb_len |= 2;

	/* Populate the FCP_PRIO. */
	if (ha->flags.fcp_prio_enabled)
		ctx->fcp_cmnd->task_attribute |=
		    sp->fcport->fcp_prio << 3;

	memcpy(ctx->fcp_cmnd->cdb, cmd->cmnd, cmd->cmd_len);

	fcp_dl = (__be32 *)(ctx->fcp_cmnd->cdb + 16 +
	    additional_cdb_len);
	*fcp_dl = htonl((uint32_t)scsi_bufflen(cmd));

	cmd_pkt->fcp_cmnd_dseg_len = cpu_to_le16(ctx->fcp_cmnd_len);
	put_unaligned_le64(ctx->fcp_cmnd_dma, &cmd_pkt->fcp_cmnd_dseg_address);

	sp->flags |= SRB_FCP_CMND_DMA_VALID;
	cmd_pkt->byte_count = cpu_to_le32((uint32_t)scsi_bufflen(cmd));
	/* Set total data segment count. */
	cmd_pkt->entry_count = (uint8_t)req_cnt;
	cmd_pkt->entry_status = 0;

	/* Build command packet. */
	req->current_outstanding_cmd = handle;
	req->outstanding_cmds[handle] = sp;
	sp->handle = handle;
	cmd->host_scribble = (unsigned char *)(unsigned long)handle;
	req->cnt -= req_cnt;
	wmb();

	/* Adjust ring index. */
	req->ring_index++;
	if (req->ring_index == req->length) {
		req->ring_index = 0;
		req->ring_ptr = req->ring;
	} else
		req->ring_ptr++;

	sp->qpair->cmd_cnt++;
	/* Set chip new ring index. */
	WRT_REG_DWORD(req->req_q_in, req->ring_index);

	spin_unlock_irqrestore(lock, flags);

#ifdef QLA2XXX_LATENCY_MEASURE
	ktime_get_real_ts64(&sp->cmd_to_req_q);
#endif
	return QLA_SUCCESS;

queuing_error_fcp_cmnd:
	dma_pool_free(ha->fcp_cmnd_dma_pool, ctx->fcp_cmnd, ctx->fcp_cmnd_dma);
queuing_error:
	if (tot_dsds)
		scsi_dma_unmap(cmd);

	if (sp->u.scmd.ct6_ctx) {
		mempool_free(sp->u.scmd.ct6_ctx, ha->ctx_mempool);
		sp->u.scmd.ct6_ctx = NULL;
	}
	spin_unlock_irqrestore(lock, flags);

	return QLA_FUNCTION_FAILED;
}


// *********************************************************
// edif update/delete sa_index list functions

//
// clear the edif_indx_list for this port
//
void qla_edif_list_del(fc_port_t *fcport)
{
	edif_list_entry_t *indx_lst;
	edif_list_entry_t *tindx_lst;
	struct list_head *indx_list = &fcport->edif.edif_indx_list;
	unsigned long flags = 0;

	list_for_each_entry_safe(indx_lst, tindx_lst, indx_list, next) {
		spin_lock_irqsave(&fcport->edif.indx_list_lock, flags);
		list_del(&indx_lst->next);
		spin_unlock_irqrestore(&fcport->edif.indx_list_lock, flags);
		kfree(indx_lst);
	}
}



// ****************************************************
// SADB functions
//

//
// allocate/retrieve an sa_index for a given spi
//
static uint16_t qla_edif_sadb_get_sa_index(fc_port_t *fcport,
    struct qla_sa_update_frame *sa_frame)
{

	edif_sa_index_entry_t *entry;
	struct list_head *sa_list;
	uint16_t sa_index;
	int dir = sa_frame->flags & SAU_FLG_TX;
	int slot = 0;
	int free_slot = -1;
	scsi_qla_host_t *vha = fcport->vha;
	struct qla_hw_data *ha = vha->hw;
	unsigned long flags = 0;
	uint16_t nport_handle = fcport->loop_id;
	ql_dbg(ql_dbg_edif, vha, 0x3063,
	    "%s: entry  fc_port: %px, nport_handle: 0x%x\n",
	    __func__, fcport, nport_handle);

	if (dir) {
		sa_list = &ha->sadb_tx_index_list;
	} else {
		sa_list = &ha->sadb_rx_index_list;
	}

	entry = qla_edif_sadb_find_sa_index_entry(nport_handle, sa_list);
	if (!entry) {

		if ( (sa_frame->flags & (SAU_FLG_TX | SAU_FLG_INV)) == SAU_FLG_INV ) {
			ql_dbg(ql_dbg_edif, vha, 0x3063,
			    "%s: rx delete request with no entry\n", __func__);
			return RX_DELETE_NO_EDIF_SA_INDEX;

		}

		//if there is no entry for this nport, add one
		entry = kzalloc((sizeof(edif_sa_index_entry_t)),GFP_ATOMIC);
		if (!entry) {
			return INVALID_EDIF_SA_INDEX;
		}

		sa_index = qla_edif_get_sa_index_from_freepool(fcport, dir);
		if (sa_index == INVALID_EDIF_SA_INDEX) {
			kfree(entry);
			return INVALID_EDIF_SA_INDEX;
		}

		INIT_LIST_HEAD(&entry->next);
		entry->handle = nport_handle;
		entry->fcport = fcport;
		entry->sa_pair[0].spi = sa_frame->spi;
		entry->sa_pair[0].sa_index = sa_index;
		entry->sa_pair[1].spi = 0;
		entry->sa_pair[1].sa_index = INVALID_EDIF_SA_INDEX;
		spin_lock_irqsave(&ha->sadb_lock, flags);
		list_add_tail(&entry->next, sa_list);
		spin_unlock_irqrestore(&ha->sadb_lock, flags);
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: Created new sadb entry for nport_handle 0x%x, "
		    "spi 0x%x, returning sa_index %d\n",
		    __func__, nport_handle, sa_frame->spi, sa_index );
		return sa_index;

	} else {

		spin_lock_irqsave(&ha->sadb_lock, flags);

		// see if we already have an entry for this spi
		for (slot=0; slot< 2; slot++) {

			if (entry->sa_pair[slot].sa_index == INVALID_EDIF_SA_INDEX) {
				free_slot = slot;
			} else {
				if (entry->sa_pair[slot].spi == sa_frame->spi) {
					spin_unlock_irqrestore(&ha->sadb_lock, flags);
					ql_dbg(ql_dbg_edif, vha, 0x3063,
					    "%s: sadb slot %d entry for "
					    "nport_handle 0x%x, spi 0x%x found, "
					    "returning sa_index %d\n",
					    __func__, slot, entry->handle,
					    sa_frame->spi,entry->sa_pair[slot].sa_index );
					return entry->sa_pair[slot].sa_index;
				}
			}

		}
		spin_unlock_irqrestore(&ha->sadb_lock, flags);

		// both slots are used,
		if (free_slot == -1) {
			ql_dbg(ql_dbg_edif, vha, 0x3063,
			    "%s: WARNING: No free slots in sadb for "
			    "nport_handle 0x%x, spi: 0x%x\n",
			    __func__, entry->handle, sa_frame->spi);
			ql_dbg(ql_dbg_edif, vha, 0x3063,
			    "%s:   Slot 0  spi: 0x%x  sa_index: %d\n",
			    __func__, entry->sa_pair[0].spi,
			    entry->sa_pair[0].sa_index);
			ql_dbg(ql_dbg_edif, vha, 0x3063,
			    "%s:   Slot 1  spi: 0x%x  sa_index: %d\n",
			    __func__, entry->sa_pair[1].spi,
			    entry->sa_pair[1].sa_index);

			return INVALID_EDIF_SA_INDEX;
		}

		// there is at least one free slot, use it
		sa_index = qla_edif_get_sa_index_from_freepool(fcport, dir);
		if (sa_index == INVALID_EDIF_SA_INDEX) {
			ql_dbg(ql_dbg_edif, fcport->vha, 0x3063,
			    "%s: empty freepool!!\n",__func__);
			return INVALID_EDIF_SA_INDEX;
		}

		spin_lock_irqsave(&ha->sadb_lock, flags);
		entry->sa_pair[free_slot].spi = sa_frame->spi;
		entry->sa_pair[free_slot].sa_index = sa_index;
		spin_unlock_irqrestore(&ha->sadb_lock, flags);
		ql_dbg(ql_dbg_edif, fcport->vha, 0x3063,
		    "%s: sadb slot %d entry for nport_handle 0x%x, "
		    "spi 0x%x added, returning sa_index %d\n",
		    __func__, free_slot, entry->handle, sa_frame->spi,
		    sa_index );

		return sa_index;
	}
}




//
// release any sadb entries -- only done at teardown
//
void qla_edif_sadb_release(struct qla_hw_data *ha)
{
	struct list_head *pos;
	struct list_head *tmp;
	edif_sa_index_entry_t *entry;

	list_for_each_safe(pos, tmp, &ha->sadb_rx_index_list) {
		entry = list_entry(pos, edif_sa_index_entry_t, next);
		list_del(&entry->next);
		kfree(entry);
	}

	list_for_each_safe(pos, tmp, &ha->sadb_tx_index_list) {
		entry = list_entry(pos, edif_sa_index_entry_t, next);
		list_del(&entry->next);
		kfree(entry);
	}
}

// *****************************************
// sadb freepool functions
//

//
// build the rx and tx sa_index free pools -- only done at fcport init
//
int qla_edif_sadb_build_free_pool(struct qla_hw_data *ha)
{
	ha->edif_tx_sa_id_map =
	kzalloc(BITS_TO_LONGS(EDIF_NUM_SA_INDEX) * sizeof(long), GFP_KERNEL);

	if (!ha->edif_tx_sa_id_map) {
		ql_log_pci(ql_log_fatal, ha->pdev, 0x0009,
		    "Unable to allocate memory for sadb tx.\n");
		return ENOMEM;
	}

	ha->edif_rx_sa_id_map =
	kzalloc(BITS_TO_LONGS(EDIF_NUM_SA_INDEX) * sizeof(long), GFP_KERNEL);
	if (!ha->edif_rx_sa_id_map) {
		kfree(ha->edif_tx_sa_id_map);
		ha->edif_tx_sa_id_map = NULL;
		ql_log_pci(ql_log_fatal, ha->pdev, 0x0009,
		    "Unable to allocate memory for sadb rx.\n");
		return ENOMEM;
	}
	return 0;
}


//
// release the free pool - only done during fcport teardown
//
void qla_edif_sadb_release_free_pool(struct qla_hw_data *ha)
{
	kfree(ha->edif_tx_sa_id_map);
	ha->edif_tx_sa_id_map = NULL;
	kfree(ha->edif_rx_sa_id_map);
	ha->edif_rx_sa_id_map = NULL;
}


static void __chk_edif_rx_sa_delete_pending(scsi_qla_host_t *vha,
    fc_port_t *fcport, uint32_t handle, uint16_t sa_index)
{
	edif_list_entry_t *edif_entry;
	struct 	edif_sa_ctl * sa_ctl;
	uint16_t delete_sa_index = INVALID_EDIF_SA_INDEX;
	unsigned long flags = 0;
	uint16_t nport_handle = fcport->loop_id;
	uint16_t cached_nport_handle;

	spin_lock_irqsave(&fcport->edif.indx_list_lock, flags);
	edif_entry = qla_edif_list_find_sa_index(fcport, nport_handle);
	if (!edif_entry) {
		spin_unlock_irqrestore(&fcport->edif.indx_list_lock, flags);
		return;		// no pending delete for this handle
	}

	//
	// check for no pending delete for this index or iocb does not
	// match rx sa_index
	//
	if ((edif_entry->delete_sa_index == INVALID_EDIF_SA_INDEX) ||
	    (edif_entry->update_sa_index != sa_index)) {
		spin_unlock_irqrestore(&fcport->edif.indx_list_lock, flags);
		return;
	}

	//
	// wait until we have seen at least EDIF_DELAY_COUNT transfers before
	// queueing RX delete
	//
	if (edif_entry->count++ < EDIF_RX_DELETE_FILTER_COUNT) {
		spin_unlock_irqrestore(&fcport->edif.indx_list_lock, flags);
		return;
	}

	ql_dbg(ql_dbg_edif, vha, 0x5033,
	    "%s: invalidating delete_sa_index,  update_sa_index: 0x%x "
	    "sa_index: 0x%x, delete_sa_index: 0x%x\n",
	    __func__, edif_entry->update_sa_index , sa_index,
	    edif_entry->delete_sa_index);

	delete_sa_index = edif_entry->delete_sa_index;
	edif_entry->delete_sa_index = INVALID_EDIF_SA_INDEX;
	cached_nport_handle = edif_entry->handle;
	spin_unlock_irqrestore(&fcport->edif.indx_list_lock, flags);

	// sanity check on the nport handle
	if (nport_handle != cached_nport_handle) {
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: POST SA DELETE nport_handle mismatch: fcport "
		    "nph: 0x%x, edif_entry nph: 0x%x, using fcport handle.\n",
		    __func__, nport_handle, cached_nport_handle);
	}

	//
	// find the sa_ctl for the delete and schedule the delete
	//
	sa_ctl = qla_edif_find_sa_ctl_by_index(fcport, delete_sa_index, 0);
	if (sa_ctl) {
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: POST SA DELETE sa_ctl: %px, index recvd %d, delete "
		    "index %d, update index: %d, nport handle: 0x%x, handle: 0x%x\n",
		    __func__, sa_ctl, sa_index, delete_sa_index,
		    edif_entry->update_sa_index, nport_handle, handle);

		sa_ctl->flags = EDIF_SA_CTL_FLG_DEL;
		set_bit(EDIF_SA_CTL_REPL, &sa_ctl->state);
		qla_post_sa_replace_work(fcport->vha, fcport,
		    nport_handle, sa_ctl);

	} else {
		ql_dbg(ql_dbg_edif, vha, 0x3063,
		    "%s: POST SA DELETE sa_ctl not found for delete_sa_index: %d\n",
		    __func__, delete_sa_index);
	}

}
void qla_chk_edif_rx_sa_delete_pending(scsi_qla_host_t *vha,
    srb_t *sp, struct sts_entry_24xx *sts24)
{
	fc_port_t *fcport = sp->fcport;
	// sa_index used by this iocb
	struct scsi_cmnd *cmd = GET_CMD_SP(sp);
	uint32_t handle;

	handle = (uint32_t) LSW(sts24->handle);

	// find out if this status iosb is for a scsi read
	if (cmd->sc_data_direction != DMA_FROM_DEVICE) {
		return;
	}

	return __chk_edif_rx_sa_delete_pending(vha, fcport, handle,
	   le16_to_cpu(sts24->edif_sa_index));
}

void qlt_chk_edif_rx_sa_delete_pending(scsi_qla_host_t *vha, fc_port_t *fcport,
    struct ctio7_from_24xx *pkt)
{
	__chk_edif_rx_sa_delete_pending(vha, fcport,
	    pkt->handle, le16_to_cpu(pkt->edif_sa_index));
}

void qla_parse_auth_els_ctl(struct srb *sp)
{
	int req_data_len = 0;
	struct qla_els_pt_arg *a = &sp->u.bsg_cmd.u.els_arg;
	bsg_job_t *bsg_job = sp->u.bsg_cmd.bsg_job;
	struct fc_bsg_request *request = bsg_job->request;
	struct qla_bsg_auth_els_request *p =
	    (struct qla_bsg_auth_els_request*)bsg_job->request;

	req_data_len = sp->remap.req.len;

	a->tx_len = a->tx_byte_count = sp->remap.req.len;
	a->tx_addr = sp->remap.req.dma;
	a->rx_len = a->rx_byte_count = sp->remap.rsp.len;
	a->rx_addr = sp->remap.rsp.dma;

	if (p->e.sub_cmd == SEND_ELS_REPLY) {
		a->control_flags = p->e.extra_control_flags << 13;
		a->rx_xchg_address = cpu_to_le32(p->e.extra_rx_xchg_address);
		if (p->e.extra_control_flags == BSG_CTL_FLAG_LS_ACC)
			a->els_opcode = ELS_LS_ACC;
		else if (p->e.extra_control_flags == BSG_CTL_FLAG_LS_RJT)
			a->els_opcode = ELS_LS_RJT;
	}
	a->did = sp->fcport->d_id;
	a->els_opcode =  request->rqst_data.h_els.command_code;
	a->nport_handle = cpu_to_le16(sp->fcport->loop_id);
	a->vp_idx = sp->vha->vp_idx;


}

int qla_edif_process_els(scsi_qla_host_t *vha, bsg_job_t *bsg_job)
{
	struct fc_bsg_request *bsg_request = bsg_job->request;
	struct fc_bsg_reply *bsg_reply = bsg_job->reply;
	fc_port_t *fcport = NULL;
	struct qla_hw_data *ha = vha->hw;
	srb_t *sp;
	int rval =  (DID_ERROR << 16);
	port_id_t d_id;
	struct qla_bsg_auth_els_request *p =
	    (struct qla_bsg_auth_els_request*)bsg_job->request;


	d_id.b.al_pa = bsg_request->rqst_data.h_els.port_id[2];
	d_id.b.area = bsg_request->rqst_data.h_els.port_id[1];
	d_id.b.domain = bsg_request->rqst_data.h_els.port_id[0];

	// find matching d_id in fcport list
	if ((fcport = qla2x00_find_fcport_by_pid(vha, &d_id)) == NULL) {
		ql_dbg(ql_dbg_edif, vha, 0x911a,
		    "%s fcport not find online portid=%06x.\n",
		    __func__, d_id.b24);
		SET_DID_STATUS(bsg_reply->result, DID_ERROR);
		return -EIO;
	}


	if (qla_bsg_check(vha, bsg_job, fcport)) {
		return 0;
	}

	if (fcport->loop_id == FC_NO_LOOP_ID) {
		ql_dbg(ql_dbg_edif, vha, 0x910d,
		    "%s ELS code %x, no loop id.\n", __func__,
		    bsg_request->rqst_data.r_els.els_code);
		SET_DID_STATUS(bsg_reply->result, DID_BAD_TARGET);
		return -ENXIO;
	}

	if (!vha->flags.online) {
		ql_log(ql_log_warn, vha, 0x7005, "Host not online.\n");
		SET_DID_STATUS(bsg_reply->result, DID_BAD_TARGET);
		rval = -EIO;
		goto done;
	}

	/* pass through is supported only for ISP 4Gb or higher */
	if (!IS_FWI2_CAPABLE(ha)) {
		ql_dbg(ql_dbg_user, vha, 0x7001,
		    "ELS passthru not supported for ISP23xx based adapters.\n");
		SET_DID_STATUS(bsg_reply->result, DID_BAD_TARGET);
		rval = -EPERM;
		goto done;
	}

	/* Alloc SRB structure */
	sp = qla2x00_get_sp(vha, fcport, GFP_KERNEL);
	if (!sp) {
		ql_dbg(ql_dbg_user, vha, 0x7004,
		    "Failed get sp pid=%06x\n", fcport->d_id.b24);
		rval = -ENOMEM;
		SET_DID_STATUS(bsg_reply->result, DID_IMM_RETRY);
		goto done;
	}

	sp->remap.req.len = bsg_job->request_payload.payload_len;
	sp->remap.req.buf = dma_pool_alloc(ha->purex_dma_pool,
	    GFP_KERNEL, &sp->remap.req.dma);
	if (!sp->remap.req.buf) {
		ql_dbg(ql_dbg_user, vha, 0x7005,
		    "Failed allocate request dma len=%x\n",
		    bsg_job->request_payload.payload_len);
		rval = -ENOMEM;
		SET_DID_STATUS(bsg_reply->result, DID_IMM_RETRY);
		goto done_free_sp;
	}

	sp->remap.rsp.len = bsg_job->reply_payload.payload_len;
	sp->remap.rsp.buf = dma_pool_alloc(ha->purex_dma_pool,
	    GFP_KERNEL, &sp->remap.rsp.dma);
	if (!sp->remap.rsp.buf) {
		ql_dbg(ql_dbg_user, vha, 0x7006,
		    "Failed allocate response dma len=%x\n",
		    bsg_job->reply_payload.payload_len);
		rval = -ENOMEM;
		SET_DID_STATUS(bsg_reply->result, DID_IMM_RETRY);
		goto done_free_remap_req;
	}

	//ql_print_bsg_sglist(ql_dbg_user, vha, 0x7008,
	//      "SG bsg->request", &bsg_job->request_payload);
	sg_copy_to_buffer(bsg_job->request_payload.sg_list,
	    bsg_job->request_payload.sg_cnt, sp->remap.req.buf,
	    sp->remap.req.len);
	sp->remap.remapped = true;

	//ql_dump_buffer(ql_dbg_edif, vha, 0x70e0,
	//    sp->remap.req.buf, bsg_job->request_payload.payload_len);

	sp->type = SRB_ELS_CMD_HST_NOLOGIN;
	sp->name = "SPCN_BSG_HST_NOLOGIN";
	sp->u.bsg_cmd.bsg_job = bsg_job;
	qla_parse_auth_els_ctl(sp);

	sp->free = qla2x00_bsg_sp_free;
	sp->done = qla2x00_bsg_job_done;

	rval = qla2x00_start_sp(sp);

	ql_dbg(ql_dbg_edif, vha, 0x700a,
	    "%s %s %8phN xchg %x ctlflag %x hdl %x reqlen %xh bsg ptr %px\n",
	    __func__, sc_to_str(p->e.sub_cmd), fcport->port_name,
	    p->e.extra_rx_xchg_address, p->e.extra_control_flags,
	    sp->handle, sp->remap.req.len, bsg_job);

	if (rval != QLA_SUCCESS) {
		ql_log(ql_log_warn, vha, 0x700e,
		    "qla2x00_start_sp failed = %d\n", rval);
		SET_DID_STATUS(bsg_reply->result, DID_IMM_RETRY);
		rval = -EIO;
		goto done_free_remap_rsp;
	}
	return rval;

done_free_remap_rsp:
	dma_pool_free(ha->purex_dma_pool, sp->remap.rsp.buf,
	    sp->remap.rsp.dma);
done_free_remap_req:
	dma_pool_free(ha->purex_dma_pool, sp->remap.req.buf,
	    sp->remap.req.dma);
done_free_sp:
	qla2x00_rel_sp(sp);
done:
	return rval;
}

void qla_edif_sess_down(struct scsi_qla_host *vha, struct fc_port *sess)
{
	if (sess->edif.app_sess_online && vha->e_dbell.db_flags & EDB_ACTIVE) {
		ql_dbg(ql_dbg_disc, vha, 0xf09c,
			"%s: sess %8phN send port_offline event\n",
			__func__, sess->port_name);
		sess->edif.app_sess_online = 0;
		qla_edb_eventcreate(vha, VND_CMD_AUTH_STATE_SESSION_SHUTDOWN,
		    sess->d_id.b24, 0, sess);
		qla2x00_post_aen_work(vha, FCH_EVT_PORT_OFFLINE, sess->d_id.b24);
	}
}