import argparse
import sys

from .decoder import JournalReport, CalderError, decode, to_json_text


def print_journal_report(report):
    if report.base_problem:
        print(f"journal: {report.base_problem}", file=sys.stderr)
    for offset, problem in report.skipped_frames:
        print(f"journal: skipped frame at offset {offset}: {problem}", file=sys.stderr)
    if report.truncated_at is not None:
        print(f"journal: truncated, torn frame at offset {report.truncated_at}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(prog="calder", description="Decode an CLD1 document or CLDJ journal to JSON.")
    parser.add_argument("file")
    parser.add_argument("--indent", type=int, default=None)
    arguments = parser.parse_args()
    with open(arguments.file, "rb") as handle:
        data = handle.read()
    try:
        result = decode(data)
    except CalderError as error:
        print(f"calder: {arguments.file}: {error}", file=sys.stderr)
        return 1
    if isinstance(result, JournalReport):
        print_journal_report(result)
        result = result.tree
    print(to_json_text(result, arguments.indent))
    return 0


sys.exit(main())
