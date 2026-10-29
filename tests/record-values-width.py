"""Record value tuples retain every parsed member across worker counts."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


engine=Path(sys.argv[1]).resolve()
checkout=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='record-values-width-',dir=os.environ.get('LAPLACE_WORK') or os.environ.get('RUNNER_TEMP')) as scratch:
    root=Path(scratch);recipes=root/'recipes'
    shutil.copytree(checkout/'recipes',recipes)
    definitions=recipes/'value-width';definitions.mkdir()
    (definitions/'source').write_text('name value-width\nwitness Complete parsed values\nclass AcademicCurated\nroot $LAPLACE_DATA/values\n',encoding='utf-8')
    (definitions/'values.recipe').write_text('name value-width\nmatch *.tsv\ntier row by \\n\nnames thing values\ntier field by \\t\npart values by space\nthing row values\ncontent thing values\nrelation "named" thing\nattest row thing\n',encoding='utf-8')
    data=root/'data/values';data.mkdir(parents=True)
    work=root/'work';work.mkdir()
    env=dict(os.environ,LAPLACE_DATA=str(root/'data'),LAPLACE_WORK=str(work),LAPLACE_RECIPES=str(recipes),LAPLACE_INGEST_ONE='1')
    for width in (64,65,512):
        values=[f'v{i:04}' for i in range(width)]
        path=data/'values.tsv';path.write_text('subject\t'+' '.join(values)+'\n',encoding='utf-8')
        expected=''.join(values)
        previous=None
        for workers in (1,4):
            result=subprocess.run([str(engine),'ingest','--claims-tsv','-d','host=127.0.0.1 port=1 connect_timeout=1',
                                   '-j',str(workers),'-s','value-width',str(path)],env=env,capture_output=True,
                                  text=True,encoding='utf-8',timeout=60)
            assert result.returncode==0,result.stdout+result.stderr
            claims=[line.split('\t')[3:] for line in result.stdout.splitlines() if line.startswith('claim\t')]
            observed=[parts for parts in claims if parts[1:]==['named','subject']]
            assert observed==[[expected,'named','subject']],observed
            assert previous is None or claims==previous,(previous,claims)
            previous=claims
    print('record-values-width: all 64, 65 and 512 members survive production parsing and tuple composition with 1 and 4 workers')
