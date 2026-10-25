/* Exercise the actual client query and binary protocol on the entire Native
 * floor plus persisted higher IDs. This is a qualification payload, not a
 * production batch size. The floor is virtual in this freshly deployed DB. */
#define main engine_cli_main
#include "../src/main.c"
#undef main
#include "laplace/tier0.h"
#include <assert.h>

int main(int argc,char **argv){
    assert(argc==3);
    const lp_tier0_record *floor=lp_tier0_map(argv[2]);assert(floor);
    PGconn *pg=db_connect(argv[1]);
    PGresult *rows=PQexecParams(pg,"SELECT id FROM public.entity",0,NULL,NULL,NULL,NULL,1);
    assert(PQresultStatus(rows)==PGRES_TUPLES_OK);
    size_t stored=(size_t)PQntuples(rows);assert(stored>0);
    size_t count=LP_TIER0_COUNT+2*stored+1;
    lp_id *ids=malloc(count*sizeof *ids);assert(ids);
    for(size_t i=0;i<LP_TIER0_COUNT;i++)ids[i]=floor[i].id;
    for(size_t i=0;i<stored;i++){
        assert(PQgetlength(rows,(int)i,0)==sizeof(lp_id));
        memcpy(&ids[LP_TIER0_COUNT+2*i],PQgetvalue(rows,(int)i,0),sizeof(lp_id));
        ids[LP_TIER0_COUNT+2*i+1]=ids[LP_TIER0_COUNT+2*i];
    }
    ids[count-1]=ids[0];
    PQclear(rows);PQfinish(pg);
    uint8_t *hit=db_recorded(argv[1],ids,count);assert(hit);
    for(size_t i=0;i<LP_TIER0_COUNT;i++)assert(!hit[i]);
    for(size_t i=LP_TIER0_COUNT;i<count-1;i++)assert(hit[i]);
    assert(!hit[count-1]);
    printf("presence-frontier: %zu IDs, %zu persisted results including duplicates; virtual floor absent\n",count,2*stored);
    free(hit);free(ids);
    return 0;
}
