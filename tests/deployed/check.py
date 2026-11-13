"""What every database check of the Engine shares: the deployed database, the deployed program, and a check's own sources.

The database checks run after Laplace-Operations has deployed this revision (its laplace.yml, job `tests`), against
the database that deployment serves (LAPLACE_CONNINFO, laplace.env's) through the program it installed
(LAPLACE_PROGRAM). Nothing here makes, drops or empties a database, and nothing reads a whole table: that database
holds everything else that was ever ingested into it, and keeps it.

A check says what it ingests as sources of its own, each a directory of a scratch copy of this revision's recipes
whose source and recipe witness names begin with WITNESS_PREFIX. Content is content-addressed, so a check run again
converges on the same IDs. A check asserts only on what is under its own witness trunks and on the IDs it composed,
and `laplace forget` takes its witnesses back out before it starts and after it ends. Every program invocation goes
through Fixture.run, which refuses an operation that could reach anything but the check's own sources.
"""
import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WITNESS_PREFIX = 'Laplace check '
SOURCE_PREFIX = 'check-'
# Operations that change what the database holds beyond a named fixture source, or its schema and settings.
REFUSED = {'deploy', 'sweep', 'merge', 'projection', 'replay'}


def connection():
    value = os.environ.get('LAPLACE_CONNINFO', '').strip()
    if not value:
        raise SystemExit('LAPLACE_CONNINFO must name the deployed database (laplace.env)')
    return value


def program():
    value = os.environ.get('LAPLACE_PROGRAM', '')
    if not value or not Path(value).is_file():
        raise SystemExit('LAPLACE_PROGRAM must name the deployed laplace program')
    return Path(value)


def psql():
    pg = os.environ.get('LAPLACE_PG_DIR')
    return str(Path(pg) / 'bin/psql') if pg else 'psql'


def sql(statement, *, variables=None, timeout=300):
    """One psql script against the deployed database; its rows as text. Variables bind as psql :'name'."""
    command = [psql(), connection(), '-XwAt', '-v', 'ON_ERROR_STOP=1']
    for name, value in (variables or {}).items():
        command += ['-v', f'{name}={value}']
    result = subprocess.run(command + ['-f', '-'], input=statement, text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f'psql exited {result.returncode}')
    return result.stdout.replace('\r', '').strip()


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def ids(values):
    """A blake3[] literal of hex IDs."""
    return 'ARRAY[' + ','.join(literal(v) + '::blake3' for v in values) + ']::blake3[]' if values else "'{}'::blake3[]"


def closure(roots):
    """Every stored ID under the given roots, the roots included: the stored DAG walked a tier at a time from the
    client (one set-oriented statement per frontier, so O(tiers) crossings), each frontier's paths read by
    laplace_paths and their constituents by laplace_vertex_ids. Codepoints have no path and end the walk."""
    seen, frontier = set(roots), sorted(set(roots))
    while frontier:
        rows = sql('SELECT DISTINCT v::text FROM laplace_paths(' + ids(frontier) + ') p'
                   ' CROSS JOIN LATERAL unnest(laplace_vertex_ids(p.path)) v')
        frontier = sorted({r for r in rows.splitlines() if r} - seen)
        seen.update(frontier)
    return sorted(seen)


def fingerprint(trunks):
    """What the deployed database holds of the given witness trunks, as text lines to compare: the entity and
    physicality rows of every ID under them (tier above 0), their claims' attestation rows by these witnesses, those
    claims' consensus rows, and the cumulative testimony laplace_contained_series reads under them. Every query is
    constrained by IDs the trunks reach; nothing else of a table is read."""
    if not trunks:
        return ''
    every = ids(closure(trunks))
    claims = ('ARRAY(SELECT DISTINCT claim FROM laplace_contained_series(' + ids(trunks) + '))')
    return sql(f"""
SELECT 'entity',count(*),md5(string_agg(to_jsonb(e)::text,E'\\n' ORDER BY e.id)) FROM entity e
 WHERE e.id=ANY({every}) AND e.tier>0;
SELECT 'physicality',count(*),md5(string_agg(to_jsonb(p)::text,E'\\n' ORDER BY p.entity,md5(ST_AsEWKB(p.path))))
 FROM physicality p WHERE p.entity=ANY({every});
SELECT 'attestation',count(*),md5(string_agg(to_jsonb(a)::text,E'\\n' ORDER BY a.claim,a.witness,a.lineage))
 FROM attestation a WHERE a.claim=ANY({claims}) AND a.witness=ANY({ids(trunks)});
SELECT 'consensus',count(*),md5(string_agg(to_jsonb(c)::text,E'\\n' ORDER BY c.claim))
 FROM consensus c WHERE c.claim=ANY({claims});
SELECT 'series',count(*),md5(string_agg(to_jsonb(s)::text,E'\\n' ORDER BY to_jsonb(s)::text))
 FROM laplace_contained_series({ids(trunks)}) s;
""")


# A source's trunks: the witness trunks that begin with its record; for a source that attests nothing (so is no
# witness), the paths that begin with its record (indexed ^@).
TRUNKS = ("WITH witnessed AS (SELECT DISTINCT p.entity::text e FROM laplace_witness_trust() w"
          " JOIN physicality p ON p.entity=w.witness WHERE p.path ^@ laplace_text_id(:'witness'))"
          " SELECT e FROM witnessed UNION SELECT DISTINCT p.entity::text FROM physicality p"
          " WHERE p.path ^@ laplace_text_id(:'witness') AND NOT EXISTS (SELECT FROM witnessed) ORDER BY 1")


class Fixture:
    """A check's scratch directory, recipes and sources. A check writes its sources, calls forget() for a clean
    slate (a run cut off before its end left its witnesses), ingests and asserts; as a context manager the fixture
    forgets the check's witnesses again on the way out and removes the scratch directory."""

    def __init__(self, check):
        if not re.fullmatch(r'[a-z0-9][a-z0-9-]*', check):
            raise ValueError('check name: ' + check)
        self.check = check
        self.program = program()
        self.scratch = Path(tempfile.mkdtemp(prefix=f'laplace-check-{check}.', dir=os.environ.get('TMPDIR')))
        self.recipes = self.scratch / 'recipes'
        shutil.copytree(Path(os.environ.get('LAPLACE_RECIPES_FROM') or ROOT / 'recipes'), self.recipes)
        self.data = self.scratch / 'data'
        self.data.mkdir()
        self.work = self.scratch / 'work'
        self.sources = []
        self.witnesses = []            # witnesses named outside a source (laplace witness --as, said --as): forgotten by name
        self.trunks_recorded = set()
        self.env = dict(os.environ, LAPLACE_RECIPES=str(self.recipes), LAPLACE_WORK=str(self.work),
                        LAPLACE_CONNINFO=connection(), LAPLACE_DATA=str(self.data))
        self.env.pop('LAPLACE_INGEST_ONE', None)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        try:
            self.forget()
        finally:
            shutil.rmtree(self.scratch, ignore_errors=True)
        return False

    def name(self, part):
        """A fixture source name and its witness, both the check's own."""
        return f'{SOURCE_PREFIX}{self.check}-{part}', f'{WITNESS_PREFIX}{self.check} {part}'

    def source(self, part, lines='', recipes=None, root=None, copy_from=None):
        """Write recipes/<check-...>/source (and recipe files) for a source of this check. `lines` adds directives
        after the name, witness and root; copy_from copies an existing source directory's recipes first (its
        `source` file is replaced). Returns (name, directory)."""
        name, witness = self.name(part)
        directory = self.recipes / name
        if copy_from:
            shutil.copytree(self.recipes / copy_from, directory)
            (directory / 'source').unlink(missing_ok=True)
        directory.mkdir(exist_ok=True)
        root = Path(root) if root else self.data / name
        root.mkdir(parents=True, exist_ok=True)
        text = f'name {name}\nwitness {witness}\n'
        if not re.search(r'^class ', lines or '', re.M):
            text += 'class UserCuratedResource\n'
        text += f'root {root.as_posix()}\n' + (lines.strip() + '\n' if lines and lines.strip() else '')
        (directory / 'source').write_text(text, encoding='utf-8', newline='\n')
        for file, body in (recipes or {}).items():
            (directory / file).write_text(body, encoding='utf-8', newline='\n')
        self.sources.append(name)
        self.guard(name)
        return name, root

    def guard(self, name):
        """A source of this check names only witnesses of this check, in its source and in each of its recipes."""
        if name not in self.sources:
            raise ValueError(f'{name} is not a source of check {self.check}')
        for path in (self.recipes / name).glob('*'):
            if path.name != 'source' and not path.name.endswith('.recipe'):
                continue
            for line in path.read_text(encoding='utf-8').splitlines():
                words = line.split(None, 1)
                if len(words) == 2 and words[0] in ('witness', 'called') and not words[1].startswith(WITNESS_PREFIX):
                    raise ValueError(f'{path}: {line!r}: a check source names only witnesses beginning {WITNESS_PREFIX!r}')

    def scratch_path(self, value):
        try:
            Path(value).resolve().relative_to(self.scratch.resolve())
            return True
        except (ValueError, OSError):
            return False

    def run(self, *arguments, check=True, timeout=900, env=None, input=None):
        """The deployed program, refused anything that could reach beyond this check's sources. Its output is read as
        UTF-8; a byte that is not (a surrogate's generalized form) stays recoverable with surrogateescape."""
        arguments = self.refuse(arguments)
        began = time.time()
        result = subprocess.run([str(self.program), *arguments], env=dict(self.env, **(env or {})), text=True,
                                encoding='utf-8', errors='surrogateescape',
                                capture_output=True, timeout=timeout, input=input)
        # An ingest runs each source in its own process and writes what that source said (decomposition, mismatches,
        # its witness ID) to <logs>/<source>.log; the stdout here is only the run's table. Read the source logs with it.
        if arguments and arguments[0] == 'ingest':
            logs = re.search(r'logs in (\S+)', result.stdout)
            if logs and Path(logs[1]).is_dir():
                said = ''.join(path.read_text(encoding='utf-8', errors='surrogateescape')
                               for path in sorted(Path(logs[1]).glob('*.log')) if path.stat().st_mtime >= began - 1)
                result.stdout += '\n' + said
        if check and result.returncode:
            raise RuntimeError(f'laplace {" ".join(arguments)} exited {result.returncode}\n{result.stdout}{result.stderr}')
        return result

    def start(self, *arguments, log, env=None):
        """The deployed program started in the background under the same refusals as run, its output to the file log
        (a check that watches the database while it runs, or stops it part way). Returns the Popen."""
        arguments = self.refuse(arguments)
        with open(log, 'wb') as output:
            return subprocess.Popen([str(self.program), *arguments], env=dict(self.env, **(env or {})),
                                    stdout=output, stderr=subprocess.STDOUT)

    def refuse(self, arguments):
        """The arguments as text, or ValueError when they could reach beyond this check's sources."""
        arguments = [str(a) for a in arguments]
        operation = arguments[0] if arguments else ''
        if operation in REFUSED:
            raise ValueError(f'laplace {operation} is not run against the deployed database by a check')
        if '-d' in arguments:
            raise ValueError('a check runs against LAPLACE_CONNINFO, never another database')
        if operation in ('ingest', 'forget', 'witness'):
            options_with_values = {'-t', '-j', '-s', '-r', '--as', '--class', '--lineage'}
            named, values, skip = [], {}, None
            for a in arguments[1:]:
                if skip:
                    values[skip] = a
                    skip = None
                    continue
                if a in options_with_values:
                    skip = a
                    continue
                if a.startswith('-'):
                    if a in ('--highway', '--except', '--whole') :
                        raise ValueError(f'laplace {operation} {a} reaches beyond a check\'s own sources')
                    continue
                named.append(a)
            offline = {'--plan', '--catalog', '--list-sources', '--no-load', '--claims', '--claims-tsv', '--entities'}
            if operation == 'ingest' and not named and not offline & set(arguments):
                raise ValueError('ingest with no source named ingests every source')
            if operation == 'ingest' and '-s' in values and values['-s'] not in self.sources:
                raise ValueError(f'ingest -s {values["-s"]}: not a source of check {self.check}')
            if operation == 'witness' and not values.get('--as', '').startswith(WITNESS_PREFIX):
                raise ValueError(f'laplace witness --as must name a witness beginning {WITNESS_PREFIX!r}')
            for a in named:
                if a in self.sources or (operation == 'forget' and a.startswith(WITNESS_PREFIX)):
                    continue
                if self.scratch_path(a) and (operation == 'witness' or '-s' in values or offline & set(arguments)):
                    continue                     # files of a named check source, a check witness's statement, or no load
                if operation == 'forget' and '--trunk' in arguments and a in self.trunks_recorded:
                    continue
                raise ValueError(f'laplace {operation} {a}: not a source of check {self.check}')
        if operation in ('said', 'turn') and '--read' not in arguments:
            # A turn is witnessed by the user (--as, else $USER) and, said elsewhere, by its responder (--by, else
            # "Laplace"): both are named, and both are this check's witnesses, forgotten with it.
            for flag in ('--as', '--by') if operation == 'said' else ('--as',):
                value = arguments[arguments.index(flag) + 1] if flag in arguments[:-1] else ''
                if value not in self.witnesses:
                    raise ValueError(f'laplace {operation} {flag} must name a witness of check {self.check} (Fixture.witness)')
        return arguments

    def trunks(self, name):
        """The trunks recorded for one of this check's sources (TRUNKS)."""
        if name not in self.sources:
            raise ValueError(f'{name} is not a source of check {self.check}')
        witness = self.witness_of(name)
        rows = sql(TRUNKS,
                   variables={'witness': witness})
        found = [r for r in rows.splitlines() if r]
        self.trunks_recorded.update(found)
        return found

    def witness_of(self, name):
        for line in (self.recipes / name / 'source').read_text(encoding='utf-8').splitlines():
            if line.startswith('witness '):
                return line[len('witness '):].strip()
        raise ValueError(f'{name} names no witness')

    def forget(self, *selectors):
        """Take every witness of this check's sources back out, or those of the sources and witnesses named (nothing
        recorded is not a failure)."""
        # A source is forgotten by its witness's name, its record: that needs no recipes, so it holds whichever
        # recipes the program reads (a release's launcher sets its own).
        default = [self.witness_of(name) for name in self.sources] + [w for w in self.witnesses]
        for selector in selectors or list(dict.fromkeys(default)):
            result = self.run('forget', selector, check=False)
            if result.returncode and 'no registered source trunk matched' not in result.stderr:
                raise RuntimeError(f'laplace forget {selector} exited {result.returncode}\n{result.stdout}{result.stderr}')

    def witness(self, part):
        """A witness name of this check for an operation that names its witness itself; forgotten with the check."""
        name = f'{WITNESS_PREFIX}{self.check} {part}'
        if name not in self.witnesses:
            self.witnesses.append(name)
        return name

    def witness_trunks(self, name):
        """The trunks recorded for a witness of this check named at run time (Fixture.witness): the witness trunks
        that begin with its name's record, as Fixture.trunks finds a source's."""
        if name not in self.witnesses:
            raise ValueError(f'{name} is not a witness of check {self.check}')
        rows = sql(TRUNKS,
                   variables={'witness': name})
        found = [r for r in rows.splitlines() if r]
        self.trunks_recorded.update(found)
        return found
