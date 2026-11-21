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
    char err[512] = "";
    PGconn *pg=db_try_err(connection, err, sizeof err);
    if(!pg){fprintf(stderr,"%s\n",err);return NULL;}
    return pg;
}
static void answer(PGconn *pg,LpSqlId id,int expected){
    Args a={0};arg_i32(&a,41);
    PGresult *result=db_call(pg,id,&a);
    args_free(&a);
    assert(PQntuples(result)==1 && PQgetlength(result,0,0)==4);
    assert(col_i32(result,0,0)==expected);PQclear(result);
}
static void binding_answer(PGconn *pg,Args *arguments,const lp_id *probe_id){
    char err[256];
    const DbBinding *binding=db_contract_binding(pg,err,sizeof err);
    assert(binding && binding->id_type);
    PGresult *r=db_call(pg,LP_SQL_TEST_ECHO_IDS,arguments);
    assert(PQntuples(r)==1 && PQftype(r,0)==binding->id_array);
    lp_id ids[2];assert(lp_pg_ids_read((const uint8_t *)PQgetvalue(r,0,0),(size_t)PQgetlength(r,0,0),ids,2)==2);
    assert(lp_id_eq(ids,ids+1));PQclear(r);
    r=db_call(pg,LP_SQL_TEST_BINDING_TYPES,NULL);
    assert(PQntuples(r)==1 && col_oid(r,0,0)==binding->id_type && col_bool(r,0,1));PQclear(r);
    /* The bound namespace reaches the stored table; the probe is no row of it outside its own transactions. */
    Args pa={0};arg_id(&pa,probe_id);
    r=db_call(pg,LP_SQL_TEST_PROBE_COUNT,&pa);
    args_free(&pa);
    assert(PQntuples(r)==1 && col_i64(r,0,0)==0);PQclear(r);
}
/* Whether a transaction ID is visible in the snapshot the connection's transaction reads. */
static int visible(PGconn *pg,int64_t xid){
    Args a={0};arg_i64(&a,xid);
    PGresult *r=db_call(pg,LP_SQL_TEST_VISIBLE_XACT,&a);
    args_free(&a);
    assert(PQntuples(r)==1);
    int seen=col_bool(r,0,0);PQclear(r);return seen;
}
/* A read snapshot is exported by one connection and imported by another; a transaction committed after it is seen by
 * neither until both begin again. The committed transaction is one that only takes an ID: no table is written. */
static void snapshot_scope(PGconn *master,const char *connection){
    PGconn *writer=connect(connection),*reader=connect(connection);assert(writer && reader);
    assert(db_read_begin(master,NULL));assert(!db_read_begin(master,NULL));
    assert(db_read_begin(reader,db_read_snapshot(master)));
    PGresult *r=db_call(writer,LP_SQL_TEST_CURRENT_XACT_TEXT,NULL);
    assert(PQntuples(r)==1);
    int64_t xid=(int64_t)strtoull(col_str(r,0,0),NULL,10);PQclear(r);
    assert(!visible(master,xid) && !visible(reader,xid));
    r=db_call(reader,LP_SQL_TEST_READ_SETTINGS,NULL);
    assert(PQntuples(r)==1 && !strcmp(col_str(r,0,0),"on") && !strcmp(col_str(r,0,1),"repeatable read"));PQclear(r);
    assert(db_read_end(reader) && db_read_end(master));
    assert(PQtransactionStatus(reader)==PQTRANS_IDLE && !db_read_snapshot(reader));
    assert(db_read_begin(master,NULL) && db_read_begin(reader,db_read_snapshot(master)));
    assert(visible(master,xid) && visible(reader,xid));
    /* A read transaction refuses a write before it reaches a row (and this one would match none). */
    char err[256];
    assert(!db_call_try(reader,LP_SQL_TEST_FORBIDDEN_DELETE,NULL,err,sizeof err));
    assert(db_read_end(reader) && PQtransactionStatus(reader)==PQTRANS_IDLE && db_read_end(master));
    assert(db_read_begin(reader,NULL));PQreset(reader);assert(PQstatus(reader)==CONNECTION_OK && !db_read_snapshot(reader));
    PQfinish(reader);PQfinish(writer);
}
static void stored_reads(PGconn *pg){
    db_tx(pg,LP_SQL_TX_BEGIN);
    Args pa={0};arg_text(&pa,XW);arg_text(&pa,YW);
    PGresult *probe=db_call(pg,LP_SQL_TEST_PROBE_COMPOSED,&pa);
    args_free(&pa);
    assert(PQntuples(probe)==1);
    lp_id xkey=*col_id(probe,0,0),ykey=*col_id(probe,0,1),known=*col_id(probe,0,2);
    int16_t tier=col_i16(probe,0,3);
    double expected[4];for(int d=0;d<4;d++)expected[d]=lp_be_f64(PQgetvalue(probe,0,4+d));
    int64_t hilbert=col_i64(probe,0,8);
    {
        Args ea={0};arg_id(&ea,&known);arg_i16(&ea,tier);
        for(int d=0;d<4;d++)arg_f64_bin(&ea,expected[d]);
        arg_i64(&ea,hilbert);
        db_exec_contract(pg,LP_SQL_TEST_INSERT_PROBE_ENTITY,&ea);
        args_free(&ea);
    }
    {
        Args xa={0};arg_id(&xa,&known);arg_i16(&xa,tier);arg_i64(&xa,hilbert);
        arg_raw(&xa,PQgetvalue(probe,0,9),(size_t)PQgetlength(probe,0,9));
        db_exec_contract(pg,LP_SQL_TEST_INSERT_PROBE_PATH,&xa);
        args_free(&xa);
    }
    PQclear(probe);
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
    {
        Args ma={0};arg_id(&ma,&known);
        db_exec_contract(pg,LP_SQL_TEST_MARK_PROBE_CLAIM,&ma);
        args_free(&ma);
    }
    {
        Args ca={0};arg_id(&ca,&known);arg_f64_bin(&ca,1550.0);arg_f64_bin(&ca,60.0);arg_f64_bin(&ca,0.06);arg_i64(&ca,5000000001LL);
        db_exec_contract(pg,LP_SQL_TEST_INSERT_PROBE_CONSENSUS,&ca);
        args_free(&ca);
    }
    paths=holds_capped(pg,keys,3,1,1,3,hub,&count);
    assert(count==2 && paths[0].src==0 && paths[1].src==2 && paths[0].claim && paths[0].stood);
    assert(paths[0].r.rating==1550 && paths[0].r.deviation==60 && paths[0].r.volatility==0.06 && paths[0].matches==5000000001LL);
    assert(!hub[0] && !hub[1] && !hub[4] && !hub[5]);holds_free(paths,count);
    paths=holds_capped(pg,keys,3,0,1,1,hub,&count);
    assert(!count && hub[0] && !hub[1] && hub[4] && !hub[5]);holds_free(paths,count);
    paths=holds_above(pg,keys,3,tier,1,1,&count);assert(!count);holds_free(paths,count);
    db_tx(pg,LP_SQL_TX_ROLLBACK);
}
int main(int argc,char **argv){
    if(argc!=2){fputs("usage: pg_plan_lifecycle conninfo\n",stderr);return 2;}
    const char *connection=argv[1];
    snprintf(XW,sizeof XW,"%s%s","lifecycleprobe","xqzalpha");
    snprintf(YW,sizeof YW,"%s%s","lifecycleprobe","xqzbeta");

    /* Connection-local plans, contract statement identity, reset, and the caller's planner policy. */
    PGconn *a=connect(connection),*b=connect(connection);if(!a || !b)return 1;
    db_exec_contract(a,LP_SQL_TEST_PLAN_MODE_CUSTOM,NULL);
    answer(a,LP_SQL_TEST_ADD,42);answer(a,LP_SQL_TEST_ADD2,43);
    answer(b,LP_SQL_TEST_ADD2,43);
    PGresult *setting=db_call(a,LP_SQL_TEST_PLAN_MODE_SHOW,NULL);
    assert(PQntuples(setting)==1 && !strcmp(col_str(setting,0,0),"force_custom_plan"));PQclear(setting);
    PQreset(a);assert(PQstatus(a)==CONNECTION_OK);answer(a,LP_SQL_TEST_ADD2,43);
    PQfinish(a);PQfinish(b);
    for(int i=0;i<4;i++){a=connect(connection);assert(a);answer(a,LP_SQL_TEST_ADD2,43);PQfinish(a);}

    /* Snapshots, the installed binding on two connections and across a reset, and the exact-ID reads. */
    T0=lp_tier0_map(getenv("LAPLACE_TIER0"));assert(T0);
    a=connect(connection);b=connect(connection);assert(a && b);
    snapshot_scope(a,connection);
    char err[256];
    const DbBinding *ba=db_contract_binding(a,err,sizeof err),*bb=db_contract_binding(b,err,sizeof err);
    assert(ba && bb && ba->id_type==bb->id_type && ba->id_array==bb->id_array);
    lp_id probe_id={0};
    {
        Args pa={0};arg_text(&pa,XW);arg_text(&pa,YW);
        PGresult *pr=db_call(a,LP_SQL_TEST_PROBE_COMPOSED,&pa);
        args_free(&pa);
        assert(PQntuples(pr)==1);
        probe_id=*col_id(pr,0,2);
        PQclear(pr);
    }
    lp_id ids[2];lp_id_codepoint(65,ids);ids[1]=ids[0];Args arguments={0};arg_ids(&arguments,ids,2);
    binding_answer(a,&arguments,&probe_id);binding_answer(b,&arguments,&probe_id);binding_answer(a,&arguments,&probe_id);
    PQreset(b);assert(PQstatus(b)==CONNECTION_OK);binding_answer(b,&arguments,&probe_id);
    stored_reads(a);stored_reads(b);stored_reads(a);
    binding_answer(a,&arguments,&probe_id);
    args_free(&arguments);PQfinish(a);PQfinish(b);
    puts("pg-plan-lifecycle: connection-local plans, caller planner policy, reset, snapshots, the installed binding and exact-ID reads of a rolled-back probe passed");
    return 0;
}
