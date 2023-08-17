#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys


def collect_wasm_env(local_env=os.environ, prefix='WASM_RUN_CHILD_'):
    return dict((key[len(prefix):], value)
                for key, value in local_env.items() if key.startswith(prefix))


class WasmtimeRunner(object):
    def __init__(self):
        pass

    def run(self, args):
        command = self.invocation(args)
        if args.verbose:
            print(' '.join(command), file=sys.stderr)

        if not args.dry_run:
            subprocess.check_call(command)

    def invocation(self, args):
        command = ["wasmtime", "run"]
        envs = collect_wasm_env()
        for key in envs:
            command.append("--env")
            command.append(f"{key}={envs[key]}")
        command.append("--")
        command.extend(args.command)
        return command


def main():
    parser = argparse.ArgumentParser()

    parser.add_argument('-v', '--verbose', action='store_true', dest='verbose',
                        help='print commands as they are run')
    parser.add_argument('-n', '--dry-run', action='store_true', dest='dry_run',
                        help="print the commands that would have been run, but"
                             " don't actually run them")
    parser.add_argument('command', nargs=argparse.REMAINDER,
                        help='the command to run', metavar='command...')

    args = parser.parse_args()
    runner = WasmtimeRunner()
    runner.run(args)


if __name__ == "__main__":
    main()
