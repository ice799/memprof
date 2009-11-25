require 'mkmf'

def add_define(name)
  $defs.push("-D#{name}")
end

if (add_define("HAVE_ELF") if have_library('elf', 'gelf_getshdr')) ||
  (add_define("HAVE_MACH") if have_header('mach-o/dyld.h'))
  create_makefile('memprof')
end
