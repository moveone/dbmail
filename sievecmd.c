/*
 Copyright (C) 2003 Aaron Stone

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

/* $Id$
 * This is dbmail-sievecmd, which provides
 * a command line interface to the sievescripts */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sievecmd.h"

#include "auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dbmail.h"
#include "list.h"
#include "debug.h"
#include "db.h"
#include <time.h>
#include <stdarg.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif
#include "dbmd5.h"

char *configFile = DEFAULT_CONFIG_FILE;

/* set up database login data */
extern db_param_t _db_params;

int main(int argc, char *argv[])
{
  struct list sysItems;
  int res = 0, opt = 0, act = 0;
  u64_t user_idnr = 0;
  char *user_name = NULL;
  char *name = NULL;
  FILE *source = NULL;
  extern char *optarg;

  openlog(PNAME, LOG_PID, LOG_MAIL);
	
  setvbuf(stdout, 0, _IONBF, 0);
  
  ReadConfig("DBMAIL", configFile, &sysItems);
  SetTraceLevel(&sysItems);
  GetDBParams(&_db_params, &sysItems);

  configure_debug(TRACE_ERROR, 1, 0);

  while (opt != -1 && act != 'h')
    {
      opt = getopt(argc, argv, "a:d:i:r:u:l");

      switch (opt)
        {
          case -1:
            /* Break right away if this is the end of the args */
            break;
          case 'a':
          case 'd':
          case 'i':
          case 'r':
            if (act != 0)
              act = 'h';
	    else
              act = opt;
            name = optarg;
            source = stdin; // FIXME to take files as input, too
            break;
          case 'u':
	    user_name = strdup(optarg);
            break;
          case 'l':
            if (act != 0)
              act = 'h';
	    else
              act = opt;
            break;
          default:
            act = 'h';
            break;
        }
    }

  if (act != 'h' && act != 0)
    {
      printf ("*** dbmail-sievecmd ***\n");
            
      /* Open database connection */
      printf ("Opening connection to database...\n");
      if (db_connect() != 0)
        {
          printf ("Failed. Could not connect to database (check log)\n");
          return -1;
        }
  
      /* Open authentication connection */
      printf ("Opening connection to authentication...\n");
      if (auth_connect() != 0)
        {
          printf ("Failed. Could not connect to authentication (check log)\n");
          return -1;
        }
  
      printf ("Ok. Connected!\n");

      /* Retrieve the user ID number */
      switch(auth_user_exists(user_name, &user_idnr))
        {
	  case 0:
            printf( "User [%s] does not exist!\n", user_name);
            break;
          case -1:
            printf( "Error retrieving User ID Number\n" );
            res = -1;
            goto mainend;
        }
    }

  switch (act)
    {
      case 'a':
        res = do_activate(user_idnr, name);
        break;
      case 'd':
        res = do_deactivate(user_idnr, name);
        break;
      case 'i':
        res = do_insert(user_idnr, name, source);
        break;
      case 'r':
        res = do_remove(user_idnr, name);
        break;
      case 'l':
        res = do_list(user_idnr);
        break;
      case 'h':
      default:
        res = do_showhelp();
        break;
    }

  mainend:
  free(user_name);
  db_disconnect();
  auth_disconnect();
  return res;
}


int do_activate(u64_t user_idnr, char *name)
{
  int res = 0;

  res = db_activate_sievescript(user_idnr, name);
  if (res == -3)
    {
      printf( "Script [%s] does not exist.\n", name );
      return -1;
    }
  else if (res != 0)
    {
      printf( "Error activating script [%s].\n"
              "It is possible that no script is currently active!\n", name );
      return -1;
    }
  printf( "Script [%s] is now active. All others are inactive.\n", name );

  return 0;
}


int do_deactivate(u64_t user_idnr, char *name)
{
  int res = 0;

  res = db_deactivate_sievescript(user_idnr, name);
  if (res == -3)
    {
      printf( "Script [%s] does not exist.\n", name );
      return -1;
    }
  else if (res != 0)
    {
      printf( "Error deactivating script [%s].\n", name );
      return -1;
    }
  printf( "Script [%s] is now deactivated. No scripts are currently active.\n", name );

  return 0;
}


int do_insert(u64_t user_idnr, char *name, FILE *source)
{
  int res = 0;
  char *buf = NULL;
  char *errmsg = NULL;

  /* Read the file into a char array */
  res = read_script_file(source, &buf);
  if (res != 0)
    {
      printf( "Error reading in your script!\n" );
      return -1;
    }

  /* Check if the script is valid */
  res = my_sieve_script_validate(buf, &errmsg);
  if (res != 0)
    {
      printf( "Script has errors: [%s].\n", name, errmsg );
      return -1;
    }

  /* Make the DB call to store the script */
  res = db_add_sievescript(user_idnr, name, buf);
  if (res == -3)
    {
      printf( "Script [%s] already exists.\n", name );
      return -1;
    }
  else if (res != 0)
    {
      printf( "Error inserting script [%s] into the database!\n", name );
      return -1;
    }

  printf( "Script [%s] successfully inserted and marked inactive!\n", name );
  return 0;
}


int do_remove(u64_t user_idnr, char *name)
{
  int res;

  res = db_delete_sievescript(user_idnr, name);
  if (res == -3)
    {
      printf( "Script [%s] does not exist.\n", name );
      return -1;
    }
  else if (res != 0)
    {
      printf( "Error deleting script [%s].\n", name );
      return -1;
    }

  printf( "Script [%s] deleted.\n", name );

  return 0;
}


int do_list(u64_t user_idnr)
{
  struct list scriptlist;
  struct element *tmp;

  if(db_get_sievescript_listall(user_idnr, &scriptlist) < 0)
    {
      printf("Error retrieving Sieve script list.\n");
      return -1;
    }

  if (list_totalnodes(&scriptlist) > 0)
    printf( "Found %ld scripts:\n", list_totalnodes(&scriptlist) );
  else
    printf( "No scripts found!\n" );

  tmp = list_getstart(&scriptlist);
  while (tmp)
    {
      struct ssinfo *info = (struct ssinfo *)tmp->data;
      if(info->active == 1)
          printf("  + ");
      else
          printf("  - ");
      printf("%s\n", info->name);
      tmp = tmp->nextnode;
    }

  if (scriptlist.start)
      list_freelist(&scriptlist.start);

  return 0;
}


int do_showhelp()
{
  printf ("*** dbmail-sievecmd ***\n");
	
  printf("Use this program to manage your users' Sieve scripts.\n");
  printf("See the man page for more info. Summary:\n\n");
  printf("     -u username            Username of script user \n");
  printf("     -l                     List scripts belonging to user \n");
  printf("     -a scriptname          Activate the named script \n");
  printf("                            (only one script can be active; \n"
         "                             deactivates any others) \n");
  printf("     -d scriptname          Deactivate the named script \n");
  printf("                            (no scripts will be active after this) \n");
  printf("     -i scriptname file     Insert the named script from file \n");
  printf("                            (a single dash, -, indicates input \n"
         "                             from STDIN) \n");
  printf("     -r scriptname          Remove the named script \n");
  printf("                            (if script was active, no script is \n"
         "                             active after deletion) \n");
  
  return 0;
}


int read_script_file(FILE *f, char **m_buf)
{
  size_t f_len=0;
  size_t f_pos=0;
  char *tmp_buf = NULL;
  char *f_buf = NULL;

  if (!f)
    {
      printf( "Received NULL as script input\n" );
      return -1;
    }

  while(!feof(f))
    {
      if( f_pos + 1 >= f_len )
        {
          tmp_buf = realloc(f_buf, sizeof(char) * (f_len+=200));
          if( tmp_buf != NULL )
            f_buf = tmp_buf;
          else
            return -2;
        }
          f_buf[f_pos] = fgetc(f);
          f_pos++;
    }

    if(f_pos)
      f_buf[f_pos] = '\0';

  /* Since f_buf is either NULL or valid, we're golden */
  *m_buf = f_buf;
  return 0;
}
