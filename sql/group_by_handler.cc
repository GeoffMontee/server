/*
   Copyright (c) 2014, SkySQL Ab & MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/*
  This file implements the group_by_handler code. This interface
  can be used by storage handlers that can intercept summary or GROUP
  BY queries from MariaDB and itself return the result to the user or
  upper level.
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "sql_priv.h"
#include "sql_select.h"

/*
  Same return values as do_select();

  @retval
    0  if ok
  @retval
    1  if error is sent
  @retval
    -1  if error should be sent
*/

int group_by_handler::execute(JOIN *join)
{
  int err;
  ha_rows max_limit;
  ha_rows *reset_limit= 0;
  Item **reset_item= 0;
  DBUG_ENTER("group_by_handler");

  if ((err= init_scan()))
    goto error;

  if (store_data_in_temp_table)
  {
    max_limit= join->tmp_table_param.end_write_records;
    reset_limit= &join->unit->select_limit_cnt;
  }
  else
  {
    max_limit= join->unit->select_limit_cnt;
    if (join->unit->fake_select_lex)
      reset_item= &join->unit->fake_select_lex->select_limit;
  }    

  while (!(err= next_row()))
  {
    if (thd->check_killed())
    {
      thd->send_kill_message();
      (void) end_scan();
      DBUG_RETURN(-1);
    }

    /* Check if we can accept the row */
    if (!having || having->val_bool())
    {
      if (store_data_in_temp_table)
      {
        if ((err= table->file->ha_write_tmp_row(table->record[0])))
        {
          bool is_duplicate;
          if (!table->file->is_fatal_error(err, HA_CHECK_DUP))
            continue;                           // Distinct elimination
          
          if (create_internal_tmp_table_from_heap(thd, table,
                                                  join->tmp_table_param.
                                                  start_recinfo,
                                                  &join->tmp_table_param.
                                                  recinfo,
                                                  err, 1, &is_duplicate))
            DBUG_RETURN(1);
          if (is_duplicate)
            continue;
        }
      }
      else
      {
        if (join->do_send_rows)
        {
          int error;
          /* result < 0 if row was not accepted and should not be counted */
          if ((error= join->result->send_data(*join->fields)))
          {
            (void) end_scan();
            DBUG_RETURN(error < 0 ? 0 : -1);
          }
        }
      }

      /* limit handling */
      if (++join->send_records >= max_limit && join->do_send_rows)
      {
        if (!(join->select_options & OPTION_FOUND_ROWS))
          break;                              // LIMIT reached
        join->do_send_rows= 0;                // Calculate FOUND_ROWS()
        if (reset_limit)
          *reset_limit= HA_POS_ERROR;
        if (reset_item)
          *reset_item= 0;
      }
    }
  }
  if (err != 0 && err != HA_ERR_END_OF_FILE)
    goto error;

  if ((err= end_scan()))
    goto error_2;
  if (!store_data_in_temp_table && join->result->send_eof())
    DBUG_RETURN(1);                              // Don't send error to client

  DBUG_RETURN(0);

error:
  (void) end_scan();
error_2:
  print_error(err, MYF(0));
  DBUG_RETURN(-1);                              // Error not sent to client
}


void group_by_handler::print_error(int error, myf errflag)
{
  my_error(ER_GET_ERRNO, MYF(0), error, hton_name(ht)->str);
}
