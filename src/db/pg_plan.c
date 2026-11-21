/* Connection-local read-snapshot lifecycle and notice routing. */
#include "pg_plan.h"
#include "laplace/os.h"
#include "libpq-events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *snapshot; } ReadConnection;

static void notice(void *context,const PGresult *result){
    (void)context;
    const char *severity=PQresultErrorField(result,PG_DIAG_SEVERITY_NONLOCALIZED);
    if(!severity || !strcmp(severity,"WARNING") || !strcmp(severity,"ERROR") ||
       !strcmp(severity,"FATAL") || !strcmp(severity,"PANIC"))fputs(PQresultErrorMessage(result),stderr);
}
void db_notices(PGconn *pg){PQsetNoticeReceiver(pg,notice,NULL);}

static void connection_clear(ReadConnection *state){
    free(state->snapshot);state->snapshot=NULL;
}

static int prepared_event(PGEventId event, void *info, void *context){
    (void)context;
    if(event==PGEVT_REGISTER){
        PGconn *pg=((PGEventRegister *)info)->conn;
        ReadConnection *state=calloc(1,sizeof *state);
        if(!state)return 0;
        if(!PQsetInstanceData(pg,prepared_event,state)){free(state);return 0;}
    }else if(event==PGEVT_CONNRESET || event==PGEVT_CONNDESTROY){
        PGconn *pg=event==PGEVT_CONNRESET?((PGEventConnReset *)info)->conn:((PGEventConnDestroy *)info)->conn;
        ReadConnection *state=PQinstanceData(pg,prepared_event);
        if(state){
            connection_clear(state);
            if(event==PGEVT_CONNDESTROY)free(state);
        }
    }
    return 1;
}
static ReadConnection *connection_state(PGconn *pg){
    ReadConnection *state=PQinstanceData(pg,prepared_event);
    if(!state){
        if(!PQregisterEventProc(pg,prepared_event,"Laplace read scope",NULL)){
            fputs("cannot register the database read scope owner\n",stderr);exit(1);
        }
        state=PQinstanceData(pg,prepared_event);
    }
    return state;
}
const char *db_read_snapshot(PGconn *pg){return connection_state(pg)->snapshot;}
int db_read_end(PGconn *pg){
    ReadConnection *state=connection_state(pg);if(!state->snapshot)return 1;
    PGTransactionStatusType status=PQtransactionStatus(pg);
    if(status!=PQTRANS_IDLE){
        if(status!=PQTRANS_INTRANS && status!=PQTRANS_INERROR)return 0;
        PGresult *r=PQexec(pg,"ROLLBACK");int ok=PQresultStatus(r)==PGRES_COMMAND_OK;PQclear(r);
        if(!ok)return 0;
    }
    free(state->snapshot);state->snapshot=NULL;return 1;
}
int db_read_begin(PGconn *pg,const char *snapshot){
    ReadConnection *state=connection_state(pg);
    if(state->snapshot || PQtransactionStatus(pg)!=PQTRANS_IDLE)return 0;
    PGresult *r;int ok=0;
    if(snapshot){
        char *quoted=PQescapeLiteral(pg,snapshot,strlen(snapshot));
        if(!quoted)goto failed;
        const char *prefix="BEGIN ISOLATION LEVEL REPEATABLE READ, READ ONLY; SET TRANSACTION SNAPSHOT ";
        size_t plen=strlen(prefix),qlen=strlen(quoted);
        char *sql=malloc(plen+qlen+1);
        if(!sql){PQfreemem(quoted);goto failed;}
        memcpy(sql,prefix,plen);memcpy(sql+plen,quoted,qlen+1);PQfreemem(quoted);
        r=PQexec(pg,sql);free(sql);
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
