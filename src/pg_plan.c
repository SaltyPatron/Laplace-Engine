/* Prepared plans belong to a libpq connection, not a process-wide PGconn
 * address cache. libpq delivers reset/destruction even when an allocator later
 * reuses that address. As with libpq itself, a connection has one active owner. */
#include "pg_plan.h"
#include "laplace/laplace.h"
#include "libpq-events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[40]; int ready; lp_buf sql; } PreparedPlan;
typedef struct { lp_strmap *plans; DbBinding binding; char *snapshot; } PreparedConnection;

static void notice(void *context,const PGresult *result){
    (void)context;
    const char *severity=PQresultErrorField(result,PG_DIAG_SEVERITY_NONLOCALIZED);
    if(!severity || !strcmp(severity,"WARNING") || !strcmp(severity,"ERROR") ||
       !strcmp(severity,"FATAL") || !strcmp(severity,"PANIC"))fputs(PQresultErrorMessage(result),stderr);
}
void db_notices(PGconn *pg){PQsetNoticeReceiver(pg,notice,NULL);}

static void connection_clear(PreparedConnection *state){
    if(state->plans)for(size_t i=0;i<lp_strmap_count(state->plans);i++)
        lp_buf_free(&((PreparedPlan *)lp_strmap_at(state->plans,i))->sql);
    lp_strmap_free(state->plans);state->plans=NULL;
    if(state->binding.native_schema)PQfreemem((void *)state->binding.native_schema);
    if(state->binding.postgis_schema)PQfreemem((void *)state->binding.postgis_schema);
    state->binding=(DbBinding){0};
    free(state->snapshot);state->snapshot=NULL;
}

static int prepared_event(PGEventId event, void *info, void *context){
    (void)context;
    if(event==PGEVT_REGISTER){
        PGconn *pg=((PGEventRegister *)info)->conn;
        PreparedConnection *state=calloc(1,sizeof *state);
        if(!state)return 0;
        if(!PQsetInstanceData(pg,prepared_event,state)){free(state);return 0;}
    }else if(event==PGEVT_CONNRESET || event==PGEVT_CONNDESTROY){
        PGconn *pg=event==PGEVT_CONNRESET?((PGEventConnReset *)info)->conn:((PGEventConnDestroy *)info)->conn;
        PreparedConnection *state=PQinstanceData(pg,prepared_event);
        if(state){
            connection_clear(state);
            if(event==PGEVT_CONNDESTROY)free(state);
        }
    }
    return 1;
}
static PreparedConnection *connection_state(PGconn *pg){
    PreparedConnection *state=PQinstanceData(pg,prepared_event);
    if(!state){
        if(!PQregisterEventProc(pg,prepared_event,"Laplace prepared SQL",NULL)){
            fputs("cannot register the database plan owner\n",stderr);exit(1);
        }
        state=PQinstanceData(pg,prepared_event);
    }
    return state;
}
const char *db_read_snapshot(PGconn *pg){return connection_state(pg)->snapshot;}
int db_read_end(PGconn *pg){
    PreparedConnection *state=connection_state(pg);if(!state->snapshot)return 1;
    PGTransactionStatusType status=PQtransactionStatus(pg);
    if(status!=PQTRANS_IDLE){
        if(status!=PQTRANS_INTRANS && status!=PQTRANS_INERROR)return 0;
        PGresult *r=PQexec(pg,"ROLLBACK");int ok=PQresultStatus(r)==PGRES_COMMAND_OK;PQclear(r);
        if(!ok)return 0;
    }
    free(state->snapshot);state->snapshot=NULL;return 1;
}
int db_read_begin(PGconn *pg,const char *snapshot){
    PreparedConnection *state=connection_state(pg);
    if(state->snapshot || PQtransactionStatus(pg)!=PQTRANS_IDLE)return 0;
    PGresult *r;int ok=0;
    if(snapshot){
        char *quoted=PQescapeLiteral(pg,snapshot,strlen(snapshot));lp_buf sql={0};
        if(!quoted)goto failed;
        const char *prefix="BEGIN ISOLATION LEVEL REPEATABLE READ, READ ONLY; SET TRANSACTION SNAPSHOT ";
        lp_buf_put(&sql,prefix,strlen(prefix));lp_buf_put(&sql,quoted,strlen(quoted)+1);PQfreemem(quoted);
        r=PQexec(pg,(const char *)sql.b);lp_buf_free(&sql);
        ok=PQresultStatus(r)==PGRES_COMMAND_OK;PQclear(r);
        if(ok)state->snapshot=strdup(snapshot);
    }else{
        r=PQexec(pg,"BEGIN ISOLATION LEVEL REPEATABLE READ, READ ONLY; SELECT pg_catalog.pg_export_snapshot()");
        ok=PQresultStatus(r)==PGRES_TUPLES_OK && PQntuples(r)==1;
        if(ok)state->snapshot=strdup(PQgetvalue(r,0,0));PQclear(r);
    }
    if(ok && state->snapshot)return 1;
failed:
    r=PQexec(pg,"ROLLBACK");PQclear(r);return 0;
}
const DbBinding *db_binding(PGconn *pg){
    PreparedConnection *state=connection_state(pg);
    if(!state->binding.id_type){
        PGresult *r=PQexec(pg,
            "SELECT n.nspname,g.nspname,t.oid,t.typarray FROM pg_catalog.pg_extension e"
            " JOIN pg_catalog.pg_namespace n ON n.oid=e.extnamespace"
            " JOIN pg_catalog.pg_type t ON t.typnamespace=e.extnamespace AND t.typname='blake3'"
            " JOIN pg_catalog.pg_extension p ON p.extname='postgis'"
            " JOIN pg_catalog.pg_namespace g ON g.oid=p.extnamespace WHERE e.extname='laplace'");
        if(PQresultStatus(r)!=PGRES_TUPLES_OK || PQntuples(r)!=1){
            fprintf(stderr,"cannot bind installed Laplace/PostGIS objects: %s",PQerrorMessage(pg));
            PQclear(r);exit(1);
        }
        state->binding.native_schema=PQescapeIdentifier(pg,PQgetvalue(r,0,0),(size_t)PQgetlength(r,0,0));
        state->binding.postgis_schema=PQescapeIdentifier(pg,PQgetvalue(r,0,1),(size_t)PQgetlength(r,0,1));
        state->binding.id_type=(uint32_t)strtoul(PQgetvalue(r,0,2),NULL,10);
        state->binding.id_array=(uint32_t)strtoul(PQgetvalue(r,0,3),NULL,10);
        PQclear(r);
        if(!state->binding.native_schema || !state->binding.postgis_schema){
            fputs("cannot quote installed extension namespaces\n",stderr);exit(1);
        }
    }
    return &state->binding;
}
static PreparedPlan *statement_plan(PGconn *pg,const char *sql){
    PreparedConnection *state=connection_state(pg);
    if(!state->plans)state->plans=lp_strmap_sized(sizeof(PreparedPlan));
    bool fresh;
    PreparedPlan *plan=lp_strmap_get(state->plans,sql,strlen(sql),&fresh);
    if(fresh){
        snprintf(plan->name,sizeof plan->name,"laplace_q%zu",lp_strmap_count(state->plans)-1);
        bool literal=false;
        while(*sql){
            const char *replacement=NULL;size_t token=0;
            if(!strncmp(sql,"@extschema@",11)){replacement=db_binding(pg)->native_schema;token=11;}
            else if(!strncmp(sql,"@postgis@",9)){replacement=db_binding(pg)->postgis_schema;token=9;}
            if(replacement){
                for(const char *p=replacement;*p;p++){
                    lp_buf_put(&plan->sql,p,1);
                    if(literal && *p=='\'')lp_buf_put(&plan->sql,p,1);
                }
                sql+=token;
            }else{
                if(*sql=='\''){
                    if(literal && sql[1]=='\''){lp_buf_put(&plan->sql,sql,2);sql+=2;continue;}
                    literal=!literal;
                }
                lp_buf_put(&plan->sql,sql++,1);
            }
        }
        lp_buf_put(&plan->sql,"",1);
    }
    return plan;
}
const char *db_sql(PGconn *pg,const char *sql){return (const char *)statement_plan(pg,sql)->sql.b;}
PGresult *db_exec(PGconn *pg,const char *sql){return PQexec(pg,db_sql(pg,sql));}
PGresult *db_exec_params(PGconn *pg,const char *sql,int n,const Oid *types,
                        const char *const *values,const int *lengths,const int *formats,int result_format){
    return PQexecParams(pg,db_sql(pg,sql),n,types,values,lengths,formats,result_format);
}
PGresult *db_ask(PGconn *pg,const char *sql,int n,const char *const *values,const int *lengths,const int *formats){
    PreparedPlan *plan=statement_plan(pg,sql);
    if(!plan->ready){
        PGresult *result=PQprepare(pg,plan->name,(const char *)plan->sql.b,n,NULL);
        if(PQresultStatus(result)!=PGRES_COMMAND_OK)return result;
        PQclear(result);plan->ready=1;
    }
    return PQexecPrepared(pg,plan->name,n,values,lengths,formats,1);
}
