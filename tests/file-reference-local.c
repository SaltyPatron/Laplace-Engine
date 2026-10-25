/* Exercise the actual precheck helper against Native ROM atoms and canonical
 * higher references. The resolver fixture makes a cache miss observable. */
#define ref_of_id file_reference_test_resolve
#include "../src/ingest.c"
#undef ref_of_id
#include <assert.h>

static Ref resident[2];
static int resident_count;
Ref file_reference_test_resolve(const lp_id *id,int *known){
    for(int i=0;i<resident_count;i++)if(lp_id_eq(id,&resident[i].id)){*known=1;return resident[i];}
    int64_t cp=lp_tier0_codepoint(T0,id);
    if(cp>=0){*known=1;return lp_ref_atom(T0,(uint32_t)cp);}
    *known=0;Ref absent={0};absent.id=*id;return absent;
}
static void equal_ref(const Ref *expected,const Ref *actual){
    assert(lp_id_eq(&expected->id,&actual->id));
    assert(!memcmp(&expected->c,&actual->c,sizeof expected->c));
    assert(expected->tier==actual->tier);
}
int main(int argc,char **argv){
    assert(argc==2);T0=lp_tier0_map(argv[1]);assert(T0);
    Ref atoms[]={lp_ref_atom(T0,65),lp_ref_atom(T0,66)};
    Ref pair=lp_ref_compose(atoms,2,1),result;
    FileRefScratch scratch={0};
    lp_id ids[]={atoms[0].id,atoms[1].id};
    size_t bytes=lp_ewkb_path(ids,2,NULL,0);uint8_t *path=malloc(bytes);
    lp_ewkb_path(ids,2,path,bytes);
    assert(ingest_local_file_ref(&pair.id,path,bytes,pair.tier,&scratch,&result)==1);
    equal_ref(&pair,&result);
    Ref children[]={pair,atoms[0]};
    Ref parent=lp_ref_compose(children,2,2);
    lp_id higher_ids[]={pair.id,atoms[0].id};
    lp_ewkb_path(higher_ids,2,path,bytes);
    assert(ingest_local_file_ref(&parent.id,path,bytes,parent.tier,&scratch,&result)==0);
    resident[0]=pair;resident_count=1;
    assert(ingest_local_file_ref(&parent.id,path,bytes,parent.tier,&scratch,&result)==1);
    equal_ref(&parent,&result);
    resident[1]=parent;resident_count=2;
    assert(ingest_local_file_ref(&parent.id,NULL,0,parent.tier,&scratch,&result)==1);
    equal_ref(&parent,&result);
    resident_count=0;
    uint64_t runs[]={3,2};Ref weighted;
    assert(lp_ref_compose_runs(atoms,runs,2,1,&weighted));
    uint64_t metadata[]={(uint64_t)lp_m_of(3,0),(uint64_t)lp_m_of(2,0)};
    size_t run_bytes=lp_ewkb_runs(ids,metadata,2,NULL,0);path=realloc(path,run_bytes);
    lp_ewkb_runs(ids,metadata,2,path,run_bytes);
    assert(ingest_local_file_ref(&weighted.id,path,run_bytes,weighted.tier,&scratch,&result)==1);
    equal_ref(&weighted,&result);
    assert(ingest_local_file_ref(&pair.id,path,run_bytes,pair.tier,&scratch,&result)==-1);
    free(path);free(scratch.vertices);free(scratch.children);free(scratch.runs);
    puts("file-reference: Native cache hits, local composition, misses, run weights and ID rejection passed");
    return 0;
}
