#!/usr/bin/env python3
"""Read one complete wide Native claim from PostgreSQL through production CLI policy."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
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
      INSERT INTO consensus SELECT id,1500,350,0.06,4294967313 FROM expected;''')
    parts=[chr(1024+i) for i in range(80)]
    expected='['+', '.join(parts)+']'
    for selectors in [(parts[0],'?',parts[-1]),(parts[0],parts[65],parts[-1]),(parts[0],parts[65],'?')]:
        report=hop(*selectors)
        assert 'attested: 1 claim' in report and expected in report and '4294967313' in report,report
    assert 'attested: 0 claims' in hop(parts[0],'?',parts[11])
    # Positional provenance selection follows the complete tuple too.
    result=subprocess.run([str(engine),'held','-d',connection,*parts],capture_output=True,
                          text=True,encoding='utf-8',timeout=120,env=dict(os.environ,OMP_NUM_THREADS='2'))
    if result.returncode:raise RuntimeError(result.stdout+result.stderr)
    assert '1 claims' in result.stdout,result.stdout
    print('claim-width: all 80 operands, 64-bit observation count, late middle match, true last operand and complete positional selection passed')
    # Follow a lexical relation whose actual endpoint lies beyond the former
    # 4096-operand buffer. Every node uses Native composition and coordinates.
    sql(database,'''CREATE TEMP TABLE navigation AS
      SELECT ch name,laplace_cp_id(ascii(ch)) id,
        ST_GeomFromEWKB(laplace_cp_coord_ewkb(ascii(ch))) coord,
        0::smallint tier,NULL::geometry path,false claim
      FROM unnest(ARRAY['A','B','C','E','F','R']) ch;
      INSERT INTO navigation
      SELECT name,laplace_compose(parts),
        (SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(n.coord))
          FROM unnest(parts) u(id) JOIN navigation n USING(id)),
        1,laplace_path(parts),false
      FROM (VALUES ('left sense',ARRAY[laplace_cp_id(65),laplace_cp_id(69),laplace_cp_id(67)]),
                   ('right sense',ARRAY[laplace_cp_id(66),laplace_cp_id(70),laplace_cp_id(67)])) v(name,parts);
      CREATE TEMP TABLE long_relation AS
      SELECT ARRAY[laplace_cp_id(66),laplace_cp_id(82)] ||
        array_agg(laplace_cp_id(4096+i) ORDER BY i) ||
        ARRAY[(SELECT id FROM navigation WHERE name='right sense')] parts,
        array_agg(ST_GeomFromEWKB(laplace_cp_coord_ewkb(4096+i)) ORDER BY i) coords
      FROM generate_series(0,4999) i;
      INSERT INTO navigation
      SELECT 'left claim',laplace_compose(parts),
        (SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(n.coord))
          FROM unnest(parts) u(id) JOIN navigation n USING(id)),
        2,laplace_path(parts),true
      FROM (SELECT ARRAY[laplace_cp_id(65),laplace_cp_id(82),
        (SELECT id FROM navigation WHERE name='left sense')] parts) v;
      INSERT INTO navigation
      SELECT 'right claim',laplace_compose(parts),
        (SELECT ST_GeomFromEWKB(laplace_centroid4d_ewkb(coord)) FROM (
          SELECT unnest(coords) coord UNION ALL
          SELECT coord FROM navigation WHERE name IN ('B','R','right sense')) points),
        2,laplace_path(parts),true FROM long_relation;
      INSERT INTO entity(id,tier,coord,hilbert,mask)
      SELECT id,tier,coord,laplace_hilbert4(coord) # '-9223372036854775808'::bigint,
        CASE WHEN claim THEN laplace_mask_slot('kind','claim') ELSE 0::bit(64) END
      FROM navigation WHERE tier>0;
      INSERT INTO physicality(entity,tier,hilbert,path,mask)
      SELECT id,tier,laplace_hilbert4(coord) # '-9223372036854775808'::bigint,path,
        CASE WHEN claim THEN laplace_mask_slot('kind','claim') ELSE 0::bit(64) END
      FROM navigation WHERE tier>0;
      INSERT INTO consensus SELECT id,1500,350,0.06,1 FROM navigation WHERE claim;''')
    with tempfile.TemporaryDirectory(prefix='claim-navigation-',dir=os.environ.get('LAPLACE_WORK') or os.environ.get('RUNNER_TEMP')) as scratch:
        root=Path(scratch)
        recipes=root/'recipes'
        shutil.copytree(Path(__file__).resolve().parents[1]/'recipes',recipes)
        (recipes/'wide-navigation.recipe').write_text(
            'name wide-navigation\nformat xml\nthing Sense lemma language concept\nrelation "R" Sense\n',encoding='utf-8')
        firmware=root/'navigation.firmware'
        firmware.write_text('for translate\nlexicalization wide-navigation Sense language concept\nfan 12\n',encoding='utf-8')
        result=subprocess.run([str(engine),'translate','-d',connection,'--firmware',str(firmware),'A','E','F'],
                              capture_output=True,text=True,encoding='utf-8',timeout=120,
                              env=dict(os.environ,OMP_NUM_THREADS='2',LAPLACE_RECIPES=str(recipes)))
        assert result.returncode==0 and re.search(r'^\s+F\s+B\s*$',result.stdout,re.MULTILINE),result.stdout+result.stderr
    print('claim-navigation: translation follows the true endpoint of a 5003-operand relation through the shared reader')
finally:
    if created:sql('postgres',f'DROP DATABASE "{database}" WITH (FORCE)')
