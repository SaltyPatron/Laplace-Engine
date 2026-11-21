#include "codecs.h"
#include "laplace/os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ESRI Shapefile (.shp + companion .dbf) -> TSV InputDecoder (shape_type, wkt, dbf fields) */

static uint32_t shp_be32(const uint8_t *p){
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint32_t shp_le32(const uint8_t *p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t shp_le16(const uint8_t *p){
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static double shp_f64(const uint8_t *p){
    uint64_t lo = shp_le32(p), hi = shp_le32(p + 4);
    uint64_t u = lo | (hi << 32);
    double d; memcpy(&d, &u, 8);
    return d;
}

typedef struct {
    char    name[12];
    char    type;
    uint8_t length;
} DbfField;

typedef struct {
    FILE     *shp;
    FILE     *dbf;
    DbfField *fields;
    size_t    nfields;
    uint16_t  dbf_rec_bytes;
    uint32_t  dbf_records;
    uint32_t  dbf_read;
    int       header_done;
    lp_buf    out;
    size_t    out_at;
    int       failed;
} ShpDecoder;

static void emit_tsv(lp_buf *out, const char *s, size_t n){
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\') lp_buf_cstr(out, "\\\\");
        else if (c == '\t') lp_buf_cstr(out, "\\t");
        else if (c == '\n') lp_buf_cstr(out, "\\n");
        else if (c == '\r') lp_buf_cstr(out, "\\r");
        else lp_buf_byte(out, (uint8_t)c);
    }
}

static const char *shape_type_name(uint32_t st){
    switch (st) {
    case 0: return "Null";
    case 1: return "Point";
    case 3: return "PolyLine";
    case 5: return "Polygon";
    case 8: return "MultiPoint";
    case 11: return "PointZ";
    case 13: return "PolyLineZ";
    case 15: return "PolygonZ";
    case 18: return "MultiPointZ";
    case 21: return "PointM";
    case 23: return "PolyLineM";
    case 25: return "PolygonM";
    case 28: return "MultiPointM";
    default: return "Unknown";
    }
}

static void emit_points_wkt(lp_buf *out, const uint8_t *pts, uint32_t from, uint32_t to){
    for (uint32_t i = from; i < to; i++) {
        if (i > from) lp_buf_cstr(out, ", ");
        double x = shp_f64(pts + (size_t)i * 16u);
        double y = shp_f64(pts + (size_t)i * 16u + 8u);
        lp_buf_fmt(out, "%.15g %.15g", x, y);
    }
}

static void format_shape_wkt(const uint8_t *buf, size_t len, uint32_t stype, lp_buf *out){
    if (stype == 0 || len < 4) { lp_buf_cstr(out, "POINT EMPTY"); return; }
    uint32_t base = stype % 10u;
    if (base == 1u) { /* Point */
        if (len < 20) { lp_buf_cstr(out, "POINT EMPTY"); return; }
        lp_buf_fmt(out, "POINT (%.15g %.15g)", shp_f64(buf + 4), shp_f64(buf + 12));
        return;
    }
    if (base == 8u) { /* MultiPoint */
        if (len < 40) { lp_buf_cstr(out, "MULTIPOINT EMPTY"); return; }
        uint32_t np = shp_le32(buf + 36);
        if (40u + (size_t)np * 16u > len) np = (uint32_t)((len - 40u) / 16u);
        lp_buf_cstr(out, "MULTIPOINT (");
        for (uint32_t i = 0; i < np; i++) {
            if (i) lp_buf_cstr(out, ", ");
            lp_buf_fmt(out, "(%.15g %.15g)", shp_f64(buf + 40u + (size_t)i * 16u), shp_f64(buf + 40u + (size_t)i * 16u + 8u));
        }
        lp_buf_byte(out, ')');
        return;
    }
    if (base == 3u || base == 5u) { /* PolyLine / Polygon */
        if (len < 44) { lp_buf_cstr(out, base == 3u ? "LINESTRING EMPTY" : "POLYGON EMPTY"); return; }
        uint32_t nparts = shp_le32(buf + 36);
        uint32_t npts = shp_le32(buf + 40);
        if ((size_t)nparts * 4u + (size_t)npts * 16u > len - 44u) {
            lp_buf_cstr(out, base == 3u ? "LINESTRING EMPTY" : "POLYGON EMPTY");
            return;
        }
        const uint8_t *parts = buf + 44u;
        const uint8_t *pts = parts + (size_t)nparts * 4u;
        if (base == 3u) {
            if (nparts <= 1u) {
                lp_buf_cstr(out, "LINESTRING (");
                emit_points_wkt(out, pts, 0, npts);
                lp_buf_byte(out, ')');
            } else {
                lp_buf_cstr(out, "MULTILINESTRING (");
                for (uint32_t p = 0; p < nparts; p++) {
                    if (p) lp_buf_cstr(out, ", ");
                    uint32_t a = shp_le32(parts + p * 4u);
                    uint32_t b = (p + 1 < nparts) ? shp_le32(parts + (p + 1u) * 4u) : npts;
                    if (a > npts) a = npts;
                    if (b > npts || b < a) b = npts;
                    lp_buf_byte(out, '(');
                    emit_points_wkt(out, pts, a, b);
                    lp_buf_byte(out, ')');
                }
                lp_buf_byte(out, ')');
            }
        } else {
            lp_buf_cstr(out, "POLYGON (");
            for (uint32_t p = 0; p < nparts; p++) {
                if (p) lp_buf_cstr(out, ", ");
                uint32_t a = shp_le32(parts + p * 4u);
                uint32_t b = (p + 1 < nparts) ? shp_le32(parts + (p + 1u) * 4u) : npts;
                if (a > npts) a = npts;
                if (b > npts || b < a) b = npts;
                lp_buf_byte(out, '(');
                emit_points_wkt(out, pts, a, b);
                lp_buf_byte(out, ')');
            }
            lp_buf_byte(out, ')');
        }
        return;
    }
    lp_buf_cstr(out, "GEOMETRYCOLLECTION EMPTY");
}

static void *shapefile_open(const char *path){
    FILE *shp = fopen(path, "rb");
    if (!shp) { perror(path); return NULL; }
    uint8_t hdr[100];
    if (fread(hdr, 1, 100, shp) != 100 || shp_be32(hdr) != 9994u || shp_le32(hdr + 28) != 1000u) {
        fclose(shp);
        fprintf(stderr, "%s: invalid Shapefile header\n", path);
        return NULL;
    }

    ShpDecoder *sd = calloc(1, sizeof *sd);
    sd->shp = shp;

    /* Try sibling .dbf file if present */
    size_t pl = strlen(path);
    char *dbf_path = malloc(pl + 8);
    snprintf(dbf_path, pl + 8, "%s", path);
    if (pl >= 4 && !strncasecmp(dbf_path + pl - 4, ".shp", 4)) memcpy(dbf_path + pl - 4, ".dbf", 5);
    else memcpy(dbf_path + pl, ".dbf", 5);

    FILE *dbf = fopen(dbf_path, "rb");
    free(dbf_path);
    if (dbf) {
        uint8_t dh[32];
        if (fread(dh, 1, 32, dbf) == 32) {
            uint32_t nrec = shp_le32(dh + 4);
            uint16_t hsz = shp_le16(dh + 8);
            uint16_t rsz = shp_le16(dh + 10);
            if (hsz >= 33 && rsz >= 1) {
                size_t nf = (size_t)(hsz - 33) / 32u;
                sd->fields = calloc(nf ? nf : 1u, sizeof(DbfField));
                for (size_t i = 0; i < nf; i++) {
                    uint8_t fd[32];
                    if (fread(fd, 1, 32, dbf) != 32 || fd[0] == 0x0Du) break;
                    memcpy(sd->fields[sd->nfields].name, fd, 11);
                    sd->fields[sd->nfields].name[11] = 0;
                    sd->fields[sd->nfields].type = (char)fd[11];
                    sd->fields[sd->nfields].length = fd[16];
                    sd->nfields++;
                }
                os_fseek64(dbf, (int64_t)hsz, SEEK_SET);
                sd->dbf = dbf;
                sd->dbf_records = nrec;
                sd->dbf_rec_bytes = rsz;
            } else fclose(dbf);
        } else fclose(dbf);
    }
    return sd;
}

static size_t shapefile_read(void *ctx, uint8_t *out, size_t capacity){
    ShpDecoder *sd = ctx;
    if (sd->failed || !capacity) return 0;

    if (!sd->header_done) {
        lp_buf_cstr(&sd->out, "shape_type\twkt");
        for (size_t i = 0; i < sd->nfields; i++) {
            lp_buf_byte(&sd->out, '\t');
            emit_tsv(&sd->out, sd->fields[i].name, strlen(sd->fields[i].name));
        }
        lp_buf_byte(&sd->out, '\n');
        sd->header_done = 1;
    }

    while (sd->out_at >= sd->out.n) {
        lp_buf_clear(&sd->out);
        sd->out_at = 0;
        uint8_t rh[8];
        if (fread(rh, 1, 8, sd->shp) != 8) break;
        uint32_t words = shp_be32(rh + 4);
        size_t blen = (size_t)words * 2u;
        if (blen < 4 || blen > (64u << 20)) { sd->failed = 1; break; }
        uint8_t *rbuf = malloc(blen);
        if (!rbuf || fread(rbuf, 1, blen, sd->shp) != blen) { free(rbuf); sd->failed = 1; break; }
        uint32_t stype = shp_le32(rbuf);
        lp_buf_cstr(&sd->out, shape_type_name(stype));
        lp_buf_byte(&sd->out, '\t');
        format_shape_wkt(rbuf, blen, stype, &sd->out);
        free(rbuf);

        if (sd->dbf && sd->dbf_read < sd->dbf_records) {
            uint8_t *rec = malloc(sd->dbf_rec_bytes);
            if (fread(rec, 1, sd->dbf_rec_bytes, sd->dbf) == sd->dbf_rec_bytes) {
                sd->dbf_read++;
                size_t off = 1; /* skip deletion flag */
                for (size_t i = 0; i < sd->nfields; i++) {
                    lp_buf_byte(&sd->out, '\t');
                    size_t fl = sd->fields[i].length;
                    if (off + fl <= sd->dbf_rec_bytes) {
                        size_t s = 0, e = fl;
                        const char *fp = (const char *)(rec + off);
                        while (s < e && fp[s] == ' ') s++;
                        while (e > s && (fp[e - 1] == ' ' || fp[e - 1] == '\0')) e--;
                        emit_tsv(&sd->out, fp + s, e - s);
                    }
                    off += fl;
                }
            }
            free(rec);
        }
        lp_buf_byte(&sd->out, '\n');
    }

    size_t avail = sd->out.n - sd->out_at;
    size_t take = avail < capacity ? avail : capacity;
    if (take) {
        memcpy(out, sd->out.b + sd->out_at, take);
        sd->out_at += take;
    }
    return take;
}

static int shapefile_error(void *ctx){
    ShpDecoder *sd = ctx;
    return sd ? sd->failed : 1;
}

static void shapefile_close(void *ctx){
    ShpDecoder *sd = ctx;
    if (!sd) return;
    if (sd->shp) fclose(sd->shp);
    if (sd->dbf) fclose(sd->dbf);
    free(sd->fields);
    lp_buf_free(&sd->out);
    free(sd);
}

const InputDecoder shapefile_decoder = {
    "shapefile", shapefile_open, shapefile_read, shapefile_error, shapefile_close
};
