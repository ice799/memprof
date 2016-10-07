# NOTE

This project is no longer maintained. It does not work for any Ruby version
above Ruby 1.8.7.

# memprof

(c) Joe Damato
@joedamato
http://timetobleed.com

Memprof is a Ruby level memory profiler that can help you find reference
leaks in your application.

Memprof can also do very lightweight function call tracing to figure out
which system and library calls are happening in your code.

Memprof currently only works on Ruby 1.8 in a 64 bits system. See the
compatibility section below.

# Installation

    gem install memprof

# API

## Memprof.stats

    Memprof.start
    12.times{ "abc" }
    Memprof.stats
    Memprof.stop

Start tracking file/line information for objects created after calling
`Memprof.start`, and print out a summary of file:line/class pairs
created.

    12 file.rb:2:String

*Note*: Call `Memprof.stats` again after `GC.start` to see which objects
are cleaned up by the garbage collector:

    Memprof.start
    10.times{ $last_str = "abc" }

    puts '=== Before GC'
    Memprof.stats

    puts '=== After GC'
    GC.start
    Memprof.stats

    Memprof.stop

After `GC.start`, only the very last instance of `"abc"` will still
exist:

    === Before GC
         10 file.rb:2:String
    === After GC
          1 file.rb:2:String

*Note*: Use `Memprof.stats("/path/to/file")` to write results to a file.

*Note*: Use `Memprof.stats!` to clear out tracking data after printing
out results.

## Memprof.track

Simple wrapper for `Memprof.stats` that will start/stop memprof around a
given block of ruby code.

    Memprof.track{
      100.times{ "abc" }
      100.times{ 1.23 + 1 }
      100.times{ Module.new }
    }

For the block of ruby code, print out file:line/class pairs for
ruby objects created.

    100  file.rb:2:String
    100  file.rb:3:Float
    100  file.rb:4:Module

*Note*: You can call GC.start at the end of the block to print out only
objects that are 'leaking' (i.e. objects that still have inbound
references).

*Note*: Use `Memprof.track("/path/to/file")` to write the results to a
file instead of stdout.

## Memprof.dump

    Memprof.dump{
      "hello" + "world"
    }

Dump out all objects created in a given ruby block as detailed json
objects.

    {
      "_id": "0x15e5018",

      "file": "file.rb",
      "line": 2,

      "type": "string",
      "class_name": "String",

      "length": 10,
      "data": "helloworld"
    }

*Note*: Use `Memprof.dump("/path/to/filename")` to write the json output
to a file, one per line.

## Memprof.dump_all

    Memprof.dump_all("myapp_heap.json")

Dump out all live objects inside the Ruby VM to `myapp_heap.json`, one
per line.

### [memprof.com](http://memprof.com) heap visualizer

    # load memprof before requiring rubygems, so objects created by
    # rubygems itself are tracked by memprof too
    require `gem which memprof/signal`.strip

    require 'rubygems'
    require 'myapp'

Installs a `URG` signal handler and starts tracking file/line
information for newly created ruby objects. When the process receives
`SIGURG`, it will fork and call `Memprof.dump_all` to write out the
entire heap to a json file.

Use the `memprof` command to send the signal and upload the heap to
[memprof.com](http://memprof.com):

    memprof --pid <PID> --name my_leaky_app --key <API_KEY>

## Memprof.trace

    require 'open-uri'
    require 'mysql'
    require 'memcached'

    Memprof.trace{
      10.times{ Module.new }
      10.times{ GC.start }
      10.times{ open('http://google.com/') }
      10.times{ Mysql.connect.query("select 1+2") }
      10.times{ Memcached.new.get('memprof') }
    }

For a given block of ruby code, count:

 - number of objects created per type
 - number of calls to and time spent in GC
 - number of calls to and time spent in connect/read/write/select
 - number of calls to and time spent in mysql queries
 - number of calls to and responses to memcached commands
 - number of calls to and bytes through malloc/realloc/free

The resulting json report looks like:

    {
      "objects": {
        "created": 10,
        "types": {
          "module": 10,  # Module.new
        }
      },

      "gc": {
        "calls": 10,     # GC.start
        "time": 0.17198
      },

      "fd": {
        "connect": {
          "calls": 10,   # open('http://google.com')
          "time": 0.0110
        }
      },

      "mysql": {
        "queries": 10,   # Mysql.connect.query("select 1+2")
        "time": 0.0006
      },

      "memcache": {
        "get": {
          "calls": 10,   # Memcached.new.get('memprof')
          "responses": {
            "notfound": 10
          }
        }
      }
    }

*Note*: To write json to a file instead, set `Memprof.trace_filename =
"/path/to/file.json"`

## Memprof.trace_request

    Memprof.trace_request(env){ @app.call(env) }

Like `Memprof.trace`, but assume an incoming Rack request and include
information about the request itself.

    {
      "start" : 1272424769750716,
      "tracers" : {
        /* ... */
      },
      "rails" : {
        "controller" : "home",
        "action" : "index"
      },
      "request" : {
        "REQUEST_URI" : "/home",
        "REQUEST_METHOD" : "GET",
        "REMOTE_ADDR" : "127.0.0.1",
        "QUERY_STRING" : null
      },
      "time" : 1.3442
    }

# Middlewares

## Memprof::Middleware

    require 'memprof/middleware'
    config.middlewares.use(Memprof::Middleware)

Wrap each request in a `Memprof.track` to print out all object
location/type pairs created during that request.

*Note*: It is preferable to run this in staging or production mode with
Rails applications, since development mode creates a lot of unnecessary
objects during each request.

*Note*: To force a GC run before printing out a report, pass in
`:force_gc => true` to the middleware.

## Memprof::Tracer

    require 'memprof/tracer'
    config.middleware.insert(0, Memprof::Tracer)

Wrap each request in a `Memprof.trace_request` and write results to
`/tmp/memprof_tracer-PID.json`

## Memprof::Filter

Similar to `Memprof::Tracer`, but for legacy Rails 2.2 applications.

    class ApplicationController < ActionController::Base
      require 'memprof/tracer'
      around_filter(Memprof::Filter)
    end

# Compatibility

Memprof supports all 1.8.x (MRI and REE) VMs, as long as they are 64-bit
and contain debugging symbols. For best results, use RVM to compile ruby
and make sure you are on a 64-bit machine.

The following ruby builds are not supported:

 - Ruby on small/medium EC2 instances (32-bit machines)
 - OSX's default system ruby (no debugging symbols, fat 32/64-bit
   binary)

*Note*: Many linux distributions do not package debugging symbols by
default. You can usually install these separately, for example using
`apt-get install libruby1.8-dbg`

## Coming soon

 - support for Ruby 1.9
 - support for i386/i686 ruby builds

# Credits

 - Jake Douglas for the Mach-O Snow Leopard support
 - Aman Gupta for various bug fixes and other cleanup
 - Rob Benson for initial 1.9 support and cleanup
 - Paul Barry for force_gc support in `Memprof::Middleware`

