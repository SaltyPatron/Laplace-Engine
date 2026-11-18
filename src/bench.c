/* laplace bench: Native's own measurement of its operations (lp_bench: each SIMD kernel next to its scalar form), then
 * text to its trunk (Laplace::text); --joins measures cross-source joins on the highway. One line per operation:
 * rate, time per operation, and bandwidth where it applies. */
#include "engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile uint64_t sink;
static void row(const char *what, double ops, double secs, double bytes){
    double rate = ops / secs;
    const char *u = rate >= 1e9 ? "G" : rate >= 1e6 ? "M" : rate >= 1e3 ? "k" : "";
    double s = rate >= 1e9 ? 1e9 : rate >= 1e6 ? 1e6 : rate >= 1e3 ? 1e3 : 1;
    printf("  %-52s %9.2f %s/s   %9.1f ns/op", what, rate / s, u, secs / ops * 1e9);
    if (bytes > 0) printf("   %7.2f GB/s", bytes / secs / 1e9);
    putchar('\n');
}

/* Corpus independence is the terminal declared source lineage. Multiple
 * records or trust scopes in one corpus cannot mint additional witnesses. */
static int highway_joins(const char *conninfo){
    static const char *names[8]={"ili","Synset","fnframe","fnfe","fnlu","vnclass","pbroleset","vaframe"};
    const lp_highway *highway=lp_highway_map(lp_highway_path());
    if(!highway){fprintf(stderr,"cannot map highway: %s\n",lp_highway_path());return 2;}
    char counts[8][16];const char *parameters[8];
    for(size_t i=0;i<8;i++){
        const lp_list *list=lp_highway_list(highway,names[i]);
        if(!list){fprintf(stderr,"highway namespace missing: %s\n",names[i]);return 2;}
        snprintf(counts[i],sizeof counts[i],"%u",list->count);parameters[i]=counts[i];
    }
    uint8_t digest[32];char fingerprint[65];static const char hex[]="0123456789abcdef";
    lp_highway_fingerprint(highway,digest);
    for(size_t i=0;i<32;i++){fingerprint[2*i]=hex[digest[i]>>4];fingerprint[2*i+1]=hex[digest[i]&15];}fingerprint[64]=0;
    PGconn *pg=db_read(conninfo);PGresult *result=db_exec(pg,"SELECT @extschema@.laplace_highway_fingerprint()");
    if(PQresultStatus(result)!=PGRES_TUPLES_OK || PQntuples(result)!=1 || PQgetisnull(result,0,0) || strcmp(PQgetvalue(result,0,0),fingerprint)){
        fprintf(stderr,"highway fingerprint differs between client and database: %s",PQerrorMessage(pg));PQclear(result);PQfinish(pg);return 2;
    }
    PQclear(result);
    /* Native performs geometry decoding, cache lookup and containment walks.
     * SQL groups complete sets; provenance calls use bounded claim arrays. */
    static const char sql[]=
      "WITH RECURSIVE namespaces(ord,name,slots) AS (VALUES "
      "(1,'ili',$1::int),(2,'Synset',$2::int),(3,'fnframe',$3::int),(4,'fnfe',$4::int),"
      "(5,'fnlu',$5::int),(6,'vnclass',$6::int),(7,'pbroleset',$7::int),(8,'vaframe',$8::int)),"
      "members AS MATERIALIZED (SELECT n.name,i slot,@extschema@.laplace_type_id(n.name,i) id FROM namespaces n "
      "CROSS JOIN LATERAL generate_series(0,n.slots-1) i),"
      "joined AS MATERIALIZED (SELECT DISTINCT m.name,m.id,c.claim FROM @extschema@.consensus c "
      "JOIN @extschema@.physicality p ON p.entity OPERATOR(@extschema@.=) c.claim CROSS JOIN LATERAL unnest(@extschema@.laplace_vertex_ids(p.path)) v(id) "
      "JOIN members m ON m.id OPERATOR(@extschema@.=) v.id),"
      "numbered AS (SELECT claim,(row_number() OVER(ORDER BY claim)-1)/4096 batch FROM (SELECT DISTINCT claim FROM joined) q),"
      "batches AS (SELECT array_agg(claim ORDER BY claim) claims FROM numbered GROUP BY batch),"
      "witness_roots AS MATERIALIZED (SELECT DISTINCT ON (witness) witness AS id,lineage FROM @extschema@.attestation ORDER BY witness,lineage NULLS FIRST),"
      "observed AS MATERIALIZED (SELECT DISTINCT a.claim,a.trunk,coalesce(w.lineage,a.trunk) initial FROM batches b "
      "CROSS JOIN LATERAL @extschema@.laplace_attested_series(b.claims) a JOIN witness_roots w ON w.id OPERATOR(@extschema@.=) a.trunk WHERE a.witness AND a.games>0),"
      "ancestry(initial,root) AS (SELECT DISTINCT initial,initial FROM observed UNION "
      "SELECT a.initial,w.lineage FROM ancestry a JOIN witness_roots w ON w.id OPERATOR(@extschema@.=) a.root WHERE w.lineage IS NOT NULL AND w.lineage OPERATOR(@extschema@.<>) a.root),"
      "terminal AS (SELECT a.initial,a.root FROM ancestry a LEFT JOIN witness_roots w ON w.id OPERATOR(@extschema@.=) a.root "
      "WHERE w.id IS NULL OR w.lineage IS NULL OR w.lineage OPERATOR(@extschema@.=) a.root),"
      "invalid AS (SELECT count(*) n FROM (SELECT DISTINCT initial FROM observed) o WHERE NOT EXISTS(SELECT FROM terminal t WHERE t.initial OPERATOR(@extschema@.=) o.initial)),"
      "support AS MATERIALIZED (SELECT DISTINCT j.name,j.id,t.root FROM joined j JOIN observed o ON o.claim OPERATOR(@extschema@.=) j.claim "
      "JOIN terminal t ON t.initial OPERATOR(@extschema@.=) o.initial),"
      "supported AS (SELECT name,id,count(*) roots FROM support GROUP BY name,id),"
      /* Cache edges describe correspondences, not additional attestations.
       * One hop is explicit: never flood a connected component with support. */
      "edges AS MATERIALIZED (SELECT DISTINCT s.name,s.id,n.name target,@extschema@.laplace_type_id(n.name,e.slot) target_id "
      "FROM supported s JOIN members m ON m.name=s.name AND m.id OPERATOR(@extschema@.=) s.id CROSS JOIN namespaces n "
      "CROSS JOIN LATERAL unnest(@extschema@.laplace_type_edges(s.name,m.slot,n.name)) e(slot)),"
      "reachable AS (SELECT name,id,root FROM support UNION "
      "SELECT e.target,e.target_id,s.root FROM edges e JOIN support s ON s.name=e.name AND s.id OPERATOR(@extschema@.=) e.id),"
      "crosswalk AS (SELECT name,id,count(DISTINCT root) roots FROM reachable WHERE id IS NOT NULL GROUP BY name,id),"
      "hops AS (SELECT name,count(*) FILTER(WHERE roots>=2) ge2,count(*) FILTER(WHERE roots>=3) ge3,"
      "count(*) FILTER(WHERE roots>=4) ge4 FROM crosswalk GROUP BY name) "
      "SELECT n.name,n.slots,count(s.id),count(s.id) FILTER(WHERE s.roots>=2),count(s.id) FILTER(WHERE s.roots>=3),"
      "count(s.id) FILTER(WHERE s.roots>=4),invalid.n,coalesce(h.ge2,0),coalesce(h.ge3,0),coalesce(h.ge4,0) "
      "FROM namespaces n LEFT JOIN supported s ON s.name=n.name LEFT JOIN hops h ON h.name=n.name CROSS JOIN invalid "
      "GROUP BY n.ord,n.name,n.slots,invalid.n,h.ge2,h.ge3,h.ge4 ORDER BY n.ord";
    result=db_exec_params(pg,sql,8,NULL,parameters,NULL,NULL,0);
    if(PQresultStatus(result)!=PGRES_TUPLES_OK || PQntuples(result)!=8){
        fprintf(stderr,"highway joins: %s",PQerrorMessage(pg));PQclear(result);PQfinish(pg);return 2;
    }
    if(strcmp(PQgetvalue(result,0,6),"0")){
        fputs("highway joins: cyclic source lineage has no terminal root\n",stderr);PQclear(result);PQfinish(pg);return 2;
    }
    printf("highway joins   fingerprint=%s   independence=terminal-source-lineage\n",fingerprint);
    puts("crosswalk columns count source reachability over existing cache edges; they do not add attestations or change standings");
    puts("namespace\tcached\tattested\tindependent_ge2\tindependent_ge3\tindependent_ge4\tcrosswalk_1hop_ge2\tcrosswalk_1hop_ge3\tcrosswalk_1hop_ge4");
    for(int r=0;r<8;r++){
        for(int c=0;c<10;c++){if(c==6)continue;printf("%s%s",c?"\t":"",PQgetvalue(result,r,c));}putchar('\n');
    }
    PQclear(result);PQfinish(pg);return 0;
}
int cmd_highway_joins(int argc,char **argv){
    const char *conninfo=laplace_db();
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--joins"))continue;
        if(!strcmp(argv[i],"-d") && i+1<argc){conninfo=argv[++i];continue;}
        fprintf(stderr,"usage: laplace %s --joins [-d conninfo]\n",argv[0]);return 2;
    }
    return highway_joins(conninfo);
}
int cmd_bench(int argc, char **argv){
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"--joins"))return cmd_highway_joins(argc,argv);
    lp_bench();                                       /* Native's operations, each kernel at every ISA level */

    printf("\ntext\n");
    { const lp_tier0_record *t0 = lp_tier0_map(NULL);
      if (!t0) printf("  tier 0 is not at %s: text is not measured\n", lp_tier0_path());
      else {
          lp_id *ids = malloc(sizeof(lp_id) * LP_NCP);
          for (uint32_t cp = 0; cp < LP_NCP; cp++) lp_id_codepoint(cp, &ids[cp]);
          lp_text *tx = lp_text_new(t0); static const char para[] = "It is a truth universally acknowledged, that a single man in possession of a good fortune, must be in want of a wife. However little known the feelings or views of such a man may be on his first entering a neighbourhood, this truth is so well fixed in the minds of the surrounding families, that he is considered the rightful property of some one or other of their daughters.";
          size_t len = sizeof para - 1; int reps = 20000; double t;
          t = now(); for (int r = 0; r < reps; r++) sink += lp_text_decompose(tx, (const uint8_t *)para, len, NULL, NULL).id.b[0];
          row("text to its trunk: ID and coordinate (per call)", reps, now() - t, (double)len * reps);
          t = now(); for (uint32_t cp = 0; cp < LP_NCP; cp++) sink += (uint64_t)lp_tier0_codepoint(t0, &ids[cp]); row("codepoint of an ID", LP_NCP, now() - t, 0);
          lp_text_free(tx); free(ids);
      }
    }
    return 0;
}
