#!/usr/bin/env python3
"""Real COPY/publication, closed subtrees during interruption, and exact replay.

Uses private databases on the configured PostgreSQL host, the actual Native
binary, and local UTF-8 input. No Python database driver or external corpus.
"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import uuid


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("--words", type=int, default=65536)
    args = parser.parse_args()
    engine = args.engine.resolve()
    psql = os.environ.get("PSQL", "psql")
    connection = ["-h", os.environ.get("PGHOST", "/tmp"), "-p", os.environ.get("PGPORT", "5432"),
                  "-U", os.environ.get("PGUSER", "laplace")]
    prefix = "laplace_publish_" + uuid.uuid4().hex[:12]
    databases = []
    processes = []

    def stop_processes():
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=30)

    def sql(db, query):
        result = subprocess.run([psql, "-XwAt", *connection, "-d", db, "-v", "ON_ERROR_STOP=1", "-c", query],
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
        return result.stdout.strip()

    def make_database(suffix):
        db = prefix + "_" + suffix
        sql("postgres", 'CREATE DATABASE "' + db + '"')
        databases.append(db)
        sql(db, "CREATE EXTENSION laplace CASCADE")
        return db

    def fingerprint(db):
        return sql(db, """
            SELECT 'entity',count(*),md5(string_agg(to_jsonb(e)::text,E'\n' ORDER BY id)) FROM public.entity e;
            SELECT 'physicality',count(*),md5(string_agg(to_jsonb(p)::text,E'\n' ORDER BY entity)) FROM public.physicality p;
            SELECT 'witness',count(*),md5(string_agg(to_jsonb(w)::text,E'\n' ORDER BY id)) FROM public.witness w;
            SELECT 'consensus',count(*),md5(string_agg(to_jsonb(c)::text,E'\n' ORDER BY claim)) FROM public.consensus c;
            SELECT 'series',count(*),md5(string_agg(to_jsonb(c)::text,E'\n' ORDER BY to_jsonb(c)::text))
            FROM public.laplace_contained_series(ARRAY(SELECT id FROM public.witness)) c;
        """)

    def missing_children(db):
        return set(sql(db, """
            SELECT DISTINCT v.id::text FROM public.physicality p
            CROSS JOIN LATERAL unnest(public.laplace_vertex_ids(p.path)) v(id)
            LEFT JOIN public.entity e ON e.id=v.id
            WHERE e.id IS NULL AND public.laplace_codepoint(v.id) IS NULL
        """).splitlines())

    try:
        with contextlib.ExitStack() as resources:
            root = Path(resources.enter_context(tempfile.TemporaryDirectory(prefix="laplace-publication-")))
            resources.callback(stop_processes)
            inputs = root / "inputs"
            inputs.mkdir()
            text = " ".join(f"q{i:08x}" for i in range(args.words)) + "\n"
            (inputs / "ordered.txt").write_text(text, encoding="utf-8", newline="\n")
            base = os.environ.copy()
            base["LAPLACE_RECIPES"] = str(Path(__file__).resolve().parents[1] / "recipes")
            base["LAPLACE_TRUNKS"] = "1"
            base["LAPLACE_WORKING_SET_BYTES"] = str(1 << 30)
            reports = []

            def start(db, writers, name):
                env = base.copy()
                env["LAPLACE_DB_POOL_SIZE"] = str(writers)
                env["LAPLACE_WORK"] = str(root / (name + "-work"))
                conn = " ".join(f"{key}='{value.replace(chr(39), chr(92)+chr(39))}'" for key, value in
                                [("host", connection[1]), ("port", connection[3]),
                                 ("user", connection[5]), ("dbname", db)])
                log = root / (name + ".log")
                with log.open("wb") as output:
                    process = subprocess.Popen([str(engine), "ingest", "-d", conn, "-j", "8", str(inputs)],
                                               env=env, stdout=output, stderr=subprocess.STDOUT)
                processes.append(process)
                return process, log

            serial = make_database("serial")
            before = time.monotonic()
            process, log = start(serial, 1, "serial")
            if process.wait(timeout=180) != 0:
                raise RuntimeError(log.read_text(errors="replace"))
            expected = fingerprint(serial)
            expected_ids = set(sql(serial, "SELECT id::text FROM public.entity").splitlines())
            assert len(expected_ids) > args.words, "fixture must exercise a multi-tier DAG"
            reports.append({"case": "serial", "seconds": round(time.monotonic() - before, 3), "entities": len(expected_ids)})

            parallel = make_database("parallel")
            before = time.monotonic()
            process, log = start(parallel, 8, "parallel")
            observations = 0
            while process.poll() is None:
                assert not (missing_children(parallel) & expected_ids), "visible parent preceded its stored child"
                observations += 1
                time.sleep(0.02)
            if process.returncode != 0:
                raise RuntimeError(log.read_text(errors="replace"))
            assert fingerprint(parallel) == expected, "parallel publication changed canonical state"
            lanes = re.findall(r"up to (\d+) writers", log.read_text(errors="replace"))
            assert lanes and max(map(int, lanes)) > 1, "fixture did not exercise parallel publication"
            reports.append({"case": "parallel_observed", "seconds": round(time.monotonic() - before, 3),
                            "observations": observations, "writers": max(map(int, lanes))})

            interrupted = make_database("interrupted")
            process, log = start(interrupted, 8, "interrupted")
            deadline = time.monotonic() + 180
            while process.poll() is None and time.monotonic() < deadline:
                if int(sql(interrupted, "SELECT count(*) FROM public.physicality")):
                    process.kill()
                    break
                time.sleep(0.02)
            assert process.poll() is None or process.returncode != 0, "fixture finished before interruption"
            process.wait(timeout=30)
            # The killed client's backend may still be finishing a statement.
            # Cancel only this test database's remaining Native statements.
            sql(interrupted, "SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE datname=current_database() AND pid<>pg_backend_pid() AND state='active'")
            for _ in range(100):
                if sql(interrupted, "SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() AND pid<>pg_backend_pid()") == "0":
                    break
                time.sleep(0.05)
            assert not (missing_children(interrupted) & expected_ids), "interruption exposed an incomplete subtree"
            process, resumed_log = start(interrupted, 8, "resumed")
            if process.wait(timeout=180) != 0:
                raise RuntimeError(resumed_log.read_text(errors="replace"))
            assert fingerprint(interrupted) == expected, "interrupted replay changed canonical state"
            process, replay_log = start(interrupted, 8, "replay")
            if process.wait(timeout=180) != 0:
                raise RuntimeError(replay_log.read_text(errors="replace"))
            assert fingerprint(interrupted) == expected, "unchanged replay changed canonical state"
            reports.append({"case": "interruption_and_replay", "closed": True, "identical": True})
            print(json.dumps(reports, indent=2))
    finally:
        for db in reversed(databases):
            sql("postgres", 'DROP DATABASE "' + db + '" WITH (FORCE)')


if __name__ == "__main__":
    main()
