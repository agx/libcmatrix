libcmatrix_sources = [
  'cm-client.c',
  'cm-common.c',
  'cm-db.c',
  'cm-device.c',
  'cm-enc.c',
  'cm-olm.c',
  'cm-olm-sas.c',
  'cm-matrix.c',
  'cm-net.c',
  'cm-room.c',
  'cm-secret-store.c',
  'cm-input-stream.c',
  'cm-utils.c',
  'events/cm-event.c',
  'events/cm-room-event.c',
  'events/cm-room-message-event.c',
  'events/cm-room-event-list.c',
  'events/cm-verification-event.c',
  'users/cm-user.c',
  'users/cm-account.c',
  'users/cm-room-member.c',
  'users/cm-user-list.c',
]

libcmatrix_headers = [
  'cmatrix.h',
  'cm-client.h',
  'cm-common.h',
  'cm-device.h',
  'cm-enums.h',
  'cm-matrix.h',
  'cm-room.h',
  'users/cm-user.h',
  'users/cm-account.h',
  'users/cm-room-member.h',
]

# Currently we do only have static library and should be
# used only as a subproject
cmatrix_lib = static_library('libcmatrix',
                             [libcmatrix_headers, libcmatrix_sources],
                             include_directories: [ root_inc, src_inc ],
                             dependencies: cmatrix_deps
)

libcmatrix_dep = declare_dependency(
  include_directories: include_directories('.'),
  dependencies: cmatrix_deps,
  link_with: cmatrix_lib
)

if get_option('gtk_doc')
  libcmatrix_gir_extra_args = [
    '-L@0@'.format(meson.current_build_dir()),
    '--quiet',
   ]

  libcmatrix_gir = gnome.generate_gir(cmatrix_lib,
                 sources: libcmatrix_sources + libcmatrix_headers,
               nsversion: '0',
               namespace: 'CM',
         export_packages: 'cmatrix-0',
           symbol_prefix: 'cm',
       identifier_prefix: 'Cm',
               link_with: cmatrix_lib,
                includes: ['Gio-2.0', 'Soup-3.0'],
                 install: false,
              extra_args: libcmatrix_gir_extra_args,
   )

endif
