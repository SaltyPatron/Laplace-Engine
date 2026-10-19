/* Engine orchestrates the shared native UCD -> scalar Tier-0 cache generator. */
#include "engine.h"
#include "laplace/tier0.h"
#include <stdio.h>
#include <stdlib.h>

int cmd_tier0(int argc, char **argv){
    char defaults[4096];
    snprintf(defaults, sizeof defaults, "%s/Public/UCD/latest", laplace_ucd());
    const char *root = defaults, *output = lp_tier0_path();
    if (opts(argc, argv, (const Opt[]){ { .flag = "-u", .kind = 's', .at = &root }, { .flag = "-o", .kind = 's', .at = &output }, { 0 } }) < argc) {
        fprintf(stderr, "usage: laplace tier0 [-u UCD_ROOT] [-o tier0.bin]\n"); return 2;
    }
    lp_tier0_record *records = malloc((size_t)LP_TIER0_COUNT * sizeof *records);
    if (!records) { perror("Tier-0 allocation"); return 1; }
    double started = now();
    bool ok = lp_tier0_from_ucd(root, records);
    if (ok) ok = lp_tier0_write(output, records);
    if (!ok) { perror("native Tier-0 generation"); free(records); return 1; }
    uint8_t digest[32]; char hex[65];
    lp_tier0_fingerprint(records, digest); lp_hex(digest, 32, hex);
    printf("%s: %u Unicode scalar records, sorted by ID\nfingerprint (BLAKE3-256 of the table) %s\n",
           output, LP_TIER0_COUNT, hex);
    printf("native generation completed in %.2f s\n", now() - started);
    free(records); return 0;
}
