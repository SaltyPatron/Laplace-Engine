/* Exercise the production selected-ID reader against real stored Native DAGs.
 *
 * With a database (reader_depth CONNINFO TIER0 [--cycle]) the DAGs are written by this program inside one transaction
 * that is never committed: every ID, coordinate and path is composed here by Native, the rows go into
 * public.entity/physicality, the production reader reads them back through the same connection, and the transaction
 * is rolled back (or, after --cycle has the reader exit, aborted when the connection closes). The deployed database
 * keeps nothing of it, and a corrupt row is never visible to any other session. A fixture ID the database already
 * holds (plain content such as "AB") is read as stored, since the same ID is the same content; the nodes whose rows
 * this program must own (the deep end of the chain and the annotated node) are required not to be stored before. */
#include "../src/read_text.c"
#include "laplace/tier0.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lp_tier0_record *T0;
void *xrealloc(void *memory,size_t bytes){void *p=realloc(memory,bytes?bytes:1);assert(p);return p;}

/* One fixture node: its ID, tier, coordinate and path as hex EWKB. */
typedef struct {lp_id id;lp_coord c;uint8_t tier;char *path;} Fixed;
typedef struct {Fixed *v;size_t n,cap;} Fixeds;
static lp_coord cp_coord(uint32_t cp){const lp_tier0_record *r=lp_tier0_at(T0,cp);assert(r);lp_coord c;memcpy(c.m,r->m,sizeof c.m);return c;}
/* A node of nv vertices (ids[i] repeated runs[i] times, said said[i]) whose children have coordinates cs and tiers. */
static Fixed *node_add(Fixeds *all,const lp_id *ids,const uint32_t *runs,const uint32_t *said,size_t nv,
                      const lp_coord *cs,const uint8_t *tiers){
    size_t expanded=0;for(size_t i=0;i<nv;i++)expanded+=runs[i];
    lp_id *children=malloc(expanded*sizeof *children);lp_coord_sum sum={0};uint8_t top=0;assert(children);
    for(size_t i=0,k=0;i<nv;i++){
        for(uint32_t j=0;j<runs[i];j++){children[k++]=ids[i];lp_coord_add(&sum,cs[i].m);}
        if(tiers[i]>top)top=tiers[i];
    }
    uint64_t *m=malloc(nv*sizeof *m);assert(m);for(size_t i=0;i<nv;i++)m[i]=lp_m_full(runs[i],said[i],0,0);
    size_t bytes=lp_ewkb_runs(ids,m,nv,NULL,0);uint8_t *ewkb=malloc(bytes);assert(ewkb && lp_ewkb_runs(ids,m,nv,ewkb,bytes)==bytes);
    lp_reserve((void **)&all->v,&all->cap,all->n+1,sizeof *all->v);Fixed *x=all->v+all->n++;
    lp_id_compose(children,expanded,&x->id);lp_coord_mean(&sum,&x->c);x->tier=(uint8_t)(top+1);
    x->path=malloc(2*bytes+1);assert(x->path);lp_hex(ewkb,bytes,x->path);
    free(children);free(m);free(ewkb);return x;
}
static Fixed pair_node(Fixeds *all,const Fixed *left,uint32_t right){
    lp_id ids[2]={left->id};lp_id_codepoint(right,ids+1);uint32_t runs[2]={1,1},said[2]={0,0};
    lp_coord cs[2]={left->c,cp_coord(right)};uint8_t tiers[2]={left->tier,0};
    return *node_add(all,ids,runs,said,2,cs,tiers);
}
static void put_text(lp_buf *b,const char *s){lp_buf_put(b,s,strlen(s));}
/* Every node in one statement: the entity rows not stored yet, and the physicality of each one this statement
 * added. Returns the IDs it added, which the caller checks against the nodes it must own. */
static PGresult *store(PGconn *pg,const Fixeds *all){
    lp_buf tiers={0},x={0},y={0},z={0},w={0},hilbert={0},paths={0};lp_vec(lp_id) ids={0};
    lp_buf *axes[4]={&x,&y,&z,&w};char value[48];
    for(size_t i=0;i<all->n;i++){
        const Fixed *node=all->v+i;const char *sep=i?",":"{";lp_push(&ids,node->id);
        snprintf(value,sizeof value,"%s%u",sep,node->tier);put_text(&tiers,value);
        double xyzm[4];lp_coord_xyzm(&node->c,xyzm);
        for(int d=0;d<4;d++){snprintf(value,sizeof value,"%s%.17g",sep,xyzm[d]);put_text(axes[d],value);}
        snprintf(value,sizeof value,"%s%lld",sep,(long long)lp_hilbert_key(lp_hilbert4(&node->c)));put_text(&hilbert,value);
        put_text(&paths,i?":":"{");put_text(&paths,node->path); /* PostGIS geometry's array delimiter is ':' (WKT has commas) */
    }
    lp_buf *arrays[6]={&tiers,&x,&y,&z,&w,&hilbert};
    for(int i=0;i<6;i++)lp_buf_put(arrays[i],"}",2);
    lp_buf_put(&paths,"}",2);
    Args args={0};arg_ids(&args,ids.v,ids.n);
    for(int i=0;i<6;i++)arg_text(&args,(const char *)arrays[i]->b);
    arg_text(&args,(const char *)paths.b);
    PGresult *added=ask_once(pg,
        "WITH node AS ("
        " SELECT u.id,u.tier,@postgis@.ST_MakePoint(u.x,u.y,u.z,u.m) coord,u.hilbert,u.path"
        " FROM unnest($1::@extschema@.blake3[],$2::int2[],$3::float8[],$4::float8[],$5::float8[],$6::float8[],"
        "  $7::int8[],$8::@postgis@.geometry[]) u(id,tier,x,y,z,m,hilbert,path)"
        "), added AS ("
        " INSERT INTO @extschema@.entity(id,tier,coord,hilbert)"
        " SELECT n.id,n.tier,n.coord,n.hilbert FROM node n"
        " WHERE NOT EXISTS (SELECT 1 FROM @extschema@.entity e WHERE e.id OPERATOR(@extschema@.=) n.id)"
        " RETURNING id"
        "), paths AS ("
        " INSERT INTO @extschema@.physicality(entity,tier,hilbert,path)"
        " SELECT n.id,n.tier,n.hilbert,n.path FROM node n WHERE n.id OPERATOR(@extschema@.=) ANY(SELECT id FROM added)"
        " RETURNING entity"
        ") SELECT entity FROM paths",&args);
    args_free(&args);lp_vec_free(&ids);
    for(int i=0;i<6;i++)lp_buf_free(arrays[i]);
    lp_buf_free(&paths);return added;
}
static bool added_has(PGresult *added,const lp_id *id){
    for(int i=0;i<PQntuples(added);i++)if(lp_id_eq(col_id(added,i,0),id))return true;
    return false;
}

int main(int argc,char **argv){
    if(argc==3 && !strcmp(argv[1],"--cache-only")){
        T0=lp_tier0_map(argv[2]);assert(T0);
        const lp_highway *highway=lp_highway_map(NULL);assert(highway);
        Reader *reader=reader_new(NULL); /* Any database call fails this test. */
        lp_text *text=lp_text_new(T0);assert(text);lp_id noun;
        assert(lp_text_id(text,"NOUN",4,&noun));assert(lp_highway_find(highway,&noun));
        char *rendered=reader_text(reader,&noun,0);assert(!strcmp(rendered,"NOUN"));free(rendered);
        lp_id children[4];assert(reader_parts(reader,&noun,children,4)==4);
        for(size_t i=0;i<4;i++)assert(lp_tier0_codepoint(T0,children+i)=="NOUN"[i]);
        const lp_list *list=lp_highway_list(highway,"fnlu");assert(list);bool tuple=false;
        for(uint32_t slot=0;slot<list->count && !tuple;slot++){
            const lp_tier0_record *record=lp_highway_at(highway,list,slot);if(!record)continue;
            const char *const *parts;size_t count=lp_highway_identifier_parts_for_id(highway,&record->id,&parts);
            if(count!=2)continue;
            lp_id actual[2],expected[2];assert(reader_parts(reader,&record->id,actual,2)==2);
            for(size_t i=0;i<2;i++)assert(lp_text_id(text,parts[i],strlen(parts[i]),expected+i) && lp_id_eq(actual+i,expected+i));
            rendered=reader_text(reader,&record->id,0);assert(strlen(rendered)==strlen(parts[0])+strlen(parts[1]));
            assert(!memcmp(rendered,parts[0],strlen(parts[0])) && !strcmp(rendered+strlen(parts[0]),parts[1]));free(rendered);tuple=true;
        }
        assert(tuple && reader_trips(reader)==0);
        lp_id ids[3]={noun,noun};lp_id_codepoint('N',ids+2);double coords[12];uint8_t has[3];
        coords_of(NULL,ids,3,coords,has);assert(has[0] && has[1] && has[2]);
        assert(!memcmp(coords,coords+4,4*sizeof(double)));
        reader_free(reader);lp_text_free(text);
        puts("reader cache: Tier-0, Highway text, tuple constituents and duplicate coordinates resolved with no database connection");return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--id-stdin")){
        uint8_t bytes[128];size_t count=fread(bytes,1,sizeof bytes,stdin);lp_id id;char hex[33];
        assert(!ferror(stdin) && count && lp_id_codepoints_utf8(bytes,count,&id));lp_id_hex(&id,hex);puts(hex);return 0;
    }
    if(argc==2){
        T0=lp_tier0_map(argv[1]);assert(T0);lp_id a,b,c,root_id,missing;
        lp_id_codepoint(65,&a);lp_id_codepoint(66,&b);lp_id_codepoint(67,&c);
        lp_id pair[2]={a,b};lp_id_compose(pair,2,&root_id);pair[1]=c;lp_id_compose(pair,2,&missing);
        Reader *r=reader_new(NULL);Ent *root=ent(r,&root_id,1);root->state=1;root->nv=2;
        lp_push(&r->children,((lp_realize_child){missing,1,0}));lp_push(&r->children,((lp_realize_child){b,1,0}));
        Out out={.limit=100};expand(r,&root_id,&out);
        assert(out.wants==1 && out.n==1 && out.b[0]=='B' && r->n==2);
        out.n=out.wants=0;expand(r,&root_id,&out); /* the same DAG walked again: no active path left behind */
        assert(out.wants==1 && out.n==1);lp_free(out.b);reader_free(r);puts("reader lazy frontier stack passed");return 0;
    }
    if(!(argc==3 || (argc==4 && !strcmp(argv[3],"--cycle")))){
        fputs("usage: reader_depth TIER0 | --cache-only TIER0 | --id-stdin | CONNINFO TIER0 [--cycle]\n",stderr);return 2;
    }
    T0=lp_tier0_map(argv[2]);assert(T0);
    PGconn *pg=PQconnectdb(argv[1]);assert(PQstatus(pg)==CONNECTION_OK);
    assert(db_binding(pg)->id_type);
    bool cycle=argc==4;
    /* A 4,096-deep chain [[[A,B],B],...,B]; a run of 100,000 A's; A beside two private-use vertices said to be a
     * claim and its voice (no part of text); 32 branches [U+0400+i,A] each under [branch,B]. */
    Fixeds all={0};Fixed deep;
    {
        lp_id a;lp_id_codepoint(65,&a);Fixed start={.id=a,.c=cp_coord(65),.tier=0};deep=start;
        for(int depth=1;depth<=4096;depth++)deep=pair_node(&all,&deep,66);
    }
    Fixed run,annotated;
    {
        lp_id a;lp_id_codepoint(65,&a);uint32_t runs[1]={100000},said[1]={0};lp_coord cs[1]={cp_coord(65)};uint8_t tiers[1]={0};
        run=*node_add(&all,&a,runs,said,1,cs,tiers);
        lp_id ids[3];lp_id_codepoint(65,ids);lp_id_codepoint(0xE000,ids+1);lp_id_codepoint(0xE001,ids+2);
        uint32_t ones[3]={1,1,1},roles[3]={0,LP_SAID_CLAIM,LP_SAID_VOICE};
        lp_coord cs3[3]={cp_coord(65),cp_coord(0xE000),cp_coord(0xE001)};uint8_t tiers3[3]={0,0,0};
        annotated=*node_add(&all,ids,ones,roles,3,cs3,tiers3);
    }
    lp_id selected[35];
    for(int i=0;i<32;i++){
        lp_id cp;lp_id_codepoint((uint32_t)(1024+i),&cp);Fixed leaf={.id=cp,.c=cp_coord((uint32_t)(1024+i)),.tier=0};
        Fixed branch=pair_node(&all,&leaf,65);selected[i]=pair_node(&all,&branch,66).id;
    }
    /* Never committed: rolled back below, or aborted with the connection when the reader exits on the cycle. */
    exec(pg,"BEGIN");
    PGresult *added=store(pg,&all);
    if(!added_has(added,&deep.id) || !added_has(added,&annotated.id)){
        fputs("reader: a fixture node this check must own is already stored\n",stderr);return 1;
    }
    PQclear(added);
    if(cycle){
        /* The deep end made its own first child: a cycle the reader must reject, not follow. */
        lp_id ids[2]={deep.id};lp_id_codepoint(66,ids+1);uint64_t m[2]={lp_m_full(1,0,0,0),lp_m_full(1,0,0,0)};
        size_t bytes=lp_ewkb_runs(ids,m,2,NULL,0);uint8_t *ewkb=malloc(bytes);assert(ewkb && lp_ewkb_runs(ids,m,2,ewkb,bytes)==bytes);
        char *hex=malloc(2*bytes+1);lp_hex(ewkb,bytes,hex);
        Args args={0};arg_ids(&args,&deep.id,1);arg_text(&args,hex);
        PQclear(ask_once(pg,"UPDATE @extschema@.physicality SET path=$2::@postgis@.geometry"
                            " WHERE entity OPERATOR(@extschema@.=) ($1::@extschema@.blake3[])[1]",&args));
        args_free(&args);free(hex);free(ewkb);
        Reader *reader=reader_new(pg);reader_want(reader,&deep.id);
        char *text=reader_text(reader,&deep.id,0);free(text);
        fputs("reader: a corrupt cycle was followed to an end\n",stderr);return 1;   /* lp_realize must have exited */
    }
    selected[32]=selected[0];lp_id_codepoint(128512,selected+33);memset(selected+34,0,sizeof(lp_id));
    Reader *batched=reader_new(pg);reader_prefetch(batched,selected,35,3);
    assert(reader_trips(batched)==2 && batched->n==65);
    for(int i=0;i<32;i++){
        char expected[16];size_t length=lp_utf8_put((uint32_t)(1024+i),(uint8_t *)expected);
        expected[length++]='A';memcpy(expected+length,"\xE2\x80\xA6",4);
        char *actual=reader_text(batched,selected+i,3);assert(!strcmp(expected,actual));free(actual);
    }
    char *duplicate=reader_text(batched,selected+32,3),*original=reader_text(batched,selected,3);
    assert(!strcmp(duplicate,original));free(duplicate);free(original);
    char *virtual=reader_text(batched,selected+33,0);assert(!strcmp(virtual,"\xF0\x9F\x98\x80"));free(virtual);
    char *missing=reader_text(batched,selected+34,0);assert(!strcmp(missing,"{00000000000000000000000000000000}"));free(missing);
    reader_prefetch(batched,NULL,0,3);assert(reader_trips(batched)==2);reader_free(batched);
    puts("reader: 32 independent display prefixes, duplicates, missing IDs and virtual scalars in two exact frontiers passed");
    Reader *reader=reader_new(pg);reader_want(reader,&deep.id);reader_want(reader,&run.id);reader_want(reader,&annotated.id);
    fputs("reader acceptance: deep\n",stderr);char *text=reader_text(reader,&deep.id,0);
    assert(strlen(text)==4097 && text[0]=='A');for(size_t i=1;i<4097;i++)assert(text[i]=='B');free(text);
    uint64_t trips=reader_trips(reader);assert(trips>64);
    fputs("reader acceptance: replay\n",stderr);text=reader_text(reader,&deep.id,0);assert(strlen(text)==4097 && reader_trips(reader)==trips);free(text);
    lp_id parts[2],a;lp_id_codepoint(65,&a);
    assert(reader_parts(reader,&run.id,parts,2)==2 && lp_id_eq(parts,&a) && lp_id_eq(parts+1,&a));
    fputs("reader acceptance: runs\n",stderr);text=reader_text(reader,&run.id,7);assert(!strcmp(text,"AAAAAAA\xE2\x80\xA6"));free(text);
    text=reader_text(reader,&run.id,3);assert(!strcmp(text,"AAA\xE2\x80\xA6"));free(text);
    fputs("reader acceptance: roles\n",stderr);text=reader_text(reader,&annotated.id,0);assert(!strcmp(text,"A"));free(text);
    lp_id emoji;lp_id_codepoint(128512,&emoji);text=reader_text(reader,&emoji,0);assert(!strcmp(text,"\xF0\x9F\x98\x80"));free(text);
    lp_id absent;assert(lp_id_unhex("00000000000000000000000000000000",&absent));
    text=reader_text(reader,&absent,0);assert(!strcmp(text,"{00000000000000000000000000000000}"));free(text);
    reader_free(reader);exec(pg,"ROLLBACK");PQfinish(pg);
    for(size_t i=0;i<all.n;i++)free(all.v[i].path);free(all.v);
    puts("reader: 4096 tiers, shared frontiers, bounded run projection, display limits and annotation roles passed");
    return 0;
}
