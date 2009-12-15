if RUBY_VERSION >= "1.9"
  STDERR.puts "\n\n"
  STDERR.puts "***************************************************************************************"
  STDERR.puts "************************** ruby 1.9 is not supported (yet) =( *************************"
  STDERR.puts "***************************************************************************************"
  exit(1)
end

require 'mkmf'
require 'fileutils'

CWD = File.expand_path(File.dirname(__FILE__))

def sys(cmd)
  puts "  -- #{cmd}"
  unless ret = xsystem(cmd)
    raise "#{cmd} failed, please report to http://github.com/ice799/memprof/issues with pastie.org link to #{CWD}/mkmf.log"
  end
  ret
end

def add_define(name)
  $defs.push("-D#{name}")
end

###
# libelf

if RUBY_PLATFORM =~ /linux/
  libelf = File.basename('libelf-0.8.13.tar.gz')
  dir = File.basename(libelf, '.tar.gz')

  unless File.exists?("#{CWD}/dst/lib/libelf_ext.a")
    puts "(I'm about to compile libelf.. this will definitely take a while)"

    Dir.chdir('src') do
      FileUtils.rm_rf(dir) if File.exists?(dir)

      sys("tar zxvf #{libelf}")
      Dir.chdir(dir) do
        ENV['CFLAGS'] = '-fPIC'
        sys("./configure --prefix=#{CWD}/dst --disable-nls --disable-shared")
        sys("make")
        sys("make install")
      end
    end

    Dir.chdir('dst/lib') do
      FileUtils.ln_s 'libelf.a', 'libelf_ext.a'
    end
  end

  $LIBPATH.unshift "#{CWD}/dst/lib"
  $INCFLAGS[0,0] = "-I#{CWD}/dst/include "

  unless have_library('elf_ext', 'gelf_getshdr')
    raise 'libelf build failed'
  end

  is_elf = true
  add_define 'HAVE_ELF'
end

if have_header('mach-o/dyld')
  is_macho = true
  add_define 'HAVE_MACH'
end

if is_elf or is_macho
  create_makefile('memprof')
else
  raise 'unsupported platform'
end
