/* Production Native claim decoding: complete operands, unique identity and ownership. */
#include "query.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void){
    enum { N=2048 };
    lp_id *ids=malloc(N*sizeof *ids);assert(ids);
    for(size_t i=0;i<N;i++)lp_id_codepoint((uint32_t)(1024+i),ids+i);
    size_t bytes=lp_ewkb_path(ids,N,NULL,0);uint8_t *path=malloc(bytes);assert(path);
    assert(lp_ewkb_path(ids,N,path,bytes)==bytes);
    Hold h={.claim=1,.stood=1,.path=path,.path_len=(int)bytes,.r=lp_rating_stock(),.matches=7};
    lp_id_compose(ids,N,&h.entity);
    Hold rows[]={h,h,h,h};rows[2].claim=0;rows[3].stood=0;
    int count=0;Claim *claims=claims_from_holds(rows,4,2,&count);
    assert(count==1 && claims[0].np==N && claims[0].matches==7 && lp_id_eq(&claims[0].id,&h.entity));
    free(path); /* The result must retain no borrowed operand storage. */
    assert(!memcmp(claims[0].part,ids,N*sizeof *ids));
    Claim *copy=claim_block(1,N);copy[0]=claims[0];lp_id *owned=(lp_id *)(copy+1);
    memcpy(owned,claims[0].part,N*sizeof *owned);copy[0].part=owned;free(claims);
    assert(!memcmp(copy[0].part,ids,N*sizeof *ids));
    free(copy);free(ids);
    puts("claim-decode: 2048 operands, duplicate holders, carrier filters and independent result ownership passed");
    return 0;
}
