/* Real libpq plan ownership, mutable SQL buffers, reset and caller policy. */
#include "pg_plan.h"
#include "laplace/laplace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static PGconn *connect(const char *connection){
    PGconn *pg=PQconnectdb(connection);
    if(PQstatus(pg)!=CONNECTION_OK){fprintf(stderr,"%s",PQerrorMessage(pg));PQfinish(pg);return NULL;}
    return pg;
}
static void answer(PGconn *pg,char *query,int expected){
    const char *value="41";PGresult *result=db_ask(pg,query,1,&value,NULL,NULL);
    if(PQresultStatus(result)!=PGRES_TUPLES_OK){fprintf(stderr,"%s",PQresultErrorMessage(result));assert(0);}
    assert(PQntuples(result)==1 && PQgetlength(result,0,0)==4);
    assert(lp_be(PQgetvalue(result,0,0),4)==(uint64_t)expected);PQclear(result);
}
int main(int argc,char **argv){
    if(argc!=2){fputs("usage: pg-plan-lifecycle conninfo\n",stderr);return 2;}
    PGconn *a=connect(argv[1]),*b=connect(argv[1]);if(!a || !b)return 1;
    PGresult *setting=PQexec(a,"SET plan_cache_mode=force_custom_plan");
    assert(PQresultStatus(setting)==PGRES_COMMAND_OK);PQclear(setting);
    char query[64]="SELECT $1::int+1";
    answer(a,query,42);strcpy(query,"SELECT $1::int+2");answer(a,query,43);
    answer(b,query,43);
    setting=PQexec(a,"SHOW plan_cache_mode");
    assert(PQresultStatus(setting)==PGRES_TUPLES_OK && !strcmp(PQgetvalue(setting,0,0),"force_custom_plan"));PQclear(setting);
    PQreset(a);assert(PQstatus(a)==CONNECTION_OK);answer(a,query,43);
    PQfinish(a);PQfinish(b);
    for(int i=0;i<4;i++){a=connect(argv[1]);assert(a);answer(a,query,43);PQfinish(a);}
    puts("pg-plan-lifecycle: connection-local plans, SQL contents, reset and caller planner policy verified");
    return 0;
}
