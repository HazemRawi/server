/* Copyright (C) 2006 MySQL AB & MySQL Finland AB & TCX DataKonsult AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*
  Creates a index for a database by reading keys, sorting them and outputing
  them in sorted order through MARIA_SORT_INFO functions.
*/

#include "ma_fulltext.h"
#include <my_check_opt.h>
#if defined(_WIN32)
#include <fcntl.h>
#else
#include <stddef.h>
#endif
#include <queues.h>

/* static variables */

#undef DISK_BUFFER_SIZE

#define MERGEBUFF 15
#define MERGEBUFF2 31
#define DISK_BUFFER_SIZE (IO_SIZE*128)

/* How many keys we can keep in memory */
typedef ulonglong ha_keys;

/*
 Pointers of functions for store and read keys from temp file
*/

extern void print_error(const char *fmt,...);

/* Functions defined in this file */

static ha_rows find_all_keys(MARIA_SORT_PARAM *info, ha_keys keys,
                             uchar **sort_keys,
                             DYNAMIC_ARRAY *buffpek,size_t *maxbuffer,
                             IO_CACHE *tempfile,
                             IO_CACHE *tempfile_for_exceptions);
static int write_keys(MARIA_SORT_PARAM *info,uchar **sort_keys,
                      ha_keys count, BUFFPEK *buffpek,IO_CACHE *tempfile);
static int write_key(MARIA_SORT_PARAM *info, uchar *key,
                     IO_CACHE *tempfile);
static int write_index(MARIA_SORT_PARAM *info, uchar **sort_keys,
                      ha_keys count);
static int merge_many_buff(MARIA_SORT_PARAM *info, ha_keys keys,
                           uchar **sort_keys,
                           BUFFPEK *buffpek, size_t *maxbuffer,
                           IO_CACHE *t_file);
static my_off_t read_to_buffer(IO_CACHE *fromfile,BUFFPEK *buffpek,
                               uint sort_length);
static int merge_buffers(MARIA_SORT_PARAM *info, ha_keys keys,
                         IO_CACHE *from_file, IO_CACHE *to_file,
                         uchar **sort_keys, BUFFPEK *lastbuff,
                         BUFFPEK *Fb, BUFFPEK *Tb);
static int merge_index(MARIA_SORT_PARAM *,ha_keys,uchar **,BUFFPEK *, size_t,
                       IO_CACHE *);
static int flush_maria_ft_buf(MARIA_SORT_PARAM *info);

static int write_keys_varlen(MARIA_SORT_PARAM *info,uchar **sort_keys,
                             ha_keys count, BUFFPEK *buffpek,
                             IO_CACHE *tempfile);
static my_off_t read_to_buffer_varlen(IO_CACHE *fromfile,BUFFPEK *buffpek,
                                      uint sort_length);
static int write_merge_key(MARIA_SORT_PARAM *info, IO_CACHE *to_file,
                           uchar *key, uint sort_length, ha_keys count);
static int write_merge_key_varlen(MARIA_SORT_PARAM *info,
                                  IO_CACHE *to_file,
                                  uchar* key, uint sort_length,
                                  ha_keys count);
static inline int
my_var_write(MARIA_SORT_PARAM *info, IO_CACHE *to_file, uchar *bufs);

/*
  Sets the appropriate read and write methods for the MARIA_SORT_PARAM
  based on the variable length key flag.
*/
static void set_sort_param_read_write(MARIA_SORT_PARAM *sort_param)
{
  if (sort_param->keyinfo->flag & HA_VAR_LENGTH_KEY)
  {
    sort_param->write_keys=     write_keys_varlen;
    sort_param->read_to_buffer= read_to_buffer_varlen;
    sort_param->write_key=      write_merge_key_varlen;
  }
  else
  {
    sort_param->write_keys=     write_keys;
    sort_param->read_to_buffer= read_to_buffer;
    sort_param->write_key=      write_merge_key;
  }
}


/*
  Creates a index of sorted keys

  SYNOPSIS
    _ma_create_index_by_sort()
    info		Sort parameters
    no_messages		Set to 1 if no output
    sortbuff_size	Size of sortbuffer to allocate

  RESULT
    0	ok
   <> 0 Error
*/

int _ma_create_index_by_sort(MARIA_SORT_PARAM *info, my_bool no_messages,
                             size_t sortbuff_size)
{
  int error;
  uint sort_length;
  size_t memavl, old_memavl, maxbuffer;
  DYNAMIC_ARRAY buffpek;
  ha_rows records, UNINIT_VAR(keys);
  uchar **sort_keys;
  IO_CACHE tempfile, tempfile_for_exceptions;
  DBUG_ENTER("_ma_create_index_by_sort");
  DBUG_PRINT("enter",("sort_buff_size: %lu  sort_length: %d  max_records: %lu",
                      (ulong) sortbuff_size, info->key_length,
                      (ulong) info->sort_info->max_records));

  set_sort_param_read_write(info);

  my_b_clear(&tempfile);
  my_b_clear(&tempfile_for_exceptions);
  bzero((char*) &buffpek,sizeof(buffpek));
  sort_keys= (uchar **) NULL; error= 1;
  maxbuffer=1;

  memavl=MY_MAX(sortbuff_size,MARIA_MIN_SORT_MEMORY);
  records=	info->sort_info->max_records;
  sort_length=	info->key_length;

  while (memavl >= MARIA_MIN_SORT_MEMORY)
  {
    /* Check if we can fit all keys into memory */
    if (((ulonglong) (records + 1) *
         (sort_length + sizeof(char*)) <= memavl))
      keys= records+1;
    else if ((info->sort_info->param->testflag &
              (T_FORCE_SORT_MEMORY | T_CREATE_MISSING_KEYS)) ==
             T_FORCE_SORT_MEMORY)
    {
      /*
        Use all of the given sort buffer for key data.
        Allocate 1000 buffers at a start for new data. More buffers
        will be allocated when needed.
      */
      keys= memavl / (sort_length+sizeof(char*));
      maxbuffer= (size_t) MY_MIN((ulonglong) 1000, (records / keys)+1);
    }
    else
    {
      /*
        All keys can't fit in memory.
        Calculate how many keys + buffers we can keep in memory
      */
      size_t maxbuffer_org;
      do
      {
	maxbuffer_org= maxbuffer;
	if (memavl < sizeof(BUFFPEK) * maxbuffer ||
	    (keys= (memavl-sizeof(BUFFPEK)*maxbuffer)/
             (sort_length+sizeof(char*))) <= 1)
	{
	  _ma_check_print_error(info->sort_info->param,
                                "aria_sort_buffer_size is too small. Current aria_sort_buffer_size: %llu  rows: %llu  sort_length: %u",
                                (ulonglong) sortbuff_size, (ulonglong) records,
                                sort_length);
          my_errno= ENOMEM;
	  goto err;
	}
        if (keys < maxbuffer)
        {
          /*
            There must be sufficient memory for at least one key per BUFFPEK,
            otherwise repair by sort/parallel repair cannot operate.
          */
          maxbuffer= (uint) keys;
          break;
        }
      }
      while ((maxbuffer= (size_t) (records/(keys-1)+1)) != maxbuffer_org);
    }

    if ((sort_keys= ((uchar**)
                     my_malloc(PSI_INSTRUMENT_ME, (size_t) (keys*(sort_length+sizeof(char*))+
                                         HA_FT_MAXBYTELEN),
                               MYF(0)))))
    {
      if (my_init_dynamic_array(PSI_INSTRUMENT_ME, &buffpek, sizeof(BUFFPEK),
                                maxbuffer, MY_MIN(maxbuffer/2, 1000), MYF(0)))
      {
	my_free(sort_keys);
        sort_keys= 0;
      }
      else
	break;
    }
    old_memavl=memavl;
    if ((memavl=memavl/4*3) < MARIA_MIN_SORT_MEMORY && old_memavl > MARIA_MIN_SORT_MEMORY)
      memavl=MARIA_MIN_SORT_MEMORY;
  }
  if (memavl < MARIA_MIN_SORT_MEMORY)
  {
    /* purecov: begin inspected */
    _ma_check_print_error(info->sort_info->param,
                          "aria_sort_buffer_size is too small. Current aria_sort_buffer_size: %llu  rows: %llu  sort_length: %u",
                          (ulonglong) sortbuff_size, (ulonglong) records, sort_length);
    my_errno= ENOMEM;
    goto err;
    /* purecov: end inspected */
  }
  (*info->lock_in_memory)(info->sort_info->param);/* Everything is allocated */

  if (!no_messages)
    my_fprintf(stdout,
               "  - Searching for keys, allocating buffer for %llu keys\n",
               (ulonglong) keys);

  if ((records=find_all_keys(info,keys,sort_keys,&buffpek,&maxbuffer,
                                  &tempfile,&tempfile_for_exceptions))
      == HA_POS_ERROR)
    goto err; /* purecov: tested */

  if (maxbuffer >= keys)
  {
    /*
      merge_many_buff will crash if maxbuffer > keys as then we cannot store in memory
      the keys for each buffer.
    */
    keys= maxbuffer + 1;
    if (!(sort_keys= ((uchar **)
                      my_realloc(PSI_INSTRUMENT_ME, sort_keys,
                                 (size_t) (keys*(sort_length+sizeof(char*))+
                                           HA_FT_MAXBYTELEN), MYF(MY_FREE_ON_ERROR)))))
      goto err;
  }

  info->sort_info->param->stage++;                         /* Merge stage */

  if (maxbuffer == 0)
  {
    if (!no_messages)
      my_fprintf(stdout, "  - Dumping %llu keys\n", (ulonglong) records);
    if (write_index(info, sort_keys, (ha_keys) records))
      goto err; /* purecov: inspected */
  }
  else
  {
    keys=(keys*(sort_length+sizeof(char*)))/sort_length;
    if (maxbuffer >= MERGEBUFF2)
    {
      if (!no_messages)
	my_fprintf(stdout, "  - Merging %llu keys\n",
                   (ulonglong) records); /* purecov: tested */
      if (merge_many_buff(info,keys,sort_keys,
                  dynamic_element(&buffpek,0,BUFFPEK *),&maxbuffer,&tempfile))
	goto err;				/* purecov: inspected */
    }
    if (flush_io_cache(&tempfile) ||
	reinit_io_cache(&tempfile,READ_CACHE,0L,0,0))
      goto err;					/* purecov: inspected */
    if (!no_messages)
      printf("  - Last merge and dumping keys\n"); /* purecov: tested */
    if (merge_index(info,keys,sort_keys,dynamic_element(&buffpek,0,BUFFPEK *),
                    maxbuffer,&tempfile))
    {
      const char *format= "Got error %iE when merging index";
      _ma_check_print_error(info->sort_info->param,
                            format, (int) my_errno);
      goto err;					/* purecov: inspected */
    }
  }

  if (flush_maria_ft_buf(info) || _ma_flush_pending_blocks(info))
    goto err;

  if (my_b_inited(&tempfile_for_exceptions))
  {
    MARIA_HA *idx=info->sort_info->info;
    uint16    key_length;
    MARIA_KEY key;
    key.keyinfo= idx->s->keyinfo + info->key;

    if (!no_messages)
      printf("  - Adding exceptions\n"); /* purecov: tested */
    if (flush_io_cache(&tempfile_for_exceptions) ||
	reinit_io_cache(&tempfile_for_exceptions,READ_CACHE,0L,0,0))
      goto err;

    while (!my_b_read(&tempfile_for_exceptions,(uchar*)&key_length,
		      sizeof(key_length))
        && !my_b_read(&tempfile_for_exceptions,(uchar*)sort_keys,
		      (uint) key_length))
    {
      key.data=       (uchar*) sort_keys;
      key.ref_length= idx->s->rec_reflength;
      key.data_length= key_length - key.ref_length;
      key.flag= 0;
      if (_ma_ck_write(idx, &key))
        goto err;
    }
  }

  error =0;

err:
  my_free(sort_keys);
  delete_dynamic(&buffpek);
  close_cached_file(&tempfile);
  close_cached_file(&tempfile_for_exceptions);

  DBUG_RETURN(error ? -1 : 0);
} /* _ma_create_index_by_sort */


/* Search after all keys and place them in a temp. file */

static ha_rows find_all_keys(MARIA_SORT_PARAM *info, ha_rows keys,
                             uchar **sort_keys, DYNAMIC_ARRAY *buffpek,
                             size_t *maxbuffer, IO_CACHE *tempfile,
                             IO_CACHE *tempfile_for_exceptions)
{
  int error;
  ha_rows idx;
  DBUG_ENTER("find_all_keys");

  idx=error=0;
  sort_keys[0]= (uchar*) (sort_keys+keys);

  info->sort_info->info->in_check_table= 1;
  while (!(error=(*info->key_read)(info,sort_keys[idx])))
  {
    if (info->real_key_length > info->key_length)
    {
      if (write_key(info,sort_keys[idx],tempfile_for_exceptions))
        goto err;                             /* purecov: inspected */
      continue;
    }

    if (++idx == keys)
    {
      if (info->write_keys(info,sort_keys,idx-1,
                           (BUFFPEK *)alloc_dynamic(buffpek),
                           tempfile))
        goto err;                             /* purecov: inspected */

      sort_keys[0]=(uchar*) (sort_keys+keys);
      memcpy(sort_keys[0],sort_keys[idx-1],(size_t) info->key_length);
      idx=1;
    }
    sort_keys[idx]=sort_keys[idx-1]+info->key_length;
  }
  if (error > 0)
    goto err;                             /* purecov: inspected */
  if (buffpek->elements)
  {
    if (info->write_keys(info,sort_keys,idx,(BUFFPEK *)alloc_dynamic(buffpek),
                         tempfile))
      goto err;                         /* purecov: inspected */      
    *maxbuffer=buffpek->elements-1;
  }
  else
    *maxbuffer=0;

  info->sort_info->info->in_check_table= 0;
  DBUG_RETURN((*maxbuffer)*(keys-1)+idx);

err:
  info->sort_info->info->in_check_table= 0;   /* purecov: inspected */
  DBUG_RETURN(HA_POS_ERROR);                  /* purecov: inspected */
} /* find_all_keys */


static my_bool _ma_thr_find_all_keys_exec(MARIA_SORT_PARAM* sort_param)
{
  int error= 0;
  ulonglong memavl, old_memavl;
  longlong sortbuff_size;
  ha_keys UNINIT_VAR(keys), idx;
  uint sort_length;
  size_t maxbuffer;
  uchar **sort_keys= NULL;
  DBUG_ENTER("_ma_thr_find_all_keys_exec");
  DBUG_PRINT("enter", ("master: %d", sort_param->master));

  if (sort_param->sort_info->got_error)
    DBUG_RETURN(TRUE);

  set_sort_param_read_write(sort_param);

  my_b_clear(&sort_param->tempfile);
  my_b_clear(&sort_param->tempfile_for_exceptions);
  bzero((char*) &sort_param->buffpek, sizeof(sort_param->buffpek));
  bzero((char*) &sort_param->unique, sizeof(sort_param->unique));

  sortbuff_size= sort_param->sortbuff_size;
  memavl=       MY_MAX(sortbuff_size, MARIA_MIN_SORT_MEMORY);
  idx=          (ha_keys) sort_param->sort_info->max_records;
  sort_length=  sort_param->key_length;
  maxbuffer=    1;

  while (memavl >= MARIA_MIN_SORT_MEMORY)
  {
    if ((my_off_t) (idx+1)*(sort_length+sizeof(char*)) <= (my_off_t) memavl)
      keys= idx+1;
    else if ((sort_param->sort_info->param->testflag &
              (T_FORCE_SORT_MEMORY | T_CREATE_MISSING_KEYS)) ==
             T_FORCE_SORT_MEMORY)
    {
      /*
        Use all of the given sort buffer for key data.
        Allocate 1000 buffers at a start for new data. More buffers
        will be allocated when needed.
      */
      keys= memavl / (sort_length+sizeof(char*));
      maxbuffer= (size_t) MY_MIN((ulonglong) 1000, (idx / keys)+1);
    }
    else
    {
      size_t maxbuffer_org;
      do
      {
        maxbuffer_org= maxbuffer;
        if (memavl < sizeof(BUFFPEK)*maxbuffer ||
            (keys=(memavl-sizeof(BUFFPEK)*maxbuffer)/
             (sort_length+sizeof(char*))) <= 1 ||
            keys < maxbuffer)
        {
          _ma_check_print_error(sort_param->sort_info->param,
                                "aria_sort_buffer_size is too small. Current aria_sort_buffer_size: %llu  rows: %llu  sort_length: %u",
                                sortbuff_size, (ulonglong) idx, sort_length);
          goto err;
        }
      }
      while ((maxbuffer= (uint) (idx/(keys-1)+1)) != maxbuffer_org);
    }
    if ((sort_keys= (uchar **)
         my_malloc(PSI_INSTRUMENT_ME, (size_t)(keys*(sort_length+sizeof(char*))+
                   (sort_param->keyinfo->key_alg == HA_KEY_ALG_FULLTEXT ?
                    HA_FT_MAXBYTELEN : 0)), MYF(0))))
    {
      if (my_init_dynamic_array(PSI_INSTRUMENT_ME, &sort_param->buffpek, sizeof(BUFFPEK),
                             maxbuffer, MY_MIN(maxbuffer / 2, 1000), MYF(0)))
      {
        my_free(sort_keys);
        sort_keys= NULL;          /* Safety against double free on error. */
      }
      else
        break;
    }
    old_memavl= memavl;
    if ((memavl= memavl/4*3) < MARIA_MIN_SORT_MEMORY &&
        old_memavl > MARIA_MIN_SORT_MEMORY)
      memavl= MARIA_MIN_SORT_MEMORY;
  }
  if (memavl < MARIA_MIN_SORT_MEMORY)
  {
    /* purecov: begin inspected */
    _ma_check_print_error(sort_param->sort_info->param,
                          "aria_sort_buffer_size is too small. Current aria_sort_buffer_size: %llu  rows: %llu  sort_length: %u",
                          sortbuff_size, (ulonglong) idx, sort_length);
    my_errno= ENOMEM;
    goto err;
  /* purecov: end inspected */
  }

  if (sort_param->sort_info->param->testflag & T_VERBOSE)
    my_fprintf(stdout,
               "Key %d - Allocating buffer for %llu keys\n",
               sort_param->key + 1, (ulonglong) keys);
  sort_param->sort_keys= sort_keys;

  idx= error= 0;
  sort_keys[0]= (uchar*) (sort_keys+keys);

  DBUG_PRINT("info", ("reading keys"));
  while (!(error= sort_param->sort_info->got_error) &&
         !(error= (*sort_param->key_read)(sort_param, sort_keys[idx])))
  {
    if (sort_param->real_key_length > sort_param->key_length)
    {
      if (write_key(sort_param, sort_keys[idx],
                    &sort_param->tempfile_for_exceptions))
        goto err;
      continue;
    }

    if (++idx == keys)
    {
      if (sort_param->write_keys(sort_param, sort_keys, idx - 1,
                                 (BUFFPEK *)alloc_dynamic(&sort_param->buffpek),
                                 &sort_param->tempfile))
        goto err;
      sort_keys[0]= (uchar*) (sort_keys+keys);
      memcpy(sort_keys[0], sort_keys[idx - 1], (size_t) sort_param->key_length);
      idx= 1;
    }
    sort_keys[idx]= sort_keys[idx - 1] + sort_param->key_length;
  }
  if (error > 0)
    goto err;
  if (sort_param->buffpek.elements)
  {
    if (sort_param->write_keys(sort_param,sort_keys, idx,
                               (BUFFPEK *) alloc_dynamic(&sort_param->buffpek),
                               &sort_param->tempfile))
      goto err;
    sort_param->keys= (uint)((sort_param->buffpek.elements - 1) * (keys - 1) + idx);
  }
  else
    sort_param->keys= (uint)idx;

  DBUG_RETURN(FALSE);

err:
  DBUG_PRINT("error", ("got some error"));
  my_free(sort_keys);
  sort_param->sort_keys= 0;
  delete_dynamic(& sort_param->buffpek);
  close_cached_file(&sort_param->tempfile);
  close_cached_file(&sort_param->tempfile_for_exceptions);

  DBUG_RETURN(TRUE);
}

/* Search after all keys and place them in a temp. file */

pthread_handler_t _ma_thr_find_all_keys(void *arg)
{
  MARIA_SORT_PARAM *sort_param= (MARIA_SORT_PARAM*) arg;
  my_bool error= FALSE;
  /* If my_thread_init fails */
  if (my_thread_init())
    error= TRUE;
  else
  {
    HA_CHECK *check= sort_param->check_param;
    if (check->init_repair_thread)
      check->init_repair_thread(check->init_repair_thread_arg);
    if (_ma_thr_find_all_keys_exec(sort_param))
      error= TRUE;
  }

  /*
     Thread must clean up after itself.
  */
  free_root(&sort_param->wordroot, MYF(0));
  /*
    Detach from the share if the writer is involved. Avoid others to
    be blocked. This includes a flush of the write buffer. This will
    also indicate EOF to the readers.
    That means that a writer always gets here first and readers -
    only when they see EOF. But if a reader finishes prematurely
    because of an error it may reach this earlier - don't allow it
    to detach the writer thread.
  */
  if (sort_param->master && sort_param->sort_info->info->rec_cache.share)
    remove_io_thread(&sort_param->sort_info->info->rec_cache);

  /* Readers detach from the share if any. Avoid others to be blocked. */
  if (sort_param->read_cache.share)
    remove_io_thread(&sort_param->read_cache);

  mysql_mutex_lock(&sort_param->sort_info->mutex);
  if (error)
    sort_param->sort_info->got_error= 1;

  if (!--sort_param->sort_info->threads_running)
    mysql_cond_signal(&sort_param->sort_info->cond);
  mysql_mutex_unlock(&sort_param->sort_info->mutex);

  my_thread_end();
  return NULL;
}


int _ma_thr_write_keys(MARIA_SORT_PARAM *sort_param)
{
  MARIA_SORT_INFO *sort_info=sort_param->sort_info;
  HA_CHECK *param=sort_info->param;
  size_t UNINIT_VAR(length), keys;
  double *rec_per_key_part= param->new_rec_per_key_part;
  int got_error=sort_info->got_error;
  uint i;
  MARIA_HA *info=sort_info->info;
  MARIA_SHARE *share= info->s;
  MARIA_SORT_PARAM *sinfo;
  uchar *mergebuf=0;
  DBUG_ENTER("_ma_thr_write_keys");

  for (i= 0, sinfo= sort_param ;
       i < sort_info->total_keys ;
       i++, sinfo++)
  {
    if (!sinfo->sort_keys)
    {
      got_error=1;
      my_free(sinfo->rec_buff);
      continue;
    }
    if (!got_error)
    {
      maria_set_key_active(share->state.key_map, sinfo->key);

      if (!sinfo->buffpek.elements)
      {
        if (param->testflag & T_VERBOSE)
        {
          my_fprintf(stdout,
                     "Key %d  - Dumping %llu keys\n", sinfo->key+1,
                     (ulonglong) sinfo->keys);
          fflush(stdout);
        }
        if (write_index(sinfo, sinfo->sort_keys, sinfo->keys) ||
            flush_maria_ft_buf(sinfo) || _ma_flush_pending_blocks(sinfo))
          got_error=1;
      }
    }
    my_free(sinfo->sort_keys);
    my_free(sinfo->rec_buff);
    sinfo->sort_keys=0;
  }

  for (i= 0, sinfo= sort_param ;
       i < sort_info->total_keys ;
       i++,
	 delete_dynamic(&sinfo->buffpek),
	 close_cached_file(&sinfo->tempfile),
	 close_cached_file(&sinfo->tempfile_for_exceptions),
         rec_per_key_part+= sinfo->keyinfo->keysegs,
	 sinfo++)
  {
    if (got_error)
      continue;

    set_sort_param_read_write(sinfo);

    if (sinfo->buffpek.elements)
    {
      size_t maxbuffer=sinfo->buffpek.elements-1;
      if (!mergebuf)
      {
        length=(size_t)param->sort_buffer_length;
        while (length >= MARIA_MIN_SORT_MEMORY)
        {
          if ((mergebuf= my_malloc(PSI_INSTRUMENT_ME, (size_t) length, MYF(0))))
              break;
          length=length*3/4;
        }
        if (!mergebuf)
        {
          got_error=1;
          continue;
        }
      }
      keys=length/sinfo->key_length;
      if (maxbuffer >= MERGEBUFF2)
      {
        if (param->testflag & T_VERBOSE)
          my_fprintf(stdout,
                     "Key %d  - Merging %llu keys\n",
                     sinfo->key+1, (ulonglong) sinfo->keys);
        if (merge_many_buff(sinfo, keys, (uchar **)mergebuf,
			    dynamic_element(&sinfo->buffpek, 0, BUFFPEK *),
			    &maxbuffer, &sinfo->tempfile))
        {
          got_error=1;
          continue;
        }
      }
      if (flush_io_cache(&sinfo->tempfile) ||
          reinit_io_cache(&sinfo->tempfile,READ_CACHE,0L,0,0))
      {
        got_error=1;
        continue;
      }
      if (param->testflag & T_VERBOSE)
        printf("Key %d  - Last merge and dumping keys\n", sinfo->key+1);
      if (merge_index(sinfo, keys, (uchar**) mergebuf,
                      dynamic_element(&sinfo->buffpek,0,BUFFPEK *),
                      maxbuffer,&sinfo->tempfile) ||
          flush_maria_ft_buf(sinfo) ||
	  _ma_flush_pending_blocks(sinfo))
      {
        got_error=1;
        continue;
      }
    }
    if (my_b_inited(&sinfo->tempfile_for_exceptions))
    {
      uint16 key_length;

      if (param->testflag & T_VERBOSE)
        printf("Key %d  - Dumping 'long' keys\n", sinfo->key+1);

      if (flush_io_cache(&sinfo->tempfile_for_exceptions) ||
          reinit_io_cache(&sinfo->tempfile_for_exceptions,READ_CACHE,0L,0,0))
      {
        got_error=1;
        continue;
      }

      while (!got_error &&
	     !my_b_read(&sinfo->tempfile_for_exceptions,(uchar*)&key_length,
			sizeof(key_length)))
      {
        uchar maria_ft_buf[HA_FT_MAXBYTELEN + HA_FT_WLEN + 10];
        if (key_length > sizeof(maria_ft_buf) ||
            my_b_read(&sinfo->tempfile_for_exceptions, (uchar*)maria_ft_buf,
                      (uint) key_length))
          got_error= 1;
        else
        {
          MARIA_KEY tmp_key;
          tmp_key.keyinfo= info->s->keyinfo + sinfo->key;
          tmp_key.data= maria_ft_buf;
          tmp_key.ref_length= info->s->rec_reflength;
          tmp_key.data_length= key_length - info->s->rec_reflength;
          tmp_key.flag= 0;
          if (_ma_ck_write(info, &tmp_key))
            got_error=1;
        }
      }
    }
    if (!got_error && (param->testflag & T_STATISTICS))
      maria_update_key_parts(sinfo->keyinfo, rec_per_key_part, sinfo->unique,
                             param->stats_method ==
                             MI_STATS_METHOD_IGNORE_NULLS ?
                             sinfo->notnull : NULL,
                             (ulonglong) share->state.state.records);

  }
  my_free(mergebuf);
  DBUG_RETURN(got_error);
}


/* Write all keys in memory to file for later merge */

static int write_keys(MARIA_SORT_PARAM *info, register uchar **sort_keys,
                      ha_keys count, BUFFPEK *buffpek, IO_CACHE *tempfile)
{
  uchar **end;
  uint sort_length=info->key_length;
  DBUG_ENTER("write_keys");

  if (!buffpek)
    DBUG_RETURN(1);                             /* Out of memory */

  my_qsort2(sort_keys, count, sizeof(uchar*),
            info->key_cmp, info);
  if (!my_b_inited(tempfile) &&
      open_cached_file(tempfile, my_tmpdir(info->tmpdir), "ST",
                       DISK_BUFFER_SIZE, info->sort_info->param->myf_rw))
    DBUG_RETURN(1); /* purecov: inspected */

  buffpek->file_pos=my_b_tell(tempfile);
  buffpek->count=count;

  for (end=sort_keys+count ; sort_keys != end ; sort_keys++)
  {
    if (my_b_write(tempfile, *sort_keys, sort_length))
      DBUG_RETURN(1); /* purecov: inspected */
  }
  DBUG_RETURN(0);
} /* write_keys */


static inline int
my_var_write(MARIA_SORT_PARAM *info, IO_CACHE *to_file, uchar *bufs)
{
  int err;
  uint16 len= _ma_keylength(info->keyinfo, bufs);

  /* The following is safe as this is a local file */
  if ((err= my_b_write(to_file, (uchar*)&len, sizeof(len))))
    return (err);
  if ((err= my_b_write(to_file,bufs, (uint) len)))
    return (err);
  return (0);
}


static int write_keys_varlen(MARIA_SORT_PARAM *info,
                             register uchar **sort_keys,
                             ha_keys count, BUFFPEK *buffpek,
                             IO_CACHE *tempfile)
{
  uchar **end;
  int err;
  DBUG_ENTER("write_keys_varlen");

  if (!buffpek)
    DBUG_RETURN(1);                             /* Out of memory */

  my_qsort2(sort_keys, count, sizeof(uchar*),
            info->key_cmp, info);
  if (!my_b_inited(tempfile) &&
      open_cached_file(tempfile, my_tmpdir(info->tmpdir), "ST",
                       DISK_BUFFER_SIZE, info->sort_info->param->myf_rw))
    DBUG_RETURN(1); /* purecov: inspected */

  buffpek->file_pos=my_b_tell(tempfile);
  buffpek->count=count;
  for (end=sort_keys+count ; sort_keys != end ; sort_keys++)
  {
    if ((err= my_var_write(info,tempfile, *sort_keys)))
      DBUG_RETURN(err);
  }
  DBUG_RETURN(0);
} /* write_keys_varlen */


static int write_key(MARIA_SORT_PARAM *info, uchar *key,
			    IO_CACHE *tempfile)
{
  uint16 key_length=info->real_key_length;
  DBUG_ENTER("write_key");

  if (!my_b_inited(tempfile) &&
      open_cached_file(tempfile, my_tmpdir(info->tmpdir), "ST",
                       DISK_BUFFER_SIZE, info->sort_info->param->myf_rw))
    DBUG_RETURN(1);

  if (my_b_write(tempfile, (uchar*)&key_length,sizeof(key_length)) ||
      my_b_write(tempfile, key, (uint) key_length))
    DBUG_RETURN(1);
  DBUG_RETURN(0);
} /* write_key */


/* Write index */

static int write_index(MARIA_SORT_PARAM *info, register uchar **sort_keys,
                       register ha_keys count)
{
  DBUG_ENTER("write_index");

  my_qsort2(sort_keys, count,sizeof(uchar*),
            info->key_cmp,info);
  while (count--)
  {
    if ((*info->key_write)(info, *sort_keys++))
      DBUG_RETURN(-1); /* purecov: inspected */
  }
  if (info->sort_info->param->max_stage != 1)          /* If not parallel */
    _ma_report_progress(info->sort_info->param, 1, 1);
  DBUG_RETURN(0);
} /* write_index */


        /* Merge buffers to make < MERGEBUFF2 buffers */

static int merge_many_buff(MARIA_SORT_PARAM *info, ha_keys keys,
                           uchar **sort_keys, BUFFPEK *buffpek,
                           size_t *maxbuffer, IO_CACHE *t_file)
{
  size_t tmp, merges, max_merges;
  IO_CACHE t_file2, *from_file, *to_file, *temp;
  BUFFPEK *lastbuff;
  DBUG_ENTER("merge_many_buff");

  if (*maxbuffer < MERGEBUFF2)
    DBUG_RETURN(0);                             /* purecov: inspected */
  if (flush_io_cache(t_file) ||
      open_cached_file(&t_file2,my_tmpdir(info->tmpdir),"ST",
                       DISK_BUFFER_SIZE, info->sort_info->param->myf_rw))
    DBUG_RETURN(1);                             /* purecov: inspected */

  /* Calculate how many merges are needed */
  max_merges= 1;                                /* Count merge_index */
  tmp= *maxbuffer;
  while (tmp >= MERGEBUFF2)
  {
    merges= (tmp-MERGEBUFF*3/2 + 1) / MERGEBUFF + 1;
    max_merges+= merges;
    tmp= merges;
  }
  merges= 0;

  from_file= t_file ; to_file= &t_file2;
  while (*maxbuffer >= MERGEBUFF2)
  {
    size_t i;
    reinit_io_cache(from_file,READ_CACHE,0L,0,0);
    reinit_io_cache(to_file,WRITE_CACHE,0L,0,0);
    lastbuff=buffpek;
    for (i=0 ; i + MERGEBUFF*3/2 <= *maxbuffer ; i+=MERGEBUFF)
    {
      if (merge_buffers(info,keys,from_file,to_file,sort_keys,lastbuff++,
                        buffpek+i,buffpek+i+MERGEBUFF-1))
        goto cleanup;
      if (info->sort_info->param->max_stage != 1) /* If not parallel */
        _ma_report_progress(info->sort_info->param, merges++, max_merges);
    }
    if (merge_buffers(info,keys,from_file,to_file,sort_keys,lastbuff++,
                      buffpek+i,buffpek+ *maxbuffer))
      break; /* purecov: inspected */
    if (flush_io_cache(to_file))
      break;                                    /* purecov: inspected */
    temp=from_file; from_file=to_file; to_file=temp;
    *maxbuffer= (size_t) (lastbuff-buffpek)-1;
    if (info->sort_info->param->max_stage != 1) /* If not parallel */
      _ma_report_progress(info->sort_info->param, merges++, max_merges);
  }
cleanup:
  close_cached_file(to_file);                   /* This holds old result */
  if (to_file == t_file)
  {
    DBUG_ASSERT(t_file2.type == WRITE_CACHE);
    *t_file=t_file2;                            /* Copy result file */
  }

  DBUG_RETURN(*maxbuffer >= MERGEBUFF2);        /* Return 1 if interrupted */
} /* merge_many_buff */


/*
   Read data to buffer

  SYNOPSIS
    read_to_buffer()
    fromfile		File to read from
    buffpek		Where to read from
    sort_length		max length to read
  RESULT
    > 0	Amount of bytes read
    -1	Error
*/

static my_off_t read_to_buffer(IO_CACHE *fromfile, BUFFPEK *buffpek,
                                uint sort_length)
{
  register ha_keys count;
  size_t length;

  if ((count= (ha_keys) MY_MIN((ha_rows) buffpek->max_keys,
                               (ha_rows) buffpek->count)))
  {
    if (my_b_pread(fromfile, (uchar*) buffpek->base,
                   (length= sort_length * (size_t)count), buffpek->file_pos))
      return(HA_OFFSET_ERROR);               /* purecov: inspected */
    buffpek->key=buffpek->base;
    buffpek->file_pos+= length;                 /* New filepos */
    buffpek->count-=    count;
    buffpek->mem_count= count;
  }
  return (((my_off_t) count) * sort_length);
} /* read_to_buffer */


static my_off_t read_to_buffer_varlen(IO_CACHE *fromfile, BUFFPEK *buffpek,
                                      uint sort_length)
{
  register ha_keys count;
  uint idx;
  uchar *buffp;

  if ((count= (ha_keys) MY_MIN((ha_rows) buffpek->max_keys,buffpek->count)))
  {
    buffp= buffpek->base;

    for (idx=1;idx<=count;idx++)
    {
      uint16 length_of_key;
      if (my_b_pread(fromfile, (uchar*)&length_of_key,
                     sizeof(length_of_key), buffpek->file_pos))
        return(HA_OFFSET_ERROR);
      buffpek->file_pos+=sizeof(length_of_key);
      if (my_b_pread(fromfile, (uchar*) buffp,
                     length_of_key, buffpek->file_pos))
        return((uint) -1);
      buffpek->file_pos+=length_of_key;
      buffp = buffp + sort_length;
    }
    buffpek->key=buffpek->base;
    buffpek->count-=    count;
    buffpek->mem_count= count;
  }
  return (((my_off_t) count) * sort_length);
} /* read_to_buffer_varlen */


static int write_merge_key_varlen(MARIA_SORT_PARAM *info,
                                  IO_CACHE *to_file, uchar* key,
                                  uint sort_length, ha_keys count)
{
  ha_keys idx;
  uchar *bufs = key;

  for (idx=1;idx<=count;idx++)
  {
    int err;
    if ((err= my_var_write(info, to_file, bufs)))
      return (err);
    bufs=bufs+sort_length;
  }
  return(0);
}


static int write_merge_key(MARIA_SORT_PARAM *info __attribute__((unused)),
                           IO_CACHE *to_file, uchar *key,
                           uint sort_length, ha_keys count)
{
  return my_b_write(to_file, key, (size_t) (sort_length * count));
}

/*
  Merge buffers to one buffer
  If to_file == 0 then use info->key_write

  Return:
  0 ok
  1 error
*/

static int
merge_buffers(MARIA_SORT_PARAM *info, ha_keys keys, IO_CACHE *from_file,
              IO_CACHE *to_file, uchar **sort_keys, BUFFPEK *lastbuff,
              BUFFPEK *Fb, BUFFPEK *Tb)
{
  int error= 1;
  uint sort_length;
  ha_keys maxcount;
  ha_rows count;
  my_off_t UNINIT_VAR(to_start_filepos), read_length;
  uchar *strpos;
  BUFFPEK *buffpek,**refpek;
  QUEUE queue;
  DBUG_ENTER("merge_buffers");

  count= 0;
  maxcount= keys/((uint) (Tb-Fb) +1);
  DBUG_ASSERT(maxcount > 0);
  if (to_file)
    to_start_filepos=my_b_tell(to_file);
  strpos= (uchar*) sort_keys;
  sort_length=info->key_length;

  if (init_queue(&queue,(uint) (Tb-Fb)+1,offsetof(BUFFPEK,key),0,
                 info->key_cmp,
                 info, 0, 0))
    DBUG_RETURN(1); /* purecov: inspected */

  for (buffpek= Fb ; buffpek <= Tb ; buffpek++)
  {
    count+= buffpek->count;
    buffpek->base= strpos;
    buffpek->max_keys= maxcount;
    strpos+= (read_length= info->read_to_buffer(from_file,buffpek,
                                                sort_length));
    if (read_length == HA_OFFSET_ERROR)
      goto err; /* purecov: inspected */
    queue_insert(&queue,(uchar*) buffpek);
  }

  while (queue.elements > 1)
  {
    for (;;)
    {
      buffpek=(BUFFPEK*) queue_top(&queue);
      if (to_file)
      {
        if (info->write_key(info,to_file, buffpek->key,
                            sort_length, 1))
          goto err;                           /* purecov: inspected */
      }
      else
      {
        if ((*info->key_write)(info,(void*) buffpek->key))
          goto err;                           /* purecov: inspected */
      }
      buffpek->key+=sort_length;
      if (! --buffpek->mem_count)
      {
        /* It's enough to check for killedptr before a slow operation */
        if (_ma_killed_ptr(info->sort_info->param))
          goto err;
        if (!(read_length= info->read_to_buffer(from_file,buffpek,sort_length)))
        {
          uchar *base= buffpek->base;
          ha_keys max_keys=buffpek->max_keys;

          queue_remove_top(&queue);

          /* Put room used by buffer to use in other buffer */
          for (refpek= (BUFFPEK**) &queue_top(&queue);
               refpek <= (BUFFPEK**) &queue_end(&queue);
               refpek++)
          {
            buffpek= *refpek;
            if (buffpek->base+buffpek->max_keys*sort_length == base)
            {
              buffpek->max_keys+=max_keys;
              break;
            }
            else if (base+max_keys*sort_length == buffpek->base)
            {
              buffpek->base=base;
              buffpek->max_keys+=max_keys;
              break;
            }
          }
          break;                /* One buffer have been removed */
        }
        else if (read_length == HA_OFFSET_ERROR)
          goto err;               /* purecov: inspected */
      }
      queue_replace_top(&queue);   /* Top element has been replaced */
    }
  }
  buffpek=(BUFFPEK*) queue_top(&queue);
  buffpek->base= (uchar*) sort_keys;
  buffpek->max_keys=keys;
  do
  {
    if (to_file)
    {
      if (info->write_key(info, to_file, buffpek->key,
                         sort_length,buffpek->mem_count))
      {
        error=1; goto err; /* purecov: inspected */
      }
    }
    else
    {
      register uchar *end;
      strpos= buffpek->key;
      for (end= strpos+buffpek->mem_count*sort_length;
           strpos != end ;
           strpos+=sort_length)
      {
        if ((*info->key_write)(info, strpos))
        {
          error=1; goto err; /* purecov: inspected */
        }
      }
    }
  }
  while ((read_length= info->read_to_buffer(from_file,buffpek,sort_length)) != HA_OFFSET_ERROR && read_length != 0);
  if (read_length == 0)
    error= 0;

  lastbuff->count=count;
  if (to_file)
    lastbuff->file_pos=to_start_filepos;
err:
  delete_queue(&queue);
  DBUG_RETURN(error);
} /* merge_buffers */


        /* Do a merge to output-file (save only positions) */

static int
merge_index(MARIA_SORT_PARAM *info, ha_keys keys, uchar **sort_keys,
            BUFFPEK *buffpek, size_t maxbuffer, IO_CACHE *tempfile)
{
  DBUG_ENTER("merge_index");
  if (merge_buffers(info,keys,tempfile,(IO_CACHE*) 0,sort_keys,buffpek,buffpek,
                    buffpek+maxbuffer))
    DBUG_RETURN(1); /* purecov: inspected */
  if (info->sort_info->param->max_stage != 1)          /* If not parallel */
    _ma_report_progress(info->sort_info->param, 1, 1);
  DBUG_RETURN(0);
} /* merge_index */


static int flush_maria_ft_buf(MARIA_SORT_PARAM *info)
{
  int err=0;
  if (info->sort_info->ft_buf)
  {
    err=_ma_sort_ft_buf_flush(info);
    my_free(info->sort_info->ft_buf);
    info->sort_info->ft_buf=0;
  }
  return err;
}
