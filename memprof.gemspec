spec = Gem::Specification.new do |s|
  s.name = 'memprof'
  s.version = '0.2.1'
  s.date = '2010-02-20'
  s.summary = 'Ruby Memory Profiler'
  s.description = "Ruby memory profiler similar to bleak_house, but without patches to the Ruby VM"
  s.email = "ice799@gmail.com"
  s.homepage = "http://github.com/ice799/memprof"
  s.has_rdoc = false
  s.authors = ["Joe Damato", "Aman Gupta", "Jake Douglas", "Rob Benson"]
  s.extensions = "ext/extconf.rb"
  s.files = `git ls-files`.split
end
