#ifndef LAPLACE_INGEST_INTERNAL_H
#define LAPLACE_INGEST_INTERNAL_H

#include "engine.h"
#include "laplace/highway.h"
#include <blake3.h>
#include <arpa/inet.h>
#include <locale.h>
#include <ftw.h>
#include <sys/stat.h>
#include <omp.h>
#include <zlib.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#define paths lp_ingest_paths
#define path_of lp_ingest_path_of
#define npaths lp_ingest_npaths
#define walking lp_ingest_walking
#define tsv_claims lp_ingest_tsv_claims
#define add_path lp_ingest_add_path
#define walk_dir lp_ingest_walk_dir
#define source_walk lp_ingest_source_walk
#define tsv_escaped lp_ingest_tsv_escaped
#define tsv_record lp_ingest_tsv_record
#define show_entities lp_ingest_show_entities
#define show_record_events lp_ingest_show_record_events
#define ingest_every lp_ingest_every
#define ingest_highway lp_ingest_highway

extern char **paths;
extern const Source **path_of;
extern int npaths;
extern const Source *walking;
extern int tsv_claims;

void add_path(const char *p);
void walk_dir(const char *dir);
void source_walk(const Source *sc);
void tsv_escaped(const uint8_t *b, size_t n);
void tsv_record(const lp_id *id, unsigned position);
void show_entities(const char *conninfo, int ask);
void show_record_events(const File *file);
int ingest_every(int argc, char **argv, Source *src, int nsrc, Recipe *rec, int nrec, const int *want);
int ingest_highway(const char *conninfo, const char *t0p, int threads, int do_load);

#endif
