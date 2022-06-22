libcmatrix_sources = [
  'cm-client.c',
  'cm-common.c',
  'cm-db.c',
  'cm-device.c',
  'cm-enc.c',
  'cm-matrix.c',
  'cm-net.c',
  'cm-room.c',
  'cm-room-member.c',
  'cm-input-stream.c',
  # 'cm-room-message.c',
  'cm-utils.c',
]

libcmatrix_headers = [
  'cmatrix.h',
  'cm-client.h',
  'cm-common.h',
  'cm-device.h',
  'cm-enums.h',
  'cm-matrix.h',
  'cm-room.h',
  'cm-room-member.h',
]

cmatrix_c_args = [
  '-DG_LOG_DOMAIN="cmatrix"',
]

# Currently we do only have static library and should be
# used only as a subproject
cmatrix_lib = static_library('libcmatrix',
                             [libcmatrix_headers, libcmatrix_sources],
                             c_args: cmatrix_c_args,
                             include_directories: [ root_inc, src_inc ],
                             dependencies: cmatrix_deps
)

libcmatrix_dep = declare_dependency(
  include_directories: include_directories('.'),
  dependencies: cmatrix_deps,
  link_with: cmatrix_lib
)
