# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors


from contextlib import contextmanager

import argparse

from . import actions
from .actions import Action


__all__ = [
    'ArgumentParser',
]


# -----------------------------------------------------------------------------

class _ActionContainer(object):
    """Container object holding partially applied actions used as a part of the
    builder DSL.
    """

    def __init__(self):
        self.append = _PartialAction(actions.AppendAction)
        self.custom_call = _PartialAction(actions.CustomCallAction)
        self.store = _PartialAction(actions.StoreAction)
        self.store_int = _PartialAction(actions.StoreIntAction)
        self.store_true = _PartialAction(actions.StoreTrueAction)
        self.store_false = _PartialAction(actions.StoreFalseAction)
        self.store_path = _PartialAction(actions.StorePathAction)
        self.toggle_true = _PartialAction(actions.ToggleTrueAction)
        self.toggle_false = _PartialAction(actions.ToggleFalseAction)
        self.unsupported = _PartialAction(actions.UnsupportedAction)


class _CompoundAction(Action):
    """Action composed of multiple actions. Default attributes are derived
    from the first action.
    """

    def __init__(self, actions, **kwargs):
        _actions = []
        for action in actions:
            _actions.append(action(**kwargs))

        kwargs.setdefault('nargs', kwargs[0].nargs)
        kwargs.setdefault('metavar', kwargs[0].metavar)
        kwargs.setdefault('choices', kwargs[0].choices)

        super(_CompoundAction, self).__init__(**kwargs)

        self.actions = _actions

    def __call__(self, *args):
        for action in self.actions:
            action(*args)


class _PartialAction(Action):
    """Action that is partially applied, creating a factory closure used to
    defer initialization of acitons in the builder DSL.
    """

    def __init__(self, action_class):
        self.action_class = action_class

    def __call__(self, dests=None, *call_args, **call_kwargs):
        def factory(**kwargs):
            kwargs.update(call_kwargs)
            if dests is not None:
                return self.action_class(dests=dests, *call_args, **kwargs)
            return self.action_class(*call_args, **kwargs)

        return factory


# -----------------------------------------------------------------------------

class _Builder(object):
    """Builder object for constructing complex ArgumentParser instances with
    a more friendly and descriptive DSL.
    """

    def __init__(self, parser, **kwargs):
        assert isinstance(parser, ArgumentParser)

        self._parser = parser
        self._current_group = self._parser
        self._defaults = dict()

        self.actions = _ActionContainer()

    def build(self):
        self._parser.set_defaults(**self._defaults)
        return self._parser

    def _add_argument(self, option_strings, *actions, **kwargs):
        # Unwrap partial actions
        _actions = []
        for action in actions:
            if isinstance(action, _PartialAction):
                action = action()
            _actions.append(action)

        if len(_actions) == 0:
            # Default to store action
            action = actions.StoreAction
        elif len(_actions) == 1:
            action = _actions[0]
        else:
            def thunk(**kwargs):
                return _CompoundAction(_actions, **kwargs)
            action = thunk

        return self._current_group.add_argument(
            *option_strings, action=action, **kwargs)

    def add_positional(self, option_strings, *actions, **kwargs):
        if isinstance(option_strings, str):
            option_strings = [option_strings]

        if any(o.startswith('-') for o in option_strings):
            raise ValueError("add_option can't add optional arguments")

        return self._add_argument(option_strings, *actions, **kwargs)

    def add_option(self, option_strings, *actions, **kwargs):
        if isinstance(option_strings, str):
            option_strings = [option_strings]

        if not all(o.startswith('-') for o in option_strings):
            raise ValueError("add_option can't add positional arguments")

        return self._add_argument(option_strings, *actions, **kwargs)

    def set_defaults(self, dests=None, value=None, **kwargs):
        if dests is None:
            dests = []
        elif isinstance(dests, str):
            dests = [dests]

        for dest in dests:
            kwargs[dest] = value

        self._defaults.update(**kwargs)

    def in_group(self, description):
        self._current_group = self._parser.add_argument_group(description)
        return self._current_group

    def reset_group(self):
        self._current_group = self._parser

    @contextmanager
    def argument_group(self, description):
        previous_group = self._current_group
        self._current_group = self._parser.add_argument_group(description)
        yield self._current_group
        self._current_group = previous_group

    @contextmanager
    def mutually_exclusive_group(self):
        previous_group = self._current_group
        self._current_group = previous_group.add_mutually_exclusive_group()
        yield self._current_group
        self._current_group = previous_group


# -----------------------------------------------------------------------------

class ArgumentParser(argparse.ArgumentParser):
    """A thin extension class to the standard ArgumentParser which incluldes
    methods to interact with a builder instance.
    """

    @staticmethod
    def builder(**kwargs):
        return _Builder(parser=ArgumentParser(**kwargs))

    def to_builder(self):
        return _Builder(parser=self)

    def parse_known_args(self, args=None, namespace=None):
        """Thin wrapper around parse_known_args which shims-in support for
        actions with multiple destinations.
        """

        if namespace is None:
            namespace = argparse.Namespace()

        # Add action defaults not present in namespace
        for action in self._actions:
            if not hasattr(action, 'dests'):
                continue

            for dest in action.dests:
                if hasattr(namespace, dest):
                    continue
                if action.default is argparse.SUPPRESS:
                    continue

                setattr(namespace, dest, action.default)

        return super(ArgumentParser, self).parse_known_args(args, namespace)
