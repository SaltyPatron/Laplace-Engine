/* Engine orchestrates the shared native UCD -> complete & modular Unicode Tier-0 cache generator. */
#include "engine.h"
#include "laplace/tier0.h"
#include <stdio.h>
#include <stdlib.h>

int cmd_tier0(int argc, char **argv){
    char defaults[4096];
    snprintf(defaults, sizeof defaults, "%s/Public/UCD/latest", laplace_ucd());
    const char *root = defaults, *output = lp_tier0_path(), *split = NULL;
    if (opts(argc, argv, (const Opt[]){
        { .flag = "-u", .kind = 's', .at = &root },
        { .flag = "-o", .kind = 's', .at = &output },
        { .flag = "-s", .kind = 's', .at = &split },
        { 0 }
    }) < argc) {
        fprintf(stderr, "usage: laplace tier0 [-u UCD_ROOT] [-o tier0.bin] [-s existing_tier0.bin]\n");
        return 2;
    }
    if (split && *split) {
        double started = now();
        if (!lp_tier0_split_file(split)) { perror("native Tier-0 shard split"); return 1; }
        for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++) {
            char spath[4096];
            if (lp_tier0_shard_path(split, (lp_tier0_shard)s, spath, sizeof spath))
                printf("%s: %u records (%s shard)\n", spath,
                       lp_tier0_shard_count((lp_tier0_shard)s),
                       lp_tier0_shard_name((lp_tier0_shard)s));
        }
        printf("native shard split completed in %.2f s\n", now() - started);
        return 0;
    }
    lp_tier0_record *records = malloc((size_t)LP_TIER0_COUNT * sizeof *records);
    if (!records) { perror("Tier-0 allocation"); return 1; }
    double started = now();
    bool ok = lp_tier0_from_ucd(root, records);
    if (ok) ok = lp_tier0_write(output, records);
    if (!ok) { perror("native Tier-0 generation"); free(records); return 1; }
    uint8_t digest[32]; char hex[65];
    lp_tier0_fingerprint(records, digest); lp_hex(digest, 32, hex);
    printf("%s: %u Unicode codepoint records (+ ascii/utf/cjk/ext shards), sorted by ID\n"
           "fingerprint (BLAKE3-256 of the table) %s\n",
           output, LP_TIER0_COUNT, hex);
    printf("native generation completed in %.2f s\n", now() - started);
    free(records); return 0;
}
