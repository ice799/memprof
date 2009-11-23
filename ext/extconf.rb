require 'mkmf'

if (have_library('elf', 'gelf_getshdr'))
  create_makefile ('memprof')
end
