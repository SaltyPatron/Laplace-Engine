"""Run database acceptance with the extension built from this run's Native source."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def run(*args, env=None):
    subprocess.run([str(arg) for arg in args], env=env, check=True)


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('start', 'stop'))
    parser.add_argument('directory', type=Path)
    parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    root = args.directory.resolve()
    state_path = root / 'state.json'
    if args.operation == 'stop':
        if state_path.exists():
            state = json.loads(state_path.read_text())
            data = Path(state['data']).resolve()
            if not data.is_relative_to(root):
                raise ValueError('Cluster state escapes its declared directory')
            # Only the postmaster this job owns; startup failure needs no stop.
            if (data / 'postmaster.pid').exists():
                run(Path(state['pgbin']) / 'pg_ctl', '-D', data, '-m', 'fast', '-w', 'stop')
        return
    if not args.source or os.name != 'posix':
        parser.error('start requires the extension source on a Linux CI host')
    root.mkdir(parents=True, exist_ok=False)
    pg_config = Path(os.environ['PG_CONFIG']).resolve()
    pgbin = pg_config.parent
    build, install = root / 'build', root / 'install'
    native = Path(os.environ['LAPLACE_NATIVE']).resolve()
    run('cmake', '-S', args.source.resolve(), '-B', build, '-G', 'Ninja',
        '-DCMAKE_C_COMPILER=icx', '-DCMAKE_CXX_COMPILER=icpx', '-DCMAKE_BUILD_TYPE=Release',
        '-DLAPLACE_NATIVE=' + str(native), '-DPG_CONFIG=' + str(pg_config))
    run('cmake', '--build', build, '--target', 'laplace_pg')
    run('cmake', '--install', build, env=dict(os.environ, DESTDIR=str(install)))
    def config(name):
        return Path(subprocess.check_output([str(pg_config), name], text=True).strip())
    share = install / str(config('--sharedir')).lstrip('/')
    library = install / str(config('--pkglibdir')).lstrip('/') / 'laplace.so'
    if not library.is_file():
        raise ValueError('Candidate extension library was not installed in the job directory')
    control = share / 'extension/laplace.control'
    text, count = re.subn(r"^module_pathname\s*=.*$", 'module_pathname = ' + quote(library),
                          control.read_text(), flags=re.MULTILINE)
    if count != 1:
        raise ValueError('Candidate extension has no unique module declaration')
    control.write_text(text)
    data, socket = root / 'data', root / 'socket'
    socket.mkdir()
    if len(str(socket / '.s.PGSQL.5432').encode()) >= 108:
        raise ValueError('The declared socket path is too long for PostgreSQL')
    run(pgbin / 'initdb', '-D', data, '--username=laplace', '--auth-local=trust', '--auth-host=reject', '--no-locale', '--encoding=UTF8')
    with (data / 'postgresql.conf').open('a') as stream:
        for name, value in {'listen_addresses': '', 'unix_socket_directories': str(socket),
                            'extension_control_path': str(share) + ':$system',
                            'shared_preload_libraries': 'pg_stat_statements'}.items():
            stream.write(name + ' = ' + quote(value) + '\n')
        stream.write('port = 5432\nmax_connections = 128\n')
    state_path.write_text(json.dumps(dict(data=str(data), pgbin=str(pgbin))) + '\n')
    run(pgbin / 'pg_ctl', '-D', data, '-l', root / 'postgres.log', '-w', 'start')
    environment = dict(os.environ, PGHOST=str(socket), PGPORT='5432', PGUSER='laplace', PGDATABASE='postgres')
    run(pgbin / 'createdb', 'laplace', env=environment)
    environment['LAPLACE_CONNINFO'] = 'host=' + quote(socket) + ' port=5432 user=laplace dbname=laplace'
    with open(os.environ['GITHUB_ENV'], 'a') as stream:
        for name in ['PGHOST', 'PGPORT', 'PGUSER', 'PGDATABASE', 'LAPLACE_CONNINFO']:
            stream.write(name + '=' + environment[name] + '\n')
    print('Database acceptance uses the candidate extension and a private PostgreSQL instance', flush=True)


if __name__ == '__main__':
    main()
