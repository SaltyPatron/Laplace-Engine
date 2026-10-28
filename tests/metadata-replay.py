#!/usr/bin/env python3
"""Real Tatoeba key restoration and context invalidation, without rereading known files."""
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
    host = os.environ.get("PGHOST", "/tmp")
    port = os.environ.get("PGPORT", "5432")
    user = os.environ.get("PGUSER", "laplace")
    databases = []
    prefix = "laplace_metadata_" + uuid.uuid4().hex[:12]

    def sql(db, query):
        result = subprocess.run([psql, "-XwAt", "-h", host, "-p", port, "-U", user,
                                 "-d", db, "-v", "ON_ERROR_STOP=1", "-c", query],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.strip()

    def database(suffix):
        name = prefix + "_" + suffix
        sql("postgres", 'CREATE DATABASE "' + name + '"')
        databases.append(name)
        sql(name, "CREATE EXTENSION laplace CASCADE")
        return name

    def fingerprint(db):
        return sql(db, """
          SELECT 'entity',count(*),md5(string_agg(to_jsonb(e)::text,E'\n' ORDER BY id)) FROM public.entity e;
          SELECT 'path',count(*),md5(string_agg(to_jsonb(p)::text,E'\n' ORDER BY entity)) FROM public.physicality p;
          SELECT 'witness',count(*),md5(string_agg(to_jsonb(w)::text,E'\n' ORDER BY id)) FROM public.witness w;
          SELECT 'standing',count(*),md5(string_agg(to_jsonb(c)::text,E'\n' ORDER BY claim)) FROM public.consensus c;
        """)

    try:
        with tempfile.TemporaryDirectory(prefix="metadata-replay-", dir=os.environ.get("LAPLACE_WORK")) as scratch:
            root = Path(scratch)
            recipes = root / "recipes"
            shutil.copytree(Path(__file__).resolve().parents[1] / "recipes", recipes)
            source = recipes / "tatoeba/source"
            source.write_text("\n".join(line for line in source.read_text().splitlines()
                                        if not line.startswith("after ")) + "\n", newline="\n")
            data = root / "data/Tatoeba"
            data.mkdir(parents=True)
            sentences = data / "sentences.csv"
            links = data / "links.csv"
            sentences.write_text("1\teng\tAlpha native sentence.\n2\tfra\tBeta native sentence.\n",
                                 encoding="utf-8", newline="\n")
            links.write_text("1\t2\n2\t1\n", encoding="utf-8", newline="\n")
            base = os.environ.copy()
            # A relocated package selects its estate with -r. The caller may
            # still carry a build-machine/default path; includes must follow -r.
            base.update(LAPLACE_DATA=str(root / "data"), LAPLACE_RECIPES=str(root / 'missing-build-checkout'),
                        LAPLACE_INGEST_ONE="1", LAPLACE_WORKING_SET_BYTES=str(1 << 28))

            def ingest(db, name):
                env = base.copy()
                work = root / (name + "-work")
                work.mkdir()
                env["LAPLACE_WORK"] = str(work)
                conn = " ".join(f"{key}='{value.replace(chr(39), chr(92)+chr(39))}'" for key, value in
                                [("host", host), ("port", port), ("user", user), ("dbname", db)])
                result = subprocess.run([str(engine), "ingest", "-r", str(recipes), "-d", conn, "-j", "2", "tatoeba"],
                                        env=env, capture_output=True, text=True, timeout=180)
                text = result.stdout + result.stderr
                (root / (name + ".log")).write_text(text, encoding="utf-8")
                if result.returncode:
                    raise RuntimeError(text)
                assert "mismatched 0" in text, text
                return text

            def source_reference(db, report):
                found = re.search(r"its ID, the witness\s+([a-f0-9]{32})", report)
                assert found, report
                witness = found[1]
                return sql(db, f"""
                  SELECT to_jsonb(e)::text FROM public.entity e WHERE id='{witness}'::public.blake3;
                  SELECT md5(string_agg(to_jsonb(s)::text,E'\n' ORDER BY to_jsonb(s)::text))
                  FROM public.laplace_contained_series(ARRAY['{witness}'::public.blake3]) s;
                """)

            warm = database("warm")
            ingest(warm, "initial")
            before = fingerprint(warm)
            replay = ingest(warm, "unchanged")
            assert re.search(r"2 files recorded already", replay), replay
            assert fingerprint(warm) == before, "unchanged replay changed canonical state"

            # Only the consumer changes. Its provider must be restored from
            # recorded metadata, retaining ordered entry fields and targets.
            links.write_text("1\t2\n2\t1\n1\t2\n", encoding="utf-8", newline="\n")
            restored = ingest(warm, "consumer_changed")
            assert re.search(r"restored [1-9][0-9]* compiled reference bindings", restored), restored
            cold = database("restored")
            cold_report = ingest(cold, "cold_consumer")
            assert source_reference(warm, restored) == source_reference(cold, cold_report), "restored provider differs from cold read"

            # The consumer bytes stay unchanged. A changed referenced meaning
            # must invalidate its recorded context and reinterpret that file.
            sentences.write_text("1\teng\tGamma native sentence.\n2\tfra\tBeta native sentence.\n",
                                 encoding="utf-8", newline="\n")
            changed = ingest(warm, "provider_changed")
            assert "source reference context changed; reinterpret" in changed, changed
            cold = database("context")
            cold_report = ingest(cold, "cold_provider")
            assert source_reference(warm, changed) == source_reference(cold, cold_report), "context invalidation differs from cold read"
            print(json.dumps({"unchanged_state": "identical", "provider_restore": "identical to cold",
                              "changed_context": "reinterpreted and identical to cold"}))
    finally:
        for name in reversed(databases):
            sql("postgres", 'DROP DATABASE "' + name + '"')


if __name__ == "__main__":
    main()
