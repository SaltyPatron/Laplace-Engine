/* Prepared plans belong to a libpq connection, not a process-wide PGconn
 * address cache. libpq delivers reset/destruction even when an allocator later
 * reuses that address. As with libpq itself, a connection has one active owner. */
#include "pg_plan.h"
#include "laplace/laplace.h"
#include "libpq-events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[40]; int ready; } PreparedPlan;
typedef struct { lp_strmap *plans; } PreparedConnection;

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
            lp_strmap_free(state->plans);state->plans=NULL;
            if(event==PGEVT_CONNDESTROY)free(state);
        }
    }
    return 1;
}
PGresult *db_ask(PGconn *pg,const char *sql,int n,const char *const *values,const int *lengths,const int *formats){
    PreparedConnection *state=PQinstanceData(pg,prepared_event);
    if(!state){
        if(!PQregisterEventProc(pg,prepared_event,"Laplace prepared SQL",NULL)){
            fputs("cannot register the database plan owner\n",stderr);exit(1);
        }
        state=PQinstanceData(pg,prepared_event);
    }
    if(!state->plans)state->plans=lp_strmap_sized(sizeof(PreparedPlan));
    bool fresh;
    PreparedPlan *plan=lp_strmap_get(state->plans,sql,strlen(sql),&fresh);
    if(fresh)snprintf(plan->name,sizeof plan->name,"laplace_q%zu",lp_strmap_count(state->plans)-1);
    if(!plan->ready){
        PGresult *result=PQprepare(pg,plan->name,sql,n,NULL);
        if(PQresultStatus(result)!=PGRES_COMMAND_OK)return result;
        PQclear(result);plan->ready=1;
    }
    return PQexecPrepared(pg,plan->name,n,values,lengths,formats,1);
}
