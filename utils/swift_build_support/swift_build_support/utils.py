# ===-- utils.py ---------------------------------------------------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https:#swift.org/LICENSE.txt for license information
# See https:#swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ===---------------------------------------------------------------------===#

import contextlib
import json
import os
import sys
import time


from build_swift.build_swift.constants import SWIFT_BUILD_ROOT


def fatal_error(message, stream=sys.stderr):
    """Writes a message to the given stream and exits. By default this
    function outputs to stderr.
    """

    stream.write('[{}] ERROR: {}\n'.format(sys.argv[0], message))
    stream.flush()
    sys.exit(1)


def exit_rejecting_arguments(message, parser=None):
    print(message, file=sys.stderr)
    if parser:
        parser.print_usage(sys.stderr)
    sys.exit(2)  # 2 is the same as `argparse` error exit code.


def log_time_path():
    return os.path.join(SWIFT_BUILD_ROOT, '.build_script_log')


def clear_log_time():
    if os.path.exists(log_time_path()):
        f = open(log_time_path(), "w")
        f.close()


def log_time(event, command, duration=0):
    # NOTE: When running a dry run, the log file isn't created
    if not os.path.exists(log_time_path()):
        return

    f = open(log_time_path(), "a")

    log_event = {
        "event": event,
        "command": command,
        "duration": "%.2f" % float(duration),
    }

    f.write("{}\n".format(json.dumps(log_event)))
    f.close()


@contextlib.contextmanager
def log_time_in_scope(action_name):
    log_time('start', action_name)
    t_start = time.time()
    try:
        yield
    finally:
        log_time('end', action_name, time.time() - t_start)


def log_analyzer():
    """
    Analyze .build_script_log and provide a summary of the time execution.
    """
    build_script_log_path = log_time_path()
    print("--- Build Script Analyzer ---", file=sys.stderr)
    build_events = []
    total_duration = 0
    if os.path.exists(build_script_log_path):
        print("Build Script Log: {}".format(build_script_log_path), file=sys.stderr)
        with open(build_script_log_path) as f:
            for event in f:
                build_event = json.loads(event)
                build_event["duration"] = float(build_event["duration"])
                total_duration += build_event["duration"]
                build_events.append(build_event)
        finish_events = [x for x in build_events if x["event"] == "end"]
        finish_events.sort(key=lambda x: x["duration"], reverse=True)

        print("Build Percentage \t Build Duration (sec) \t Build Phase",
              file=sys.stderr)
        print("================ \t ==================== \t ===========",
              file=sys.stderr)
        event_row = '{:<17.1%} \t {:<21} \t {}'
        for build_event in finish_events:
            duration_percentage = \
                (float(build_event["duration"]) / float(total_duration))
            print(event_row.format(duration_percentage,
                                   build_event["duration"],
                                   build_event["command"]), file=sys.stderr)

        hours, remainder = divmod(total_duration, 3600)
        minutes, seconds = divmod(remainder, 60)

        if hours > 0:
            formatted_duration = " ({}h {}m {}s)".format(
                int(hours), int(minutes), int(seconds))
        elif minutes > 0:
            formatted_duration = " ({}m {}s)".format(int(minutes), int(seconds))
        else:
            formatted_duration = ""

        print("Total Duration: {:.2f} seconds".format(
            total_duration) + formatted_duration, file=sys.stderr)
    else:
        print("Skip build script analyzer", file=sys.stderr)
        print(".build_script_log file not found at {}".format(build_script_log_path),
              file=sys.stderr)
