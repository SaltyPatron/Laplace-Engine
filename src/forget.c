/* Forgetting removes witness authority and only the unreferenced part of its
 * structural closure. All discovery, sweeping and Native series updates share
 * one database transaction; no partition is exported to client RAM. */
#include "engine.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void candidates_open(PGconn *pg){
    exec(pg,"CREATE TEMP TABLE forget_candidates(id @extschema@.blake3 PRIMARY KEY) ON COMMIT DROP;"
        " CREATE TEMP TABLE forget_round(id @extschema@.blake3 PRIMARY KEY) ON COMMIT DROP;"
        " CREATE TEMP TABLE forget_stats(deleted bigint NOT NULL) ON COMMIT DROP;"
        " INSERT INTO forget_stats VALUES(0)");
}
static uint64_t sweep_candidates(PGconn *pg){
    /* Who still holds a candidate is asked of the containment index (laplace_holders), 4096 candidates a call, unless
     * the candidates are a large share of what physicality holds (a sweep of every entity): then one pass over every
     * path is cheaper. A whole-table pass per round for the closure of one small source made forgetting it take as
     * long as reading every path of a populated database, once for each round of the sweep. */
    exec(pg,"DO $sweep$ DECLARE n bigint; c_cnt bigint; total bigint; ids @extschema@.blake3[]; BEGIN"
        " SELECT coalesce(sum(c.reltuples), 0)::bigint INTO total FROM pg_catalog.pg_inherits i"
        " JOIN pg_catalog.pg_class c ON c.oid = i.inhrelid WHERE i.inhparent = '@extschema@.physicality'::regclass;"
        " LOOP"
        " TRUNCATE forget_round;"
        " SELECT count(*) INTO c_cnt FROM forget_candidates;"
        " IF c_cnt = 0 THEN EXIT; END IF;"
        " IF c_cnt <= 4096 OR c_cnt * 64 < total THEN"
        "  FOR ids IN SELECT array_agg(s.id ORDER BY s.id) FROM (SELECT id, (row_number() OVER (ORDER BY id) - 1) / 4096 AS g"
        "   FROM forget_candidates) s GROUP BY s.g LOOP"
        "   WITH h AS MATERIALIZED (SELECT ordinal, entity, hub FROM @extschema@.laplace_holders(ids, '{}'::@extschema@.blake3[], true, -1::smallint, 2::bigint, 3::smallint)),"
        "        referenced AS MATERIALIZED (SELECT DISTINCT ids[h.ordinal::integer] AS id FROM h WHERE h.hub OR h.entity OPERATOR(@extschema@.<>) ids[h.ordinal::integer])"
        "   INSERT INTO forget_round SELECT c.id FROM unnest(ids) c(id)"
        "   WHERE NOT EXISTS(SELECT FROM @extschema@.attestation w WHERE w.witness OPERATOR(@extschema@.=) c.id OR w.lineage OPERATOR(@extschema@.=) c.id)"
        "   AND NOT EXISTS(SELECT FROM referenced r WHERE r.id OPERATOR(@extschema@.=) c.id);"
        "  END LOOP;"
        " ELSE"
        "   WITH referenced AS MATERIALIZED (SELECT DISTINCT v.id FROM @extschema@.physicality p"
        "   CROSS JOIN LATERAL unnest(@extschema@.laplace_vertex_ids(p.path)) v(id)"
        "   JOIN forget_candidates c ON c.id OPERATOR(@extschema@.=) v.id WHERE p.entity OPERATOR(@extschema@.<>) v.id)"
        "   INSERT INTO forget_round SELECT c.id FROM forget_candidates c"
        "   WHERE NOT EXISTS(SELECT FROM @extschema@.attestation w WHERE w.witness OPERATOR(@extschema@.=) c.id OR w.lineage OPERATOR(@extschema@.=) c.id)"
        "   AND NOT EXISTS(SELECT FROM referenced r WHERE r.id OPERATOR(@extschema@.=) c.id);"
        " END IF;"
        " SELECT count(*) INTO n FROM forget_round; EXIT WHEN n=0;"
        " DELETE FROM @extschema@.consensus c USING forget_round d WHERE c.claim OPERATOR(@extschema@.=) d.id;"
        " DELETE FROM @extschema@.physicality p USING forget_round d WHERE p.entity OPERATOR(@extschema@.=) d.id;"
        " DELETE FROM @extschema@.entity e USING forget_round d WHERE e.id OPERATOR(@extschema@.=) d.id;"
        " DELETE FROM forget_candidates c USING forget_round d WHERE c.id OPERATOR(@extschema@.=) d.id;"
        " UPDATE forget_stats SET deleted=deleted+n;"
        " END LOOP; END $sweep$");
    PGresult *q=ask_once(pg,"SELECT deleted FROM forget_stats",NULL);uint64_t n=lp_be(PQgetvalue(q,0,0),8);PQclear(q);return n;
}
static int forget_ids(PGconn *pg,const lp_id *roots,size_t n){
    if(!n)return 1;
    candidates_open(pg);Args args={0};arg_ids(&args,roots,n);
    PQclear(ask_once(pg,"WITH RECURSIVE below(id) AS (SELECT unnest($1::@extschema@.blake3[]) UNION"
        " SELECT child FROM below b JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) b.id"
        " CROSS JOIN LATERAL unnest(@extschema@.laplace_vertex_ids(p.path)) child)"
        " INSERT INTO forget_candidates SELECT b.id FROM below b JOIN @extschema@.entity e ON e.id OPERATOR(@extschema@.=) b.id",&args));
    PGresult *q=ask_once(pg,"SELECT p.entity FROM @extschema@.physicality p JOIN forget_candidates c ON c.id OPERATOR(@extschema@.=) p.entity"
        " WHERE @extschema@.laplace_mask_has_value(p.mask,'kind','claim') AND @extschema@.laplace_path_length(p.path)>=2"
        " UNION SELECT s.claim FROM @extschema@.consensus s JOIN forget_candidates c ON c.id OPERATOR(@extschema@.=) s.claim",NULL);
    size_t nc=(size_t)PQntuples(q);lp_id *claims=malloc((nc?nc:1)*sizeof *claims);
    if(!claims){perror("affected claim IDs");exit(1);}
    for(size_t i=0;i<nc;i++)claims[i]=*col_id(q,(int)i,0);PQclear(q);
    PQclear(ask_once(pg,"DELETE FROM @extschema@.attestation WHERE witness OPERATOR(@extschema@.=) ANY($1::@extschema@.blake3[])",&args));args_free(&args);
    /* The Native solver validates claim paths. Remove witness authority and
     * recompute while that closure is still present, then sweep its orphans. */
    uint64_t standing=0,orphaned=0;
    standings_replay(pg,claims,nc,&standing,&orphaned);free(claims);
    uint64_t removed=sweep_candidates(pg);
    printf("  %llu witnesses removed; %llu unreferenced entities removed; %llu remaining standings recomputed; %llu claims without a witness\n",
        (unsigned long long)n,(unsigned long long)removed,(unsigned long long)standing,(unsigned long long)orphaned);
    return 0;
}
static void begin_maintenance(PGconn *pg){
    exec(pg,"BEGIN ISOLATION LEVEL REPEATABLE READ;"
        " SELECT pg_advisory_xact_lock(1279348803,-2);"
        " LOCK TABLE @extschema@.attestation IN SHARE ROW EXCLUSIVE MODE");
}
int cmd_sweep(int argc,char **argv){
    const char *conninfo=laplace_db();int limit=0,dry=0;
    int a=opts(argc,argv,(const Opt[]){{.flag="-d",.kind='s',.at=&conninfo},{.flag="-j",.kind='i',.at=&limit},
        {.flag="--dry",.kind='b',.at=&dry},{0}});
    if(a<argc||limit<0){fputs("usage: laplace sweep [-d conninfo] [-j connection-cap] [--dry]\n",stderr);return 2;}
    PGconn *pg=db_connect(conninfo);begin_maintenance(pg);candidates_open(pg);
    exec(pg,"INSERT INTO forget_candidates SELECT id FROM @extschema@.entity");
    uint64_t n=sweep_candidates(pg);exec(pg,dry?"ROLLBACK":"COMMIT");
    printf("laplace sweep   %s   %llu unreferenced entities %s\n",PQdb(pg),(unsigned long long)n,dry?"would be removed":"removed");
    PQfinish(pg);return 0;
}
static lp_id *selected_roots(PGconn *pg,const lp_id *ids,const lp_id *records,size_t n,int except,size_t *count){
    Args args={0};arg_ids(&args,ids,n);arg_ids(&args,records,n);uint8_t invert=except!=0;arg_raw(&args,&invert,1);
    PGresult *q=ask_once(pg,"SELECT w.id FROM (SELECT DISTINCT witness AS id FROM @extschema@.attestation) w JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) w.id"
        " WHERE (w.id OPERATOR(@extschema@.=) ANY($1::@extschema@.blake3[]) OR EXISTS(SELECT FROM unnest($2::@extschema@.blake3[]) r(id)"
        " WHERE @extschema@.laplace_path_starts(p.path,r.id)))<>$3::boolean"
        /* A named source that attests nothing is no witness, yet its trunk is recorded: the paths that begin with its
         * record, for a record that begins no witness trunk. */
        " UNION SELECT p.entity FROM unnest($2::@extschema@.blake3[]) r(id) JOIN @extschema@.physicality p"
        " ON p.path OPERATOR(@extschema@.^@) r.id WHERE NOT $3::boolean AND NOT EXISTS("
        "  SELECT FROM (SELECT DISTINCT witness AS id FROM @extschema@.attestation) w JOIN @extschema@.physicality q"
        "  ON q.entity OPERATOR(@extschema@.=) w.id WHERE q.path OPERATOR(@extschema@.^@) r.id)"
        " ORDER BY 1",&args);
    size_t nr=(size_t)PQntuples(q);lp_id *roots=malloc((nr?nr:1)*sizeof *roots);if(!roots){perror("witness IDs");exit(1);}
    for(size_t i=0;i<nr;i++)roots[i]=*col_id(q,(int)i,0);*count=nr;PQclear(q);args_free(&args);return roots;
}
int cmd_forget(int argc,char **argv){
    const char *conninfo=laplace_db();int limit=0,except=0,trunk=0;
    int a=opts(argc,argv,(const Opt[]){{.flag="-d",.kind='s',.at=&conninfo},{.flag="-j",.kind='i',.at=&limit},
        {.flag="--except",.kind='b',.at=&except},{.flag="--trunk",.kind='b',.at=&trunk},{0}});
    if((a>=argc&&!except)||limit<0){fputs("usage: laplace forget [-d conninfo] [-j connection-cap] [--except] [--trunk] source-or-trunk...\n",stderr);return 2;}
    tier0_open(NULL);Ctx *ctx=lp_text_new(T0);size_t n=(size_t)(argc-a);
    lp_id *ids=calloc(n?n:1,sizeof *ids),*records=calloc(n?n:1,sizeof *records);
    if(!ids||!records){perror("source selectors");exit(1);}
    Recipe *recipes=NULL;recipes_load(laplace_recipes(),&recipes);int ns=0;Source *sources=sources_loaded(&ns);
    for(size_t i=0;i<n;i++){
        const char *name=argv[a+(int)i];int parsed=id_parse(name,&ids[i]);
        if(trunk&&!parsed){fprintf(stderr,"%s: not a trunk ID\n",name);free(ids);free(records);lp_text_free(ctx);return 2;}
        if(!trunk){
            for(int j=0;j<ns;j++)if(!strcmp(name,sources[j].name)){const char *called=source_called(&sources[j]);if(called)name=called;break;}
            records[i]=lp_text_decompose(ctx,(const uint8_t *)name,strlen(name),NULL,NULL).id;
        }
    }
    lp_text_free(ctx);PGconn *pg=db_connect(conninfo);begin_maintenance(pg);
    size_t nr=0;lp_id *roots=selected_roots(pg,ids,records,n,except,&nr);free(ids);free(records);
    int result=forget_ids(pg,roots,nr);free(roots);
    exec(pg,result?"ROLLBACK":"COMMIT");
    if(result)fputs("laplace forget: no registered source trunk matched\n",stderr);
    PQfinish(pg);return result;
}
int forget_superseded(const char *conninfo,int connection_cap,const lp_id *trunk){
    (void)connection_cap;PGconn *pg=db_connect(conninfo);begin_maintenance(pg);
    Reader *reader=reader_new(pg);lp_id first[2];size_t n=reader_parts(reader,trunk,first,2);reader_free(reader);
    if(!n){exec(pg,"ROLLBACK");PQfinish(pg);return 1;}
    lp_id unused={{0}};size_t nr=0;lp_id *all=selected_roots(pg,&unused,first,1,0,&nr);size_t kept=0;
    for(size_t i=0;i<nr;i++)if(!lp_id_eq(&all[i],trunk))all[kept++]=all[i];
    int result=kept?forget_ids(pg,all,kept):0;free(all);exec(pg,result?"ROLLBACK":"COMMIT");PQfinish(pg);return result;
}
