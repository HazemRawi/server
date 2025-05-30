/* Copyright (c) 2008, 2023, Oracle and/or its affiliates.

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
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/**
  @file storage/perfschema/table_performance_timers.cc
  Table PERFORMANCE_TIMERS (implementation).
*/

#include "my_global.h"
#include "my_thread.h"
#include "table_performance_timers.h"
#include "pfs_timer.h"
#include "pfs_global.h"
#include "field.h"

THR_LOCK table_performance_timers::m_table_lock;

PFS_engine_table_share_state
table_performance_timers::m_share_state = {
  false /* m_checked */
};

PFS_engine_table_share
table_performance_timers::m_share=
{
  { C_STRING_WITH_LEN("performance_timers") },
  &pfs_readonly_acl,
  table_performance_timers::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_performance_timers::get_row_count,
  sizeof(PFS_simple_index), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE performance_timers("
                      "TIMER_NAME ENUM ('CYCLE', 'NANOSECOND', 'MICROSECOND', 'MILLISECOND') not null comment 'Timer name',"
                      "TIMER_FREQUENCY BIGINT comment 'Number of timer units per second. Dependent on the processor speed.',"
                      "TIMER_RESOLUTION BIGINT comment 'Number of timer units by which timed values increase each time.',"
                      "TIMER_OVERHEAD BIGINT comment 'Minimum timer overhead, determined during initialization by calling the timer 20 times and selecting the smallest value. Total overhead will be at least double this, as the timer is called at the beginning and end of each timed event.')") },
  false, /* m_perpetual */
  false, /* m_optional */
  &m_share_state
};

PFS_engine_table* table_performance_timers::create(void)
{
  return new table_performance_timers();
}

ha_rows
table_performance_timers::get_row_count(void)
{
  return COUNT_TIMER_NAME;
}

table_performance_timers::table_performance_timers()
  : PFS_engine_table(&m_share, &m_pos),
    m_row(NULL), m_pos(0), m_next_pos(0)
{
  int index;

  index= (int)TIMER_NAME_CYCLE - FIRST_TIMER_NAME;
  m_data[index].m_timer_name= TIMER_NAME_CYCLE;
  m_data[index].m_info= sys_timer_info.cycles;

  index= (int)TIMER_NAME_NANOSEC - FIRST_TIMER_NAME;
  m_data[index].m_timer_name= TIMER_NAME_NANOSEC;
  m_data[index].m_info= sys_timer_info.nanoseconds;

  index= (int)TIMER_NAME_MICROSEC - FIRST_TIMER_NAME;
  m_data[index].m_timer_name= TIMER_NAME_MICROSEC;
  m_data[index].m_info= sys_timer_info.microseconds;

  index= (int)TIMER_NAME_MILLISEC - FIRST_TIMER_NAME;
  m_data[index].m_timer_name= TIMER_NAME_MILLISEC;
  m_data[index].m_info= sys_timer_info.milliseconds;
}

void table_performance_timers::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

int table_performance_timers::rnd_next(void)
{
  int result;

  m_pos.set_at(&m_next_pos);

  if (m_pos.m_index < COUNT_TIMER_NAME)
  {
    m_row= &m_data[m_pos.m_index];
    m_next_pos.set_after(&m_pos);
    result= 0;
  }
  else
  {
    m_row= NULL;
    result= HA_ERR_END_OF_FILE;
  }

  return result;
}

int table_performance_timers::rnd_pos(const void *pos)
{
  set_position(pos);
  assert(m_pos.m_index < COUNT_TIMER_NAME);
  m_row= &m_data[m_pos.m_index];
  return 0;
}

int table_performance_timers::read_row_values(TABLE *table,
                                              unsigned char *buf,
                                              Field **fields,
                                              bool read_all)
{
  Field *f;

  assert(m_row);

  /* Set the null bits */
  assert(table->s->null_bytes == 1);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /* TIMER_NAME */
        set_field_enum(f, m_row->m_timer_name);
        break;
      case 1: /* TIMER_FREQUENCY */
        if (m_row->m_info.routine != 0)
          set_field_ulonglong(f, m_row->m_info.frequency);
        else
          f->set_null();
        break;
      case 2: /* TIMER_RESOLUTION */
        if (m_row->m_info.routine != 0)
          set_field_ulonglong(f, m_row->m_info.resolution);
        else
          f->set_null();
        break;
      case 3: /* TIMER_OVERHEAD */
        if (m_row->m_info.routine != 0)
          set_field_ulonglong(f, m_row->m_info.overhead);
        else
          f->set_null();
        break;
      default:
        assert(false);
      }
    }
  }

  return 0;
}

