require File.dirname(__FILE__) + "/../ext/memprof"

require 'rubygems'
require 'bacon'
Bacon.summary_on_exit

require 'tempfile'

describe 'Memprof tracers' do
  @tempfile = Tempfile.new('tracing_spec')

  def filename
    @tempfile.path
  end

  def filedata
    File.read(filename)
  end

  should 'trace i/o for block of code' do
    require 'open-uri'
    Memprof.trace(filename) do
      open("http://google.com").read
    end

    filedata.should =~ /"read":\{"calls":\d+/
    filedata.should =~ /"write":\{"calls":\d+/
    filedata.should =~ /"connect":\{"calls":\d+/
  end

  should 'trace objects created for block of code' do
    Memprof.trace(filename) do
      10.times{1.1+1.2}
    end

    filedata.should =~ /"float":10/
  end

  should 'trace gc runs for block of code' do
    Memprof.trace(filename) do
      10.times{GC.start}
    end

    filedata.should =~ /"gc":\{"calls":10,"time":[\d.]+/
  end

  should 'trace memory allocation for block of code' do
    Memprof.trace(filename) do
      10.times{ "abc" << "def" }
    end

    filedata.should =~ /"malloc":\{"calls":10/
    filedata.should =~ /"realloc":\{"calls":10/
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
    env = {"HEADER" => "value"}

    Memprof.trace_filename = filename
    Memprof.trace_filename.should == filename

    Memprof.trace_request(env) do
    end

    Memprof.trace_filename = nil
    Memprof.trace_filename.should.be.nil

    filedata.should =~ /"HEADER":"value"/
  end
end