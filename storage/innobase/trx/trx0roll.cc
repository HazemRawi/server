/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file trx/trx0roll.cc
Transaction rollback

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0roll.h"

#include <my_service_manager.h>
#include <mysql/service_wsrep.h>

#include "fsp0fsp.h"
#include "lock0lock.h"
#include "mach0data.h"
#include "pars0pars.h"
#include "que0que.h"
#include "row0mysql.h"
#include "row0undo.h"
#include "srv0mon.h"
#include "srv0start.h"
#include "trx0rec.h"
#include "trx0rseg.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "trx0undo.h"
#include "log.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t	trx_rollback_clean_thread_key;
#endif

tpool::task_group rollback_all_recovered_group(1);
tpool::waitable_task rollback_all_recovered_task(trx_rollback_all_recovered,
                                                 nullptr,
                                                 &rollback_all_recovered_group);

/** true if trx_rollback_all_recovered() thread is active */
bool			trx_rollback_is_active;

/** In crash recovery, the current trx to be rolled back; NULL otherwise */
const trx_t*		trx_roll_crash_recv_trx;

bool trx_t::rollback_finish() noexcept
{
  apply_online_log= false;
  if (UNIV_LIKELY(error_state == DB_SUCCESS))
  {
    commit();
    return true;
  }

  ut_a(error_state == DB_INTERRUPTED);
  ut_ad(srv_shutdown_state != SRV_SHUTDOWN_NONE);
  ut_a(!srv_undo_sources);
  ut_ad(srv_fast_shutdown);
  ut_d(in_rollback= false);
  if (trx_undo_t *&undo= rsegs.m_redo.undo)
  {
    UT_LIST_REMOVE(rsegs.m_redo.rseg->undo_list, undo);
    ut_free(undo);
    undo= nullptr;
  }
  if (trx_undo_t *&undo= rsegs.m_noredo.undo)
  {
    UT_LIST_REMOVE(rsegs.m_noredo.rseg->undo_list, undo);
    ut_free(undo);
    undo= nullptr;
  }
  commit_low();
  return commit_cleanup();
}

dberr_t trx_t::rollback_low(const undo_no_t *savept) noexcept
{
  op_info= "rollback";
  mem_heap_t *heap= mem_heap_create(512);
  roll_node_t *roll_node= roll_node_create(heap);

  roll_node->savept= savept ? *savept : 0;

  ut_ad(!in_rollback);
#ifdef UNIV_DEBUG
  if (savept)
  {
    ut_ad(state == TRX_STATE_ACTIVE);
    ut_ad(mysql_thd);
    ut_ad(!is_recovered);
  }
#endif

  error_state= DB_SUCCESS;

  if (has_logged())
  {
    ut_ad(rsegs.m_redo.rseg || rsegs.m_noredo.rseg);
    que_thr_t *thr= pars_complete_graph_for_exec(roll_node, this, heap,
                                                 nullptr);
    ut_a(thr == que_fork_start_command(static_cast<que_fork_t*>
                                       (que_node_get_parent(thr))));
    que_run_threads(thr);
    que_run_threads(roll_node->undo_thr);

    /* Free the memory reserved by the undo graph. */
    que_graph_free(static_cast<que_t*>(roll_node->undo_thr->common.parent));
  }

  if (!savept)
  {
    rollback_finish();
    MONITOR_INC(MONITOR_TRX_ROLLBACK);
  }
  else
  {
    /* There must not be partial rollback if transaction was chosen as deadlock
    victim. Galera transaction abort can be invoked during partial rollback. */
    ut_ad(!(lock.was_chosen_as_deadlock_victim & 1));
    ut_a(error_state == DB_SUCCESS);
    const undo_no_t limit{*savept};
    apply_online_log= false;
    for (trx_mod_tables_t::iterator i= mod_tables.begin();
         i != mod_tables.end(); )
    {
      trx_mod_tables_t::iterator j= i++;
      ut_ad(j->second.valid());
      if (j->second.rollback(limit))
      {
        j->second.clear_bulk_buffer();
        mod_tables.erase(j);
      }
      else if (!apply_online_log)
        apply_online_log= j->first->is_native_online_ddl();
    }
    MONITOR_INC(MONITOR_TRX_ROLLBACK_SAVEPOINT);
  }

  mem_heap_free(heap);
  op_info= "";
  return error_state;
}

dberr_t trx_t::rollback(const undo_no_t *savept) noexcept
{
  ut_ad(!mutex_is_owner());
  switch (state) {
  case TRX_STATE_ABORTED:
    ut_ad(!savept);
    state= TRX_STATE_NOT_STARTED;
    /* fall through */
  case TRX_STATE_NOT_STARTED:
    error_state= DB_SUCCESS;
    return DB_SUCCESS;
  case TRX_STATE_PREPARED:
  case TRX_STATE_PREPARED_RECOVERED:
  case TRX_STATE_COMMITTED_IN_MEMORY:
    ut_ad("invalid state" == 0);
    /* fall through */
  case TRX_STATE_ACTIVE:
    break;
  }
#ifdef WITH_WSREP
  if (!savept && is_wsrep() && wsrep_thd_is_SR(mysql_thd))
    wsrep_handle_SR_rollback(nullptr, mysql_thd);
#endif /* WITH_WSREP */
  return rollback_low(savept);
}

/** Rollback a transaction used in MySQL
@param[in, out]	trx	transaction
@return error code or DB_SUCCESS */
dberr_t trx_rollback_for_mysql(trx_t* trx)
{
	/* We are reading trx->state without holding trx->mutex
	here, because the rollback should be invoked for a running
	active MySQL transaction (or recovered prepared transaction)
	that is associated with the current thread. */

	switch (trx->state) {
	case TRX_STATE_ABORTED:
		trx->state = TRX_STATE_NOT_STARTED;
		/* fall through */
	case TRX_STATE_NOT_STARTED:
		trx->will_lock = false;
		ut_ad(trx->mysql_thd);
		/* Galera transaction abort can be invoked from MDL acquisition
		code, so trx->lock.was_chosen_as_deadlock_victim can be set
		even if trx->state is TRX_STATE_NOT_STARTED. */
		ut_ad(!(trx->lock.was_chosen_as_deadlock_victim & 1));
#ifdef WITH_WSREP
		ut_ad(!trx->is_wsrep());
		trx->lock.was_chosen_as_deadlock_victim= false;
#endif
		ut_a(UT_LIST_GET_LEN(trx->lock.trx_locks) == 0);
		return(DB_SUCCESS);
	case TRX_STATE_ACTIVE:
		ut_ad(trx->mysql_thd);
		ut_ad(!trx->is_recovered);
		ut_ad(!trx->is_autocommit_non_locking() || trx->read_only);
		return trx->rollback_low();

	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
		ut_ad(!trx->is_autocommit_non_locking());
		if (trx->rsegs.m_redo.undo) {
			/* The XA ROLLBACK of a XA PREPARE transaction
			will consist of multiple mini-transactions.

			As the very first step of XA ROLLBACK, we must
			change the undo log state back from
			TRX_UNDO_PREPARED to TRX_UNDO_ACTIVE, in order
			to ensure that recovery will complete the
			rollback.

			Failure to perform this step could cause a
			situation where we would roll back part of
			a XA PREPARE transaction, the server would be
			killed, and finally, the transaction would be
			recovered in XA PREPARE state, with some of
			the actions already having been rolled back. */
			ut_ad(trx->rsegs.m_redo.undo->rseg
			      == trx->rsegs.m_redo.rseg);
			mtr_t		mtr;
			mtr.start();
			if (trx_undo_t* undo = trx->rsegs.m_redo.undo) {
				trx_undo_set_state_at_prepare(trx, undo, true,
							      &mtr);
			}
			/* Write the redo log for the XA ROLLBACK
			state change to the global buffer. It is
			not necessary to flush the redo log. If
			a durable log write of a later mini-transaction
			takes place for whatever reason, then this state
			change will be durable as well. */
			mtr.commit();
			ut_ad(mtr.commit_lsn() > 0);
		}
		return trx->rollback_low();

	case TRX_STATE_COMMITTED_IN_MEMORY:
		ut_ad(!trx->is_autocommit_non_locking());
		break;
	}

	ut_error;
	return(DB_CORRUPTION);
}

/*******************************************************************//**
Roll back an active transaction. */
static
void
trx_rollback_active(
/*================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	mem_heap_t*	heap;
	que_fork_t*	fork;
	que_thr_t*	thr;
	roll_node_t*	roll_node;
	const trx_id_t	trx_id = trx->id;

	ut_ad(trx_id);

	heap = mem_heap_create(512);

	fork = que_fork_create(heap);
	fork->trx = trx;

	thr = que_thr_create(fork, heap, NULL);

	roll_node = roll_node_create(heap);

	thr->child = roll_node;
	roll_node->common.parent = thr;

	trx->graph = fork;

	ut_a(thr == que_fork_start_command(fork));

	trx_roll_crash_recv_trx	= trx;

	const bool dictionary_locked = trx->dict_operation;

	if (dictionary_locked) {
		row_mysql_lock_data_dictionary(trx);
	}

	que_run_threads(thr);
	ut_a(roll_node->undo_thr != NULL);

	que_run_threads(roll_node->undo_thr);

	que_graph_free(
		static_cast<que_t*>(roll_node->undo_thr->common.parent));

	if (UNIV_UNLIKELY(!trx->rollback_finish())) {
		ut_ad(!dictionary_locked);
	} else {
		sql_print_information(
			"InnoDB: Rolled back recovered transaction "
			TRX_ID_FMT, trx_id);
	}

	if (dictionary_locked) {
		row_mysql_unlock_data_dictionary(trx);
	}

	mem_heap_free(heap);

	trx_roll_crash_recv_trx	= NULL;
}


struct trx_roll_count_callback_arg
{
  uint32_t n_trx;
  uint64_t n_rows;
  trx_roll_count_callback_arg(): n_trx(0), n_rows(0) {}
};


static my_bool trx_roll_count_callback(rw_trx_hash_element_t *element,
                                       trx_roll_count_callback_arg *arg)
{
  element->mutex.wr_lock();
  if (trx_t *trx= element->trx)
  {
    if (trx->is_recovered && trx_state_eq(trx, TRX_STATE_ACTIVE))
    {
      arg->n_trx++;
      arg->n_rows+= trx->undo_no;
    }
  }
  element->mutex.wr_unlock();
  return 0;
}

/** Report progress when rolling back a row of a recovered transaction. */
void trx_roll_report_progress()
{
	time_t now = time(NULL);
	mysql_mutex_lock(&recv_sys.mutex);
	bool report = recv_sys.report(now);
	mysql_mutex_unlock(&recv_sys.mutex);

	if (report) {
		trx_roll_count_callback_arg arg;

		/* Get number of recovered active transactions and number of
		rows they modified. Numbers must be accurate, because only this
		thread is allowed to touch recovered transactions. */
		trx_sys.rw_trx_hash.iterate_no_dups(
			trx_roll_count_callback, &arg);

		if (arg.n_rows > 0) {
			service_manager_extend_timeout(
				INNODB_EXTEND_TIMEOUT_INTERVAL,
				"To roll back: " UINT32PF " transactions, "
				UINT64PF " rows", arg.n_trx, arg.n_rows);
		}

		ib::info() << "To roll back: " << arg.n_trx
			   << " transactions, " << arg.n_rows << " rows";

	}
}


static my_bool trx_rollback_recovered_callback(rw_trx_hash_element_t *element,
                                               std::vector<trx_t*> *trx_list)
{
  element->mutex.wr_lock();
  if (trx_t *trx= element->trx)
  {
    trx->mutex_lock();
    if (trx_state_eq(trx, TRX_STATE_ACTIVE) && trx->is_recovered)
      trx_list->push_back(trx);
    trx->mutex_unlock();
  }
  element->mutex.wr_unlock();
  return 0;
}

/**
  Rollback any incomplete transactions which were encountered in crash recovery.

  If the transaction already was committed, then we clean up a possible insert
  undo log. If the transaction was not yet committed, then we roll it back.

  Note: For XA recovered transactions, we rely on MySQL to
  do rollback. They will be in TRX_STATE_PREPARED state. If the server
  is shutdown and they are still lingering in trx_sys_t::trx_list
  then the shutdown will hang.

  @param[in]  all  true=roll back all recovered active transactions;
                   false=roll back any incomplete dictionary transaction
*/

void trx_rollback_recovered(bool all)
{
  std::vector<trx_t*> trx_list;

  ut_a(srv_force_recovery <
       ulong(all ? SRV_FORCE_NO_TRX_UNDO : SRV_FORCE_NO_DDL_UNDO));

  /*
    Collect list of recovered ACTIVE transaction ids first. Once collected, no
    other thread is allowed to modify or remove these transactions from
    rw_trx_hash.
  */
  trx_sys.rw_trx_hash.iterate_no_dups(trx_rollback_recovered_callback,
                                      &trx_list);

  while (!trx_list.empty())
  {
    trx_t *trx= trx_list.back();
    trx_list.pop_back();

    ut_ad(trx);
    ut_d(trx->mutex_lock());
    ut_ad(trx->is_recovered);
    ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));
    ut_d(trx->mutex_unlock());

    if (srv_shutdown_state != SRV_SHUTDOWN_NONE && !srv_undo_sources &&
        srv_fast_shutdown)
      goto discard;

    if (all || trx->dict_operation || trx->has_stats_table_lock())
    {
      trx_rollback_active(trx);
      if (trx->error_state != DB_SUCCESS)
      {
        ut_ad(trx->error_state == DB_INTERRUPTED);
        trx->error_state= DB_SUCCESS;
        ut_ad(!srv_undo_sources);
        ut_ad(srv_fast_shutdown);
discard:
        /* Note: before kill_server() invoked innobase_end() via
        unireg_end(), it invoked close_connections(), which should initiate
        the rollback of any user transactions via THD::cleanup() in the
        connection threads, and wait for all THD::cleanup() to complete.
        So, no active user transactions should exist at this point.

        srv_undo_sources=false was cleared early in innobase_end().

        Generally, the server guarantees that all connections using
        InnoDB must be disconnected by the time we are reaching this code,
        be it during shutdown or UNINSTALL PLUGIN.

        Because there is no possible race condition with any
        concurrent user transaction, we do not have to invoke
        trx->commit_state() or wait for !trx->is_referenced()
        before trx_sys.deregister_rw(trx). */
        trx_sys.deregister_rw(trx);
        trx_free_at_shutdown(trx);
      }
      else
        trx->free();
    }
  }
}

/*******************************************************************//**
Rollback or clean up any incomplete transactions which were
encountered in crash recovery.  If the transaction already was
committed, then we clean up a possible insert undo log. If the
transaction was not yet committed, then we roll it back.
Note: this is done in a background thread. */
void trx_rollback_all_recovered(void*)
{
	ut_ad(!srv_read_only_mode);

	if (trx_sys.rw_trx_hash.size()) {
		ib::info() << "Starting in background the rollback of"
			" recovered transactions";
		trx_rollback_recovered(true);
		ib::info() << "Rollback of non-prepared transactions"
			" completed";
	}

	trx_rollback_is_active = false;
}

/****************************************************************//**
Builds an undo 'query' graph for a transaction. The actual rollback is
performed by executing this query graph like a query subprocedure call.
The reply about the completion of the rollback will be sent by this
graph.
@return own: the query graph */
static
que_t*
trx_roll_graph_build(
/*=================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	mem_heap_t*	heap;
	que_fork_t*	fork;
	que_thr_t*	thr;

	ut_ad(trx->mutex_is_owner());
	heap = mem_heap_create(512);
	fork = que_fork_create(heap);
	fork->trx = trx;

	thr = que_thr_create(fork, heap, NULL);

	thr->child = row_undo_node_create(trx, thr, heap);

	return(fork);
}

/*********************************************************************//**
Starts a rollback operation, creates the UNDO graph that will do the
actual undo operation.
@return query graph thread that will perform the UNDO operations. */
static
que_thr_t*
trx_rollback_start(
/*===============*/
	trx_t*		trx,		/*!< in: transaction */
	undo_no_t	roll_limit)	/*!< in: rollback to undo no (for
					partial undo), 0 if we are rolling back
					the entire transaction */
{
	/* Initialize the rollback field in the transaction */

	ut_ad(trx->mutex_is_owner());
	ut_ad(!trx->roll_limit);
	ut_ad(!trx->in_rollback);

	trx->roll_limit = roll_limit;
	trx->in_rollback = true;

	ut_a(trx->roll_limit <= trx->undo_no);

	trx->pages_undone = 0;

	/* Build a 'query' graph which will perform the undo operations */

	que_t*	roll_graph = trx_roll_graph_build(trx);

	trx->graph = roll_graph;

	return(que_fork_start_command(roll_graph));
}

/*********************************************************************//**
Creates a rollback command node struct.
@return own: rollback node struct */
roll_node_t*
roll_node_create(
/*=============*/
	mem_heap_t*	heap)	/*!< in: mem heap where created */
{
	roll_node_t*	node;

	node = static_cast<roll_node_t*>(mem_heap_zalloc(heap, sizeof(*node)));

	node->state = ROLL_NODE_SEND;

	node->common.type = QUE_NODE_ROLLBACK;

	return(node);
}

/***********************************************************//**
Performs an execution step for a rollback command node in a query graph.
@return query thread to run next, or NULL */
que_thr_t*
trx_rollback_step(
/*==============*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	roll_node_t*	node;

	node = static_cast<roll_node_t*>(thr->run_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_ROLLBACK);

	if (thr->prev_node == que_node_get_parent(node)) {
		node->state = ROLL_NODE_SEND;
	}

	if (node->state == ROLL_NODE_SEND) {
		trx_t*		trx;

		trx = thr_get_trx(thr);

		node->state = ROLL_NODE_WAIT;

		ut_a(node->undo_thr == NULL);

		trx->mutex_lock();

		trx_commit_or_rollback_prepare(trx);

		node->undo_thr = trx_rollback_start(trx, node->savept);

		trx->mutex_unlock();
	} else {
		ut_ad(node->state == ROLL_NODE_WAIT);

		thr->run_node = que_node_get_parent(node);
	}

	return(thr);
}
