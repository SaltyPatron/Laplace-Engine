/* Real libpq plan ownership, mutable SQL buffers, reset, caller planner policy, read snapshots and the exact-ID reads,
 * against the deployed database (tests/deployed/db-binding.py runs it with LAPLACE_CONNINFO).
 *
 *   pg_plan_lifecycle CONNINFO
 *
 * Nothing it does is committed but one transaction ID that writes nothing (the snapshot probe). Every stored row it
 * reads is one it inserted inside a transaction it rolls back: a composition of two words no source holds, so every
 * holder, path and standing a read returns is the probe's own and the database's other rows are never counted. */
#include "engine.h"
#include "laplace/laplace.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lp_tier0_record *T0;
/* The probe's two words. They are put together at run time, so this file, read as code by a source, records neither. */
static char XW[64], YW[64];

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
static void command(PGconn *pg,const char *sql){
    PGresult *r=db_exec(pg,sql);if(PQresultStatus(r)!=PGRES_COMMAND_OK){fputs(PQerrorMessage(pg),stderr);assert(0);}PQclear(r);
}
/* The probe composition [XW, YW] as SQL, the two words' IDs as SQL. */
static void probe_sql(char *out,size_t n){
    snprintf(out,n,"@extschema@.laplace_compose(ARRAY[@extschema@.laplace_id('%s'),@extschema@.laplace_id('%s')])",XW,YW);
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
    /* The bound namespace reaches the stored table; the probe is no row of it outside its own transactions. */
    char probe[512],sql[640];probe_sql(probe,sizeof probe);
    snprintf(sql,sizeof sql,"SELECT count(*) FROM @extschema@.entity WHERE id OPERATOR(@extschema@.=) %s",probe);
    r=db_exec(pg,sql);
    assert(PQresultStatus(r)==PGRES_TUPLES_OK && !strcmp(PQgetvalue(r,0,0),"0"));PQclear(r);
}
/* Whether a transaction ID is visible in the snapshot the connection's transaction reads. */
static int visible(PGconn *pg,const char *xid){
    char sql[192];snprintf(sql,sizeof sql,"SELECT pg_catalog.pg_visible_in_snapshot('%s'::pg_catalog.xid8,pg_catalog.pg_current_snapshot())",xid);
    PGresult *r=db_exec(pg,sql);assert(PQresultStatus(r)==PGRES_TUPLES_OK && PQntuples(r)==1);
    int seen=PQgetvalue(r,0,0)[0]=='t';PQclear(r);return seen;
}
/* A read snapshot is exported by one connection and imported by another; a transaction committed after it is seen by
 * neither until both begin again. The committed transaction is one that only takes an ID: no table is written. */
static void snapshot_scope(PGconn *master,const char *connection){
    PGconn *writer=connect(connection),*reader=connect(connection);assert(writer && reader);
    assert(db_read_begin(master,NULL));assert(!db_read_begin(master,NULL));
    assert(db_read_begin(reader,db_read_snapshot(master)));
    PGresult *r=PQexec(writer,"SELECT pg_catalog.pg_current_xact_id()::text");
    assert(PQresultStatus(r)==PGRES_TUPLES_OK && PQntuples(r)==1);
    char xid[32];snprintf(xid,sizeof xid,"%s",PQgetvalue(r,0,0));PQclear(r);
    assert(!visible(master,xid) && !visible(reader,xid));
    r=db_exec(reader,"SELECT current_setting('transaction_read_only'),current_setting('transaction_isolation')");
    assert(PQresultStatus(r)==PGRES_TUPLES_OK && !strcmp(PQgetvalue(r,0,0),"on") && !strcmp(PQgetvalue(r,0,1),"repeatable read"));PQclear(r);
    assert(db_read_end(reader) && db_read_end(master));
    assert(PQtransactionStatus(reader)==PQTRANS_IDLE && !db_read_snapshot(reader));
    assert(db_read_begin(master,NULL) && db_read_begin(reader,db_read_snapshot(master)));
    assert(visible(master,xid) && visible(reader,xid));
    /* A read transaction refuses a write before it reaches a row (and this one would match none). */
    r=db_exec(reader,"DELETE FROM @extschema@.entity WHERE false");assert(PQresultStatus(r)==PGRES_FATAL_ERROR);PQclear(r);
    assert(db_read_end(reader) && PQtransactionStatus(reader)==PQTRANS_IDLE && db_read_end(master));
    assert(db_read_begin(reader,NULL));PQreset(reader);assert(PQstatus(reader)==CONNECTION_OK && !db_read_snapshot(reader));
    PQfinish(reader);PQfinish(writer);
}
static void stored_reads(PGconn *pg){
    char probe[512],sql[2048];probe_sql(probe,sizeof probe);
    command(pg,"BEGIN");
    snprintf(sql,sizeof sql,"INSERT INTO @extschema@.entity(id,tier,coord,hilbert) SELECT %s,"
        "pg_catalog.greatest(@extschema@.laplace_tier('%s'),@extschema@.laplace_tier('%s'))+1,"
        "@extschema@.laplace_coord('%s'),@extschema@.laplace_hilbert('%s')",probe,XW,YW,XW,XW);
    command(pg,sql);
    snprintf(sql,sizeof sql,"INSERT INTO @extschema@.physicality(entity,tier,hilbert,path) SELECT id,tier,hilbert,"
        "@extschema@.laplace_path(ARRAY[@extschema@.laplace_id('%s'),@extschema@.laplace_id('%s')])"
        " FROM @extschema@.entity WHERE id OPERATOR(@extschema@.=) %s",XW,YW,probe);
    command(pg,sql);
    snprintf(sql,sizeof sql,"SELECT @extschema@.laplace_id('%s'),@extschema@.laplace_id('%s')",XW,YW);
    PGresult *rows=ask_once(pg,sql,NULL);assert(PQntuples(rows)==1);
    lp_id xkey=*col_id(rows,0,0),ykey=*col_id(rows,0,1);PQclear(rows);
    snprintf(sql,sizeof sql,"SELECT id,tier,@postgis@.ST_X(coord),@postgis@.ST_Y(coord),"
        "@postgis@.ST_Z(coord),@postgis@.ST_M(coord) FROM @extschema@.entity WHERE id OPERATOR(@extschema@.=) %s",probe);
    rows=ask_once(pg,sql,NULL);
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
    lp_id child=lp_path_id(path,0);assert(lp_id_eq(&child,&xkey));
    assert(paths[0].path_len==paths[1].path_len && !memcmp(paths[0].path,paths[1].path,(size_t)paths[0].path_len));
    holds_free(paths,count);paths=paths_of(pg,NULL,0,&count);assert(!paths && !count);
    /* The holders of the probe's words: the probe alone, since no source holds either word. */
    lp_id absent={0};
    lp_id keys[3]={xkey,absent,xkey},both[2]={xkey,ykey};int hub[6];
    paths=holds_above(pg,both,2,-1,0,0,&count);
    assert(count==1 && paths[0].src==0 && !paths[0].claim && !paths[0].stood);
    holds_free(paths,count);
    paths=holds_any(pg,keys,3,&count);assert(count==1 && paths[0].src==-1);holds_free(paths,count);
    paths=holds_both(pg,keys,3,&ykey,&count);assert(count==2 && paths[0].src==0 && paths[1].src==2);holds_free(paths,count);
    assert(holds_count(pg,both,2,0)==1 && holds_count(pg,both,2,1)==0);
    paths=holds_capped(pg,keys,3,0,0,3,hub,&count);
    assert(!count && !hub[0] && hub[1] && !hub[2] && !hub[3] && !hub[4] && hub[5]);holds_free(paths,count);
    snprintf(sql,sizeof sql,"UPDATE @extschema@.physicality SET mask=@extschema@.laplace_mask_slot('kind','claim')"
        " WHERE entity OPERATOR(@extschema@.=) %s",probe);
    command(pg,sql);
    snprintf(sql,sizeof sql,"INSERT INTO @extschema@.consensus SELECT id,1550,60,0.06,5000000001 FROM @extschema@.entity"
        " WHERE id OPERATOR(@extschema@.=) %s",probe);
    command(pg,sql);
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
    if(argc!=2){fputs("usage: pg_plan_lifecycle conninfo\n",stderr);return 2;}
    const char *connection=argv[1];
    snprintf(XW,sizeof XW,"%s%s","lifecycleprobe","xqzalpha");
    snprintf(YW,sizeof YW,"%s%s","lifecycleprobe","xqzbeta");

    /* Connection-local plans, the SQL buffer a plan was prepared from, reset, and the caller's planner policy. */
    PGconn *a=connect(connection),*b=connect(connection);if(!a || !b)return 1;
    PGresult *setting=PQexec(a,"SET plan_cache_mode=force_custom_plan");
    assert(PQresultStatus(setting)==PGRES_COMMAND_OK);PQclear(setting);
    char query[64]="SELECT $1::int+1";
    answer(a,query,42);strcpy(query,"SELECT $1::int+2");answer(a,query,43);
    answer(b,query,43);
    setting=PQexec(a,"SHOW plan_cache_mode");
    assert(PQresultStatus(setting)==PGRES_TUPLES_OK && !strcmp(PQgetvalue(setting,0,0),"force_custom_plan"));PQclear(setting);
    PQreset(a);assert(PQstatus(a)==CONNECTION_OK);answer(a,query,43);
    PQfinish(a);PQfinish(b);
    for(int i=0;i<4;i++){a=connect(connection);assert(a);answer(a,query,43);PQfinish(a);}

    /* Snapshots, the installed binding on two connections and across a reset, and the exact-ID reads. */
    T0=lp_tier0_map(getenv("LAPLACE_TIER0"));assert(T0);
    a=connect(connection);b=connect(connection);assert(a && b);
    snapshot_scope(a,connection);
    assert(db_binding(a)->id_type==db_binding(b)->id_type && db_binding(a)->id_array==db_binding(b)->id_array);
    lp_id ids[2];lp_id_codepoint(65,ids);ids[1]=ids[0];Args arguments={0};arg_ids(&arguments,ids,2);
    binding_answer(a,&arguments);binding_answer(b,&arguments);binding_answer(a,&arguments);
    PQreset(b);assert(PQstatus(b)==CONNECTION_OK);binding_answer(b,&arguments);
    stored_reads(a);stored_reads(b);stored_reads(a);
    binding_answer(a,&arguments);
    args_free(&arguments);PQfinish(a);PQfinish(b);
    puts("pg-plan-lifecycle: connection-local plans, caller planner policy, reset, snapshots, the installed binding and exact-ID reads of a rolled-back probe passed");
    return 0;
}
