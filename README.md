<div align="center">
  <a href="https://puri.sm">
    <img src="https://path/to/image/raw/master/data/icons/icon.png" width="150" />
  </a>
  <br>

  <a href="https://puri.sm"><b>libcmatrix</b></a>
  <br>

  A matrix protocol library writting in GObjectified C
  <br>

  <a href="https://source.puri.sm/Librem5/libcmatrix/pipelines"><img
     src="https://source.puri.sm/Librem5/libcmatrix/badges/main/pipeline.svg" /></a>
  <a href="https://source.puri.sm/Librem5/libcmatrix/coverage"><img
     src="https://source.puri.sm/Librem5/libcmatrix/badges/main/coverage.svg" /></a>
</div>

---

libcmatrix is a [Matrix][matrix] client library written in
GObjectified C library.

You could use the library if you are writing a matrix client
in C.

libcmatrix requires GObject and Gio, and the library assumes
that a glib main event loop is running (All GTK apps have one)

libcmatrix handles all E2EE transparently.  The messages/events
are not stored and the client should store them for chat history,
which may change in the future.

## Dependencies
   - glib >= 2.66
   - gio >= 2.66
   - libsoup-3
   - ligbcrypt
   - libolm3
   - libsqlite3 >= 3.34

Source Repository: [GitLab][gitlab]

Issues and Feature Requests: [GitLab][issues]

## Getting started

   See `examples` directory for examples on how to use the library.
   You shall have to use libcmatrix as a subproject.  Currently
   libcmatrix provides no API nor ABI guarantee until it's stable
   enough.


<!-- Links referenced elsewhere -->
<!-- To be updated -->
[matrix]: https://matrix.org
[home]: https://puri.sm
[coverage]: https://source.puri.sm/Librem5/libcmatrix/coverage
[gitlab]: https://source.puri.sm/Librem5/libcmatrix/
[issues]: https://source.puri.sm/Librem5/libcmatrix/issues
