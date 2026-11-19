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
    for(size_t i=0;i<8;i++){
        const lp_list *list=lp_highway_list(highway,names[i]);
        if(!list){fprintf(stderr,"highway namespace missing: %s\n",names[i]);return 2;}
    }
    uint8_t digest[32];char fingerprint[65];static const char hex[]="0123456789abcdef";
    lp_highway_fingerprint(highway,digest);
    for(size_t i=0;i<32;i++){fingerprint[2*i]=hex[digest[i]>>4];fingerprint[2*i+1]=hex[digest[i]&15];}fingerprint[64]=0;
    PGconn *pg=db_read(conninfo);
    char err[512];PGresult *result=db_call_try(pg,LP_SQL_HIGHWAY_FINGERPRINT,NULL,err,sizeof err);
    if(!result || PQntuples(result)!=1 || PQgetisnull(result,0,0) || strcmp(col_str(result,0,0),fingerprint)){
        fprintf(stderr,"highway fingerprint differs between client and database: %s\n",result?"fingerprint mismatch":err);
        PQclear(result);PQfinish(pg);return 2;
    }
    PQclear(result);
    result=db_call_try(pg,LP_SQL_HIGHWAY_AUDIT,NULL,err,sizeof err);
    if(!result || PQntuples(result)!=8){
        fprintf(stderr,"highway joins: %s\n",result?"expected 8 namespace rows":err);
        PQclear(result);PQfinish(pg);return 2;
    }
    if(col_i64(result,0,9)!=0){
        fputs("highway joins: cyclic source lineage has no terminal root\n",stderr);PQclear(result);PQfinish(pg);return 2;
    }
    printf("highway joins   fingerprint=%s   independence=terminal-source-lineage\n",fingerprint);
    puts("crosswalk columns count source reachability over existing cache edges; they do not add attestations or change standings");
    puts("namespace\tcached\tattested\tindependent_ge2\tindependent_ge3\tindependent_ge4\tcrosswalk_1hop_ge2\tcrosswalk_1hop_ge3\tcrosswalk_1hop_ge4");
    for(int r=0;r<8;r++){
        printf("%s",col_str(result,r,0));
        for(int c=1;c<9;c++) printf("\t%lld",(long long)col_i64(result,r,c));
        putchar('\n');
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
