if not get_option('build-examples')
  subdir_done()
endif

example_items = [
  'simple-client',
]

foreach item: example_items
  executable(
    item,
    item + '.c',
    dependencies: libcmatrix_dep,
  )
endforeach
