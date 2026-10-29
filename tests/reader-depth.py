#!/usr/bin/env python3
"""Qualify the production C text/parts reader over a real private database."""
import argparse
import os
from pathlib import Path
import subprocess
import uuid


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("reader", type=Path)
    args = parser.parse_args()
    psql = os.environ.get("PSQL", "psql")
    values = {"host": os.environ.get("PGHOST", "/tmp"), "port": os.environ.get("PGPORT", "5432"),
              "user": os.environ.get("PGUSER", "laplace")}
    db = "laplace_reader_" + uuid.uuid4().hex[:12]
    subprocess.run([str(args.reader.resolve()), os.environ["LAPLACE_TIER0"]], check=True)

    def sql(database, query):
        result = subprocess.run([psql, "-XwAt", "-h", values["host"], "-p", values["port"],
                                 "-U", values["user"], "-d", database, "-v", "ON_ERROR_STOP=1", "-c", query],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.strip()

    sql("postgres", 'CREATE DATABASE "' + db + '"')
    try:
        sql(db, "CREATE EXTENSION laplace CASCADE")
        identifiers = sql(db, """
          CREATE FUNCTION pg_temp.pair_coord(a geometry,b geometry) RETURNS geometry
          LANGUAGE SQL IMMUTABLE BEGIN ATOMIC
            SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(point)) FROM (VALUES(a),(b)) p(point);
          END;
          CREATE TEMP TABLE expected AS
          WITH RECURSIVE chain(depth,id,coord,path) AS (
            SELECT 0,laplace_cp_id(65),ST_GeomFromEWKB(laplace_cp_coord_ewkb(65)),NULL::geometry
            UNION ALL SELECT depth+1,laplace_compose(ARRAY[id,laplace_cp_id(66)]),
              pg_temp.pair_coord(coord,ST_GeomFromEWKB(laplace_cp_coord_ewkb(66))),
              laplace_path(ARRAY[id,laplace_cp_id(66)]) FROM chain WHERE depth<4096
          ) SELECT *,laplace_hilbert4(coord) # '-9223372036854775808'::bigint hilbert FROM chain WHERE depth>0;
          INSERT INTO entity(id,tier,coord,hilbert) SELECT id,depth::smallint,coord,hilbert FROM expected;
          INSERT INTO physicality(entity,tier,hilbert,path) SELECT id,depth::smallint,hilbert,path FROM expected;
          CREATE TEMP TABLE extra AS
          SELECT 'run' name,array_fill(laplace_cp_id(65),ARRAY[100000]) parts
          UNION ALL SELECT 'annotation',ARRAY[laplace_cp_id(65),laplace_cp_id(67),laplace_cp_id(68)];
          CREATE TEMP TABLE extra_nodes AS SELECT name,laplace_compose(parts) id,laplace_path(parts) path,
            ST_GeomFromEWKB((SELECT laplace_centroid4d_ewkb(ST_GeomFromEWKB(laplace_cp_coord_ewkb(laplace_codepoint(x))))
                            FROM unnest(parts) x)) coord FROM extra;
          UPDATE extra_nodes SET path=ST_SetPoint(ST_SetPoint(path,1,ST_MakePoint(
            ST_X(ST_PointN(path,2)),ST_Y(ST_PointN(path,2)),ST_Z(ST_PointN(path,2)),ST_M(ST_PointN(path,2))+1073741824)),
            2,ST_MakePoint(ST_X(ST_PointN(path,3)),ST_Y(ST_PointN(path,3)),ST_Z(ST_PointN(path,3)),ST_M(ST_PointN(path,3))+6442450944))
            WHERE name='annotation';
          INSERT INTO entity(id,tier,coord,hilbert) SELECT id,1,coord,laplace_hilbert4(coord) # '-9223372036854775808'::bigint FROM extra_nodes;
          INSERT INTO physicality(entity,tier,hilbert,path) SELECT id,1,laplace_hilbert4(coord) # '-9223372036854775808'::bigint,path FROM extra_nodes;
          CREATE TEMP TABLE batch_branches AS
          SELECT i,laplace_compose(ARRAY[laplace_cp_id(1024+i),laplace_cp_id(65)]) id,
            laplace_path(ARRAY[laplace_cp_id(1024+i),laplace_cp_id(65)]) path,
            pg_temp.pair_coord(ST_GeomFromEWKB(laplace_cp_coord_ewkb(1024+i)),
                               ST_GeomFromEWKB(laplace_cp_coord_ewkb(65))) coord
          FROM generate_series(0,31) i;
          INSERT INTO entity(id,tier,coord,hilbert)
            SELECT id,1,coord,laplace_hilbert4(coord) # '-9223372036854775808'::bigint FROM batch_branches;
          INSERT INTO physicality(entity,tier,hilbert,path)
            SELECT id,1,laplace_hilbert4(coord) # '-9223372036854775808'::bigint,path FROM batch_branches;
          CREATE TABLE public.reader_batch_fixture AS
          SELECT i,laplace_compose(ARRAY[id,laplace_cp_id(66)]) id,
            laplace_path(ARRAY[id,laplace_cp_id(66)]) path,
            pg_temp.pair_coord(coord,ST_GeomFromEWKB(laplace_cp_coord_ewkb(66))) coord FROM batch_branches;
          INSERT INTO entity(id,tier,coord,hilbert)
            SELECT id,2,coord,laplace_hilbert4(coord) # '-9223372036854775808'::bigint FROM public.reader_batch_fixture;
          INSERT INTO physicality(entity,tier,hilbert,path)
            SELECT id,2,laplace_hilbert4(coord) # '-9223372036854775808'::bigint,path FROM public.reader_batch_fixture;
          SELECT id::text FROM expected WHERE depth=4096;
          SELECT id::text FROM extra_nodes WHERE name='run';
          SELECT id::text FROM extra_nodes WHERE name='annotation';
        """).splitlines()
        identifiers = [value for value in identifiers if len(value) == 32 and all(c in "0123456789abcdef" for c in value)]
        assert len(identifiers) == 3, identifiers
        connection = " ".join(f"{key}='{value.replace(chr(39), chr(92)+chr(39))}'"
                              for key, value in [*values.items(), ("dbname", db)])
        command = [str(args.reader.resolve()), connection, os.environ["LAPLACE_TIER0"], *identifiers]
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        assert result.returncode == 0, result.stderr + result.stdout
        print(result.stdout.strip())
        sql(db, f"UPDATE physicality SET path=laplace_path(ARRAY['{identifiers[0]}'::blake3,laplace_cp_id(66)]) WHERE entity='{identifiers[0]}'::blake3")
        result = subprocess.run([*command, "--cycle"], capture_output=True, text=True, timeout=30)
        assert result.returncode != 0 and "reader: cyclic composition" in result.stderr, result.stderr + result.stdout
        print("reader: corrupt cycles rejected explicitly")
    finally:
        sql("postgres", 'DROP DATABASE "' + db + '"')


if __name__ == "__main__":
    main()
