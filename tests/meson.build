tests_inc = [
  root_inc,
  src_inc,
]

env = environment()
env.set('G_TEST_SRCDIR', meson.current_source_dir())
env.set('G_TEST_BUILDDIR', meson.current_build_dir())
env.set('MALLOC_CHECK_', '2')

test_items = []

test_items = [
  'client',
  'enc-chat',
  'cm-db',
  'cm-enc',
  'room',
  'room-member',
  'cm-utils',
]

foreach item: test_items
  t = executable(
    item,
    item + '.c',
    include_directories: tests_inc,
    link_with: cmatrix_lib,
    dependencies: cmatrix_deps,
  )
  test(item, t, env: env, timeout: 120)
endforeach
