Building
========
For build instructions see the [README.md](./README.md)

Development Documentation
=========================
For API documentation see [here](https://agx.github.io/libcmatrix/).

Merge requests
==============
Before filing a pull request run the tests:

```sh
meson test -C _build --print-errorlogs
```

Use descriptive commit messages, see

   https://wiki.gnome.org/Git/CommitMessages

and check

   https://wiki.openstack.org/wiki/GitCommitMessages

for good examples. The commits in a merge request should have "recipe"
style history rather than being a work log. See
[here](https://www.bitsnbites.eu/git-history-work-log-vs-recipe/) for
an explanation of the difference. The advantage is that the code stays
bisectable and individual bits can be cherry-picked or reverted.

Checklist
---------
When submitting a merge request consider checking these first:

- [ ] Is the commit history in recipe style (see above)?
- [ ] Do the commit messages reference the bugs they fix?
- [ ] Does the code crash or introduce new `CRITICAL` or `WARNING`
      messages in the log or when run form the console. If so, fix
      these first!
- [ ] Does new API have documentation and `Since:` annotations?
- [ ] Does the code break any existing API? If so, this should be documented.

If any of the above criteria aren't met yet it's still fine (and encouraged) to
open a merge request marked as draft. Please indicate why you consider it draft
in this case. As libcmatrix is used with different clients please indicate in
what scenarios you tested your code.

