/*  Copyright (c) 2012, 2013, Adrian M. Partl, eScience Group at the
    Leibniz Institut for Astrophysics, Potsdam

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

/*****************************************************************
 * *******               UDF_VALIDATE_SQL                  *******
 *****************************************************************
 * (C) 2012 A. Partl, eScience Group AIP - Distributed under GPL
 * 
 * exposes the SQL query validation routines in mysql to the user
 * parses the query - just returns errors in the grammar 
 * 
 *****************************************************************
 */

#define MYSQL_SERVER 1
#define MYSQL_LEX 1

#include "daemon_thd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mysql.h>
#include <tztime.h>
#include <unireg.h>
#include <sql_parse.h>
#include <sql_base.h>
#include <sql_select.h>
#include <sql_table.h>
#include <sp_head.h>
#include <strfunc.h>
#include <transaction.h>
#include <sql_db.h>

extern "C" {

    // validate SQL query using internal functions returning error as string...
    my_bool paqu_validateSQL_init(UDF_INIT* initid, UDF_ARGS* args, char* message);
    void paqu_validateSQL_deinit(UDF_INIT* initid);
    char* paqu_validateSQL(UDF_INIT* initid, UDF_ARGS* args, char* result, unsigned long* res_length,
            char* is_null, char* is_error);
}

pthread_handler_t validate_sql(void* p);
bool validate_and_check_statment(THD * thd);
bool validate_and_check_select(THD * thd, LEX * lex, SELECT_LEX * select_lex);
bool validate_and_check_create_table(THD * thd, LEX * lex, SELECT_LEX * select_lex);
extern int MYSQLparse(void *thd); // from sql_yacc.cc

static bool stmt_causes_implicit_commit(THD *thd, uint mask);

struct validate_sql_info {
    char * query;
    long queryLen;
    char * error;
    long errorLen;
    int errNum;
    const char * usrName;
    const char * host;
    const char * db;
};

my_bool paqu_validateSQL_init(UDF_INIT* initid, UDF_ARGS* args, char* message) {
    //checking stuff to be correct
    if (args->arg_count != 1) {
        strcpy(message, "wrong number of arguments: paqu_validateSQL() requires one parameters");
        return 1;
    }

    if (args->arg_type[0] != STRING_RESULT) {
        strcpy(message, "paqu_validateSQL() requires a string as parameter one");
        return 1;
    }

    initid->maybe_null = 0;
    initid->max_length = 1024;

    return 0;
}

void paqu_validateSQL_deinit(UDF_INIT* initid) {

}

char* paqu_validateSQL(UDF_INIT* initid, UDF_ARGS* args, char* result, unsigned long* res_length,
        char* is_null, char* is_error) {

    validate_sql_info * query = new validate_sql_info;

    query->query = strdup((char*) args->args[0]);
    query->queryLen = strlen((char*) args->args[0]);
    query->error = NULL;
    query->errorLen = -1;
    query->usrName = current_thd->security_ctx->user;
    query->host = current_thd->security_ctx->ip;

    if(query->host == NULL) {
      query->host = current_thd->security_ctx->host_or_ip;
    }

    query->db = current_thd->db;

#ifdef __VALIDATE_DEBUG__
    fprintf(stderr, "Validating query: %s\n", query->query);
    fprintf(stderr, "on host: %s\n", query->host);
#endif

    //start independent thread for SQL checking (I haven't figured out yet how to 
    //avoid this... better ideas are welcome)

    pthread_t thread;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if (pthread_create(&thread, &attr, validate_sql, (void *) query) != 0) {
        fprintf(stderr, "paqu_validateSQL: could not create SQL validation thread!\n");
        *is_error = 1;
        delete query;
        return NULL;
    }

    pthread_join(thread, NULL);

    char * tmp = NULL;
    if (query->errorLen > 0) {
        tmp = (char*)malloc(query->errorLen + 16);
        if(tmp == NULL) {
            fprintf(stderr, "paqu_validateSQL: could not create result string!\n");
            delete query;
            return NULL;
        }

        sprintf(tmp, "ERROR %i: %s", query->errNum, query->error);
        *res_length = strlen(tmp);
        free(query->error);
    } else {
        *is_null = 1;
    }

    free(query->query);
    delete query;
    return tmp;
}

//takes a pointer to a validate_sql_info struct

pthread_handler_t validate_sql(void* p) {
    validate_sql_info * query = (validate_sql_info*) p;
    THD * thd = NULL;

#ifdef __VALIDATE_DEBUG__
    fprintf(stderr, "paqu_validateSQL: entering validate_sql\n");
#endif

    init_thread(&thd, "Validating SQL Query");
    thd->init_for_queries();

#ifdef __VALIDATE_DEBUG__
    fprintf(stderr, "paqu_validateSQL: thread initialised\n");
#endif

    LEX * lex = new LEX;
    thd->lex = lex;

    Security_context * old = NULL;
    Security_context newContext;
    LEX_STRING user;
    LEX_STRING host;
    LEX_STRING db;

    if (query->db == NULL) {
      query->error = strdup("paqu_validateSQL: No database selected!\n");
      query->errorLen = strlen(query->error);
      query->errNum = -2;
      delete lex;

#ifdef __VALIDATE_DEBUG__
      fprintf(stderr, "paqu_validateSQL: DB is null!\n");
#endif

      deinit_thread(&thd);
      pthread_exit(NULL);
      return NULL;
    }


    char *usrStr = my_strdup(query->usrName, MYF(0));
    char *hostStr = my_strdup(query->host, MYF(0));
    char *dbStr = my_strdup(query->db, MYF(0));

    user.str = usrStr;
    user.length = strlen(usrStr);
    host.str = hostStr;
    host.length = strlen(hostStr);
    db.str = dbStr;
    db.length = strlen(dbStr);

    lex_start(thd);

#ifdef __VALIDATE_DEBUG__
    fprintf(stderr, "paqu_validateSQL: trying to change security context to: user %s - host %s - db %s\n", user.str, host.str, db.str);
#endif

    newContext.change_security_context(thd, &user, &host, &db, &old);

#ifdef __VALIDATE_DEBUG__
    fprintf(stderr, "paqu_validateSQL: security context successfully changed\n");
#endif

    if (mysql_change_db(thd, &db, FALSE)) {
        //let's try again if the host is localhost, with 127.0.0.1
        bool realError = true;
        if(strcmp(host.str, "localhost") == 0) {
            my_free(hostStr);
            hostStr = my_strdup("127.0.0.1", MYF(0));
            host.str = hostStr;
            host.length = strlen(hostStr);

            newContext.change_security_context(thd, &user, &host, &db, &old);

            if (!mysql_change_db(thd, &db, FALSE)) {
                realError = false;
#ifdef __VALIDATE_DEBUG__
                fprintf(stderr, "paqu_validateSQL: resolved localhost as 127.0.0.1\n", db.str);
#endif
            }
        }

        if(realError == true) {
            query->error = NULL;
            query->errorLen = -2;
            delete lex;
#ifdef __VALIDATE_DEBUG__
            fprintf(stderr, "paqu_validateSQL: could not change database to %s!\n", db.str);
            fprintf(stderr, "user: %s, host: %s\n", user.str, host.str);
#endif
            pthread_exit(NULL);
        }
    }

    thd->client_capabilities |= CLIENT_MULTI_STATEMENTS;
    thd->set_query((char*) query->query, query->queryLen);

    Parser_state parser_state;

    char *packet_end = thd->query() + thd->query_length();
    if (parser_state.init(thd, thd->query(), thd->query_length())) {
        query->error = NULL;
        query->errorLen = -2;
        delete lex;
#ifdef __VALIDATE_DEBUG__
        fprintf(stderr, "paqu_validateSQL: could not initialise parser!\n");
#endif
        pthread_exit(NULL);
    }

    /* Set parser state. */
    thd->m_parser_state = &parser_state;

    /* Parse the query. */
    bool mysql_parse_status = validate_and_check_statment(thd);

    /* handle multiple queries */
    while (!mysql_parse_status && parser_state.m_lip.found_semicolon != NULL && !thd->is_error()) {
        /*                                                                                                                                               
          Multiple queries exits, parse them individually                                                                                              
         */
        char *beginning_of_next_stmt = (char*) parser_state.m_lip.found_semicolon;

        ulong length = (ulong) (packet_end - beginning_of_next_stmt);
        /* Remove garbage at start of query */
        while (length > 0 && my_isspace(thd->charset(), *beginning_of_next_stmt)) {
            beginning_of_next_stmt++;
            length--;
        }

#ifdef __VALIDATE_DEBUG__
        fprintf(stderr, "Multiple query detected at %s\n", beginning_of_next_stmt);
#endif

        parser_state.reset(beginning_of_next_stmt, length);

        lex_start(thd);
        mysql_parse_status |= validate_and_check_statment(thd);
    }

    /* Reset parser state. */
    thd->m_parser_state = NULL;

    thd->security_ctx->restore_security_context(thd, old);

    if (mysql_parse_status) {
        query->error = strdup(thd->stmt_da->message());
        query->errorLen = strlen(query->error);
        query->errNum = thd->stmt_da->sql_errno();
    } else {
        query->error = NULL;
        query->errorLen = -1;
#ifdef __VALIDATE_DEBUG__
        fprintf(stderr, "paqu_validateSQL: no error found in query...\n");
#endif
    }

    my_free(usrStr);
    my_free(hostStr);
    my_free(dbStr);

    deinit_thread(&thd);
    pthread_exit(NULL);
    return NULL;
}

bool validate_and_check_statment(THD * thd) {
    LEX * lex = thd->lex;
    SELECT_LEX * select_lex = &lex->select_lex;

    bool mysql_parse_status = MYSQLparse(thd);

#ifdef __VALIDATE_DEBUG__
    fprintf(stderr, "paqu_validateSQL: entering validate_and_check_statement\n");
#endif

    thd->stmt_arena->state = Query_arena::STMT_INITIALIZED;

    TABLE_LIST *all_tables = NULL;
    lex->first_lists_tables_same();
    all_tables = lex->query_tables;
    select_lex->
            context.resolve_in_table_list_only(select_lex->
            table_list.first);

    ulong privileges_requested = lex->exchange ? SELECT_ACL | FILE_ACL : SELECT_ACL;

#ifdef __VALIDATE_DEBUG__
    fprintf(stderr, "paqu_validateSQL: checking table access\n");
#endif

    if (all_tables) {
#ifdef __VALIDATE_DEBUG__
        fprintf(stderr, "paqu_validateSQL: checking table access with all_tables\n");
#endif
        mysql_parse_status |= check_table_access(thd,
                privileges_requested,
                all_tables, FALSE, UINT_MAX, FALSE);

#ifdef __VALIDATE_DEBUG__
        fprintf(stderr, "paqu_validateSQL: trying to open_and_lock_tables\n");
#endif
        mysql_parse_status |= open_and_lock_tables(thd, all_tables, TRUE, 0);
    } else {
#ifdef __VALIDATE_DEBUG__
        fprintf(stderr, "paqu_validateSQL: no all_tables\n");
#endif
        mysql_parse_status |= check_access(thd, privileges_requested, any_db, NULL, NULL, 0, 0);
    }

    //did we pass the check if all databases and table names are correct? if yes, continue checking
    //columns...

#ifdef __VALIDATE_DEBUG__
    fprintf(stderr, "paqu_validateSQL: access OK\n");
#endif

    if (!mysql_parse_status) {

        //if this command changes the database, fullfill that whish
        switch (lex->sql_command) {
            case SQLCOM_CHANGE_DB:
            {
                LEX_STRING db_str = {(char *) lex->select_lex.db, strlen(lex->select_lex.db)};

                mysql_parse_status |= mysql_change_db(thd, &db_str, FALSE);

#ifdef __VALIDATE_DEBUG__
		fprintf(stderr, "paqu_validateSQL: changed database %s\n", (char *) lex->select_lex.db);
#endif

                break;
            }
            case SQLCOM_SELECT:
            {
                mysql_parse_status |= validate_and_check_select(thd, lex, select_lex);

#ifdef __VALIDATE_DEBUG__
                fprintf(stderr, "paqu_validateSQL: validateing SELECT\n");
#endif

                break;
            }
            case SQLCOM_CREATE_TABLE:
            {
                mysql_parse_status |= validate_and_check_create_table(thd, lex, select_lex);

#ifdef __VALIDATE_DEBUG__
                fprintf(stderr, "paqu_validateSQL: validateing CREATE TABLE\n");
#endif

                break;
            }
        }
    }

    lex->unit.cleanup();
    close_thread_tables(thd);

    return mysql_parse_status;
}

bool validate_and_check_select(THD * thd, LEX * lex, SELECT_LEX * select_lex) {
    bool mysql_parse_status = FALSE;
    bool free_join = 1;
    Item ***rref_pointer_array = &select_lex->ref_pointer_array;
    TABLE_LIST *tables = select_lex->table_list.first;
    uint wild_num = select_lex->with_wild;
    List<Item> &fields = select_lex->item_list;
    COND *conds = select_lex->where;
    uint og_num = select_lex->order_list.elements + select_lex->group_list.elements;
    ORDER *order = select_lex->order_list.first;
    ORDER *group = select_lex->group_list.first;
    Item *having = select_lex->having;
    ORDER *proc_param = lex->proc_list.first;
    ulonglong select_options = select_lex->options | thd->variables.option_bits;
    select_result *result = lex->result;

    if (!result)
        result = new select_send();

    SELECT_LEX_UNIT *unit = &lex->unit;

    unit->set_limit(unit->global_parameters);

    if (select_lex->master_unit()->is_union() ||
            select_lex->master_unit()->fake_select_lex) {
        fprintf(stderr, "PaQu SQL Validator: UNION not yet implemented!!\n");
        return 1;
    } else {

        select_lex->context.resolve_in_select_list = TRUE;
        JOIN *join;
        if (select_lex->join != 0) {
            join = select_lex->join;
            /*      
              is it single SELECT in derived table, called in derived table            
              creation       
             */
            if (select_lex->linkage != DERIVED_TABLE_TYPE ||
                    (select_options & SELECT_DESCRIBE)) {

                if (select_lex->linkage != GLOBAL_OPTIONS_TYPE) {
                    //here is EXPLAIN of subselect or derived table                                                                              
                    if (join->change_result(result)) {
                        DBUG_RETURN(TRUE);
                    }
                    /*            
                      Original join tabs might be overwritten at first                             
                      subselect execution. So we need to restore them.     
                     */
                    Item_subselect *subselect = select_lex->master_unit()->item;
                    if (subselect && subselect->is_uncacheable() && join->reinit())
                        DBUG_RETURN(TRUE);
                } else {
                    mysql_parse_status |= join->prepare(rref_pointer_array, tables, wild_num,
                            conds, og_num, order, group, having, proc_param,
                            select_lex, unit);
                }
            }
            free_join = 0;
            join->select_options = select_options;
        } else {
            if (!(join = new JOIN(thd, fields, select_options, result)))
                DBUG_RETURN(TRUE);

#if MYSQL_VERSION_ID >= 50516
            thd->lex->used_tables = 0; // Updated by setup_fields                                                      
#else
            thd->used_tables = 0; // Updated by setup_fields                                                      
#endif

            mysql_parse_status |= join->prepare(rref_pointer_array, tables, wild_num,
                    conds, og_num, order, group, having, proc_param,
                    select_lex, unit);
        }
    }

    if (result != lex->result)
        delete result;

    return mysql_parse_status;
}

bool validate_and_check_create_table(THD * thd, LEX * lex, SELECT_LEX * select_lex) {
    bool mysql_parse_status = FALSE;
    TABLE_LIST *first_table = select_lex->table_list.first;
    TABLE_LIST *create_table = first_table;
    TABLE_LIST *select_tables = lex->create_last_non_select_table->next_global;
    Alter_info alter_info(lex->alter_info, thd->mem_root);

    mysql_parse_status |= create_table_precheck(thd, select_tables, create_table);

    if (select_lex->item_list.elements) // With select  
    {
        bool link_to_local;
        lex->unlink_first_table(&link_to_local);
        mysql_parse_status |= validate_and_check_select(thd, lex, select_lex);
    }

    if (!alter_info.create_list.elements && !select_lex->item_list.elements) {
        my_message(ER_TABLE_MUST_HAVE_COLUMNS, ER(ER_TABLE_MUST_HAVE_COLUMNS),
                MYF(0));
        mysql_parse_status |= 1;
    }
    if (check_db_dir_existence(create_table->db)) {
        my_error(ER_BAD_DB_ERROR, MYF(0), create_table->db);
        mysql_parse_status |= 1;
    }

    if (select_lex->item_list.elements) // With select				
    {
        bool link_to_local;

        /* The table already exists */
        if (!mysql_parse_status && create_table->table) {
            my_error(ER_TABLE_EXISTS_ERROR, MYF(0), create_table->table->alias);
            mysql_parse_status |= 1;
            return mysql_parse_status;
        }


        lex->unlink_first_table(&link_to_local);

        mysql_parse_status |= validate_and_check_select(thd, lex, select_lex);
    }

    return mysql_parse_status;
}
