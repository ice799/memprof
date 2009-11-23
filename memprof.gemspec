spec = Gem::Specification.new do |s|
  s.name = 'memprof'
  s.version = '0.0.1'
  s.date = '2009-11-21'
  s.summary = 'Ruby memory profiler gem'
  s.email = "ice799@gmail.com"
  s.homepage = "http://github.com/ice799/memprof"
  s.description = "Ruby memory profiler gem"
  s.has_rdoc = false
  s.authors = ["Joe Damato"]
  s.extensions = "ext/extconf.rb"
  s.require_paths << "ext"
  s.files = ["README",
             "memprof.gemspec",
             "ext/memprof.c",
             "ext/extconf.rb"]
end
