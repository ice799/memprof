spec = Gem::Specification.new do |s|
  s.name = 'memprof'
  s.version = '0.2.5'
  s.date = '2010-02-27'
  s.summary = 'Ruby Memory Profiler'
  s.description = "Ruby memory profiler similar to bleak_house, but without patches to the Ruby VM"
  s.homepage = "http://github.com/ice799/memprof"
  s.has_rdoc = false
  s.authors = ["Joe Damato", "Aman Gupta", "Jake Douglas"]
  s.email = ["joe@memprof.com", "aman@memprof.com", "jake@memprof.com"]
  s.extensions = "ext/extconf.rb"
  s.files = `git ls-files`.split
end
