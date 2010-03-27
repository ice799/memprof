begin
  require File.expand_path('../../memprof', __FILE__)
rescue LoadError
  require File.expand_path('../../../ext/memprof', __FILE__)
end

Memprof.start
old_handler = trap('URG'){
  pid = Process.pid
  fork{
    GC.start
    Memprof.dump_all("/tmp/memprof-#{pid}-#{Time.now.to_i}.json")
    exit!
  }
  old_handler.call if old_handler
}
