libcmatrix
==========

A [Matrix][matrix] protocol library written in C using GObject.

<div>
  <a href="https://source.puri.sm/Librem5/libcmatrix/pipelines"><img
     src="https://source.puri.sm/Librem5/libcmatrix/badges/main/pipeline.svg" /></a>
  <a href="https://source.puri.sm/Librem5/libcmatrix/coverage"><img
     src="https://source.puri.sm/Librem5/libcmatrix/badges/main/coverage.svg" /></a>
</div>

---

You can use the library if you are writing a matrix client in C.

libcmatrix requires GObject and Gio, and the library assumes that a glib main
event loop is running (All GTK apps have one)

libcmatrix handles all E2EE transparently. Handled messages/events are stored
in a local database.

## Dependencies
   - glib >= 2.70
   - gio >= 2.70
   - libsoup-3
   - ligbcrypt
   - libolm3
   - libsqlite3 >= 3.34

Source Repository: [GitLab][gitlab]

Issues and Feature Requests: [GitLab][issues]

## Getting started

   See `examples` directory for examples on how to use the library.
   You typically use it as a [meson subproject](./doc/build-howto.md).

   Currently libcmatrix provides no API nor ABI guarantee until it's
   stable enough.

## Documentation

The API documentation can be browsed [here][]

## Known users

These applications use libcmatrix:

- [Chatty][chatty]: An instant messaging app
- [Eigenvalue][eigenvalue]: A client to test libcmatrix


<!-- Links referenced elsewhere -->
[matrix]: https://matrix.org
[coverage]: https://source.puri.sm/Librem5/libcmatrix/coverage
[gitlab]: https://source.puri.sm/Librem5/libcmatrix/
[issues]: https://source.puri.sm/Librem5/libcmatrix/issues
[chatty]: https://gitlab.gnome.org/World/Chatty
[eigenvalue]: https://github.com/agx/eigenvalue
[here]: https://agx.github.io/libcmatrix/
