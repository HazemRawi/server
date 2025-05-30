/* Copyright (c) 2010, 2023, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file storage/perfschema/table_ets_by_thread_by_event_name.cc
  Table EVENTS_TRANSACTIONS_SUMMARY_BY_HOST_BY_EVENT_NAME (implementation).
*/

#include "my_global.h"
#include "my_thread.h"
#include "pfs_instr_class.h"
#include "pfs_column_types.h"
#include "pfs_column_values.h"
#include "table_ets_by_thread_by_event_name.h"
#include "pfs_global.h"
#include "pfs_visitor.h"
#include "pfs_buffer_container.h"
#include "field.h"

THR_LOCK table_ets_by_thread_by_event_name::m_table_lock;

PFS_engine_table_share_state
table_ets_by_thread_by_event_name::m_share_state = {
  false /* m_checked */
};

PFS_engine_table_share
table_ets_by_thread_by_event_name::m_share=
{
  { C_STRING_WITH_LEN("events_transactions_summary_by_thread_by_event_name") },
  &pfs_truncatable_acl,
  table_ets_by_thread_by_event_name::create,
  NULL, /* write_row */
  table_ets_by_thread_by_event_name::delete_all_rows,
  table_ets_by_thread_by_event_name::get_row_count,
  sizeof(pos_ets_by_thread_by_event_name),
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE events_transactions_summary_by_thread_by_event_name("
  "THREAD_ID BIGINT unsigned not null comment 'Thread for which summary is generated.',"
  "EVENT_NAME VARCHAR(128) not null comment 'Event name for which summary is generated.',"
  "COUNT_STAR BIGINT unsigned not null comment 'The number of summarized events. This value includes all events, whether timed or nontimed.',"
  "SUM_TIMER_WAIT BIGINT unsigned not null comment 'The total wait time of the summarized timed events. This value is calculated only for timed events because nontimed events have a wait time of NULL. The same is true for the other xxx_TIMER_WAIT values.',"
  "MIN_TIMER_WAIT BIGINT unsigned not null comment 'The minimum wait time of the summarized timed events.',"
  "AVG_TIMER_WAIT BIGINT unsigned not null comment 'The average wait time of the summarized timed events.',"
  "MAX_TIMER_WAIT BIGINT unsigned not null comment 'The maximum wait time of the summarized timed events.',"
  "COUNT_READ_WRITE BIGINT unsigned not null comment 'The total number of only READ/WRITE transaction events.',"
  "SUM_TIMER_READ_WRITE BIGINT unsigned not null comment 'The total wait time of only READ/WRITE transaction events.',"
  "MIN_TIMER_READ_WRITE BIGINT unsigned not null comment 'The minimum wait time of only READ/WRITE transaction events.',"
  "AVG_TIMER_READ_WRITE BIGINT unsigned not null comment 'The average wait time of only READ/WRITE transaction events.',"
  "MAX_TIMER_READ_WRITE BIGINT unsigned not null comment 'The maximum wait time of only READ/WRITE transaction events.',"
  "COUNT_READ_ONLY BIGINT unsigned not null comment 'The total number of only READ ONLY transaction events.',"
  "SUM_TIMER_READ_ONLY BIGINT unsigned not null comment 'The total wait time of only READ ONLY transaction events.',"
  "MIN_TIMER_READ_ONLY BIGINT unsigned not null comment 'The minimum wait time of only READ ONLY transaction events.',"
  "AVG_TIMER_READ_ONLY BIGINT unsigned not null comment 'The average wait time of only READ ONLY transaction events.',"
  "MAX_TIMER_READ_ONLY BIGINT unsigned not null comment 'The maximum wait time of only READ ONLY transaction events.')")},
  false, /* m_perpetual */
  false, /* m_optional */
  &m_share_state
};

PFS_engine_table*
table_ets_by_thread_by_event_name::create(void)
{
  return new table_ets_by_thread_by_event_name();
}

int
table_ets_by_thread_by_event_name::delete_all_rows(void)
{
  reset_events_transactions_by_thread();
  return 0;
}

ha_rows
table_ets_by_thread_by_event_name::get_row_count(void)
{
  return global_thread_container.get_row_count() * transaction_class_max;
}

table_ets_by_thread_by_event_name::table_ets_by_thread_by_event_name()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(), m_next_pos()
{
  m_normalizer= time_normalizer::get_transaction();
}

void table_ets_by_thread_by_event_name::reset_position(void)
{
  m_pos.reset();
  m_next_pos.reset();
}

int table_ets_by_thread_by_event_name::rnd_init(bool scan)
{
  return 0;
}

int table_ets_by_thread_by_event_name::rnd_next(void)
{
  PFS_thread *thread;
  PFS_transaction_class *transaction_class;
  bool has_more_thread= true;

  for (m_pos.set_at(&m_next_pos);
       has_more_thread;
       m_pos.next_thread())
  {
    thread= global_thread_container.get(m_pos.m_index_1, & has_more_thread);
    if (thread != NULL)
    {
      transaction_class= find_transaction_class(m_pos.m_index_2);
      if (transaction_class)
      {
        make_row(thread, transaction_class);
        m_next_pos.set_after(&m_pos);
        return 0;
      }
    }
  }

  return HA_ERR_END_OF_FILE;
}

int
table_ets_by_thread_by_event_name::rnd_pos(const void *pos)
{
  PFS_thread *thread;
  PFS_transaction_class *transaction_class;

  set_position(pos);

  thread= global_thread_container.get(m_pos.m_index_1);
  if (thread != NULL)
  {
    transaction_class= find_transaction_class(m_pos.m_index_2);
    if (transaction_class)
    {
      make_row(thread, transaction_class);
      return 0;
    }
  }

  return HA_ERR_RECORD_DELETED;
}

void table_ets_by_thread_by_event_name
::make_row(PFS_thread *thread, PFS_transaction_class *klass)
{
  pfs_optimistic_state lock;
  m_row_exists= false;

  /* Protect this reader against a thread termination */
  thread->m_lock.begin_optimistic_lock(&lock);

  m_row.m_thread_internal_id= thread->m_thread_internal_id;

  m_row.m_event_name.make_row(klass);

  PFS_connection_transaction_visitor visitor(klass);
  PFS_connection_iterator::visit_thread(thread, &visitor);

  if (! thread->m_lock.end_optimistic_lock(&lock))
    return;

  m_row_exists= true;
  m_row.m_stat.set(m_normalizer, &visitor.m_stat);
}

int table_ets_by_thread_by_event_name
::read_row_values(TABLE *table, unsigned char *, Field **fields,
                  bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  /* Set the null bits */
  assert(table->s->null_bytes == 0);

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* THREAD_ID */
        set_field_ulonglong(f, m_row.m_thread_internal_id);
        break;
      case 1: /* EVENT_NAME */
        m_row.m_event_name.set_field(f);
        break;
      default:
        /**
          COUNT_STAR, SUM/MIN/AVG/MAX_TIMER_WAIT
          COUNT_READ_WRITE, SUM/MIN/AVG/MAX_TIMER_READ_WRITE
          COUNT_READ_ONLY, SUM/MIN/AVG/MAX_TIMER_READ_ONLY
        */
        m_row.m_stat.set_field(f->field_index-2, f);
        break;
      }
    }
  }

  return 0;
}

