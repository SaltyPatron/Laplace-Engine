#!/usr/bin/env python3
"""Replay real files through shared OS metadata and deep recorded ancestry.

The fixture keeps canonical IDs, coordinates and closed stored subtrees. It
repackages recorded metadata in its private database; source bytes stay put.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import uuid


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("engine", type=Path)
    args = parser.parse_args()
    engine = args.engine.resolve()
    psql = os.environ.get("PSQL", "psql")
    connection = {"host": os.environ.get("PGHOST", "/tmp"),
                  "port": os.environ.get("PGPORT", "5432"),
                  "user": os.environ.get("PGUSER", "laplace")}
    db = "laplace_precheck_" + uuid.uuid4().hex[:12]
    created = False

    def sql(database, statement):
        result = subprocess.run([psql, "-XwAtq", "-h", connection["host"], "-p", connection["port"],
                                 "-U", connection["user"], "-d", database,
                                 "-v", "ON_ERROR_STOP=1", "-c", statement],
                                capture_output=True, text=True, encoding="utf-8")
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.strip()

    try:
        with tempfile.TemporaryDirectory(prefix="file-precheck-", dir=os.environ.get("LAPLACE_WORK")) as scratch:
            root = Path(scratch)
            recipes = root / "recipes"
            shutil.copytree(Path(__file__).resolve().parents[1] / "recipes", recipes)
            # Hard links are one OS artifact admitted at two occurrences. This
            # declared metadata policy omits their two path spellings only.
            stock = recipes / "file.recipe"
            stock.write_text(stock.read_text(encoding="utf-8").replace("metadata pathname filename ", "metadata ")
                             + "\nomit pathname filename\n", encoding="utf-8", newline="\n")
            inputs = root / "inputs"
            inputs.mkdir()
            (inputs / "a.txt").write_text("Shared recorded words.\n", encoding="utf-8", newline="\n")
            os.link(inputs / "a.txt", inputs / "b.txt")
            (inputs / "c.txt").write_text("A distinct file follows both occurrences.\n", encoding="utf-8", newline="\n")
            env = os.environ.copy()
            env.update(LAPLACE_RECIPES=str(recipes), LAPLACE_INGEST_ONE="1",
                       LAPLACE_WORKING_SET_BYTES=str(1 << 28))
            conn = " ".join(f"{key}='{value.replace(chr(39), chr(92)+chr(39))}'"
                            for key, value in [*connection.items(), ("dbname", db)])

            def ingest(name):
                work = root / (name + "-work")
                work.mkdir()
                env["LAPLACE_WORK"] = str(work)
                result = subprocess.run([str(engine), "ingest", "-r", str(recipes), "-d", conn,
                                         "-j", "2", str(inputs)], env=env, capture_output=True,
                                        text=True, encoding="utf-8", timeout=180)
                report = result.stdout + result.stderr
                (root / (name + ".log")).write_text(report, encoding="utf-8")
                if result.returncode:
                    raise RuntimeError(report)
                return report

            sql("postgres", 'CREATE DATABASE "' + db + '"')
            created = True
            sql(db, "CREATE EXTENSION laplace CASCADE")
            ingest("initial")
            assert sql(db, "SELECT count(*) FROM public.witness") == "2", "hard-link fixture did not share its canonical file"

            before = sql(db, """
                SELECT md5(string_agg(to_jsonb(e)::text,E'\n' ORDER BY id)) FROM public.entity e;
                SELECT md5(string_agg(to_jsonb(p)::text,E'\n' ORDER BY entity)) FROM public.physicality p;
                SELECT md5(string_agg(to_jsonb(w)::text,E'\n' ORDER BY id)) FROM public.witness w;
                SELECT md5(string_agg(to_jsonb(c)::text,E'\n' ORDER BY claim)) FROM public.consensus c;
            """)
            replay = ingest("shared")
            assert re.search(r"3 files recorded already", replay), replay
            after = sql(db, """
                SELECT md5(string_agg(to_jsonb(e)::text,E'\n' ORDER BY id)) FROM public.entity e;
                SELECT md5(string_agg(to_jsonb(p)::text,E'\n' ORDER BY entity)) FROM public.physicality p;
                SELECT md5(string_agg(to_jsonb(w)::text,E'\n' ORDER BY id)) FROM public.witness w;
                SELECT md5(string_agg(to_jsonb(c)::text,E'\n' ORDER BY claim)) FROM public.consensus c;
            """)
            assert before == after, "shared metadata replay changed canonical state"

            # Extend each metadata path by 32 canonical parents. Retire the old
            # file roots AND all ancestors, so every remaining stored parent
            # still owns its complete subtree. No content descendant is lost.
            expected = json.loads(sql(db, """
                CREATE TEMP TABLE old_files AS
                  SELECT p.entity,(public.laplace_vertex_ids(public.ST_PointN(p.path,1)))[1] AS metadata,
                         (public.laplace_vertex_ids(public.ST_PointN(p.path,2)))[1] AS content
                  FROM public.witness w JOIN public.physicality p ON p.entity=w.id;
                CREATE TEMP TABLE removed(id public.blake3 PRIMARY KEY);
                INSERT INTO removed SELECT entity FROM old_files;
                DO $walk$ DECLARE added bigint; BEGIN LOOP
                  INSERT INTO removed SELECT DISTINCT p.entity FROM removed r
                    JOIN public.physicality p ON p.path @> ARRAY[r.id] ON CONFLICT DO NOTHING;
                  GET DIAGNOSTICS added=ROW_COUNT; EXIT WHEN added=0;
                END LOOP; END $walk$;
                DELETE FROM public.witness w USING removed r WHERE w.id=r.id;
                DELETE FROM public.consensus c USING removed r WHERE c.claim=r.id;
                DELETE FROM public.physicality p USING removed r WHERE p.entity=r.id;
                DELETE FROM public.entity e USING removed r WHERE e.id=r.id;
                CREATE FUNCTION pg_temp.node(parts public.blake3[],metadata boolean DEFAULT false)
                RETURNS public.blake3 LANGUAGE plpgsql AS $node$
                DECLARE id public.blake3:=public.laplace_compose(parts);
                  path public.geometry:=public.laplace_path(parts); point public.geometry;
                  projected record; altitude smallint;
                BEGIN
                  SELECT * INTO projected FROM public.laplace_project_path(id,path,parts,
                    ARRAY(SELECT coalesce(e.coord,public.ST_GeomFromEWKB(public.laplace_cp_coord_ewkb(public.laplace_codepoint(u.child))))
                          FROM unnest(parts) WITH ORDINALITY u(child,n)
                          LEFT JOIN public.entity e ON e.id=u.child ORDER BY u.n));
                  SELECT (max(coalesce(e.tier,0))+1)::smallint INTO altitude
                    FROM unnest(parts) u(child) LEFT JOIN public.entity e ON e.id=u.child;
                  IF metadata THEN
                    point:=public.ST_PointN(path,1);
                    path:=public.ST_SetPoint(path,0,public.ST_MakePoint(public.ST_X(point),public.ST_Y(point),
                      public.ST_Z(point),public.ST_M(point)+4294967296));
                  END IF;
                  INSERT INTO public.entity(id,tier,coord,hilbert)
                    VALUES(id,altitude,public.ST_GeomFromEWKB(projected.coord_ewkb),projected.hilbert);
                  INSERT INTO public.physicality(entity,tier,hilbert,path) VALUES(id,altitude,projected.hilbert,path);
                  RETURN id;
                END $node$;
                CREATE TEMP TABLE new_files(id public.blake3 PRIMARY KEY);
                DO $wrap$ DECLARE f record; m public.blake3; BEGIN
                  FOR f IN SELECT * FROM old_files ORDER BY entity LOOP
                    m:=f.metadata;
                    FOR depth IN 1..32 LOOP m:=pg_temp.node(ARRAY[m,public.laplace_cp_id(33)]); END LOOP;
                    INSERT INTO new_files VALUES(pg_temp.node(ARRAY[m,f.content],true));
                  END LOOP;
                END $wrap$;
                SELECT jsonb_agg(id::text ORDER BY id) FROM new_files;
            """))
            deep = ingest("deep")
            assert re.search(r"3 files recorded already", deep), deep
            observed = json.loads(sql(db, "SELECT jsonb_agg(id::text ORDER BY id) FROM public.witness"))
            assert observed == expected, "deep replay selected different file trunks"
            assert sql(db, """
                SELECT count(*) FROM public.physicality p
                CROSS JOIN LATERAL unnest(public.laplace_vertex_ids(p.path)) u(child)
                LEFT JOIN public.entity e ON e.id=u.child
                WHERE e.id IS NULL AND public.laplace_codepoint(u.child) IS NULL
            """) == "0", "fixture or replay left an open subtree"
            print(json.dumps({"shared_metadata_occurrences": 3, "canonical_files": 2,
                              "unchanged_replay": "identical", "metadata_ancestry": 32,
                              "deep_replay": "same recorded trunks", "closed_subtrees": True}))
    finally:
        if created:
            sql("postgres", 'DROP DATABASE "' + db + '"')


if __name__ == "__main__":
    main()
