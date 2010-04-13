spec = Gem::Specification.new do |s|
  s.name = 'memprof'
  s.version = '0.3.2'
  s.date = '2010-04-13'
  s.summary = 'Ruby Memory Profiler'
  s.description = "Ruby memory profiler similar to bleak_house, but without patches to the Ruby VM"
  s.homepage = "http://github.com/ice799/memprof"
  s.has_rdoc = false
  s.authors = ["Joe Damato", "Aman Gupta", "Jake Douglas", "Rob Benson"]
  s.email = ["joe@memprof.com", "aman@memprof.com", "jake@memprof.com"]
  s.extensions = "ext/extconf.rb"
  s.bindir = 'bin'
  s.executables << 'memprof'
  s.files = `git ls-files`.split
  s.add_dependency('rest-client', '>= 1.4.2')
  s.add_dependency('term-ansicolor')
end
