require File.dirname(__FILE__) + "/../ext/memprof"

require 'rubygems'
require 'bacon'
Bacon.summary_on_exit

require 'tempfile'

# XXX must require upfront before tracers are installed
require 'socket'
require 'open-uri'
begin; require 'mysql';     rescue LoadError; end
begin; require 'memcached'; rescue LoadError; end

describe 'Memprof tracers' do
  @tempfile = Tempfile.new('tracing_spec')

  def filename
    @tempfile.path
  end

  def filedata
    File.read(filename)
  end

  should 'trace i/o for block' do
    Memprof.trace(filename) do
      open("http://google.com").read
    end

    filedata.should =~ /"read":\{"calls":\d+/
    filedata.should =~ /"write":\{"calls":\d+/
    filedata.should =~ /"connect":\{"calls":\d+/
  end

  should 'trace select for block' do
    Memprof.trace(filename) do
      select(nil, nil, nil, 0.15)
    end

    filedata.should =~ /"select":\{"calls":1,"time":0\.1[567]/
    time = filedata[/"select":\{"calls":\d+,"time":([\d.]+)/, 1].to_f
    time.should.be.close(0.15, 0.1)
  end

  should 'trace objects created for block' do
    Memprof.trace(filename) do
      10.times{1.1+1.2}
    end

    filedata.should =~ /"float":10/
  end

  should 'trace gc runs for block' do
    Memprof.trace(filename) do
      10.times{GC.start}
    end

    filedata.should =~ /"gc":\{"calls":10,"time":[\d.]+/
  end

  should 'trace memory allocation for block' do
    Memprof.trace(filename) do
      10.times{ "abc" << "def" }
    end

    filedata.should =~ /"malloc":\{"calls":10/
    filedata.should =~ /"realloc":\{"calls":10/
  end

  if defined? Mysql
    begin
      conn = Mysql.connect('localhost', 'root')

      should 'trace mysql calls for block' do
        Memprof.trace(filename) do
          5.times{ conn.query("select sleep(0.05)") }
        end

        filedata.should =~ /"mysql":\{"queries":5,"time":([\d.]+)/
        time = filedata[/"mysql":\{"queries":5,"time":([\d.]+)/, 1].to_f
        time.should.be.close(0.25, 0.1)
      end
    rescue Mysql::Error => e
      raise unless e.message =~ /connect/
    end
  end

  if defined? Memcached
    begin
      conn = Memcached.new("localhost:11211", :show_backtraces => true)
      conn.stats

      should 'trace memcached calls for block' do
        Memprof.trace(filename) do
          conn.delete("memprof") rescue nil
          conn.get("memprof") rescue nil
          conn.set("memprof", "is cool")
          conn.get("memprof")
        end

        filedata.should =~ /"memcache":\{"get":\{"calls":2,"responses":\{"success":1,"notfound":1/
        filedata.should =~ /"set":\{"calls":1,"responses":\{"success":1/
      end
    rescue Memcached::SomeErrorsWereReported
    end
  end
end

describe 'Memprof request tracing' do
  @tempfile = Tempfile.new('tracing_spec')

  def filename
    @tempfile.path
  end

  def filedata
    File.read(filename)
  end

  should 'trace request env' do
    env = {"REQUEST_PATH" => "value"}

    Memprof.trace_filename = filename
    Memprof.trace_filename.should == filename

    Memprof.trace_request(env) do
    end

    Memprof.trace_filename = nil
    Memprof.trace_filename.should.be.nil

    filedata.should =~ /"REQUEST_PATH":"value"/
  end
end
