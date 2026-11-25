#ifndef LAPLACE_CODECS_H
#define LAPLACE_CODECS_H

#include "engine.h"

/* Native grammars registered in grammars.c NATIVE[] */
extern const NativeGrammar image_grammar;
extern const NativeGrammar audio_grammar;
extern const NativeGrammar video_grammar;
extern const NativeGrammar git_grammar;

/* Binary input decoders registered in grammars.c DECODERS[] */
extern const InputDecoder parquet_decoder;
extern const InputDecoder shapefile_decoder;

/* Shared process-wide scalar table (256 u8 + lazy u16/i16 decimal compositions) */
lp_scalars *codecs_scalars(void);

/* Lossless DAG realizers (image -> PNG, audio -> PCM16 WAV) */
int lp_image_realize_png(const lp_id *root, const lp_tier0_record *t0,
                         uint8_t **png_out, size_t *png_len);
int lp_audio_realize_wav(const lp_id *root, const lp_tier0_record *t0,
                         uint8_t **wav_out, size_t *wav_len);

/* Realize any leaf text entity in the local node table into a malloc'd NUL string */
int lp_codec_realize_text(const lp_id *id, const lp_tier0_record *t0, char **out, size_t *len);

/* Emit claim events and attach an LP_SAID_RECORD envelope to f->said so claims persist in physicality. */
void lp_codec_emit_claim_record(File *f, const Ref *subject, const Ref *claims, size_t nclaims);

/* Deterministic toolchain execution witness (ToolchainVerification = 0.90) */
typedef struct {
    const char *conninfo;
    const char *witness_name;   /* e.g. "icx", "ctest", "psql" */
    const char *target_label;   /* file / symbol / test identifier */
    const Ref  *subject;        /* AST / delta / test entity being verified */
    const Ref  *constituents;   /* optional failing/passing child entities for salience */
    size_t      nconstituents;
    int         passed;         /* nonzero -> WIN (1.0), 0 -> LOSS (0.0) */
    uint32_t    position;       /* monotonic ordinal inside witness stream */
} LpToolchainReport;

int lp_toolchain_witness(const LpToolchainReport *rep, lp_id *claim_out, LoadStats *stats);

/* Native C CPU Mold-a-Model export (mold.c) and GGUF/Safetensors ingestion (model.c) */
int cmd_model_export(int argc, char **argv);

typedef struct {
    const char *name;
    const char *out_path;
    size_t      n_vocab;
    const char **tokens;        /* n_vocab UTF-8 token strings */
    const lp_id *token_ids;     /* optional canonical lp_id per token (computed via TX if NULL) */
    const double *coords4d;     /* optional n_vocab * 4 anchors in [-1, 1]^4 (computed if NULL) */
    int         bos_id;
    int         eos_id;
    size_t      n_edges;
    const int  *rows;           /* n_edges source token indices */
    const int  *cols;           /* n_edges target token indices */
    const double *weights;      /* n_edges positive edge/trajectory weights */
    const char **edge_witnesses;/* optional witness label per edge (length n_edges) */
    const char **edge_lineages; /* optional causal lineage per edge (length n_edges, Fisher dedup) */
    size_t      hidden_dim;     /* embedding width D (D > 6, rank > 4 when n_vocab > 6) */
    size_t      n_layers;
    size_t      n_heads;
    size_t      n_kv_heads;
    size_t      ffn_dim;
    double      rms_eps;
    double      rope_base;
    int         f16_weights;    /* 0: F32, 1: F16 weights */
    int         format;         /* 0: GGUF, 1: SafeTensors */
    int         tied_embeddings;/* 0: untied output/lm_head, 1: tied */
} LpMoldSpec;

int lp_mold_export(const LpMoldSpec *spec);

#endif
