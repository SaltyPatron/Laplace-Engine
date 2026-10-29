/* Connection-owned exact-ID reads over the shared Native PostgreSQL owner. */
#include "engine.h"
#include <stdlib.h>
#include <string.h>

int hold_order(const void *a, const void *b){
    const Hold *x = a, *y = b; int c = memcmp(x->entity.b, y->entity.b, 16);
    return c ? c : x->src != y->src ? (x->src < y->src ? -1 : 1) : 0;
}
void tiers_of(PGconn *pg,const lp_id *ids,int n,int *tiers){
    if(n<=0)return;
    lp_idmap *wanted=lp_idmap_new();
    for(int i=0;i<n;i++){
        tiers[i]=lp_tier0_codepoint(T0,ids+i)>=0?0:-1;
        if(tiers[i]<0)lp_idmap_put(wanted,ids+i,NULL);
    }
    size_t count=lp_idmap_count(wanted);int *found=malloc(sizeof(int)*(count?count:1));
    for(size_t i=0;i<count;i++)found[i]=-1;Args args={0};
    for(size_t at=0;at<count;){
        size_t amount=count-at;if(amount>DB_ID_WINDOW)amount=DB_ID_WINDOW;
        args_reset(&args);arg_ids(&args,lp_idmap_keys(wanted)+at,amount);
        PGresult *rows=ask(pg,"SELECT ordinal,tier FROM @extschema@.laplace_entities($1::@extschema@.blake3[])",&args);
        for(int row=0;row<PQntuples(rows);row++)found[at+(size_t)col_int(rows,row,0)-1]=(int)col_int(rows,row,1);
        PQclear(rows);at+=amount;
    }
    for(int i=0;i<n;i++)if(tiers[i]<0)tiers[i]=found[lp_idmap_find(wanted,ids+i)];
    args_free(&args);free(found);lp_idmap_free(wanted);
}
int tier_max(PGconn *pg,const lp_id *ids,int n){
    if(n<=0)return -1;
    int maximum=-1,*tiers=malloc(sizeof(int)*(size_t)n);tiers_of(pg,ids,n,tiers);
    for(int i=0;i<n;i++)if(tiers[i]>maximum)maximum=tiers[i];
    free(tiers);return maximum;
}

/* Stored paths share the extension's exact-ID reader with presence, text,
 * export and managed consumers. The ordinal maps every duplicate occurrence
 * back to the caller; storage partition names never cross this boundary. */
Hold *paths_of(PGconn *pg,const lp_id *ids,int n,int *nout){
    lp_vec(Hold) bag={0};Args args={0};
    for(int at=0;at<n;){
        int amount=n-at;if(amount>DB_ID_WINDOW)amount=DB_ID_WINDOW;
        args_reset(&args);arg_ids(&args,ids+at,(size_t)amount);
        PGresult *rows=ask(pg,"SELECT ordinal,entity,tier,path,mask FROM @extschema@.laplace_physicalities($1::@extschema@.blake3[])",&args);
        for(int row=0;row<PQntuples(rows);row++){
            Hold x={0};x.src=at+(int)col_int(rows,row,0)-1;x.entity=*col_id(rows,row,1);
            x.tier=(int)col_int(rows,row,2);x.path_len=PQgetlength(rows,row,3);
            x.path=malloc((size_t)(x.path_len?x.path_len:1));memcpy(x.path,PQgetvalue(rows,row,3),(size_t)x.path_len);
            x.claim=(uint8_t)(PQgetlength(rows,row,4)>=5 && ((const uint8_t *)PQgetvalue(rows,row,4))[4]&0x80);lp_push(&bag,x);
        }
        PQclear(rows);at+=amount;
    }
    args_free(&args);if(bag.n>1)qsort(bag.v,(size_t)bag.n,sizeof(Hold),hold_order);
    *nout=bag.n;return bag.v;
}
/* Atoms remain mmap reads. Only the remaining unique IDs cross the socket;
 * request ordinals distribute each coordinate back to every occurrence. */
void coords_of(PGconn *pg,const lp_id *ids,int n,double *out,uint8_t *has){
    if(n<=0)return;memset(has,0,(size_t)n);
    lp_idmap *wanted=lp_idmap_new();
    for(int i=0;i<n;i++){
        int64_t cp=lp_tier0_codepoint(T0,ids+i);
        if(cp>=0){lp_ref atom=lp_ref_atom(T0,(uint32_t)cp);lp_coord_xyzm(&atom.c,out+4*(size_t)i);has[i]=1;}
        else lp_idmap_put(wanted,ids+i,NULL);
    }
    size_t count=lp_idmap_count(wanted);double *coords=malloc(sizeof(double)*4*(count?count:1));
    uint8_t *found=calloc(count?count:1,1);Args args={0};
    for(size_t at=0;at<count;){
        size_t amount=count-at;if(amount>DB_ID_WINDOW)amount=DB_ID_WINDOW;
        args_reset(&args);arg_ids(&args,lp_idmap_keys(wanted)+at,amount);
        PGresult *rows=ask(pg,"SELECT ordinal,@postgis@.ST_X(coord),@postgis@.ST_Y(coord),@postgis@.ST_Z(coord),@postgis@.ST_M(coord) FROM @extschema@.laplace_entities($1::@extschema@.blake3[])",&args);
        for(int row=0;row<PQntuples(rows);row++){
            size_t index=at+(size_t)col_int(rows,row,0)-1;
            for(int d=0;d<4;d++)coords[index*4+(size_t)d]=lp_be_f64(PQgetvalue(rows,row,d+1));found[index]=1;
        }
        PQclear(rows);at+=amount;
    }
    for(int i=0;i<n;i++)if(!has[i]){
        int64_t index=lp_idmap_find(wanted,ids+i);
        if(index>=0 && found[index]){memcpy(out+4*(size_t)i,coords+4*(size_t)index,4*sizeof(double));has[i]=1;}
    }
    args_free(&args);free(coords);free(found);lp_idmap_free(wanted);
}

void holds_free(Hold *h, int n){ if (!h) return; for (int i = 0; i < n; i++) free(h[i].path); free(h); }
