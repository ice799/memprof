spec = Gem::Specification.new do |s|
  s.name = 'memprof'
  s.version = '0.1.3'
  s.date = '2009-12-14'
  s.summary = 'Ruby Memory Profiler'
  s.description = "Ruby memory profiler similar to bleak_house, but without patches to the Ruby VM"
  s.email = "ice799@gmail.com"
  s.homepage = "http://github.com/ice799/memprof"
  s.has_rdoc = false
  s.authors = ["Joe Damato", "Aman Gupta", "Jake Douglas"]
  s.extensions = "ext/extconf.rb"
  s.files = %w[
    .gitignore
    README
    ext/arch.h
    ext/bin_api.h
    ext/elf.c
    ext/extconf.rb
    ext/i386.c
    ext/i386.h
    ext/mach.c
    ext/memprof.c
    ext/src/libdwarf-20091118.tar.gz
    ext/src/libelf-0.8.13.tar.gz
    ext/src/yajl-1.0.8.tar.gz
    ext/x86_64.c
    ext/x86_64.h
    ext/x86_gen.h
    memprof.gemspec
    spec/memprof_spec.rb
  ]
end
