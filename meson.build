project('libcmatrix', 'c',
          version: '0.0.0',
          license: 'LGPL-2.1-or-later',
    meson_version: '>= 0.56.0',
  default_options: [ 'warning_level=2', 'buildtype=debugoptimized', 'c_std=gnu18' ],
)

gnome = import('gnome')
i18n  = import('i18n')

VERSION = meson.project_version()
ABI_MAJOR = VERSION.split('.')[0]
ABI_MINOR = VERSION.split('.')[1]
ABI_MICRO = VERSION.split('.')[2]

prefix = get_option('prefix')
bindir = join_paths(prefix, get_option('bindir'))
datadir = join_paths(prefix, get_option('datadir'))
localedir = join_paths(prefix, get_option('localedir'))
libdir = join_paths(prefix, get_option('libdir'))

cc = meson.get_compiler('c')

global_c_args = [
  '-DCMATRIX_COMPILATION',
]

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
  '-DHAVE_CONFIG_H',
  '-DCMATRIX_COMPILATION',
  '-DCMATRIX_USE_EXPERIMENTAL_API',
], language: 'c')

config_h = configuration_data()
config_h.set10('HAVE_EXPLICIT_BZERO', cc.has_function('explicit_bzero'))
config_h.set_quoted('GETTEXT_PACKAGE', 'libcmatrix')
config_h.set_quoted('LOCALEDIR', localedir)

libolm_dep = cc.find_library('olm', required: true)

if (cc.has_function('olm_account_unpublished_fallback_key', dependencies: libolm_dep))
  config_h.set('OLM_ACCOUNT_PICKLE_V4', true)
else
  config_h.set('OLM_ACCOUNT_PICKLE_V4', false)
endif

if (cc.has_function('olm_pk_key_from_private', dependencies: libolm_dep))
  config_h.set('HAVE_OLM3', true)
else
  config_h.set('HAVE_OLM2', true)
endif

configure_file(
  output: 'config.h',
  configuration: config_h,
)

root_inc = include_directories('.')
src_inc = include_directories('src')

gio_dep = dependency('gio-2.0', version: '>= 2.66')

# libsoup-3 requires glib 2.69.1+, so if we have an older
# glib, assume libsoup-2.4
if gio_dep.version().version_compare('<= 2.69') or get_option('soup2')
  soup_dep = dependency('libsoup-2.4', required: true)
else
  soup_dep = dependency('libsoup-3.0', required: true)
endif

cmatrix_deps = [
  dependency('libgcrypt'),
  dependency('libsecret-1'),
  dependency('json-glib-1.0'),
  dependency('sqlite3', version: '>=3.26.0'),
  libolm_dep,
  soup_dep,
  gio_dep,
  cc.find_library('m', required: false),
]

subdir('src')
subdir('tests')
subdir('doc')

if (get_option('build-examples'))
  subdir('examples')
endif
