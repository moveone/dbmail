/*
 Copyright (C) 1999-2003 IC & S  dbmail@ic-s.nl

 This program is free software; you can redistribute it and/or 
 modify it under the terms of the GNU General Public License 
 as published by the Free Software Foundation; either 
 version 2 of the License, or (at your option) any later 
 version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/**
 * \file dbsearch.c
 * \author copyright 2003 IC&S, The Netherlands (http://www.ic-s.nl)
 * \date August 19th, 2003
 * \brief functions implementing searching for messages
 *        the functions in this file used to be located in the 
 *        dbpgsql.c (PostgreSQL) and dbmysql (MySQL), but have
 *        been made backend-independent, so they can be used
 *        by any SQL database.
 */ 

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "dbsearch.h"
#include "db.h"
#include "dbmailtypes.h"
#include "rfcmsg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

/**
 * abbreviated names of the months
 */
const char *month_desc[]= 
{ 
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};


/* used only locally */
/** \brief performs a binary search on an array to find key. Array should
 * be ascending in values.
 * \param array array to be searched through
 * \param arraysize 
 * \param key key to be found in array
 * \return
 *    - -1 if not found
 *    -  index of key in array if found
 */
static int db_binary_search(const u64_t *array, int arraysize, u64_t key);
/**
 * \brief perform search on on the body of a message
 * \param msg mime_message_t struct of message
 * \param sk search key
 * \param msg_idnr
 * \return 
 *     - 0 if no match
 *     - 1 if match
 */
static int db_exec_search(mime_message_t *msg, search_key_t *sk,
		u64_t msg_idnr);

/**
 * \brief search the specified range of a message for a key
 * \param start of range
 * \param end of range
 * \param key key to search for
 * \param msg_idnr 
 * \return 
 *    - 0 if not found
 *    - 1 if found
 */
static int db_search_range(db_pos_t start, db_pos_t end, const char *key,
		u64_t msg_idnr);
/**
 * \brief converts an IMAP date to a number (strictly ascending in date)
 * valid IMAP dates:
 *     - d-mon-yyyy
 *     - dd-mon-yyyy  ('-' may be a space)
 * \param date the IMAP date
 * \return integer representation of the date
 */
static int num_from_imapdate(const char *date);

int db_search(int *rset, int setlen, const char *key, mailbox_t *mb, int type)
{
  u64_t uid;
  int msn;
  unsigned i;

  if (!key)
    return -2;

  memset(rset, 0, setlen * sizeof(int));

  if (type == IST_IDATE) {
       /** \todo this next solution (pms.%s) is really dirty. If anything,
	   the IMAP search algorithm is dirty, and should be fixed */
       snprintf(query, DEF_QUERYSIZE,
		"SELECT msg.message_idnr FROM messages msg, physmessage pms "
		"WHERE msg.mailbox_idnr = '%llu' "
		"AND msg.physmessage_id = pms.id "
		"AND msg.status < 2 "
		"AND msg.unique_id <> '' "
		"AND pms.%s", mb->uid, key);
  } else {
       snprintf(query, DEF_QUERYSIZE, 
		"SELECT message_idnr FROM messages "
		"WHERE mailbox_idnr = '%llu' "
		"AND status<2 AND unique_id!='' AND %s", mb->uid, key);
  }
  if (db_query(query) == -1) {
      trace(TRACE_ERROR, "%s,%s: could not execute query",
			  __FILE__, __FUNCTION__);
      return (-1);
  }

  for (i = 0; i < db_num_rows(); i++) {
	  uid = strtoull(db_get_result(i, 0), NULL, 10);
      msn = db_binary_search(mb->seq_list, mb->exists, uid);

      if (msn == -1 || msn >= setlen) {
		  db_free_result();
		  return 1;
	  }

	  rset[msn] = 1;
  }
	  
  db_free_result();
  return 0;
}

int db_search_parsed(int *rset, unsigned int setlen, 
		     search_key_t *sk, mailbox_t *mb)
{
     unsigned i;
     int result;
     mime_message_t msg;
     
     if (mb->exists != setlen)
	  return 1;
     
     memset(rset, 0, sizeof(int)*setlen);
     
     for (i=0; i<setlen; i++)
     {
	  memset(&msg, 0, sizeof(msg));
	  
	  result = db_fetch_headers(mb->seq_list[i], &msg);
	  if (result != 0)
	       continue; /* ignore parse errors */
	  
	  if (sk->type == IST_SIZE_LARGER)
	  {
	       rset[i] = 
		    ((msg.rfcheadersize + msg.bodylines + msg.bodysize) > sk->size)
		    ? 1 : 0;
	  }
	  else if (sk->type == IST_SIZE_SMALLER)
	  {
	       rset[i] = 
		    ((msg.rfcheadersize + msg.bodylines + msg.bodysize) < sk->size)
		    ? 1 : 0;
	  }
	  else
	  {
	       rset[i] = db_exec_search(&msg, sk, mb->seq_list[i]);
	  }
	  
	  db_free_msg(&msg);
     }
     return 0;
}

int db_binary_search(const u64_t *array, int arraysize, u64_t key)
{
  int low,high,mid;

  low = 0;
  high = arraysize-1;

  while (low <= high)
    {
      mid = (high+low)/2;
      if (array[mid] < key)
	low = mid+1;
      else if (array[mid] > key)
	high = mid-1;
      else
	return mid;
    }

  return -1; /* not found */
}

int db_exec_search(mime_message_t *msg, search_key_t *sk, u64_t msg_idnr)
{
	struct element *el;
	struct mime_record *mr;
	int i,givendate,sentdate;

	if (!sk->search)
		return 0;

	switch (sk->type)
	{
		case IST_HDR:
			if (list_getstart(&msg->mimeheader))
			{
				mime_findfield(sk->hdrfld, &msg->mimeheader, &mr);
				if (mr)
				{
					for (i=0; mr->value[i]; i++)
						if (strncasecmp(&mr->value[i], sk->search,
									strlen(sk->search)) == 0)
							return 1;
				}
			}
			if (list_getstart(&msg->rfcheader))
			{
				mime_findfield(sk->hdrfld, &msg->rfcheader, &mr);
				if (mr)
				{
					for (i=0; mr->value[i]; i++)
						if (strncasecmp(&mr->value[i], sk->search,
									strlen(sk->search)) == 0)
							return 1;
				}
			}

			break;

		case IST_HDRDATE_BEFORE:
		case IST_HDRDATE_ON: 
		case IST_HDRDATE_SINCE:
			/* do not check children */
			if (list_getstart(&msg->rfcheader))
			{
				mime_findfield("date", &msg->rfcheader, &mr);
				if (mr && strlen(mr->value) >= strlen("Day, d mon yyyy "))
					/* 01234567890123456 */     
				{
					givendate = num_from_imapdate(sk->search);

					if (mr->value[6] == ' ')
						mr->value[15] = 0;
					else
						mr->value[16] = 0;

					sentdate = num_from_imapdate(&mr->value[5]);

					switch (sk->type)
					{
						case IST_HDRDATE_BEFORE: return sentdate < givendate;
						case IST_HDRDATE_ON:     return sentdate == givendate;
						case IST_HDRDATE_SINCE:  return sentdate > givendate;
					}
				}
			}
			return 0;

		case IST_DATA_TEXT:
			el = list_getstart(&msg->rfcheader);
			while (el)
			{
				mr = (struct mime_record*)el->data;

				for (i=0; mr->field[i]; i++)
					if (strncasecmp(&mr->field[i], sk->search,
								strlen(sk->search)) == 0)
						return 1;

				for (i=0; mr->value[i]; i++)
					if (strncasecmp(&mr->value[i], sk->search,
								strlen(sk->search)) == 0)
						return 1;

				el = el->nextnode;
			}

			el = list_getstart(&msg->mimeheader);
			while (el)
			{
				mr = (struct mime_record*)el->data;

				for (i=0; mr->field[i]; i++)
					if (strncasecmp(&mr->field[i], sk->search,
								strlen(sk->search)) == 0)
						return 1;

				for (i=0; mr->value[i]; i++)
					if (strncasecmp(&mr->value[i], sk->search,
								strlen(sk->search)) == 0)
						return 1;

				el = el->nextnode;
			}

		case IST_DATA_BODY: 
			/* only check body if there are no children */
			if (list_getstart(&msg->children))
				break;

			/* only check text bodies */
			mime_findfield("content-type", &msg->mimeheader, &mr);
			if (mr && strncasecmp(mr->value, "text", 4) != 0)
				break;

			mime_findfield("content-type", &msg->rfcheader, &mr);
			if (mr && strncasecmp(mr->value, "text", 4) != 0)
				break;

			return db_search_range(msg->bodystart, msg->bodyend, sk->search,
					msg_idnr);
	}  
	/* no match found yet, try the children */
	el = list_getstart(&msg->children);
	while (el)
	{
		if (db_exec_search((mime_message_t*)el->data, sk, msg_idnr) == 1)
			return 1;

		el = el->nextnode;
	}
	return 0;
}

int db_search_range(db_pos_t start, db_pos_t end, 
		const char *key, u64_t msg_idnr)
{
     unsigned i,j;
     unsigned startpos, endpos;
     int distance;

  char *query_result;

  if (start.block > end.block) {
      trace(TRACE_ERROR,"%s,%s: bad range specified",
			  __FILE__, __FUNCTION__);
      return 0;
    }

  if (start.block == end.block && start.pos > end.pos)
    {
      trace(TRACE_ERROR,"%s,%s: bad range specified",
			  __FILE__, __FUNCTION__);
      return 0;
    }

  snprintf(query, DEF_QUERYSIZE,
		  "SELECT messageblk FROM messageblks "
		  "WHERE message_idnr = '%llu' "
	      "ORDER BY messageblk_idnr", 
		  msg_idnr);

  if (db_query(query) == -1) {
      trace(TRACE_ERROR, "%s,%s: could not get message",
			  __FILE__, __FUNCTION__);
      return 0;
  }

  if (db_num_rows() == 0) {
      trace (TRACE_ERROR,"%s,%s: bad range specified",
			  __FILE__, __FUNCTION__);
	  db_free_result();
      return 0;
  }
        
  query_result = db_get_result(start.block, 0);

  if (!query_result) {
      trace(TRACE_ERROR,"%s,%s: bad range specified",
			  __FILE__, __FUNCTION__);
	  db_free_result();
      return 0;
  }

  /* just one block? */
  if (start.block == end.block)
  {
       for (i=start.pos; i <= end.pos - strlen(key); i++)
       {
	  if (strncasecmp(&query_result[i], key, strlen(key)) == 0)
            {
				db_free_result();
	      return 1;
            }
        }

	  db_free_result();
      return 0;
    }


  /* 
   * multiple block range specified
   */

  for (i=start.block; i<=end.block; i++)
    {
      if (!query_result)
        {
	  trace(TRACE_ERROR,"%s,%s: bad range specified",
			  __FILE__, __FUNCTION__);
	  db_free_result();
	  return 0;
        }

      startpos = (i == start.block) ? start.pos : 0;
      endpos   = (i == end.block) ? end.pos+1 : db_get_length(i, 0);

      distance = endpos - startpos;

      for (j=0; j<distance-strlen(key); j++)
        {
	  if (strncasecmp(&query_result[i], key, strlen(key)) == 0)
            {
				db_free_result();
	      return 1;
            }
        }

      query_result = db_get_result(i, 0); /* fetch next row */
    }

  db_free_result();

  return 0;
}

int num_from_imapdate(const char *date)
{
  int j=0,i;
  char datenum[] = "YYYYMMDD";
  char sub[4];

  if (date[1] == ' ' || date[1] == '-')
    j = 1;

  strncpy(datenum, &date[7-j], 4);

  strncpy(sub, &date[3-j], 3);
  sub[3] = 0;

  for (i=0; i<12; i++)
    {
      if (strcasecmp(sub, month_desc[i]) == 0)
	break;
    }

  i++;
  if (i > 12)
    i = 12;

  sprintf(&datenum[4], "%02d", i);

  if (j)
    {
      datenum[6] = '0';
      datenum[7] = date[0];
    }
  else
    {
      datenum[6] = date[0];
      datenum[7] = date[1];
    }

  return atoi(datenum);
}