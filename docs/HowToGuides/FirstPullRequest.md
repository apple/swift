# How to Submit Your First Pull Request

So you've decided to contribute to the Swift toolchain, welcome!
Maybe this is your first time contributing to an open source project, or maybe
you are an experienced open source contributor who is excited about Swift, or
maybe you are somewhere in-between. Regardless of your background, we are
excited to have you contribute and improve the developer experience for Swift
programmers all over the globe.
✨🧒🏾🧑🏼‍🎓👩🏽‍💻🧑🏻‍💻👨🏿‍💻✨

This document provides a high-level overview of different parts of the
contribution process.

## How do I pick something to work on?

In case you don't have anything specific to work on, such as implementing a
[Swift evolution proposal](https://www.swift.org/swift-evolution), you could
start off by picking a [good first issue][good-first-issues]. Before you start
working on an issue:
* Check the comments, assignees, and any references to pull requests — make sure
  nobody else is actively working on it, or awaiting help or review.

  If someone is assigned to the issue or volunteered to work on it, and there
  are no signs of progress or activity over at least the past month, don't
  hesitate to check in with them — it might be that the person moved on.

* Leave a comment that you have started working on it.

## Getting Help

Usually, [good first issues][good-first-issues] try to provide some instructions
to help you get started. In case those are missing, please ask the bug reporter
for more detailed steps and they will be happy to help.

Once you start working on the bug, you will inevitably end up having a lot of
questions. Don't be afraid to ask for help! The codebase is large and wrapping
your head around it will take time. For example, you might have questions like:

- Where can I find documentation on X?
- I'm seeing a cryptic error E when trying to build the compiler. How do I fix
  it or work around it?
- I'm seeing very long build times even for incremental builds. How do I speed
  up iteration time?
- I'm not sure how to implement X. Any suggestions on where I should start?
- What is the difference between types T1 and T2? They look very similar.
- Should I split my new X into a separate file?
- Should I create a new test file or update an existing test?
- How should I test that I actually fixed this bug?
- Test X is failing and I can't understand why. What might be going wrong here?
- Test X is failing in CI but passing locally. Any tips for debugging?
- I made some change but that seems to be not getting picked up. What should
  I do to fix it?
- I need to update the CMake but I'm not familiar with CMake. Could you give me
  more guidance?
- How do I do X in git?

Some of these already have answers in our [FAQ](/docs/HowToGuides/FAQ.md).
Maybe you have a question that's not on this list. That's fine.
We're here to help. There are a couple of options to ask for help:

- [Development category on the Swift forums](https://forums.swift.org/c/development):
  Prefer using the forums for broad questions, such as those related to
  building the toolchain, or understanding how something works at a high-level.
  Since more people are likely to see and be able to answer your question, the
  question is likely to get an answer sooner. Another benefit of asking in
  public is that the answers you receive will be helpful to bystanders too.
- Bug report/Pull request comments: Prefer asking in the bug report/pull request
  when the question involves additional context specific to the
  bug report/pull request.

These are suggestions, not rules. For example, it's okay if you ask a broad
question in a bug report or a pull request.

When asking for help, prefer giving as much information as possible, while
highlighting the parts that you think are important.

Remember that the [Swift Code of Conduct][] applies whenever you are
participating in the Swift project.

[Swift Code of Conduct]: https://swift.org/code-of-conduct/

### I didn't get a response from someone. What should I do?

It's possible that you ask someone a question in a bug report/pull request and
you don't get a response as quickly as you'd like. Maybe they are juggling
several tasks and the discussion with you accidentally slipped by. Maybe they
are on vacation or on leave for some reason. If you don't get a response
within a week, it's okay to politely ping them using an `@` mention with a
reminder. If you don't get a response for 2-3 weeks in a row, please ping
someone else.

## Working on a change

Please see our [Getting Started guide][] to understand how to build the code,
make changes, run tests and debug issues.

[Getting Started guide]: /docs/HowToGuides/GettingStarted.md

## Submitting a pull request

Alright! You've implemented a change and would like to submit it.

### Tidying up

> **Note**  
> If you intend to create a [draft pull request][draft-pr], you can tidy up
> later *before* marking it as ready for review.

1. [Tidy up your commit history](FAQ.md#how-do-i-clean-up-my-git-history):
   * Squash work-in-progress commits.
   * Break up your changes into as many self-sufficient, meaningful commits as
     you can, rather than cramming everything into a single commit.
     For example, a patch that renames a function (1) and modifies an algorithm
     (2) can be split into two commits.
     A self-sufficient commit must compile, pass all tests, and contain
     any associated test changes.
     This practice is key to efficient and rigorous code review processes,
     as well as a neat, transparent, and actionable commit history.
   * [Top off your commit messages](https://swift.org/contributing/#commit-messages).
1. Tidy up and [format your code changes](FAQ.md#how-do-i-format-my-changes).

### Pushing and creating a pull request

Assuming you followed the steps in our [Getting Started guide][], you should now
be able to push your latest changes to GitHub using `git push`.

Next, [create a pull request][] (PR). Usually, if you navigate to
https://github.com/swiftlang/swift right after pushing your change, GitHub will
show a helpful "Compare & Pull Request" button.

![Compare & Pull Request button in GitHub UI](/docs/GitHubCreatePRScreenshot.png)

[draft-pr]: https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/proposing-changes-to-your-work-with-pull-requests/about-pull-requests#draft-pull-requests
[create a pull request]: https://docs.github.com/en/github/collaborating-with-issues-and-pull-requests/creating-a-pull-request#creating-the-pull-request

## Asking for code review

Reviews are automatically requested from code owners per the
[CODEOWNERS](/.github/CODEOWNERS) file upon opening a non-draft pull request.
If this doesn't happen, @mention and ask a suggested person under **Reviewers**
to review your changes in a comment. If no suggestions are shown either, please
ask @swiftlang/contributor-experience to sort out review requests for you.

You are welcome to invite other people for review as well — say, someone you
had an active discussion with on how to implement your change.
Anyone with a GitHub account can review or comment on changes proposed to
this repository!

Contributors without [write access][write-access] are not able to run the
continuous integration (CI) bot, which builds the project and runs tests.
Please ask a code reviewer with write access to invoke the bot for you.

## Responding to code review comments

During the process of code review, someone might suggest changes or have
questions about the implementation. If something is unclear, such as someone
using a technical term you don't recognize, check our
[Lexicon](/docs/Lexicon.md) or ask someone instead of trying to figure out
everything by yourself. Code review does not need to be a one-way
street. It is also a good opportunity for you to ask highly contextual
questions on topics that you struggled with or were unable to understand.

While making changes based on code review, if you are comfortable with
rebasing, prefer rebasing and force-pushing for small patches (say < 100 lines).
For larger patches, you can add fixup commits (`git commit --fixup ...`)
addressing the suggestions and rebase after it the patch has been approved
to clean up the history.

When you push again and want the tests to be re-run, please ask the reviewer
to invoke `swift-ci` for you.

At the end, once the tests are passing, the pull request is approved by
the reviewer, and you are satisfied with your changes, ask your reviewer
to merge your changes. :tada:

## I can't finish the contribution I started. :frowning_face:

That's totally okay! There is no shame in that. You only have limited time and
energy in a day. If you can, leave a comment on the bug report/pull request
that you will not be able to continue and unassign yourself from the issue on
GitHub. Don't worry about trying to explain _why_ you aren't
able to contribute further. We understand. Unanticipated things come up all 
the time and you should do what _works for you_.

This point also applies if you don't have time right now but hope to get to
something in the near future. Please don't feel sad or apologetic!

## I submitted and merged my first pull request. What now?

Awesome! You are welcome to tackle as many [good first issues][good-first-issues]
as it takes for you to gain a desired level of confidence in working with the
codebase. Beyond that, there is an endless supply of [other issues](https://github.com/swiftlang/swift/issues)
waiting for a hero. Don't hesitate to ask for help if you need directions or
get stuck!

Once you've made multiple substantial contributions, you can
[ask for commit access][write-access], which will allow you to pick reviewers,
trigger the CI bot and merge changes upon approval.

[good-first-issues]: https://github.com/swiftlang/swift/contribute
[write-access]: https://swift.org/contributing/#commit-access
