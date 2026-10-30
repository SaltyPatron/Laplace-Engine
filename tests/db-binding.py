#!/usr/bin/env python3
"""Reuse the production libpq adapter across distinct installed namespaces and type OIDs."""
import argparse
import os
from pathlib import Path
import subprocess
import re
import shutil
import tempfile
import uuid


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('adapter', type=Path)
    parser.add_argument('engine', type=Path)
    args=parser.parse_args()
    adapter,engine=args.adapter.resolve(),args.engine.resolve()
    psql = os.environ.get('PSQL', 'psql')
    connection = {name: os.environ.get(variable, default) for name, variable, default in
                  [('host', 'PGHOST', '/tmp'), ('port', 'PGPORT', '5432'), ('user', 'PGUSER', 'laplace')]}
    databases = []
    prefix = 'laplace_binding_' + uuid.uuid4().hex[:12]

    def sql(database, query):
        result = subprocess.run([psql, '-XwAtq', '-h', connection['host'], '-p', connection['port'],
                                 '-U', connection['user'], '-d', database, '-v', 'ON_ERROR_STOP=1', '-c', query],
                                capture_output=True, text=True, encoding='utf-8', timeout=180)
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.strip()

    try:
        connections = []
        for suffix in ['a', 'b']:
            database = prefix + '_' + suffix
            sql('postgres', 'CREATE DATABASE "' + database + '"')
            databases.append(database)
            sql(database, f'''
              CREATE SCHEMA "spatial {suffix}";
              CREATE EXTENSION postgis SCHEMA "spatial {suffix}";
              CREATE DOMAIN public.blake3 AS integer;
              CREATE TABLE public.entity(id integer);
              INSERT INTO public.entity VALUES(17);
            ''')
            values = [*connection.items(), ('dbname', database), ('options', '-c search_path=pg_catalog')]
            connections.append(' '.join(f"{name}='{value.replace(chr(39), chr(92)+chr(39))}'"
                                        for name, value in values))
            installed=subprocess.run([str(engine),'deploy','--schema',f'native {suffix}','-d',connections[-1]],
                                     capture_output=True,text=True,encoding='utf-8',timeout=180)
            if installed.returncode:raise RuntimeError(installed.stdout+installed.stderr)
        result = subprocess.run([str(adapter), *connections], capture_output=True, text=True,
                                encoding='utf-8', timeout=60)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        print(result.stdout.strip())
        database=databases[0]
        # A binary repair can retain the SQL version. Simulate a predecessor
        # binding through another spelling of the same installed module, then
        # require the normal deploy command to restore its canonical binding.
        module=sql(database,'''SELECT probin FROM pg_catalog.pg_proc
          WHERE oid='"native a".laplace_isa()'::regprocedure''')
        parent,name=module.rsplit('/',1)
        alias=parent+'/./'+name
        alias_literal="'"+alias.replace("'","''")+"'"
        definition=sql(database,"""SELECT pg_catalog.format(
          'CREATE OR REPLACE FUNCTION %I.laplace_isa() RETURNS text LANGUAGE C STABLE PARALLEL SAFE AS %L,%L',
          'native a',"""+alias_literal+",'laplace_isa')")
        sql(database,definition)
        assert sql(database,'''SELECT probin FROM pg_catalog.pg_proc
          WHERE oid='"native a".laplace_isa()'::regprocedure''')==alias,(module,alias,definition)
        repaired=subprocess.run([str(engine),'deploy','-d',connections[0]],
                                capture_output=True,text=True,encoding='utf-8',timeout=180)
        if repaired.returncode:raise RuntimeError(repaired.stdout+repaired.stderr)
        current_module=sql(database,'''SELECT probin FROM pg_catalog.pg_proc
          WHERE oid='"native a".laplace_fingerprint()'::regprocedure''')
        assert sql(database,'''SELECT probin FROM pg_catalog.pg_proc
          WHERE oid='"native a".laplace_isa()'::regprocedure''')==current_module
        before=sql(database,"SELECT coalesce(array_to_string(setconfig,E'\\n'),'') FROM pg_catalog.pg_db_role_setting WHERE setdatabase=(SELECT oid FROM pg_catalog.pg_database WHERE datname=current_database()) ORDER BY setrole")
        with tempfile.TemporaryDirectory(prefix='db-binding-',dir=os.environ.get('LAPLACE_WORK')) as scratch:
            root=Path(scratch);recipes=root/'recipes'
            shutil.copytree(Path(__file__).resolve().parents[1]/'recipes',recipes)
            shutil.copytree(Path(__file__).resolve().parents[1]/'firmware',root/'firmware')
            definitions=recipes/'binding-reading';definitions.mkdir()
            (definitions/'source').write_text('name binding-reading\nwitness Native namespace binding\nclass AcademicCurated\nroot $LAPLACE_DATA/binding\n',encoding='utf-8',newline='\n')
            (definitions/'reading.recipe').write_text('name binding-reading\nmatch *.tsv\ntier row by \\n\nnames thing value\ntier field by \\t\nthing row thing\ncontent thing value\nrelation "has value" value\nattest row value\n',encoding='utf-8',newline='\n')
            data=root/'data/binding';data.mkdir(parents=True)
            (data/'facts.tsv').write_text('dog\twarm\ncat\tcold\ndog\twarm\n',encoding='utf-8',newline='\n')
            work=root/'work';work.mkdir()
            env=dict(os.environ,LAPLACE_DATA=str(root/'data'),LAPLACE_WORK=str(work),LAPLACE_RECIPES=str(recipes),LAPLACE_INGEST_ONE='1',OMP_NUM_THREADS='2')
            def run(*arguments):
                completed=subprocess.run([str(engine),*arguments],env=env,capture_output=True,text=True,encoding='utf-8',timeout=180)
                if completed.returncode:raise RuntimeError(completed.stdout+completed.stderr)
                return completed.stdout+completed.stderr
            run('deploy','-d',connections[0])
            after=sql(database,"SELECT coalesce(array_to_string(setconfig,E'\\n'),'') FROM pg_catalog.pg_db_role_setting WHERE setdatabase=(SELECT oid FROM pg_catalog.pg_database WHERE datname=current_database()) ORDER BY setrole")
            assert before==after,(before,after)
            mismatch=subprocess.run([str(engine),'deploy','--schema','other','-d',connections[0]],capture_output=True,text=True,timeout=60)
            assert mismatch.returncode and 'does not move' in mismatch.stderr,mismatch.stdout+mismatch.stderr
            assert sql(database,"SELECT count(*) FROM pg_catalog.pg_depend WHERE classid='pg_catalog.pg_class'::regclass AND objid='public.entity'::regclass AND deptype='e'")=='0'
            command=['ingest','-r',str(recipes),'-d',connections[0],'-j','2','-s','binding-reading',str(data)]
            report=run(*command)
            assert 'mismatched 0' in report,report
            def recorded():
                return sql(database,'SELECT count(*) FROM "native a".entity; SELECT count(*) FROM "native a".physicality; SELECT count(*) FROM "native a".witness; SELECT count(*) FROM "native a".consensus')
            counts=recorded();assert all(int(value)>0 for value in counts.splitlines()),counts
            run(*command);assert recorded()==counts
            run('hop','-d',connections[0],'dog')
            replay=run('replay','-d',connections[0]);assert '0 different' in replay,replay
            run('index','-d',connections[0])
            witness=re.search(r'its ID, the witness\s+([a-f0-9]{32})',report);assert witness,report
            run('forget','-d',connections[0],'--trunk',witness[1])
            assert sql(database,'SELECT count(*) FROM "native a".witness')=='0'
            assert sql(database,'SELECT id FROM public.entity')=='17'
            run('deploy','--configure-server-cache','-d',connections[1])
            assert sql(databases[1],"SELECT current_setting('laplace.tier0')")==os.environ['LAPLACE_TIER0']
        print('db-binding: real ingestion, replay, indexed reads and removal preserve foreign public objects and server configuration')
    finally:
        for database in reversed(databases):
            sql('postgres', 'DROP DATABASE "' + database + '" WITH (FORCE)')


if __name__ == '__main__':
    main()
