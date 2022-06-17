example_items = [
  'simple-client',
]

foreach item: example_items
  executable(
    item,
    item + '.c',
    include_directories: src_inc,
    link_with: cmatrix_lib,
    dependencies: cmatrix_deps,
  )
endforeach
