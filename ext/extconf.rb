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
    raise "#{cmd} failed, please report to gdb@tmm1.net with pastie.org link to #{CWD}/mkmf.log and #{CWD}/src/gdb-7.0/config.log"
  end
  ret
end

yajl = File.basename('yajl-1.0.7.tar.gz')
dir = File.basename(yajl, '.tar.gz')

unless File.exists?("#{CWD}/dst/lib/libyajl_s.a")
  puts "(I'm about to compile yajl.. this will definitely take a while)"

  Dir.chdir('src') do
    FileUtils.rm_rf(dir) if File.exists?(dir)

    sys("tar zxvf #{yajl}")
    Dir.chdir(dir) do
      sys("./configure --prefix=#{CWD}/dst/")
      sys("make")
      sys("make install")
    end
  end
end

$LIBPATH << "#{CWD}/dst/lib"
$INCFLAGS << "-I#{CWD}/dst/include"

unless have_library('yajl_s') and have_header('yajl/yajl_gen.h')
  raise 'Yajl build failed'
end

def add_define(name)
  $defs.push("-D#{name}")
end

if (add_define("HAVE_ELF") if have_library('elf', 'gelf_getshdr')) ||
  (add_define("HAVE_MACH") if have_header('mach-o/dyld.h'))
  create_makefile('memprof')
end
