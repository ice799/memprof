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
    raise "#{cmd} failed, please report to memprof@tmm1.net with pastie.org link to #{CWD}/mkmf.log"
  end
  ret
end

yajl = File.basename('yajl-1.0.8.tar.gz')
dir = File.basename(yajl, '.tar.gz')

unless File.exists?("#{CWD}/src/#{dir}/src/libyajl_ext.so")
  puts "(I'm about to compile yajl.. this will definitely take a while)"

  Dir.chdir('src') do
    FileUtils.rm_rf(dir) if File.exists?(dir)

    sys("tar zxvf #{yajl}")
    Dir.chdir("#{dir}/src") do
      FileUtils.mkdir_p "api/yajl"
      %w[ common parse gen ].each do |f|
        FileUtils.cp "api/yajl_#{f}.h", 'api/yajl/'
      end

      File.open("extconf.rb",'w') do |f|
        f.puts "require 'mkmf'; $INCFLAGS[0,0] = '-I./api/ '; create_makefile 'libyajl_ext'"
      end
      sys("#{Config::CONFIG['bindir']}/#{Config::CONFIG['ruby_install_name']} extconf.rb")

      sys("make")
    end
  end
end

$LIBPATH.unshift "#{CWD}/src/#{dir}/src/"
$INCFLAGS[0,0] = "-I#{CWD}/src/#{dir}/src/api/ "

unless have_library('yajl_ext') and have_header('yajl/yajl_gen.h')
  raise 'Yajl build failed'
end

def add_define(name)
  $defs.push("-D#{name}")
end

if (add_define("HAVE_ELF") if have_library('elf', 'gelf_getshdr')) ||
  (add_define("HAVE_MACH") if have_header('mach-o/dyld.h'))
  create_makefile('memprof')
end
