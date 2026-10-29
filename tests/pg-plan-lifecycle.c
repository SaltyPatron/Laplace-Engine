/* Real libpq plan ownership, mutable SQL buffers, reset and caller policy. */
#include "engine.h"
#include "laplace/laplace.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lp_tier0_record *T0;

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
static void stored_reads(PGconn *pg){
    command(pg,"BEGIN");
    command(pg,"INSERT INTO @extschema@.entity(id,tier,coord,hilbert) SELECT "
        "@extschema@.laplace_id('ab'),@extschema@.laplace_tier('ab'),"
        "@extschema@.laplace_coord('ab'),@extschema@.laplace_hilbert('ab')");
    command(pg,"INSERT INTO @extschema@.physicality(entity,tier,hilbert,path) SELECT id,tier,hilbert,"
        "@extschema@.laplace_path(ARRAY[@extschema@.laplace_cp_id(97),@extschema@.laplace_cp_id(98)])"
        " FROM @extschema@.entity");
    PGresult *rows=ask_once(pg,"SELECT id,tier,@postgis@.ST_X(coord),@postgis@.ST_Y(coord),"
        "@postgis@.ST_Z(coord),@postgis@.ST_M(coord) FROM @extschema@.entity",NULL);
    assert(PQntuples(rows)==1);lp_id known=*col_id(rows,0,0);int tier=(int)col_int(rows,0,1);
    double expected[4];for(int d=0;d<4;d++)expected[d]=lp_be_f64(PQgetvalue(rows,0,d+2));PQclear(rows);
    lp_id ids[4]={known,{0},known};lp_id_codepoint(65,ids+3);
    double coords[16]={0};uint8_t has[4]={0};coords_of(pg,ids,4,coords,has);
    assert(has[0] && !has[1] && has[2] && has[3]);
    assert(!memcmp(coords,expected,sizeof expected) && !memcmp(coords+8,expected,sizeof expected));
    lp_ref atom=lp_ref_atom(T0,65);double atom_coord[4];lp_coord_xyzm(&atom.c,atom_coord);
    assert(!memcmp(coords+12,atom_coord,sizeof atom_coord));
    DbTraffic before=db_traffic();coords_of(pg,ids+3,1,coords,has);
    assert(has[0] && db_traffic().commands==before.commands);
    int tiers[4];tiers_of(pg,ids,4,tiers);
    assert(tiers[0]==tier && tiers[1]==-1 && tiers[2]==tier && tiers[3]==0);
    before=db_traffic();tiers_of(pg,ids+3,1,tiers);
    assert(tiers[0]==0 && db_traffic().commands==before.commands);
    assert(tier_max(pg,ids,4)==tier && tier_max(pg,ids+1,1)==-1 && tier_max(pg,NULL,0)==-1);
    int count;Hold *paths=paths_of(pg,ids,4,&count);assert(count==2);
    assert(paths[0].src==0 && paths[1].src==2 && paths[0].path!=paths[1].path);
    assert(paths[0].tier==tier && !paths[0].claim);
    lp_path path=lp_path_of(paths[0].path,(size_t)paths[0].path_len);assert(path.n==2);
    lp_id child=lp_path_id(path,0),a;lp_id_codepoint(97,&a);assert(lp_id_eq(&child,&a));
    assert(paths[0].path_len==paths[1].path_len && !memcmp(paths[0].path,paths[1].path,(size_t)paths[0].path_len));
    holds_free(paths,count);paths=paths_of(pg,NULL,0,&count);assert(!paths && !count);
    lp_id akey,bkey,absent={0};lp_id_codepoint(97,&akey);lp_id_codepoint(98,&bkey);
    lp_id keys[3]={akey,absent,akey},both[2]={akey,bkey};int hub[6];
    paths=holds_above(pg,both,2,-1,0,0,&count);
    assert(count==1 && paths[0].src==0 && !paths[0].claim && !paths[0].stood);
    holds_free(paths,count);
    paths=holds_any(pg,keys,3,&count);assert(count==1 && paths[0].src==-1);holds_free(paths,count);
    paths=holds_both(pg,keys,3,&bkey,&count);assert(count==2 && paths[0].src==0 && paths[1].src==2);holds_free(paths,count);
    assert(holds_count(pg,both,2,0)==1 && holds_count(pg,both,2,1)==0);
    paths=holds_capped(pg,keys,3,0,0,3,hub,&count);
    assert(!count && !hub[0] && hub[1] && !hub[2] && !hub[3] && !hub[4] && hub[5]);holds_free(paths,count);
    command(pg,"UPDATE @extschema@.physicality SET mask=@extschema@.laplace_mask_slot('kind','claim')");
    command(pg,"INSERT INTO @extschema@.consensus SELECT id,1550,60,0.06,5000000001 FROM @extschema@.entity");
    paths=holds_capped(pg,keys,3,1,1,3,hub,&count);
    assert(count==2 && paths[0].src==0 && paths[1].src==2 && paths[0].claim && paths[0].stood);
    assert(paths[0].r.rating==1550 && paths[0].r.deviation==60 && paths[0].r.volatility==0.06 && paths[0].matches==5000000001LL);
    assert(!hub[0] && !hub[1] && !hub[4] && !hub[5]);holds_free(paths,count);
    paths=holds_capped(pg,keys,3,0,1,1,hub,&count);
    assert(!count && hub[0] && !hub[1] && hub[4] && !hub[5]);holds_free(paths,count);
    paths=holds_above(pg,keys,3,tier,1,1,&count);assert(!count);holds_free(paths,count);
    command(pg,"ROLLBACK");
}
int main(int argc,char **argv){
    if(argc==3){
        T0=lp_tier0_map(getenv("LAPLACE_TIER0"));assert(T0);
        PGconn *a=connect(argv[1]),*b=connect(argv[2]);assert(a && b);
        snapshot_scope(a,argv[1]);
        assert(db_binding(a)->id_type!=db_binding(b)->id_type);
        lp_id ids[2];lp_id_codepoint(65,ids);ids[1]=ids[0];Args arguments={0};arg_ids(&arguments,ids,2);
        binding_answer(a,&arguments);binding_answer(b,&arguments);binding_answer(a,&arguments);
        PQreset(b);assert(PQstatus(b)==CONNECTION_OK);binding_answer(b,&arguments);
        stored_reads(a);stored_reads(b);stored_reads(a);
        args_free(&arguments);PQfinish(a);PQfinish(b);
        puts("pg-plan-lifecycle: namespaces, OIDs, snapshots, exact row reads, duplicate/missing/virtual IDs and reset passed");return 0;
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
