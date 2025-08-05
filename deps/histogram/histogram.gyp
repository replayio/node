{
  'targets': [
    {
      'target_name': 'histogram',
      'type': 'static_library',
      'cflags': ['-fvisibility=hidden', '-fPIE'],
      'xcode_settings': {
        'GCC_SYMBOLS_PRIVATE_EXTERN': 'YES',  # -fvisibility=hidden
      },
      'include_dirs': ['src'],
      'direct_dependent_settings': {
        'include_dirs': [ 'src' ]
      },
      'sources': [
        'src/hdr_histogram.c',
      ]
    }
  ]
}
