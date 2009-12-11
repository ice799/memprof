spec = Gem::Specification.new do |s|
  s.name = 'memprof'
  s.version = '0.1.1'
  s.date = '2009-12-10'
  s.summary = 'Ruby Memory Profiler'
  s.description = "Ruby memory profiler similar to bleak_house, but without patches to the Ruby VM"
  s.email = "ice799@gmail.com"
  s.homepage = "http://github.com/ice799/memprof"
  s.has_rdoc = false
  s.authors = ["Joe Damato"]
  s.extensions = "ext/extconf.rb"
  s.files = %w[
    .gitignore
    README
    ext/bin_api.h
    ext/elf.c
    ext/extconf.rb
    ext/mach.c
    ext/memprof.c
    memprof.gemspec
  ]
end
