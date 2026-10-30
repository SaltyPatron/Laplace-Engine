/* Exercise the production selected-ID reader against real stored Native DAGs. */
#include "../src/read_text.c"
#include "laplace/tier0.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lp_tier0_record *T0;
void *xrealloc(void *memory,size_t bytes){void *p=realloc(memory,bytes?bytes:1);assert(p);return p;}

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
        lp_push(&r->children,((ReadChild){missing,1}));lp_push(&r->children,((ReadChild){b,1}));
        Out out={.limit=100};expand(r,&root_id,&out);
        assert(!r->e[0].active && out.wants==1);lp_free(out.b);reader_free(r);puts("reader lazy frontier stack passed");return 0;
    }
    assert(argc==6 || argc==7);T0=lp_tier0_map(argv[2]);assert(T0);
    PGconn *pg=PQconnectdb(argv[1]);assert(PQstatus(pg)==CONNECTION_OK);
    assert(db_binding(pg)->id_type);
    PGresult *batch=ask_once(pg,"SELECT id FROM public.reader_batch_fixture ORDER BY i",NULL);
    assert(PQntuples(batch)==32);lp_id selected[35];
    for(int i=0;i<32;i++)selected[i]=*col_id(batch,i,0);PQclear(batch);
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
    lp_id deep,run,annotated;assert(lp_id_unhex(argv[3],&deep));assert(lp_id_unhex(argv[4],&run));assert(lp_id_unhex(argv[5],&annotated));
    Reader *reader=reader_new(pg);reader_want(reader,&deep);reader_want(reader,&run);reader_want(reader,&annotated);
    fputs("reader acceptance: deep\n",stderr);char *text=reader_text(reader,&deep,0);
    assert(argc!=7); /* A corrupt cycle must terminate in the production reader. */
    assert(strlen(text)==4097 && text[0]=='A');for(size_t i=1;i<4097;i++)assert(text[i]=='B');free(text);
    uint64_t trips=reader_trips(reader);assert(trips>64);
    fputs("reader acceptance: replay\n",stderr);text=reader_text(reader,&deep,0);assert(strlen(text)==4097 && reader_trips(reader)==trips);free(text);
    lp_id parts[2],a;lp_id_codepoint(65,&a);
    assert(reader_parts(reader,&run,parts,2)==2 && lp_id_eq(parts,&a) && lp_id_eq(parts+1,&a));
    fputs("reader acceptance: runs\n",stderr);text=reader_text(reader,&run,7);assert(!strcmp(text,"AAAAAAA\xE2\x80\xA6"));free(text);
    text=reader_text(reader,&run,3);assert(!strcmp(text,"AAA\xE2\x80\xA6"));free(text);
    fputs("reader acceptance: roles\n",stderr);text=reader_text(reader,&annotated,0);assert(!strcmp(text,"A"));free(text);
    lp_id emoji;lp_id_codepoint(128512,&emoji);text=reader_text(reader,&emoji,0);assert(!strcmp(text,"\xF0\x9F\x98\x80"));free(text);
    lp_id absent;assert(lp_id_unhex("00000000000000000000000000000000",&absent));
    text=reader_text(reader,&absent,0);assert(!strcmp(text,"{00000000000000000000000000000000}"));free(text);
    reader_free(reader);PQfinish(pg);
    puts("reader: 4096 tiers, shared frontiers, bounded run projection, display limits and annotation roles passed");
    return 0;
}
