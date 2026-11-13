/* Exercise the actual client presence query and binary protocol (db_recorded: laplace_present_ids) on the entire
 * Native floor plus a check's own persisted higher IDs, each sent twice, plus IDs that cannot be stored, in one
 * frontier. This is a qualification payload, not a production batch size.
 *
 *   presence_frontier CONNINFO TIER0 IDS
 *
 * IDS is a file of the hex IDs the check composed and stored, one a line (tests/deployed/presence-frontier.py writes
 * the closure of its own trunk). Every one, and its duplicate, must be found. 4,096 random 128-bit values must not.
 * The database is shared, so the floor is not required to be absent: db_recorded makes no promise for tier 0 (its
 * membership belongs to the client's cache) beyond reporting what is stored. Each floor answer must equal an
 * independent indexed probe of public.entity by the same IDs, and the duplicate of the first floor ID at the end of
 * the frontier must agree with the first. */
#define main engine_cli_main
#include "../src/main.c"
#undef main
#include "laplace/tier0.h"
#include <assert.h>
#include <time.h>
#include <unistd.h>

enum { RANDOM_IDS = 4096 };

int main(int argc,char **argv){
    if(argc!=4){fputs("usage: presence_frontier CONNINFO TIER0 IDS\n",stderr);return 2;}
    const lp_tier0_record *floor=lp_tier0_map(argv[2]);assert(floor);
    FILE *file=fopen(argv[3],"r");if(!file){perror(argv[3]);return 2;}
    lp_vec(lp_id) stored={0};char line[128];
    while(fgets(line,sizeof line,file)){
        size_t n=strcspn(line,"\r\n");line[n]=0;if(!n)continue;
        lp_id id;if(n!=32 || !lp_id_unhex(line,&id)){fprintf(stderr,"%s: not an ID: %s\n",argv[3],line);return 2;}
        lp_push(&stored,id);
    }
    fclose(file);
    if(!stored.n){fputs("presence-frontier: no stored IDs given\n",stderr);return 2;}
    size_t at_stored=LP_TIER0_COUNT,at_random=at_stored+2*stored.n,count=at_random+RANDOM_IDS+1;
    lp_id *ids=malloc(count*sizeof *ids);assert(ids);
    for(size_t i=0;i<LP_TIER0_COUNT;i++)ids[i]=floor[i].id;
    for(size_t i=0;i<stored.n;i++)ids[at_stored+2*i]=ids[at_stored+2*i+1]=stored.v[i];
    /* 128 random bits: an ID no composition is expected to have. */
    srand((unsigned)time(NULL)^(unsigned)getpid());
    for(size_t i=0;i<RANDOM_IDS;i++)for(size_t b=0;b<sizeof(lp_id);b++)ids[at_random+i].b[b]=(uint8_t)(rand()>>7);
    ids[count-1]=ids[0];

    /* The independent answer for the floor: an indexed probe of public.entity by the floor's IDs. */
    PGconn *pg=db_connect(argv[1]);
    Args args={0};arg_ids(&args,ids,LP_TIER0_COUNT);
    PGresult *rows=ask_once(pg,"SELECT u.ordinal FROM unnest($1::@extschema@.blake3[]) WITH ORDINALITY u(id,ordinal)"
        " WHERE EXISTS (SELECT 1 FROM @extschema@.entity e WHERE e.id OPERATOR(@extschema@.=) u.id)",&args);
    uint8_t *floor_stored=calloc(LP_TIER0_COUNT,1);assert(floor_stored);
    for(int r=0;r<PQntuples(rows);r++){
        int64_t ordinal=(int64_t)lp_be(PQgetvalue(rows,r,0),8);assert(ordinal>0 && (uint64_t)ordinal<=LP_TIER0_COUNT);
        floor_stored[ordinal-1]=1;
    }
    size_t floor_present=(size_t)PQntuples(rows);
    PQclear(rows);args_free(&args);PQfinish(pg);

    uint8_t *hit=db_recorded(argv[1],ids,count);assert(hit);
    for(size_t i=0;i<LP_TIER0_COUNT;i++)
        if(hit[i]!=floor_stored[i]){fprintf(stderr,"presence-frontier: floor ID %zu: presence %u, stored %u\n",i,hit[i],floor_stored[i]);return 1;}
    for(size_t i=at_stored;i<at_random;i++)
        if(!hit[i]){char hex[33];lp_id_hex(ids+i,hex);fprintf(stderr,"presence-frontier: stored ID %s not found\n",hex);return 1;}
    for(size_t i=at_random;i<count-1;i++)
        if(hit[i]){fputs("presence-frontier: a random 128-bit value was found stored\n",stderr);return 1;}
    if(hit[count-1]!=hit[0]){fputs("presence-frontier: a repeated floor ID was answered differently\n",stderr);return 1;}
    printf("presence-frontier: %zu IDs, %zu stored results including duplicates, %d random absent;"
           " floor answers agree with the stored rows (%zu of %u floor IDs stored)\n",
           count,2*stored.n,RANDOM_IDS,floor_present,(unsigned)LP_TIER0_COUNT);
    free(hit);free(ids);free(floor_stored);lp_vec_free(&stored);
    return 0;
}
