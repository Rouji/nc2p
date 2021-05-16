project('nc2p', 'c',
  version : '0.1',
  default_options : ['warning_level=3'])

executable(
    'nc2p',
    'nc2p.c',
    dependencies: [
        dependency('libcurl'),
        dependency('threads'),
    ],
    install : true
)
