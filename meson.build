project('libcmatrix', 'c',
          version: '0.0.3-dev',
          license: 'LGPL-2.1-or-later',
    meson_version: '>= 0.63.0',
  default_options: [ 'warning_level=2', 'buildtype=debugoptimized', 'c_std=gnu18' ],
)

gnome     = import('gnome')
i18n      = import('i18n')
pkgconfig = import('pkgconfig')

VERSION = meson.project_version()
ABI_MAJOR = VERSION.split('.')[0]
ABI_MINOR = VERSION.split('.')[1]
ABI_MICRO = VERSION.split('.')[2]

prefix = get_option('prefix')
bindir = join_paths(prefix, get_option('bindir'))
datadir = join_paths(prefix, get_option('datadir'))
localedir = join_paths(prefix, get_option('localedir'))
libdir = join_paths(prefix, get_option('libdir'))
includedir = prefix / get_option('includedir') / 'libcmatrix'

cc = meson.get_compiler('c')

global_c_args = []

test_c_args = [
  '-Wcast-align',
  '-Wdate-time',
  '-Wdeclaration-after-statement',
  ['-Werror=format-security', '-Werror=format=2'],
  '-Wendif-labels',
  '-Werror=incompatible-pointer-types',
  '-Werror=missing-declarations',
  '-Werror=overflow',
  '-Werror=return-type',
  '-Werror=shift-count-overflow',
  '-Werror=shift-overflow=2',
  '-Werror=implicit-fallthrough=3',
  '-Wfloat-equal',
  '-Wformat-nonliteral',
  '-Wformat-security',
  '-Winit-self',
  '-Wmaybe-uninitialized',
  '-Wmissing-field-initializers',
  '-Wmissing-include-dirs',
  '-Wmissing-noreturn',
  '-Wnested-externs',
  '-Wno-strict-aliasing',
  '-Wno-unused-parameter',
  '-Wold-style-definition',
  '-Wpointer-arith',
  '-Wredundant-decls',
  '-Wshadow',
  '-Wsign-compare',
  '-Wstrict-prototypes',
  '-Wswitch-default',
  '-Wswitch-enum',
  '-Wtype-limits',
  '-Wundef',
  '-Wunused-function',
]
if get_option('buildtype') != 'plain'
  test_c_args += '-fstack-protector-strong'
endif

foreach arg: test_c_args
  if cc.has_multi_arguments(arg)
    global_c_args += arg
  endif
endforeach
add_project_arguments(
  global_c_args,
  language: 'c'
)

add_project_arguments([
  '-DCMATRIX_COMPILATION',
  '-DCMATRIX_USE_EXPERIMENTAL_API',
], language: 'c')

config_h = configuration_data()
config_h.set10('HAVE_EXPLICIT_BZERO', cc.has_function('explicit_bzero'))
config_h.set_quoted('GETTEXT_PACKAGE', 'libcmatrix')
config_h.set_quoted('LOCALEDIR', localedir)

configure_file(
  output: 'cm-config.h',
  configuration: config_h,
)

install_lib = not meson.is_subproject()

root_inc = include_directories('.')
src_inc = include_directories('src')

libolm_dep = dependency('olm', version: '>= 3.2.11')
gio_dep = dependency('gio-2.0', version: '>= 2.70')
soup_dep = dependency('libsoup-3.0', version: '>= 3.4.4')
json_glib_dep = dependency('json-glib-1.0', version: '>= 1.8.0')

cmatrix_deps = [
  dependency('libgcrypt'),
  dependency('libsecret-1'),
  dependency('sqlite3', version: '>=3.26.0'),
  json_glib_dep,
  libolm_dep,
  soup_dep,
  gio_dep,
  cc.find_library('m', required: false),
]

subdir('src')
subdir('tests')
subdir('doc')
subdir('examples')

summary({
    'Build Examples': get_option('build-examples'),
    'Build Tests': get_option('build-tests'),
    'Introspection': get_option('introspection'),
    'Documentation': get_option('gtk_doc'),
    'Install lib': install_lib,
  },
  bool_yn: true,
)
