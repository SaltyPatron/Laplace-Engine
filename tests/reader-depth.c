/* Exercise the production selected-ID reader against real stored Native DAGs. */
#include "../src/read_text.c"
#include "laplace/tier0.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const lp_tier0_record *T0;
uint32_t id_oid;
void *xrealloc(void *memory,size_t bytes){void *p=realloc(memory,bytes?bytes:1);assert(p);return p;}

int main(int argc,char **argv){
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
    PGresult *type=PQexec(pg,"SELECT 'public.blake3'::regtype::oid");
    assert(PQresultStatus(type)==PGRES_TUPLES_OK && PQntuples(type)==1);
    id_oid=(uint32_t)strtoul(PQgetvalue(type,0,0),NULL,10);PQclear(type);
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
