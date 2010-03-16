require File.expand_path('../../memprof', __FILE__)
Memprof.start
trap('USR2'){
  pid = Process.pid
  fork{
    GC.start
    Memprof.dump_all("/tmp/memprof-#{pid}-#{Time.now.to_i}.json")
    exit!
  }
}
