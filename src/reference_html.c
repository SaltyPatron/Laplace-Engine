/* HTML text-state character references. The parser supplies admitted text
 * spans; this codec neither scans packaging nor changes its retained bytes.
 * Named references are the checked-in WHATWG table, including two-scalar
 * values and the specified legacy spellings without a semicolon. */
#include "engine.h"
#include <string.h>
typedef struct { const char *name; uint32_t first, second; } HtmlReference;
#include "html_references.inc"

static const HtmlReference *named_reference(const uint8_t *name, size_t n){
    size_t lo=0, hi=sizeof html_references/sizeof *html_references;
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2; const char *key=html_references[mid].name;
        size_t length=strlen(key), common=n<length?n:length;
        int order=memcmp(name,key,common);
        if (!order) order=n<length?-1:n>length?1:0;
        if (!order) return &html_references[mid];
        if (order<0) hi=mid; else lo=mid+1;
    }
    return NULL;
}
size_t html_unescape(const uint8_t *s, size_t n, uint8_t *out){
    static const uint32_t c1[32]={0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,
        0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,0x90,0x2018,0x2019,0x201c,
        0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
    size_t used=0;
    for (size_t i=0;i<n;) {
        if (s[i]=='\r') { out[used++]='\n'; i+=i+1<n && s[i+1]=='\n'?2:1; continue; }
        if (!s[i]) { used+=lp_utf8_put(0xfffd,out+used); i++; continue; }
        if (s[i]!='&') { out[used++]=s[i++]; continue; }
        size_t begin=i, at=i+1;
        if (at<n && s[at]=='#') {
            at++; unsigned radix=10;
            if (at<n && (s[at]=='x'||s[at]=='X')) { radix=16; at++; }
            size_t digits=at; uint32_t cp=0;
            while (at<n) {
                unsigned c=s[at], digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
                if (digit>=radix) break;
                cp=cp>0x10ffff/radix?0x110000:cp*radix+digit; at++;
            }
            if (at>digits) {
                if (at<n && s[at]==';') at++;
                if (!cp || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) cp=0xfffd;
                else if (cp>=0x80 && cp<=0x9f) cp=c1[cp-0x80];
                used+=lp_utf8_put(cp,out+used); i=at; continue;
            }
        } else {
            const HtmlReference *best=NULL; size_t end=at;
            while (at<n && at-begin<=32) {
                unsigned c=s[at]; if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c==';')) break;
                at++; const HtmlReference *found=named_reference(s+begin+1,at-begin-1);
                if (found) { best=found; end=at; }
                if (c==';') break;
            }
            if (best) { used+=lp_utf8_put(best->first,out+used); if(best->second)used+=lp_utf8_put(best->second,out+used); i=end; continue; }
        }
        out[used++]=s[i++];
    }
    return used;
}
