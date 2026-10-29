/* Real libpq plan ownership, mutable SQL buffers, reset and caller policy. */
#include "engine.h"
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
static void binding_answer(PGconn *pg,Args *arguments){
    const DbBinding *binding=db_binding(pg);
    PGresult *r=ask(pg,"SELECT $1::@extschema@.blake3[]",arguments);
    assert(PQntuples(r)==1 && PQftype(r,0)==binding->id_array);
    lp_id ids[2];assert(lp_pg_ids_read((const uint8_t *)PQgetvalue(r,0,0),(size_t)PQgetlength(r,0,0),ids,2)==2);
    assert(lp_id_eq(ids,ids+1));PQclear(r);
    r=ask_once(pg,"SELECT pg_catalog.to_regtype('@extschema@.blake3')::oid,"
        " pg_catalog.to_regprocedure('@postgis@.st_astext(@postgis@.geometry)') IS NOT NULL",NULL);
    assert(PQntuples(r)==1 && lp_be(PQgetvalue(r,0,0),4)==binding->id_type && PQgetvalue(r,0,1)[0]);PQclear(r);
    r=db_exec(pg,"SELECT count(*) FROM @extschema@.entity");
    assert(PQresultStatus(r)==PGRES_TUPLES_OK && !strcmp(PQgetvalue(r,0,0),"0"));PQclear(r);
}
static void command(PGconn *pg,const char *sql){
    PGresult *r=db_exec(pg,sql);if(PQresultStatus(r)!=PGRES_COMMAND_OK){fputs(PQerrorMessage(pg),stderr);assert(0);}PQclear(r);
}
static void count_at(PGconn *pg,const char *expected){
    PGresult *r=db_exec(pg,"SELECT count(*) FROM public.snapshot_fixture");
    assert(PQresultStatus(r)==PGRES_TUPLES_OK && !strcmp(PQgetvalue(r,0,0),expected));PQclear(r);
}
static void snapshot_scope(PGconn *master,const char *connection){
    PGconn *writer=connect(connection),*reader=connect(connection);assert(writer && reader);
    command(writer,"CREATE TABLE public.snapshot_fixture(id integer)");
    assert(db_read_begin(master,NULL));assert(!db_read_begin(master,NULL));
    assert(db_read_begin(reader,db_read_snapshot(master)));
    command(writer,"INSERT INTO public.snapshot_fixture VALUES(1)");
    count_at(master,"0");count_at(reader,"0");
    PGresult *r=db_exec(reader,"SELECT current_setting('transaction_read_only'),current_setting('transaction_isolation')");
    assert(PQresultStatus(r)==PGRES_TUPLES_OK && !strcmp(PQgetvalue(r,0,0),"on") && !strcmp(PQgetvalue(r,0,1),"repeatable read"));PQclear(r);
    assert(db_read_end(reader) && db_read_end(master));
    assert(PQtransactionStatus(reader)==PQTRANS_IDLE && !db_read_snapshot(reader));
    assert(db_read_begin(master,NULL) && db_read_begin(reader,db_read_snapshot(master)));
    count_at(master,"1");count_at(reader,"1");
    r=db_exec(reader,"INSERT INTO public.snapshot_fixture VALUES(2)");assert(PQresultStatus(r)==PGRES_FATAL_ERROR);PQclear(r);
    assert(db_read_end(reader) && PQtransactionStatus(reader)==PQTRANS_IDLE && db_read_end(master));
    assert(db_read_begin(reader,NULL));PQreset(reader);assert(PQstatus(reader)==CONNECTION_OK && !db_read_snapshot(reader));
    command(writer,"DROP TABLE public.snapshot_fixture");PQfinish(reader);PQfinish(writer);
}
int main(int argc,char **argv){
    if(argc==3){
        PGconn *a=connect(argv[1]),*b=connect(argv[2]);assert(a && b);
        snapshot_scope(a,argv[1]);
        assert(db_binding(a)->id_type!=db_binding(b)->id_type);
        lp_id ids[2];lp_id_codepoint(65,ids);ids[1]=ids[0];Args arguments={0};arg_ids(&arguments,ids,2);
        binding_answer(a,&arguments);binding_answer(b,&arguments);binding_answer(a,&arguments);
        PQreset(b);assert(PQstatus(b)==CONNECTION_OK);binding_answer(b,&arguments);
        args_free(&arguments);PQfinish(a);PQfinish(b);
        puts("pg-plan-lifecycle: installed namespaces and OIDs, reusable arrays, shared read-only snapshots, refresh and reset passed");return 0;
    }
    if(argc!=2){fputs("usage: pg-plan-lifecycle conninfo [other-connection]\n",stderr);return 2;}
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
