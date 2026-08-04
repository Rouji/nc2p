project('nc2p', 'c',
  version : '0.1',
  default_options : ['warning_level=3', 'c_std=c11'])

add_project_arguments(
  '-D_FORTIFY_SOURCE=2', '-fstack-protector-strong',
  '-fPIE', '-pie',
  '-Wformat', '-Wformat-security',
  language : 'c')

executable(
    'nc2p',
    'nc2p.c',
    dependencies: [
        dependency('libcurl'),
        dependency('threads'),
    ],
    install : true
)
