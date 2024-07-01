# ===--- test_stash.py -----------------------------------------------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2024 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https:#swift.org/LICENSE.txt for license information
# See https:#swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ===----------------------------------------------------------------------===#

from . import scheme_mock
import os

class StashTestCase(scheme_mock.SchemeMockTestCase):
    def __init__(self, *args, **kwargs):
        super(StashTestCase, self).__init__(*args, **kwargs)

    def test_stash_untracked_files(self):
        self.call([self.update_checkout_path,
                   '--config', self.config_path,
                   '--source-root', self.source_root,
                   '--clone'])

        for repo in self.get_all_repos():
            repo_path = os.path.join(self.source_root, repo)
            with open(os.path.join(repo_path, 'untracked_file.txt'), 'w') as f:
                f.write('This is an untracked file.')

        self.call([self.update_checkout_path,
                   '--config', self.config_path,
                   '--source-root', self.source_root,
                   '--stash'])

        for repo in self.get_all_repos():
            repo_path = os.path.join(self.source_root, repo)
            untracked_file_path = os.path.join(repo_path, 'untracked_file.txt')
            self.assertFalse(
                os.path.exists(untracked_file_path),
                f"{untracked_file_path} should be stashed and not present in the working directory."
            )
