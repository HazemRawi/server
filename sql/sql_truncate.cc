/* Copyright (c) 2010, 2015, Oracle and/or its affiliates.
   Copyright (c) 2012, 2018, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mariadb.h"
#include "debug_sync.h"  // DEBUG_SYNC
#include "table.h"       // TABLE, FOREIGN_KEY_INFO
#include "sql_class.h"   // THD
#include "sql_base.h"    // open_and_lock_tables
#include "sql_table.h"   // write_bin_log
#include "datadict.h"    // dd_recreate_table()
#include "lock.h"        // MYSQL_OPEN_* flags
#include "sql_acl.h"     // DROP_ACL
#include "sql_parse.h"   // check_one_table_access()
#include "sql_truncate.h"
#include "wsrep_mysqld.h"
#include "sql_show.h"    //append_identifier()
#include "sql_select.h"
#include "sql_delete.h"

/**
  Append a list of field names to a string.

  @param  str     The string.
  @param  fields  The list of field names.

  @return TRUE on failure, FALSE otherwise.
*/

static bool fk_info_append_fields(THD *thd, String *str,
                                  List<LEX_CSTRING> *fields)
{
  bool res= FALSE;
  LEX_CSTRING *field;
  List_iterator_fast<LEX_CSTRING> it(*fields);

  while ((field= it++))
  {
    res|= append_identifier(thd, str, field);
    res|= str->append(STRING_WITH_LEN(", "));
  }

  str->chop();
  str->chop();

  return res;
}


/**
  Generate a foreign key description suitable for a error message.

  @param thd          Thread context.
  @param fk_info   The foreign key information.

  @return A human-readable string describing the foreign key.
*/

static const char *fk_info_str(THD *thd, FOREIGN_KEY_INFO *fk_info)
{
  bool res= FALSE;
  char buffer[STRING_BUFFER_USUAL_SIZE*2];
  String str(buffer, sizeof(buffer), system_charset_info);

  str.length(0);

  /*
    `db`.`tbl`, CONSTRAINT `id` FOREIGN KEY (`fk`) REFERENCES `db`.`tbl` (`fk`)
  */

  res|= append_identifier(thd, &str, fk_info->foreign_db);
  res|= str.append('.');
  res|= append_identifier(thd, &str, fk_info->foreign_table);
  res|= str.append(STRING_WITH_LEN(", CONSTRAINT "));
  res|= append_identifier(thd, &str, fk_info->foreign_id);
  res|= str.append(STRING_WITH_LEN(" FOREIGN KEY ("));
  res|= fk_info_append_fields(thd, &str, &fk_info->foreign_fields);
  res|= str.append(STRING_WITH_LEN(") REFERENCES "));
  res|= append_identifier(thd, &str, fk_info->referenced_db);
  res|= str.append('.');
  res|= append_identifier(thd, &str, fk_info->referenced_table);
  res|= str.append(STRING_WITH_LEN(" ("));
  res|= fk_info_append_fields(thd, &str, &fk_info->referenced_fields);
  res|= str.append(')');

  return res ? NULL : thd->strmake(str.ptr(), str.length());
}


/**
  Check and emit a fatal error if the table which is going to be
  affected by TRUNCATE TABLE is a parent table in some non-self-
  referencing foreign key.

  @remark The intention is to allow truncate only for tables that
          are not dependent on other tables.

  @param  thd    Thread context.
  @param  table  Table handle.

  @retval FALSE  This table is not parent in a non-self-referencing foreign
                 key. Statement can proceed.
  @retval TRUE   This table is parent in a non-self-referencing foreign key,
                 error was emitted.
*/

static bool
fk_truncate_illegal_if_parent(THD *thd, TABLE *table)
{
  FOREIGN_KEY_INFO *fk_info;
  List<FOREIGN_KEY_INFO> fk_list;
  List_iterator_fast<FOREIGN_KEY_INFO> it;

  /*
    Bail out early if the table is not referenced by a foreign key.
    In this case, the table could only be, if at all, a child table.
  */
  if (! table->file->referenced_by_foreign_key())
    return FALSE;

  /*
    This table _is_ referenced by a foreign key. At this point, only
    self-referencing keys are acceptable. For this reason, get the list
    of foreign keys referencing this table in order to check the name
    of the child (dependent) tables.
  */
  table->file->get_parent_foreign_key_list(thd, &fk_list);

  /* Out of memory when building list. */
  if (unlikely(thd->is_error()))
    return TRUE;

  it.init(fk_list);

  /* Loop over the set of foreign keys for which this table is a parent. */
  while ((fk_info= it++))
  {
    if (!table->s->db.streq(*fk_info->referenced_db) ||
        !table->s->table_name.streq(*fk_info->referenced_table) ||
        !table->s->db.streq(*fk_info->foreign_db) ||
        !table->s->table_name.streq(*fk_info->foreign_table))
      break;
  }

  /* Table is parent in a non-self-referencing foreign key. */
  if (fk_info)
  {
    my_error(ER_TRUNCATE_ILLEGAL_FK, MYF(0), fk_info_str(thd, fk_info));
    return TRUE;
  }

  return FALSE;
}


/*
  Open and truncate a locked table.

  @param  thd           Thread context.
  @param  table_ref     Table list element for the table to be truncated.
  @param  is_tmp_table  True if element refers to a temp table.

  @retval TRUNCATE_OK   Truncate was successful and statement can be safely
                        binlogged.
  @retval TRUNCATE_FAILED_BUT_BINLOG Truncate failed but still go ahead with
                        binlogging as in case of non transactional tables
                        partial truncation is possible.

  @retval TRUNCATE_FAILED_SKIP_BINLOG Truncate was not successful hence donot
                        binlong the statement.
*/

enum Sql_cmd_truncate_table::truncate_result
Sql_cmd_truncate_table::handler_truncate(THD *thd, TABLE_LIST *table_ref,
                                         bool is_tmp_table)
{
  int error= 0;
  uint flags= 0;
  TABLE *table;
  DBUG_ENTER("Sql_cmd_truncate_table::handler_truncate");

  /*
    Can't recreate, the engine must mechanically delete all rows
    in the table. Use open_and_lock_tables() to open a write cursor.
  */

  /* If it is a temporary table, no need to take locks. */
  if (!is_tmp_table)
  {
    /* We don't need to load triggers. */
    DBUG_ASSERT(table_ref->trg_event_map == 0);
    /*
      Our metadata lock guarantees that no transaction is reading
      or writing into the table. Yet, to open a write cursor we need
      a thr_lock lock. Allow to open base tables only.
    */
    table_ref->required_type= TABLE_TYPE_NORMAL;
    /*
      Ignore pending FLUSH TABLES since we don't want to release
      the MDL lock taken above and otherwise there is no way to
      wait for FLUSH TABLES in deadlock-free fashion.
    */
    flags= MYSQL_OPEN_IGNORE_FLUSH;
    /*
      Even though we have an MDL lock on the table here, we don't
      pass MYSQL_OPEN_HAS_MDL_LOCK to open_and_lock_tables
      since to truncate a MERGE table, we must open and lock
      merge children, and on those we don't have an MDL lock.
      Thus clear the ticket to satisfy MDL asserts.
    */
    table_ref->mdl_request.ticket= NULL;
  }

  /* Open the table as it will handle some required preparations. */
  if (open_and_lock_tables(thd, table_ref, FALSE, flags))
    DBUG_RETURN(TRUNCATE_FAILED_SKIP_BINLOG);

  /* Whether to truncate regardless of foreign keys. */
  if (! (thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS))
    if (fk_truncate_illegal_if_parent(thd, table_ref->table))
      DBUG_RETURN(TRUNCATE_FAILED_SKIP_BINLOG);

  table= table_ref->table;

  if ((table->file->ht->flags & HTON_TRUNCATE_REQUIRES_EXCLUSIVE_USE) &&
      !is_tmp_table)
  {
    if (wait_while_table_is_used(thd, table, HA_EXTRA_FORCE_REOPEN))
      DBUG_RETURN(TRUNCATE_FAILED_SKIP_BINLOG);
    /*
      Get rid of all TABLE instances belonging to this thread
      except one to be used for TRUNCATE
    */
    close_all_tables_for_name(thd, table->s,
			      HA_EXTRA_NOT_USED,
                              table);
  }

  error= table->file->ha_truncate();

  if (!is_tmp_table && !error)
  {
    backup_log_info ddl_log;
    bzero(&ddl_log, sizeof(ddl_log));
    ddl_log.query= { C_STRING_WITH_LEN("TRUNCATE") };
    ddl_log.org_partitioned=  table->file->partition_engine();
    lex_string_set(&ddl_log.org_storage_engine_name,
                   table->file->real_table_type());
    ddl_log.org_database=     table->s->db;
    ddl_log.org_table=        table->s->table_name;
    ddl_log.org_table_id=     table->s->tabledef_version;
    backup_log_ddl(&ddl_log);
  }

  if (unlikely(error))
  {
    table->file->print_error(error, MYF(0));
    /*
      If truncate method is not implemented then we don't binlog the
      statement. If truncation has failed in a transactional engine then also
      we don't binlog the statement. Only in non transactional engine we binlog
      inspite of errors.
     */
    if (error == HA_ERR_WRONG_COMMAND ||
        table->file->has_transactions_and_rollback())
      DBUG_RETURN(TRUNCATE_FAILED_SKIP_BINLOG);
    else
      DBUG_RETURN(TRUNCATE_FAILED_BUT_BINLOG);
  }
  DBUG_RETURN(TRUNCATE_OK);
}


/*
  Handle locking a base table for truncate.

  @param[in]  thd               Thread context.
  @param[in]  table_ref         Table list element for the table to
                                be truncated.
  @param[out] hton_can_recreate Set to TRUE if table can be dropped
                                and recreated.

  @retval  FALSE  Success.
  @retval  TRUE   Error.
*/

bool Sql_cmd_truncate_table::lock_table(THD *thd, TABLE_LIST *table_ref,
                                        bool *hton_can_recreate)
{
  const handlerton *hton;
  bool versioned;
  bool sequence= false;
  TABLE *table= NULL;
  DBUG_ENTER("Sql_cmd_truncate_table::lock_table");

  /* Lock types are set in the parser. */
  DBUG_ASSERT(table_ref->lock_type == TL_WRITE);
  /* The handler truncate protocol dictates a exclusive lock. */
  DBUG_ASSERT(table_ref->mdl_request.type == MDL_EXCLUSIVE);

  /*
    Before doing anything else, acquire a metadata lock on the table,
    or ensure we have one.  We don't use open_and_lock_tables()
    right away because we want to be able to truncate (and recreate)
    corrupted tables, those that we can't fully open.

    MySQL manual documents that TRUNCATE can be used to repair a
    damaged table, i.e. a table that can not be fully "opened".
    In particular MySQL manual says: As long as the table format
    file tbl_name.frm is valid, the table can be re-created as
    an empty table with TRUNCATE TABLE, even if the data or index
    files have become corrupted.
  */
  if (thd->locked_tables_mode)
  {
    if (!(table= find_table_for_mdl_upgrade(thd, table_ref->db.str,
                                            table_ref->table_name.str, NULL)))
      DBUG_RETURN(TRUE);

    versioned= table->versioned();
    hton= table->file->ht;
#ifdef WITH_WSREP
    /* Resolve should we replicate truncate. It should
       be replicated if storage engine(s) associated
       are replicated by Galera. If this is partitioned
       table we need to find out default partition
       handlerton.
    */
    if (WSREP(thd) &&
        !wsrep_should_replicate_ddl(thd, table->file->partition_ht() ?
                                    table->file->partition_ht() : hton))
      DBUG_RETURN(TRUE);
#endif

    table_ref->mdl_request.ticket= table->mdl_ticket;
  }
  else
  {
    DBUG_ASSERT(table_ref->next_global == NULL);
    if (lock_table_names(thd, table_ref, NULL,
                         thd->variables.lock_wait_timeout, 0))
      DBUG_RETURN(TRUE);

    TABLE_SHARE *share= tdc_acquire_share(thd, table_ref, GTS_TABLE | GTS_VIEW);
    if (share == NULL)
      DBUG_RETURN(TRUE);
    DBUG_ASSERT(share != UNUSABLE_TABLE_SHARE);

    versioned= share->versioned;
    sequence= share->table_type == TABLE_TYPE_SEQUENCE;
    hton= share->db_type();
#ifdef WITH_WSREP
    if (WSREP(thd) && hton != view_pseudo_hton)
    {
      /* Resolve should we replicate truncate. It should
         be replicated if storage engine(s) associated
         are replicated by Galera. If this is partitioned
         table we need to find out default partition
         handlerton.
      */
      const handlerton* const ht=
#ifdef WITH_PARTITION_STORAGE_ENGINE
        share->default_part_plugin ?
          plugin_hton(share->default_part_plugin) :
#endif
        hton;

      if (ht && !wsrep_should_replicate_ddl(thd, ht))
      {
        tdc_release_share(share);
        DBUG_RETURN(TRUE);
      }
    }
#endif

    if (!versioned)
      tdc_remove_referenced_share(thd, share);
    else
      tdc_release_share(share);

    if (hton == view_pseudo_hton)
    {
      my_error(ER_NO_SUCH_TABLE, MYF(0), table_ref->db.str,
               table_ref->table_name.str);
      DBUG_RETURN(TRUE);
    }
  }

  *hton_can_recreate= (!sequence &&
                       ha_check_storage_engine_flag(hton, HTON_CAN_RECREATE));

  if (versioned)
  {
    my_error(ER_VERS_NOT_SUPPORTED, MYF(0), "TRUNCATE TABLE");
    DBUG_RETURN(TRUE);
  }

  /*
    A storage engine can recreate or truncate the table only if there
    are no references to it from anywhere, i.e. no cached TABLE in the
    table cache.
  */
  if (thd->locked_tables_mode)
  {
    DEBUG_SYNC(thd, "upgrade_lock_for_truncate");
    /* To remove the table from the cache we need an exclusive lock. */
    if (wait_while_table_is_used(thd, table,
          *hton_can_recreate ? HA_EXTRA_PREPARE_FOR_DROP : HA_EXTRA_NOT_USED))
      DBUG_RETURN(TRUE);
    m_ticket_downgrade= table->mdl_ticket;
    /* Close if table is going to be recreated. */
    if (*hton_can_recreate)
      close_all_tables_for_name(thd, table->s, HA_EXTRA_NOT_USED, NULL);
  }
  DBUG_RETURN(FALSE);
}


/*
  Optimized delete of all rows by doing a full generate of the table.

  @remark Will work even if the .MYI and .MYD files are destroyed.
          In other words, it works as long as the .FRM is intact and
          the engine supports re-create.

  @param  thd         Thread context.
  @param  table_ref   Table list element for the table to be truncated.

  @retval  FALSE  Success.
  @retval  TRUE   Error.
*/

bool Sql_cmd_truncate_table::truncate_table(THD *thd, TABLE_LIST *table_ref)
{
  int error;
  bool binlog_stmt;
  DBUG_ENTER("Sql_cmd_truncate_table::truncate_table");

  DBUG_ASSERT((!table_ref->table) ||
              (table_ref->table && table_ref->table->s));

  /* Initialize, or reinitialize in case of reexecution (SP). */
  m_ticket_downgrade= NULL;

  /* If it is a temporary table, no need to take locks. */
  if (is_temporary_table(table_ref))
  {
    /*
      In RBR, the statement is not binlogged if the table is temporary or
      table is not up to date in binlog.
    */
    binlog_stmt= (!thd->is_binlog_format_row() &&
                  table_ref->table->s->using_binlog());

    thd->close_unused_temporary_table_instances(table_ref);

    error= handler_truncate(thd, table_ref, TRUE);

    /*
      No need to invalidate the query cache, queries with temporary
      tables are not in the cache. No need to write to the binary
      log a failed row-by-row delete even if under RBR as the table
      might not exist on the slave.
    */
  }
  else /* It's not a temporary table. */
  {
    bool hton_can_recreate;

#ifdef WITH_WSREP
    if (WSREP(thd) && wsrep_thd_is_local(thd))
    {
      wsrep::key_array keys;
      /* Do not start TOI if table is not found */
      if (!wsrep_append_fk_parent_table(thd, table_ref, &keys))
      {
        if (keys.empty())
        {
          if (wsrep_to_isolation_begin(thd, table_ref->db.str, table_ref->table_name.str, NULL))
            DBUG_RETURN(TRUE);
        }
        else
        {
          if (wsrep_to_isolation_begin(thd, NULL, NULL, table_ref, NULL, &keys))
            DBUG_RETURN(TRUE);
        }
      }
    }
#endif /* WITH_WSREP */

    if (lock_table(thd, table_ref, &hton_can_recreate))
      DBUG_RETURN(TRUE);

    /*
      This is mainly here for truncate_notembedded.test, but it is still
      useful to check killed after we got the lock
    */

    if (thd->killed)
      DBUG_RETURN(TRUE);

    if (hton_can_recreate)
    {
     /*
        The storage engine can truncate the table by creating an
        empty table with the same structure.
      */
      error= dd_recreate_table(thd, table_ref->db.str, table_ref->table_name.str);

      if (thd->locked_tables_mode && thd->locked_tables_list.reopen_tables(thd, false))
      {
          thd->locked_tables_list.unlink_all_closed_tables(thd, NULL, 0);
          error= 1;
      }
      /* No need to binlog a failed truncate-by-recreate. */
      binlog_stmt= !error;
    }
    else
    {
      /*
        The engine does not support truncate-by-recreate.
        Attempt to use the handler truncate method.
      */
      error= handler_truncate(thd, table_ref, FALSE);

      if (error == TRUNCATE_OK && thd->locked_tables_mode &&
          (table_ref->table->file->ht->flags &
           (HTON_REQUIRES_CLOSE_AFTER_TRUNCATE |
            HTON_TRUNCATE_REQUIRES_EXCLUSIVE_USE)))
      {
        thd->locked_tables_list.mark_table_for_reopen(table_ref->table);
        if (unlikely(thd->locked_tables_list.reopen_tables(thd, false)))
          thd->locked_tables_list.unlink_all_closed_tables(thd, NULL, 0);
      }

      /*
        All effects of a TRUNCATE TABLE operation are committed even if
        truncation fails in the case of non transactional tables. Thus, the
        query must be written to the binary log. The only exception is a
        unimplemented truncate method.
      */
      if (unlikely(error == TRUNCATE_OK || error == TRUNCATE_FAILED_BUT_BINLOG))
        binlog_stmt= true;
      else
        binlog_stmt= false;
    }

    /*
      If we tried to open a MERGE table and failed due to problems with the
      children tables, the table will have been closed and table_ref->table
      will be invalid. Reset the pointer here in any case as
      query_cache_invalidate does not need a valid TABLE object.
    */
    table_ref->table= NULL;
    query_cache_invalidate3(thd, table_ref, FALSE);
  }

  /* DDL is logged in statement format, regardless of binlog format. */
  if (binlog_stmt)
    error|= write_bin_log(thd, !error, thd->query(), thd->query_length());

  /*
    A locked table ticket was upgraded to a exclusive lock. After the
    the query has been written to the binary log, downgrade the lock
    to a shared one.
  */
  if (m_ticket_downgrade)
    m_ticket_downgrade->downgrade_lock(MDL_SHARED_NO_READ_WRITE);

  DBUG_RETURN(error);
}

/**
  Execute a TRUNCATE statement at runtime.

  @param  thd   The current thread.

  @return FALSE on success.
*/

bool Sql_cmd_truncate_table::execute(THD *thd)
{
  bool res= TRUE;
  TABLE_LIST *table= thd->lex->first_select_lex()->table_list.first;
  DBUG_ENTER("Sql_cmd_truncate_table::execute");

  if (check_one_table_access(thd, DROP_ACL, table))
    DBUG_RETURN(res);

  if (! (res= truncate_table(thd, table)))
    my_ok(thd);

  DBUG_RETURN(res);
}
