/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2003-2014 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */
#include "qla_def.h"
#include "qla_gbl.h"

/* SCM Private Functions */

static bool
qla2xxx_scmr_check_low_wm(struct qla_scmr_flow_control *sfc,
			  int curr, int base)
{
	bool ret = false;
	if (sfc->mode == QLA_MODE_Q_DEPTH) {
		if (qla_scmr_is_tgt(sfc)) {
			if (curr == QLA_MIN_TGT_Q_DEPTH)
				ret = true;
		} else {
			if (curr == QLA_MIN_HBA_Q_DEPTH)
				ret = true;
		}
	} else if (sfc->mode == QLA_MODE_FLOWS) {
		if (curr < base *
			    ql2x_scmr_drop_pct_low_wm/100) {
			ret = true;
		}
	}

	if (ret == true) {
		ql_dbg(ql_dbg_timer, sfc->vha, 0x0203,
		    "SCMR: Reached low watermark, permitted: %d baseline: %d\n",
			curr, base);
		sfc->rstats->throttle_hit_low_wm++;
	}
	return ret;
}

static void
qla2xxx_change_queue_depth(struct qla_scmr_flow_control *sfc, int new)
{
	if (qla_scmr_is_tgt(sfc)) {
		if (new >= QLA_MIN_TGT_Q_DEPTH)
			atomic_set(&sfc->scmr_permitted, new);
		else
			atomic_set(&sfc->scmr_permitted, QLA_MIN_TGT_Q_DEPTH);
	} else {
		if (new >= QLA_MIN_HBA_Q_DEPTH)
			atomic_set(&sfc->scmr_permitted, new);
		else
			atomic_set(&sfc->scmr_permitted, QLA_MIN_HBA_Q_DEPTH);
	}
}

static int
qla2xxx_calculate_delta(struct qla_scmr_flow_control *sfc, int base)
{
	int delta;

	if (sfc->dir == QLA_DIR_UP) {
		if (sfc->level)
			delta = (base * ql2x_scmr_up_pct *
			    (sfc->level)) / 100;
		else
			delta = (base * ql2x_scmr_up_pct) / 100;
	} else {
		if (sfc->level)
			delta = (base * ql2x_scmr_drop_pct *
			    (sfc->level)) / 100;
		else
			delta = (base * ql2x_scmr_drop_pct) / 100;
	}

	return (delta ? delta : 1);
}

/* Reduce throttle based on IOs/period or bytes/period */
static void
qla2xxx_scmr_reduce_throttle(struct qla_scmr_flow_control *sfc)
{
	int delta, current_val, new;
	bool low_wm = false;

	current_val = new = 0;

	current_val = atomic_read(&sfc->scmr_permitted);
	if (current_val) {
		low_wm = qla2xxx_scmr_check_low_wm(sfc, current_val,
				atomic_read(&sfc->scmr_base));
	} else {
		current_val = atomic_read(&sfc->scmr_base);
	}

	if (low_wm == true)
		return;

	if (sfc->mode == QLA_MODE_Q_DEPTH) {
		if (sfc->dir == QLA_DIR_UP) {
			new = current_val - 1;
		} else {
			new = current_val >> 1;
		}
		qla2xxx_change_queue_depth(sfc, new);
	} else if (sfc->mode == QLA_MODE_FLOWS) {
		delta = qla2xxx_calculate_delta(sfc,
			    atomic_read(&sfc->scmr_base));
		new = current_val - delta;
		atomic_set(&sfc->scmr_permitted, new);
	}

	sfc->rstats->throttle_down_count++;
	ql_dbg(ql_dbg_timer, sfc->vha, 0x0203,
	    "SCMR: Congested, throttling down, permitted: %d baseline: %d\n",
		atomic_read(&sfc->scmr_permitted), atomic_read(&sfc->scmr_base));

	return;
}

/* Increase by @ql2x_scmr_up_pct percent, every QLA_SCMR_THROTTLE_PERIOD
 * secs.
 */
static void
qla2xxx_scmr_increase_flows(struct qla_scmr_flow_control *sfc)
{
	int delta, current_val, base_val, new;

	new = 0;

	if (sfc->throttle_period--)
		return;

	sfc->throttle_period = sfc->event_period + sfc->event_period_buffer;
	current_val = atomic_read(&sfc->scmr_permitted);
	base_val = atomic_read(&sfc->scmr_base);

	/* Unlikely */
	if (!current_val)
		return;

	if (sfc->mode == QLA_MODE_Q_DEPTH) {
		new = current_val + 1;
	} else if (sfc->mode == QLA_MODE_FLOWS) {
		delta = (base_val * ql2x_scmr_up_pct) / 100;
		new = current_val + (delta ? delta: 1);
	}

	if (new > base_val) {
		qla2xxx_scmr_clear_throttle(sfc);
		ql_log(ql_log_info, sfc->vha, 0x0203,
		    "SCMR: Clearing throttle \n");
		return;
	} else {
		ql_dbg(ql_dbg_timer, sfc->vha, 0x0203,
		    "SCMR: throttling up, permitted: %d baseline: %d \n",
		    atomic_read(&sfc->scmr_permitted), base_val);
		if (sfc->mode == QLA_MODE_Q_DEPTH) {
			qla2xxx_change_queue_depth(sfc, new);
		} else if (sfc->mode == QLA_MODE_FLOWS) {
			atomic_set(&sfc->scmr_permitted, new);
		}
		sfc->dir = QLA_DIR_UP;
		sfc->rstats->throttle_up_count++;
	}
}

static void
qla2xxx_check_congestion_timeout(struct qla_scmr_flow_control *sfc)
{
	if (sfc->expiration_jiffies &&
	    (time_after(jiffies, sfc->expiration_jiffies))) {
		ql_log(ql_log_info, sfc->vha, 0x0203,
		    "SCMR: Clearing Congestion, event period expired\n");
		qla2xxx_scmr_clear_congn(sfc);
	}

}

static bool
qla2xxx_check_fpin_event(struct qla_scmr_flow_control *sfc)
{
	if (qla_scmr_get_sig(sfc) == QLA_SIG_CLEAR) {
		qla_scmr_clear_sig(sfc, scmr_congn_signal);
		qla2xxx_scmr_clear_congn(sfc);
		qla2xxx_scmr_clear_throttle(sfc);
		ql_log(ql_log_info, sfc->vha, 0x0203,
		    "SCMR:(H) Clear Congestion for WWN %8phN\n",
		    sfc->vha->port_name);
		return false;
	}

	if (qla_scmr_get_sig(sfc) == QLA_SIG_CREDIT_STALL) {
		qla_scmr_clear_sig(sfc, scmr_congn_signal);
		return true;
	} else if (qla_scmr_get_sig(sfc) == QLA_SIG_OVERSUBSCRIPTION) {
		/* Check if profile policy asks for Global/Targeted Throttling (where relevant)
		 * Check if Targeted throttling is possible
		 */
			qla_scmr_clear_sig(sfc, scmr_congn_signal);
			return true;
#if 0
		/* GSS: Can be enabled after we have rolled in
		 * Global/Targeted throttling. When we do, ensure
		 * that the FAST_TGT implementation is complete.
		 */
		if (sfc->profile.policy & QLA_THROTTLING_POLICY) {
			qla_scmr_clear_sig(sfc, scmr_congn_signal);
			return true;
		} else {
			qla_scmr_clear_sig(sfc, scmr_congn_signal);
			if (qla_scmr_has_fast_tgt(sfc)) {
				atomic_set(&sfc->scmr_congn_signal,
					   QLA_SIG_THROTTLE_FAST_TGT);
				return false;
			} else
				return true;
		}
#endif
	}
	return false;
}

static bool
qla2xxx_check_cn_event(struct qla_scmr_flow_control *sfc)
{
	bool congested = false;

	if (IS_ARB_CAPABLE(sfc->vha->hw)) {
		/* Handle ARB Signals */
		if (atomic_read(&sfc->num_sig_warning) >=
		    QLA_SCMR_WARN_THRESHOLD) {
			sfc->level = QLA_CONG_LOW;
			sfc->expiration_jiffies =
			    jiffies + (2 * HZ);
			congested = true;
			atomic_set(&sfc->num_sig_warning, 0);
		}

		if (atomic_read(&sfc->num_sig_alarm) >=
		    QLA_SCMR_ALARM_THRESHOLD) {
			sfc->level = QLA_CONG_HIGH;
			sfc->expiration_jiffies =
			    jiffies + (2 * HZ);
			congested = true;
			atomic_set(&sfc->num_sig_alarm, 0);
		}
	}

	if (congested == false)
		congested = qla2xxx_check_fpin_event(sfc);

	return congested;

}

#define SCMR_PERIODS_PER_SEC	10

static bool
qla2xxx_scmr_set_baseline(struct qla_scmr_flow_control *sfc)
{
	bool ret = false;

	if (sfc->mode == QLA_MODE_Q_DEPTH) {
		if (atomic_read(&sfc->max_q_depth) >
		    QLA_MIN_BASELINE_QDEPTH)
			atomic_set(&sfc->scmr_base,
			   atomic_read(&sfc->max_q_depth));
		else
			atomic_set(&sfc->scmr_base,
			    QLA_MIN_BASELINE_QDEPTH);
		qla_scmr_set_throttle_qdepth(sfc);
		sfc->dir = QLA_DIR_DOWN;
		ret = true;
	} else if (sfc->mode == QLA_MODE_FLOWS) {
		if (atomic_read(&sfc->scmr_reqs) >
		    QLA_MIN_BASELINE_IOS) {
			atomic_set(&sfc->scmr_base,
			    atomic_read(&sfc->scmr_reqs) /
			    QLA_SCMR_PERIODS_PER_SEC);
			qla_scmr_set_reduce_throttle_ios(sfc);
			sfc->dir = QLA_DIR_DOWN;
			ret = true;
		} else if (atomic_read(&sfc->scmr_bytes) >
			   QLA_MIN_BASELINE_BPS) {
			atomic_set(&sfc->scmr_base,
			    atomic_read(&sfc->scmr_bytes) /
			    QLA_SCMR_PERIODS_PER_SEC);
			qla_scmr_set_reduce_throttle_bps(sfc);
			sfc->dir = QLA_DIR_DOWN;
			ret = true;
		}
	}
	return ret;
}

static void
qla2xxx_reduce_flows(struct qla_scmr_flow_control *sfc)
{
	bool throttle = false;

	/* Congestion Signal/FPIN received */
	if (!qla_scmr_reduced_throttle(sfc)) {
		throttle = qla2xxx_scmr_set_baseline(sfc);
	} else {
		throttle = true;
	}

	if (throttle == true)
		qla2xxx_scmr_reduce_throttle(sfc);
	else
		ql_log(ql_log_info, sfc->vha, 0x0203,
		    "SCMR: IOs too low, not throttling for WWN %8phN\n",
		    qla_scmr_is_tgt(sfc) ?
		    sfc->fcport->port_name: sfc->vha->port_name);

	if (!qla_scmr_is_congested(sfc)) {
		ql_log(ql_log_info, sfc->vha, 0x0203,
		    "SCMR: Set Congestion for WWN %8phN\n",
		    qla_scmr_is_tgt(sfc) ?
		    sfc->fcport->port_name: sfc->vha->port_name);
		qla_scmr_set_congested(sfc);
		if (qla_scmr_is_tgt(sfc) &&
		    IS_NPVC_CAPABLE(sfc->vha->hw)) {
			qla_scmr_set_notify_fw(sfc);
			set_bit(SCM_NOTIFY_FW, &
				sfc->vha->dpc_flags);
		}
	}
}

static void
qla2xxx_tune_tgt_throttle(struct fc_port *fcport)
{
	bool congested, throttle;
	struct qla_scmr_flow_control *sfc = &fcport->sfc;

	congested = throttle = false;

	if (qla_scmr_get_sig(sfc) == QLA_SIG_CLEAR) {
		if (IS_NPVC_CAPABLE(sfc->vha->hw)) {
			qla_scmr_set_notify_fw(sfc);
			set_bit(SCM_NOTIFY_FW,
				&sfc->vha->dpc_flags);
		}
	}

	congested = qla2xxx_check_fpin_event(sfc);

	if (congested == true) {
		if (ql2x_scmr_flow_ctl_tgt)
			qla2xxx_reduce_flows(sfc);
		qla_scmr_set_congested(sfc);
	} else {
		qla2xxx_check_congestion_timeout(sfc);
		if (ql2x_scmr_flow_ctl_tgt) {
			if (!qla_scmr_reduced_throttle(sfc))
				goto exit_func;

			qla2xxx_scmr_increase_flows(sfc);
		}
	}

exit_func:
	atomic_set(&sfc->scmr_reqs, 0);
	atomic_set(&sfc->scmr_bytes, 0);
}

static void
qla2xxx_tune_host_throttle(struct qla_scmr_flow_control *sfc)
{
	bool congested = false;

	congested = qla2xxx_check_cn_event(sfc);
 
	if (congested == true) {
		if (ql2x_scmr_flow_ctl_host)
			qla2xxx_reduce_flows(sfc);
		qla_scmr_set_congested(sfc);
	} else {
		qla2xxx_check_congestion_timeout(sfc);
		if (ql2x_scmr_flow_ctl_host) {
			if (!qla_scmr_reduced_throttle(sfc))
				goto exit_func;

			qla2xxx_scmr_increase_flows(sfc);
		}
	}

exit_func:
	atomic_set(&sfc->scmr_reqs, 0);
	atomic_set(&sfc->scmr_bytes, 0);
}

/*
 * qla2xxx_throttle_curr_req() - Check if this request should be sent
 * back for a retry because of congestion on this host.
 *
 * @sfc: Pointer to the flow control struct for the given request queue.
 * @cmd: SCSI Command.
 *
 * Returns true for retry, false otherwise.
 */
static bool
qla2xxx_throttle_curr_req(struct qla_scmr_flow_control *sfc)
{
	/* Throttle down reqs if the host has oversubscribed */

	if (sfc->mode == QLA_MODE_Q_DEPTH) {
		if (qla_scmr_throttle_qdepth(sfc)) {
			if (atomic_read(&sfc->scmr_permitted) <
			    atomic_read(&sfc->q_depth)) {
				sfc->rstats->busy_status_count++;
				return true;
			}
		}
	} else if (sfc->mode == QLA_MODE_FLOWS) {
		if (qla_scmr_throttle_bps(sfc)) {
			if (atomic_read(&sfc->scmr_permitted) <
			    atomic_read(&sfc->scmr_bytes_per_period)) {
				sfc->rstats->busy_status_count++;
				return true;
			}
		} else if (qla_scmr_throttle_ios(sfc)) {
			if (atomic_read(&sfc->scmr_permitted) <
			    atomic_read(&sfc->scmr_reqs_per_period)) {
				sfc->rstats->busy_status_count++;
				return true;
			}
		}
	}

	return false;
}

static inline void
qla2x00_restart_perf_timer(scsi_qla_host_t *vha)
{
	/* Currently used for 82XX only. */
	if (vha->device_flags & DFLG_DEV_FAILED) {
		ql_dbg(ql_dbg_timer, vha, 0x600d,
		    "Device in a failed state, returning.\n");
		return;
	}

	mod_timer(&vha->perf_timer, jiffies + HZ/10);
}

static void
qla2xxx_minute_stats(struct qla_scmr_flow_control *sfc)
{
	sfc->ticks++;

	if (!(sfc->ticks % 60)) {
		atomic_set(&sfc->max_q_depth, 0);
	}
}

/* Externally Used APIs */

/**************************************************************************
*   qla2xxx_perf_timer
*
* Description:
*   100 ms timer. Should be maintained as a lightweight thread because
*   of its frequency.
*
* Context: Interrupt
***************************************************************************/
void
qla2xxx_perf_timer(qla_timer_arg_t t)
{
	scsi_qla_host_t *vha = qla_from_timer(vha, t, perf_timer);
	struct qla_hw_data *ha = vha->hw;
	fc_port_t *fcport;

	if (ha->flags.eeh_busy) {
		ql_dbg(ql_dbg_timer, vha, 0x6000,
		    "EEH = %d, restarting timer.\n",
		    ha->flags.eeh_busy);
		qla2x00_restart_perf_timer(vha);
		return;
	}

	qla2xxx_atomic_add(&ha->sfc.scmr_bytes,
	    atomic_read(&ha->sfc.scmr_bytes_per_period));
	atomic_set(&ha->sfc.scmr_bytes_per_period, 0);
	qla2xxx_atomic_add(&ha->sfc.scmr_reqs,
	    atomic_read(&ha->sfc.scmr_reqs_per_period));
	atomic_set(&ha->sfc.scmr_reqs_per_period, 0);

	list_for_each_entry(fcport, &vha->vp_fcports, list) {
		if (!(fcport->port_type & FCT_TARGET) &&
		    !(fcport->port_type & FCT_NVME_TARGET))
			continue;

		qla2xxx_atomic_add(&fcport->sfc.scmr_bytes,
		    atomic_read(&fcport->sfc.scmr_bytes_per_period));
		atomic_set(&fcport->sfc.scmr_bytes_per_period, 0);
		qla2xxx_atomic_add(&fcport->sfc.scmr_reqs,
		    atomic_read(&fcport->sfc.scmr_reqs_per_period));
		atomic_set(&fcport->sfc.scmr_reqs_per_period, 0);
	}

	qla2x00_restart_perf_timer(vha);
}

/*
 * qla2xxx_throttle_req - To rate limit I/O on congestion.
 *
 * Returns true to throttle down, false otherwise.
 */
bool
qla2xxx_throttle_req(struct qla_hw_data *ha, fc_port_t *fcport)
{
	bool ret = false;

	if (ql2x_scmr_flow_ctl_host) {
		ret = qla2xxx_throttle_curr_req(&ha->sfc);
		if (ret == true)
			return ret;
	}

	if (ql2x_scmr_flow_ctl_tgt) {
		ret = qla2xxx_throttle_curr_req(&fcport->sfc);
		if (ret == true && ql2x_scmr_flow_ctl_host) {
			return ret;
		}
	}

	return ret;
}

void
qla2xxx_scmr_manage_qdepth(struct fc_port *fcport, bool inc)
{
	int curr;
	struct scsi_qla_host *vha = fcport->vha;

	if (!IS_SCM_CAPABLE(vha->hw))
		return;

	if (inc == true) {
		atomic_inc(&vha->hw->sfc.q_depth);
		curr = atomic_read(&vha->hw->sfc.q_depth);
		if (atomic_read(&vha->hw->sfc.max_q_depth) <
		    curr)
			atomic_set(&vha->hw->sfc.max_q_depth, curr);

		atomic_inc(&fcport->sfc.q_depth);
		curr = atomic_read(&fcport->sfc.q_depth);
		if (atomic_read(&fcport->sfc.max_q_depth) <
		    curr)
			atomic_set(&fcport->sfc.max_q_depth, curr);
	} else {
		atomic_dec(&vha->hw->sfc.q_depth);
		atomic_dec(&fcport->sfc.q_depth);
	}
}

void
qla2xxx_scmr_cleanup(scsi_qla_host_t *vha, struct scsi_cmnd *cmd)
{
	fc_port_t *fcport = (struct fc_port *)cmd->device->hostdata;

	if (!IS_SCM_CAPABLE(vha->hw))
		return;

	atomic_dec(&fcport->sfc.scmr_reqs_per_period);
	qla2xxx_atomic_sub(&fcport->sfc.scmr_bytes_per_period,
	    scsi_bufflen(cmd));
	atomic_dec(&vha->hw->sfc.scmr_reqs_per_period);
	qla2xxx_atomic_sub(&vha->hw->sfc.scmr_bytes_per_period,
	    scsi_bufflen(cmd));
	qla2xxx_scmr_manage_qdepth(fcport, false);
}

/*
 * qla2xxx_scmr_flow_control - To rate limit I/O on congestion.
 *
 */
void
qla2xxx_scmr_flow_control(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;
	struct fc_port *fcport;

	qla2xxx_minute_stats(&ha->sfc);
	/* Controlled at the port level */
	qla2xxx_tune_host_throttle(&ha->sfc);

	list_for_each_entry(fcport, &vha->vp_fcports, list) {
		if (!(fcport->port_type & FCT_TARGET) &&
		    !(fcport->port_type & FCT_NVME_TARGET))
                        continue;

		qla2xxx_minute_stats(&fcport->sfc);
		qla2xxx_tune_tgt_throttle(fcport);
	}
}

void
qla2xxx_scmr_clear_congn(struct qla_scmr_flow_control *sfc)
{
	struct qla_hw_data *ha = sfc->vha->hw;

	qla_scmr_clear_congested(sfc);
	sfc->level = QLA_CONG_NONE;
	sfc->expiration_jiffies = 0;

	/* Clear severity status for the application as well */
	ha->scm.congestion.severity = 0;
	ha->scm.last_event_timestamp = qla_get_real_seconds();
}

void
qla2xxx_scmr_clear_throttle(struct qla_scmr_flow_control *sfc)
{
	if (sfc->mode == QLA_MODE_Q_DEPTH) {
		qla_scmr_clear_throttle_qdepth(sfc);
	} else if (sfc->mode == QLA_MODE_FLOWS) {
		if (qla_scmr_throttle_bps(sfc))
			qla_scmr_clear_throttle_bps(sfc);
		if (qla_scmr_throttle_ios(sfc))
			qla_scmr_clear_throttle_ios(sfc);
	}
	atomic_set(&sfc->scmr_base, 0);
	atomic_set(&sfc->scmr_permitted, 0);
	sfc->rstats->throttle_cleared++;
	sfc->dir = QLA_DIR_NONE;
	sfc->throttle_period =
	    sfc->event_period + sfc->event_period_buffer;
	ql_dbg(ql_dbg_timer, sfc->vha, 0x0203,
	    "SCMR: Clearing Throttling\n");
}

void
qla2xxx_update_scm_fcport(scsi_qla_host_t *vha)
{
	fc_port_t *fcport;
	struct qla_scmr_flow_control *sfc;

	list_for_each_entry(fcport, &vha->vp_fcports, list) {
		if (!(fcport->port_type & FCT_TARGET) &&
		    !(fcport->port_type & FCT_NVME_TARGET))
			continue;

		sfc = &fcport->sfc;
		if (qla_scmr_test_notify_fw(sfc)) {
			if (qla_scmr_is_congested(sfc))
				qla2xxx_set_scm_params(fcport, true);
			else
				qla2xxx_set_scm_params(fcport, false);

			qla_scmr_clr_notify_fw(sfc);
		}
	}
}

void
qla2xxx_update_sfc_ios(struct qla_hw_data *ha,
		       fc_port_t *fcport, int new)
{
	if (!IS_SCM_CAPABLE(ha))
		return;

	atomic_inc(&ha->sfc.scmr_reqs_per_period);
	atomic_inc(&fcport->sfc.scmr_reqs_per_period);
	qla2xxx_atomic_add(&ha->sfc.scmr_bytes_per_period, new);
	qla2xxx_atomic_add(&fcport->sfc.scmr_bytes_per_period, new);
	qla2xxx_scmr_manage_qdepth(fcport, true);
	return;
}

/* Helper routine to prepare RDF ELS payload.
 * Refer to FC LS 5.01 for a detailed explanation
 */
static void qla_prepare_rdf_payload(scsi_qla_host_t *vha)
{
	vha->rdf_els_payload.els_code = RDF_OPCODE;
	vha->rdf_els_payload.desc_len = cpu_to_be32(sizeof(struct rdf_els_descriptor));
	vha->rdf_els_payload.rdf_desc.desc_tag =
		cpu_to_be32(QLA_ELS_DTAG_FPIN_REGISTER);
	vha->rdf_els_payload.rdf_desc.desc_cnt =
		cpu_to_be32(ELS_RDF_REG_TAG_CNT);
	vha->rdf_els_payload.rdf_desc.desc_len =
		cpu_to_be32(sizeof(struct rdf_els_descriptor) - 8);
	vha->rdf_els_payload.rdf_desc.desc_tags[0] =
		cpu_to_be32(QLA_ELS_DTAG_LNK_INTEGRITY);
	vha->rdf_els_payload.rdf_desc.desc_tags[1] =
		cpu_to_be32(QLA_ELS_DTAG_DELIVERY);
	vha->rdf_els_payload.rdf_desc.desc_tags[2] =
		cpu_to_be32(QLA_ELS_DTAG_PEER_CONGEST);
	vha->rdf_els_payload.rdf_desc.desc_tags[3] =
		cpu_to_be32(QLA_ELS_DTAG_CONGESTION);
}

/* Helper routine to prepare RDF ELS payload.
 * Refer to FC LS 5.01 for a detailed explanation
 */
static void qla_prepare_edc_payload(scsi_qla_host_t *vha)
{
	struct edc_els_payload *edc = &vha->hw->edc_els_payload;

	edc->els_code = EDC_OPCODE;
	edc->desc_len = cpu_to_be32(sizeof(struct edc_els_descriptor));

	edc->edc_desc.link_fault_cap_descriptor_tag = cpu_to_be32(ELS_EDC_LFC_INFO);
	edc->edc_desc.lfc_descriptor_length = cpu_to_be32(12);

	edc->edc_desc.cong_sig_cap_descriptor_tag = cpu_to_be32(ELS_EDC_CONG_SIG_INFO);
	edc->edc_desc.csc_descriptor_length = cpu_to_be32(16);
}

/*
 * Update various fields of SP to send the ELS via the ELS PT
 * IOCB.
 */

static void qla_update_sp(srb_t *sp, scsi_qla_host_t *vha, u8 cmd)
{
	struct qla_els_pt_arg *a = &sp->u.iocb_cmd.u.drv_els.els_pt_arg;
	struct srb_iocb *iocb_cmd = &sp->u.iocb_cmd;
	void *buf;
	dma_addr_t dma_addr;
	int len;
	u8 al_pa;

	if (cmd == RDF_OPCODE)
		al_pa = 0xFD;
	else
		al_pa = 0xFE;

	a->els_opcode = cmd;
	a->nport_handle = cpu_to_le16(sp->fcport->loop_id);
	a->vp_idx = sp->vha->vp_idx;
	a->control_flags = 0;
	a->rx_xchg_address = 0; //No resp DMA from fabric

	a->did.b.al_pa = al_pa;
	a->did.b.area = 0xFF;
	a->did.b.domain = 0xFF;

	if (cmd == RDF_OPCODE)
		a->tx_len = a->tx_byte_count =
			cpu_to_le32(sizeof(iocb_cmd->u.drv_els.els_req.rdf_cmd));
	else
		a->tx_len = a->tx_byte_count =
			cpu_to_le32(sizeof(iocb_cmd->u.drv_els.els_req.edc_cmd));

	a->rx_len = a->rx_byte_count = cpu_to_le32(sizeof(iocb_cmd->u.drv_els.els_rsp));

	len = iocb_cmd->u.drv_els.dma_addr.cmd_len = sizeof(iocb_cmd->u.drv_els.els_req);

	buf = dma_alloc_coherent(&vha->hw->pdev->dev, len,
		&dma_addr, GFP_KERNEL);

	iocb_cmd->u.drv_els.dma_addr.cmd_buf = buf;
	iocb_cmd->u.drv_els.dma_addr.cmd_dma = dma_addr;

	if (cmd == RDF_OPCODE)
		memcpy(iocb_cmd->u.drv_els.dma_addr.cmd_buf, &vha->rdf_els_payload, len);
	else
		memcpy(iocb_cmd->u.drv_els.dma_addr.cmd_buf, &vha->hw->edc_els_payload, len);

	a->tx_addr = iocb_cmd->u.drv_els.dma_addr.cmd_dma;

	len = iocb_cmd->u.drv_els.dma_addr.rsp_len = sizeof(iocb_cmd->u.drv_els.els_rsp);

	buf = dma_alloc_coherent(&vha->hw->pdev->dev, len,
		&dma_addr, GFP_KERNEL);

	iocb_cmd->u.drv_els.dma_addr.rsp_buf = buf;
	iocb_cmd->u.drv_els.dma_addr.rsp_dma = dma_addr;

	a->rx_addr = iocb_cmd->u.drv_els.dma_addr.rsp_dma;
}

/*
 * qla2xxx_scm_get_features -
 * Get the firmware/Chip related supported features w.r.t SCM
 * Issue mbox 5A.
 * Parse through the response and get relevant values
 */
int
qla2xxx_scm_get_features(scsi_qla_host_t *vha)
{
	dma_addr_t fdma;
	u16 sz = FW_FEATURES_SIZE;
	int rval = 0;
	u8 *f;
	int i;
	u8 scm_feature;
	struct edc_els_descriptor *edc = &vha->hw->edc_els_payload.edc_desc;

	f = dma_alloc_coherent(&vha->hw->pdev->dev, sz,
		&fdma, GFP_KERNEL);
		if (!f) {
			ql_log(ql_log_warn, vha, 0x7035,
				"DMA alloc failed for feature buf.\n");
			return -ENOMEM;
		}

	rval = qla_get_features(vha, fdma, FW_FEATURES_SIZE);
	if (rval != QLA_SUCCESS) {
		ql_log(ql_log_warn, vha, 0x7035,
			"Get features failed :0x%x.\n", rval);
	} else {
		scm_feature = f[0];
		/* If both bits 3 and 4 are zero, firmware sends the ELS
		 * 27xx has bit 4 set and bit 3 cleared
		 */
		if ((!(scm_feature & BIT_3) && !(scm_feature & BIT_4))) {
			rval = 1;
			goto free;
		}
		i = 4;
		if (scm_feature & BIT_3) {
			/* The next 3 words contain Link fault Capability */
			edc->degrade_activate_threshold = get_unaligned_be32(&f[i]);
			i += 4;
			edc->degrade_deactivate_threshold = get_unaligned_be32(&f[i]);
			i += 4;
			edc->fec_degrade_interval = get_unaligned_be32(&f[i]);
			i += 4;
		}
		if (scm_feature & BIT_4) {
			/* The next 4 words contain Cong. Sig. Capability */
			i = 16;
			edc->tx_signal_cap = (get_unaligned_be32(&f[i]));
			i += 4;
			edc->tx_signal_freq = get_unaligned_be32(&f[i]);
			i += 4;
			edc->rx_signal_cap = get_unaligned_be32(&f[i]);
			i += 4;
			edc->rx_signal_freq = get_unaligned_be32(&f[i]);
			i += 4;
		}
	}
free:
	dma_free_coherent(&vha->hw->pdev->dev, sz, f, fdma);
	return rval;
}

void qla2x00_scm_els_sp_done(srb_t *sp, int res)
{
	struct scsi_qla_host *vha = sp->vha;
	struct els_resp *rsp =
		(struct els_resp *)sp->u.iocb_cmd.u.drv_els.dma_addr.rsp_buf;
	u8 err_code;

	if (res == QLA_SUCCESS) {
		ql_log(ql_log_info, vha, 0x700f,
			"%s ELS completed for port:%8phC\n",
			(sp->type == SRB_ELS_EDC)?"EDC":"RDF", vha->port_name);

		if (rsp->resp_code == ELS_LS_RJT) {
			struct fc_els_ls_rjt *rjt =
				(struct fc_els_ls_rjt *)sp->u.iocb_cmd.u.drv_els.dma_addr.rsp_buf;
			err_code = rjt->er_reason;
			ql_log(ql_log_info, vha, 0x503f,
				"%s rejected with code:0x%x\n",(sp->type == SRB_ELS_EDC)?"EDC":"RDF",
					err_code);
			if (sp->type == SRB_ELS_EDC) {
				if (err_code == ELS_RJT_UNAB && ++vha->hw->edc_retry_cnt < MAX_USCM_ELS_RETRIES)
					set_bit(SCM_SEND_EDC, &vha->dpc_flags);
			} else {
				if (err_code == ELS_RJT_UNAB && ++vha->rdf_retry_cnt < MAX_USCM_ELS_RETRIES)
					set_bit(SCM_SEND_RDF, &vha->dpc_flags);
			}
		} else if ((rsp->resp_code == ELS_LS_ACC) && (sp->type == SRB_ELS_RDF)) {
			/* RDF completion indicates that SCM can be supported */
			ql_dbg(ql_dbg_scm, vha, 0x503f,
				"RDF completed \n");
			vha->hw->flags.scm_enabled = 1;
			vha->hw->scm.scm_fabric_connection_flags |= SCM_FLAG_RDF_COMPLETED;
		}
	} else {
		ql_log(ql_log_warn, vha, 0x701a,
			"%s ELS failed for port:%8phC, res:0x%x\n",
			(sp->type == SRB_ELS_EDC)?"EDC":"RDF", vha->port_name, res);
		if (sp->type == SRB_ELS_EDC) {
			if (++vha->hw->edc_retry_cnt < MAX_USCM_ELS_RETRIES) {
				ql_log(ql_log_info, vha, 0x701b,
					"Retrying EDC:retry:%d\n",vha->hw->edc_retry_cnt);
				set_bit(SCM_SEND_EDC, &vha->dpc_flags);
			}
		} else if (sp->type == SRB_ELS_RDF) {
			if (++vha->rdf_retry_cnt < MAX_USCM_ELS_RETRIES) {
				ql_log(ql_log_info, vha, 0x701c,
					"Retrying RDF:retry:%d\n",vha->rdf_retry_cnt);
				set_bit(SCM_SEND_RDF, &vha->dpc_flags);
			}
		}
	}
	sp->free(sp);
}

void qla2x00_scm_els_sp_free(srb_t *sp)
{
	void *cmd_buf, *rsp_buf;
	dma_addr_t cmd_dma, rsp_dma;
	int cmd_len, rsp_len;
	struct qla_work_evt *e;

	cmd_buf = sp->u.iocb_cmd.u.drv_els.dma_addr.cmd_buf;
	cmd_dma = sp->u.iocb_cmd.u.drv_els.dma_addr.cmd_dma;
	cmd_len = sp->u.iocb_cmd.u.drv_els.dma_addr.cmd_len;

	rsp_buf = sp->u.iocb_cmd.u.drv_els.dma_addr.rsp_buf;
	rsp_dma = sp->u.iocb_cmd.u.drv_els.dma_addr.rsp_dma;
	rsp_len = sp->u.iocb_cmd.u.drv_els.dma_addr.rsp_len;

	ql_dbg(ql_dbg_scm, sp->vha, 0x700a,
			"cmd_buf:%p, cmd_dma:%llx, len:%d\n",
			cmd_buf, cmd_dma, cmd_len);

	e = qla2x00_alloc_work(sp->vha, QLA_EVT_UNMAP);
	if (!e) {
		dma_free_coherent(&sp->vha->hw->pdev->dev,
				cmd_len,
				cmd_buf,
				cmd_dma);
		cmd_buf = NULL;
		dma_free_coherent(&sp->vha->hw->pdev->dev,
				rsp_len,
				rsp_buf,
				rsp_dma);
		rsp_buf = NULL;
		qla2x00_free_fcport(sp->fcport);
		qla2x00_rel_sp(sp);
	} else {
		e->u.iosb.sp = sp;
		qla2x00_post_work(sp->vha, e);
	}
}

/*
 * qla2xxx_scm_send_rdf_els - Send RDF ELS to the switch
 * Called by both base port and vports
 */

int
qla2xxx_scm_send_rdf_els(scsi_qla_host_t *vha)
{
	srb_t *sp;
	fc_port_t *fcport = NULL;
	int rval = 0;

	/* Allocate a dummy fcport structure, since functions
	 * preparing the IOCB and mailbox command retrieves port
	 * specific information from fcport structure.
	 */

	fcport = qla2x00_alloc_fcport(vha, GFP_KERNEL);
	if (!fcport) {
		rval = -ENOMEM;
		return rval;
	}

	qla_prepare_rdf_payload(vha);

	/* Initialize all required  fields of fcport */
	fcport->vha = vha;
	fcport->d_id = vha->d_id;
	fcport->loop_id = NPH_FABRIC_CONTROLLER; // RDF, EDC -> F_PORT
	ql_dbg(ql_dbg_scm, vha, 0x700a,
		"loop-id=%x portid=%-2x%02x%02x.\n",
		fcport->loop_id,
		fcport->d_id.b.domain, fcport->d_id.b.area, fcport->d_id.b.al_pa);

	/* Alloc SRB structure */
	sp = qla2x00_get_sp(vha, fcport, GFP_KERNEL);
	if (!sp) {
		qla2x00_free_fcport(fcport);
		rval = -ENOMEM;
		return rval;
	}

	sp->type = SRB_ELS_RDF;
	sp->name = "rdf_els";
	sp->u.iocb_cmd.u.drv_els.els_req.rdf_cmd = vha->rdf_els_payload;

	qla_update_sp(sp, vha, RDF_OPCODE);

	sp->free = qla2x00_scm_els_sp_free;
	sp->done = qla2x00_scm_els_sp_done;

	rval = qla2x00_start_sp(sp);
	if (rval != QLA_SUCCESS) {
		ql_log(ql_log_warn, vha, 0x700e,
		    "qla2x00_start_sp failed = %d\n", rval);
		qla2x00_rel_sp(sp);
		qla2x00_free_fcport(fcport);
		rval = -EIO;
	}
	return rval;
}

/*
 * qla2xxx_scm_send_edc_els - Send EDC ELS to the switch
 * Called by base port - post the initial login to the fabric
 */
int
qla2xxx_scm_send_edc_els(scsi_qla_host_t *vha)
{
	srb_t *sp;
	fc_port_t *fcport = NULL;
	int rval = 0;

	/* Allocate a dummy fcport structure, since functions
	 * preparing the IOCB and mailbox command retrieves port
	 * specific information from fcport structure.
	 */
	fcport = qla2x00_alloc_fcport(vha, GFP_KERNEL);
	if (!fcport) {
		rval = -ENOMEM;
		return rval;
	}

	qla_prepare_edc_payload(vha);

	/* Initialize all required  fields of fcport */
	fcport->vha = vha;
	fcport->d_id = vha->d_id;
	fcport->loop_id = NPH_F_PORT;
	ql_dbg(ql_dbg_scm, vha, 0x700a,
	    "loop-id=%x "
	    "portid=%-2x%02x%02x.\n",
	    fcport->loop_id,
	    fcport->d_id.b.domain, fcport->d_id.b.area, fcport->d_id.b.al_pa);

	/* Alloc SRB structure */
	sp = qla2x00_get_sp(vha, fcport, GFP_KERNEL);
	if (!sp) {
		rval = -ENOMEM;
		return rval;
	}

	sp->type = SRB_ELS_EDC;
	sp->name = "edc_els";
	sp->u.iocb_cmd.u.drv_els.els_req.edc_cmd = vha->hw->edc_els_payload;

	qla_update_sp(sp, vha, EDC_OPCODE);

	sp->free = qla2x00_scm_els_sp_free;
	sp->done = qla2x00_scm_els_sp_done;

	rval = qla2x00_start_sp(sp);
	if (rval != QLA_SUCCESS) {
		ql_log(ql_log_warn, vha, 0x700e,
		    "qla2x00_start_sp failed = %d\n", rval);
		qla2x00_rel_sp(sp);
		rval = -EIO;
	}
	return rval;
}

void qla2xxx_send_uscm_els(scsi_qla_host_t *vha)
{
	if (test_and_clear_bit(SCM_SEND_EDC, &vha->dpc_flags)) {
		ql_log(ql_log_info, vha, 0x20ad,
		    "Driver sending EDC for port :%8phC\n", vha->port_name);
		qla2xxx_scm_send_edc_els(vha);
	}
	if (test_and_clear_bit(SCM_SEND_RDF, &vha->dpc_flags)) {
		ql_log(ql_log_info, vha, 0x20ae,
		    "Driver sending RDF for port :%8phC\n", vha->port_name);
		qla2xxx_scm_send_rdf_els(vha);
	}
}
