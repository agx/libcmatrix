Title: Compiling with libcmatrix
Slug: building

# Compiling with libcmatrix

If you need to build libcmatrix, get the source from
[here](https://source.puri.sm/Librem5/libcmatrix/) and see the `README.md` file.

## Bundling the library

libcmatrix is not meant to be used as a shared library. It should be embedded in your source
tree as a git submodule instead:

```
git submodule add https://source.puri.sm/Librem5/libcmatrix.git subprojects/libcmatrix
```

Add this to your `meson.build`:

```meson
libcmatrix = subproject('libcmatrix',
  default_options: [
    'package_name=' + meson.project_name(),
    'package_version=' + meson.project_version(),
    'pkgdatadir=' + pkgdatadir,
    'pkglibdir=' + pkglibdir,
    'examples=false',
    'gtk_doc=false',
    'tests=false',
  ])
libcmatrix_dep = libcmatrix.get_variable('libcmatrix_dep')
```
