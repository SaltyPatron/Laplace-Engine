#!/usr/bin/env python3
"""Read one complete wide Native claim from PostgreSQL through production CLI policy."""
import os
from pathlib import Path
import subprocess
import sys
import uuid


engine=Path(sys.argv[1]).resolve()
psql=os.environ.get('PSQL','psql')
host,port,user=(os.environ.get(key,default) for key,default in
                [('PGHOST','/tmp'),('PGPORT','5432'),('PGUSER','laplace')])
database='laplace_claim_width_'+uuid.uuid4().hex[:12]
connection=f'host={host} port={port} user={user} dbname={database}'
created=False


def sql(db,query):
    result=subprocess.run([psql,'-XwAtq','-h',host,'-p',port,'-U',user,'-d',db,
                           '-v','ON_ERROR_STOP=1','-c',query],capture_output=True,text=True,encoding='utf-8',timeout=180)
    if result.returncode:raise RuntimeError(result.stderr)
    return result.stdout.strip()


def hop(*selectors):
    result=subprocess.run([str(engine),'hop','-d',connection,*selectors],capture_output=True,
                          text=True,encoding='utf-8',timeout=120,env=dict(os.environ,OMP_NUM_THREADS='2'))
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    return result.stdout


try:
    sql('postgres',f'CREATE DATABASE "{database}"');created=True
    sql(database,'''CREATE EXTENSION laplace CASCADE;
      CREATE TEMP TABLE expected AS
      WITH operands AS (SELECT i,laplace_cp_id(1024+i) id,
        ST_GeomFromEWKB(laplace_cp_coord_ewkb(1024+i)) point FROM generate_series(0,79) i)
      SELECT laplace_compose(array_agg(id ORDER BY i)) id,
        ST_GeomFromEWKB(laplace_centroid4d_ewkb(point)) coord,
        laplace_path(array_agg(id ORDER BY i)) path FROM operands;
      INSERT INTO entity(id,tier,coord,hilbert,mask)
        SELECT id,1,coord,laplace_hilbert4(coord) # '-9223372036854775808'::bigint,
          laplace_mask_slot('kind','claim') FROM expected;
      INSERT INTO physicality(entity,tier,hilbert,path,mask)
        SELECT id,1,laplace_hilbert4(coord) # '-9223372036854775808'::bigint,path,
          laplace_mask_slot('kind','claim') FROM expected;
      INSERT INTO consensus SELECT id,1500,350,0.06,1 FROM expected;''')
    parts=[chr(1024+i) for i in range(80)]
    expected='['+', '.join(parts)+']'
    for selectors in [(parts[0],'?',parts[-1]),(parts[0],parts[65],parts[-1]),(parts[0],parts[65],'?')]:
        report=hop(*selectors)
        assert 'attested: 1 claim' in report and expected in report,report
    assert 'attested: 0 claims' in hop(parts[0],'?',parts[11])
    # Positional provenance selection follows the complete tuple too.
    result=subprocess.run([str(engine),'held','-d',connection,*parts],capture_output=True,
                          text=True,encoding='utf-8',timeout=120,env=dict(os.environ,OMP_NUM_THREADS='2'))
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    assert '1 claims' in result.stdout,result.stdout
    print('claim-width: all 80 operands, late middle match, true last operand and complete positional selection passed')
finally:
    if created:sql('postgres',f'DROP DATABASE "{database}" WITH (FORCE)')
