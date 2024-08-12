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
  'cm-pusher.c',
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

cmatrix_enums = gnome.mkenums_simple(
  'cm-enum-types',
  sources : 'cm-enums.h',
  install_dir: includedir,
  install_header : install_lib,
)

libcmatrix_headers = [
  'cmatrix.h',
  'cm-client.h',
  'cm-common.h',
  'cm-enums.h',
  'cm-matrix.h',
  'cm-pusher.h',
  'cm-room.h',
  'cm-types.h',
  'cm-utils.h',
  'events/cm-event.h',
  'events/cm-room-event.h',
  'events/cm-room-message-event.h',
  'events/cm-verification-event.h',
  'users/cm-user.h',
  'users/cm-account.h',
]

cmatrix_sources = [libcmatrix_headers, libcmatrix_sources, cmatrix_enums]

cmatrix_lib = both_libraries('cmatrix',
                             cmatrix_sources,
                             include_directories: [ root_inc, src_inc ],
                             soversion: '0u',
                             dependencies: cmatrix_deps,
                             install: install_lib,
)

libcmatrix_dep = declare_dependency(
  sources: [
    # Ensure the resources are available to indirect users too
    cmatrix_enums[1],
  ],
  include_directories: include_directories('.'),
  dependencies: cmatrix_deps,
  link_with: cmatrix_lib.get_static_lib(),
)

if install_lib
  install_headers (
    libcmatrix_headers,
    preserve_path: true,
    subdir: 'libcmatrix',
  )

  pkgconfig.generate(
    cmatrix_lib,
    subdirs: 'libcmatrix',
    name: 'libcmatrix',
    version : meson.project_version(),
    description: 'Matrix client library',
    url: 'https://source.puri.sm/Librem5/libcmatrix',
    requires: gio_dep,
  )
endif

if get_option('gtk_doc') or get_option('introspection')
  libcmatrix_gir_extra_args = [
    '-L@0@'.format(meson.current_build_dir()),
    '--quiet',
   ]

  libcmatrix_gir = gnome.generate_gir(cmatrix_lib,
                 sources: cmatrix_sources,
               nsversion: '0',
               namespace: 'CM',
         export_packages: 'cmatrix-0',
           symbol_prefix: 'cm',
       identifier_prefix: 'Cm',
               link_with: cmatrix_lib,
                includes: ['Gio-2.0', 'Soup-3.0', 'Secret-1'],
                 install: install_lib and get_option('introspection'),
              extra_args: libcmatrix_gir_extra_args,
          fatal_warnings: true,
   )

endif
