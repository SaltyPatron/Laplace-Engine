#include "query.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/* Claim arrays own their complete Native-decoded operands in one trailing
 * arena. Filtering and sorting move descriptors, never truncate a tuple. */
Claim *claim_block(size_t count,size_t operands){
    size_t capacity=count?count:1;
    if(capacity>SIZE_MAX/sizeof(Claim) || operands>(SIZE_MAX-capacity*sizeof(Claim))/sizeof(lp_id)){
        fputs("claim result exceeds addressable memory\n",stderr);exit(1);
    }
    Claim *out=malloc(capacity*sizeof(Claim)+operands*sizeof(lp_id));
    if(!out){perror("claim result");exit(1);}return out;
}
Claim *claims_from_holds(const Hold *holds,int count,double k,int *n){
    size_t operands=0;lp_idmap *seen=lp_idmap_new();
    for(int i=0;i<count;i++)if(holds[i].claim && holds[i].stood){
        bool fresh;lp_idmap_put(seen,&holds[i].entity,&fresh);if(!fresh)continue;
        size_t length=lp_path_ids(holds[i].path,(size_t)holds[i].path_len,NULL,0);
        if(length>INT_MAX || length>SIZE_MAX-operands){fputs("claim operand count exceeds the reader's addressable range\n",stderr);exit(1);}
        operands+=length;
    }
    size_t unique=lp_idmap_count(seen);Claim *out=claim_block(unique,operands);
    lp_id *parts=(lp_id *)(out+(unique?unique:1));int used=0;lp_idmap_clear(seen);
    for(int i=0;i<count;i++)if(holds[i].claim && holds[i].stood){
        bool fresh;lp_idmap_put(seen,&holds[i].entity,&fresh);if(!fresh)continue;
        size_t length=lp_path_ids(holds[i].path,(size_t)holds[i].path_len,NULL,0);if(length<2)continue;
        lp_path_ids(holds[i].path,(size_t)holds[i].path_len,parts,length);
        out[used++]=(Claim){.id=holds[i].entity,.part=parts,.np=(int)length,.r=holds[i].r,
            .matches=holds[i].matches,.conf=lp_confidence(&holds[i].r,k)};
        parts+=length;
    }
    lp_idmap_free(seen);*n=used;return out;
}
