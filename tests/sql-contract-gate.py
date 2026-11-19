#!/usr/bin/env python3
"""Fail if Laplace-Engine or Laplace-MCP introduces inline SQL or direct libpq calls outside the contract owner."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MCP_NATIVE = ROOT.parent / "Laplace-MCP" / "native"

ALLOWED_PQ = {"pg_contract.c", "pg_plan.c", "pg.c"}
ALLOWED_SQL = {"sql_catalog.def", "pg_contract.c", "pg_plan.c"}

PQ_CALL_RE = re.compile(r"\b(PQexec|PQexecParams|PQprepare|PQexecPrepared)\s*\(")
LEGACY_CALL_RE = re.compile(
    r"\b(ask|ask_once|ask_try|db_ask|db_exec|db_exec_params|db_sql|ids_param|over_ids|db_binding|db_command|db_command_binary|arg_int|arg_f64|api_q1|api_q2)\s*\("
)
SQL_PATTERNS = (
    re.compile(r"@extschema@|@postgis@"),
    re.compile(r"\bSELECT\b.+\bFROM\b", re.IGNORECASE | re.DOTALL),
    re.compile(r"\bINSERT\s+INTO\b", re.IGNORECASE),
    re.compile(r"\bUPDATE\s+[A-Za-z_@\"][^\n;]*\bSET\b", re.IGNORECASE),
    re.compile(r"\bDELETE\s+FROM\b", re.IGNORECASE),
    re.compile(r"\bCREATE\s+(?:OR\s+REPLACE\s+)?(?:TABLE|TEMP|TEMPORARY|INDEX|SCHEMA|EXTENSION|FUNCTION|VIEW)\b", re.IGNORECASE),
    re.compile(r"\bDROP\s+(?:TABLE|INDEX|SCHEMA|EXTENSION|FUNCTION|VIEW)\b", re.IGNORECASE),
    re.compile(r"\bALTER\s+(?:DATABASE|TABLE|SYSTEM)\b", re.IGNORECASE),
    re.compile(r"\bCOPY\s+.+\bFROM\s+STDIN\b", re.IGNORECASE | re.DOTALL),
    re.compile(r"\bBEGIN\s+ISOLATION\b", re.IGNORECASE),
)


def tokenize_c(text: str):
    """Yield (code_without_comments_or_strings, list_of_(line, joined_string_literal))."""
    code_out = []
    strings = []
    i = 0
    n = len(text)
    line = 1
    cur_str = None
    cur_line = 1
    in_preproc = False

    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if cur_str is not None:
            if ch == "\\" and i + 1 < n:
                if nxt == "\n":
                    line += 1
                    i += 2
                    continue
                cur_str.append(nxt)
                i += 2
                continue
            if ch == '"':
                # Look ahead across whitespace/comments for adjacent concatenated C strings.
                j = i + 1
                tmp_line = line
                while j < n:
                    if text[j] in " \t\r\n":
                        if text[j] == "\n":
                            tmp_line += 1
                        j += 1
                    elif text[j : j + 2] == "//":
                        j = text.find("\n", j)
                        if j < 0:
                            j = n
                    elif text[j : j + 2] == "/*":
                        end = text.find("*/", j + 2)
                        if end < 0:
                            j = n
                        else:
                            tmp_line += text[j : end + 2].count("\n")
                            j = end + 2
                    else:
                        break
                if j < n and text[j] == '"':
                    line = tmp_line
                    i = j + 1
                    continue
                strings.append((cur_line, "".join(cur_str)))
                cur_str = None
                code_out.append('""')
                i += 1
                continue
            if ch == "\n":
                strings.append((cur_line, "".join(cur_str)))
                cur_str = None
                line += 1
                code_out.append("\n")
                i += 1
                continue
            cur_str.append(ch)
            i += 1
            continue

        if ch == "/" and nxt == "/":
            end = text.find("\n", i + 2)
            if end < 0:
                break
            code_out.append("\n")
            line += 1
            i = end + 1
            continue

        if ch == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            if end < 0:
                break
            newlines = text[i : end + 2].count("\n")
            code_out.extend("\n" * newlines)
            line += newlines
            i = end + 2
            continue

        if ch == '"':
            cur_str = []
            cur_line = line
            in_preproc = False
            i += 1
            continue

        if ch == "'":
            # Skip char literal
            i += 1
            if i < n and text[i] == "\\":
                i += 2
            elif i < n:
                i += 1
            if i < n and text[i] == "'":
                i += 1
            code_out.append("''")
            continue

        if ch == "\n":
            line += 1
            in_preproc = False
        code_out.append(ch)
        i += 1

    return "".join(code_out), strings


def check_file(path: Path, violations: list[str]) -> None:
    text = path.read_text(encoding="utf-8", errors="replace")
    code, strings = tokenize_c(text)
    name = path.name

    for idx, raw_line in enumerate(code.splitlines(), 1):
        m = PQ_CALL_RE.search(raw_line)
        if m and name not in ALLOWED_PQ:
            violations.append(f"{path}:{idx}: direct libpq call {m.group(1)}() outside contract owner")
        m = LEGACY_CALL_RE.search(raw_line)
        if m:
            violations.append(f"{path}:{idx}: forbidden legacy SQL wrapper {m.group(1)}()")

    if name not in ALLOWED_SQL:
        for line_no, literal in strings:
            for pat in SQL_PATTERNS:
                if pat.search(literal):
                    preview = literal.replace("\n", " ")[:100]
                    violations.append(f"{path}:{line_no}: inline SQL outside sql_catalog.def: {preview!r}")
                    break


def main() -> int:
    roots = [ROOT / "src", ROOT / "tests"]
    if not all(r.is_dir() for r in roots):
        print("sql-contract-gate: Engine src/tests missing", file=sys.stderr)
        return 2
    if MCP_NATIVE.is_dir():
        roots.append(MCP_NATIVE)

    violations: list[str] = []
    for base in roots:
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in {".c", ".h", ".def"}:
                check_file(path, violations)

    if violations:
        for v in violations:
            print(v, file=sys.stderr)
        print(f"sql-contract-gate: {len(violations)} violation(s)", file=sys.stderr)
        return 1

    scope = "Laplace-Engine + Laplace-MCP" if MCP_NATIVE.is_dir() else "Laplace-Engine"
    print(f"sql-contract-gate: OK ({scope}; zero inline SQL, zero legacy wrappers, zero bypasses)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
