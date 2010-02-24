require File.dirname(__FILE__) + "/../ext/memprof"

require 'rubygems'
require 'bacon'
Bacon.summary_on_exit

require 'tempfile'

describe Memprof do
  @tempfile = Tempfile.new('memprof_spec')

  def filename
    @tempfile.path
  end

  def filedata
    File.read(filename)
  end

  before do
    Memprof.stop
  end

  should 'print stats to a file' do
    Memprof.start
    "abc"
    Memprof.stats(filename)

    filedata.strip.should == "1 #{__FILE__}:#{__LINE__-3}:String"
  end

  should 'allow calling ::stats multiple times' do
    Memprof.start
    []
    Memprof.stats(filename)
    []
    Memprof.stats(filename)

    filedata.strip.split("\n").size.should == 2
  end

  should 'clear stats after ::stats!' do
    Memprof.start
    []
    Memprof.stats!(filename)
    Memprof.stats(filename)

    filedata.strip.should.be.empty
  end

  should 'collect stats via ::track' do
    Memprof.track(filename) do
      "abc"
    end

    filedata.should =~ /1 #{__FILE__}:#{__LINE__-3}:String/
  end

  should 'dump objects as json' do
    Memprof.start
    1.23+1
    Memprof.dump(filename)

    filedata.should =~ /"file": "#{__FILE__}"/
    filedata.should =~ /"line": #{__LINE__-4}/
    filedata.should =~ /"type": "float"/
    filedata.should =~ /"data": 2\.23/
  end

  should 'raise error when calling ::stats or ::dump without ::start' do
    lambda{ Memprof.stats }.should.raise(RuntimeError).message.should =~ /Memprof.start/
    lambda{ Memprof.dump }.should.raise(RuntimeError).message.should =~ /Memprof.start/
  end

  should 'dump out the entire heap' do
    Memprof.stop
    Memprof.dump_all(filename)

    File.open(filename, 'r').each_line do |line|
      if line =~ /"data": "dump out the entire heap"/
        break :found
      end
    end.should == :found
  end
end

