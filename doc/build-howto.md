Title: Compiling with libcmatrix
Slug: building

# Compiling with libcmatrix

If you need to build libcmatrix, get the source from
[here](https://source.puri.sm/Librem5/libcmatrix/) and see the `README.md` file.

## Bundling the library

libcmatrix is not yet meant to be used as a shared library. It should be embedded in your source
code as a meson subproject. Add this as `subprojects/libcmatrix.wrap`:

```
[wrap-git]
directory=libcmatrix
url=https://source.puri.sm/Librem5/libcmatrix
revision=main
depth=1
```

Add this to your `meson.build`:

```meson
add_project_arguments([
  '-DCMATRIX_USE_EXPERIMENTAL_API',
], language: 'c')


libcmatrix_dep = dependency('libcmatrix',
  fallback: ['libcmatrix', 'libcmatrix_dep'],
  default_options: [
    'build-tests=false',
    'build-examples=false',
    'gtk_doc=false',
  ])
```

