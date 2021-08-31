# ===--- compiler_stage.py -----------------------------------------------===#
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

class StageArgs(object):
    def __init__(self, stage, args):
        self.__dict__['postfix'] = stage.postfix
        self.__dict__['stage'] = stage
        self.__dict__['args'] = args
        assert(not isinstance(self.args, StageArgs))

    def _get_stage_prefix(self):
        return self.__dict__['postfix']

    def __getattr__(self, key):
        real_key = '{}{}'.format(key, self._get_stage_prefix())
        args = self.__dict__['args']
        if not hasattr(args, real_key):
            return None
        return getattr(args, real_key)

    def __setattr__(self, key, value):
        real_key = '{}{}'.format(key, self._get_stage_prefix())
        args = self.__dict__['args']
        setattr(args, real_key, value)


class Stage(object):
    def __init__(self, identifier, postfix=""):
        self.identifier = identifier
        self.postfix = postfix


STAGE_1 = Stage(1, "")
STAGE_2 = Stage(2, "_stage2")
STAGE_2_STDLIB = Stage(2, "_stdlib_stage2")
